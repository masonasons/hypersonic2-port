// 103 Amp Simulator (ctor FUN_10041620, setSampleRate FUN_10041680, setParam FUN_10041fe0, process FUN_100418a0,
// vtable +8 FUN_10041670 = setPitchSource: stores the part's note-pitch pointer at this+0xe0).
// Note tracking (process, when the pitch value changed): pitch <= -9 (no note) -> feedback gain 0 and comb term 0;
// else the feedback read offset becomes one period sr/(16.3516*exp(pitch)) samples (integer part clamped to the
// 50 ms line, fraction used for linear interpolation) and the line also feeds back -0.99 x its own read (a comb
// tuned to the note). Without a pitch source the offset stays 1 sample, the comb term 0, the feedback gain p4.
// Per channel (mono: L+R summed; stereo: 2x each channel, separate state): x = in + feedback, clipped drive stage
// clamp(x*exp(6*p3), -0.3, 0.6) added to a 2-pole resonant "speaker" section whose output feeds a 5 ms cabinet line
// with two taps (model dependent gains/delays), then two SVF-style lowpass stages; out = lp*0.05 - hp*treble.
// The (mono-summed) output goes through a one-pole lowpass (2000/sr) into a 50 ms feedback line (gain 0.06*p4^2).
// p0 mix, p2 model (8 cabinets: trunc(p*7.9)), p3 drive, p4 feedback, p5 treble (+-, |t|*t*0.1), p6 mono/stereo.
// Quirk kept: in stereo mode the first lowpass integrator of each channel is updated as s1 -= z instead of
// s1 -= z*f (mono uses f). Intermediates follow the original x87 register/float-spill pattern exactly.
// Oracle-verified: bit-exact (-240 dB) on all 8 models, mono and stereo, --random 8 + extremes, both with
// fx_compare --pitch (default note timeline incl. note-off) and without a pitch source; --gated (with and without
// --pitch): -240 dB, 0 flag mismatches. Gate order: mix == 0 returns first (flag untouched), a new held note clears
// the silent flag, then the standard countdown.
#include "effect_base.h"

namespace hs2 {
namespace {

inline float bitsf(unsigned u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

class AmpSimulator : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        a_ = Lp{};
        b_ = Lp{};
        r0_ = r1_ = 0;            // 0xa0, 0xa4
        s0_ = s1_ = 0;            // 0x114, 0x118
        d0_ = 0;                  // 0xd0
        f4_ = 0;                  // 0xf4
        f0_ = float(inv * 2000.0);
        const int nb = int(double(sr) * double(0.005f));
        if (nb != b4_) {
            b4_ = nb;
            buf1_.assign(size_t(nb) + 2, 0.f);
            buf3_.assign(size_t(nb) + 2, 0.f);
            b8_ = 0;
        }
        std::fill(buf1_.begin(), buf1_.end(), 0.f);
        std::fill(buf3_.begin(), buf3_.end(), 0.f);
        const int nf = int(double(sr) * double(0.05f));
        if (nf != d4_) {
            d4_ = nf;
            buf2_.assign(size_t(nf) + 2, 0.f);
            d8_ = 0;
            dc_ = 1;
        }
        std::fill(buf2_.begin(), buf2_.end(), 0.f);
        reapplyParams();
    }
    // vtable +8 (FUN_10041670): the element connects the part's note pitch (log-frequency, -10 = no note held)
    void setPitchSource(const float* p) override { pitch_ = p; }
    // Order in the original: mix == 0 -> return (flag untouched, no gate); pitch update (a new held note clears
    // the silent flag); then the standard gate countdown; then the audio loop.
    void processGated(float* lr, int frames, bool& silent) override {
        if (mix_ == 0.f) return;
        updatePitch(&silent);
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= frames;
            silent = false;
        }
        run(lr, frames);
    }
    void process(float* io, int n) override {
        if (mix_ == 0.f) return;
        updatePitch(nullptr);
        run(io, n);
    }

private:
    void updatePitch(bool* silent) {
        if (pitch_ && *pitch_ != e4_) {
            const float v = *pitch_;
            e4_ = v;
            if (v <= -9.f) {
                f4_ = 0;
            } else {  // feedback delay = one period of the note: sr / (16.3516 Hz * exp(v))
                const double per = double(sr_) / (std::exp(double(v)) * double(16.351599f));
                const int whole = int(per);
                dc_ = whole;
                ec_ = float(double(float(per)) - whole);
                if (dc_ > d4_) dc_ = d4_;
                f4_ = 0.99f;  // 0x3f7d70a4
                if (silent) *silent = false;
            }
        }
    }
    void run(float* io, int n) {
        if (b4_ <= 0 || d4_ <= 0) return;
        const float fbk = e4_ <= -9.f ? 0.f : e8_;
        const int b1 = b4_ - 1;
        int i14 = bc_ + b8_;
        if (b1 < i14) i14 -= b4_;
        int i16 = c0_ + b8_;
        if (b1 < i16) i16 -= b4_;
        const int L = d4_, L1 = d4_ - 1;
        int r = dc_ + d8_;
        if (L1 < r) r -= L;
        const double drive = drive_;
        float* B1 = buf1_.data();
        float* B2 = buf2_.data();
        float* B3 = buf3_.data();
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const float fb = float((double(B2[r + 1]) - B2[r]) * double(ec_) + B2[r]);
            if (--r < 0) r = L1;
            float outLf;
            double outR, mixIn;
            if (!stereo_) {
                const double s = (double(fb) + inR) + inL;
                double c = s * drive;
                if (c > double(0.6f)) c = double(0.6f);
                else if (c < double(-0.3f)) c = double(-0.3f);
                const double t = ((s - r1_) - double(ac_) * r0_) + c;
                const double a0 = t * a8_ + r0_;
                r0_ = float(a0);
                r1_ = float(a0 * a8_ + r1_);
                B1[b8_] = float(t);
                if (--b8_ < 0) b8_ = b1;
                --i14;
                double y = double(B1[i14 + 1]) * c4_ + t;
                if (i14 < 0) i14 = b1;
                --i16;
                y = double(B1[i16 + 1]) * c8_ + y;
                if (i16 < 0) i16 = b1;
                const double z = ((double(a_.s0) * k98_ + a_.s2) + a_.s1) + y;
                const double n80 = double(a_.s0) - z * k94_;
                a_.s0 = float(n80);
                const double v84 = n80 * k94_ + a_.s1;
                a_.s1 = float(v84);
                a_.s2 = float(y);
                const double v8c = double(a_.s3) - ((v84 + a_.s4) + a_.s3) * k9c_;
                a_.s3 = float(v8c);
                const double v90 = v8c * k9c_ + a_.s4;
                a_.s4 = float(v90);
                const double out = v90 * double(0.05f) - z * f8_;
                mixIn = out;
                outLf = 0;  // unused
                outR = out;
                const double vd0 = (out - d0_) * f0_ + d0_;
                d0_ = float(vd0);
                B2[d8_] = float(vd0 * fbk - double(fb) * f4_);
                if (--d8_ < 0) {
                    d8_ = L1;
                    B2[L] = B2[0];
                }
                io[2 * i] = float((out - inL) * mix_ + inL);
                io[2 * i + 1] = float((out - inR) * mix_ + inR);
                continue;
            }
            // stereo
            const float sLf = float((double(inL) + inL) + fb);
            const double sR = (double(inR) + inR) + fb;
            float cL = float(double(sLf) * drive);  // spilled as float, but the 0.6 test uses the register value
            if (double(sLf) * drive > double(0.6f)) cL = 0.6f;
            else if (cL < -0.3f) cL = -0.3f;
            double cR = sR * drive;
            if (cR > double(0.6f)) cR = double(0.6f);
            else if (cR < double(-0.3f)) cR = double(-0.3f);
            const double tL = ((double(sLf) - r1_) - double(ac_) * r0_) + cL;
            const float tLf = float(tL);
            {
                const double a0 = tL * a8_ + r0_;
                r0_ = float(a0);
                r1_ = float(a0 * a8_ + r1_);
            }
            const double tR = ((sR - s1_) - double(s0_) * ac_) + cR;
            const float tRf = float(tR);
            {
                const double a0 = tR * a8_ + s0_;
                s0_ = float(a0);
                s1_ = float(a0 * a8_ + s1_);
            }
            B1[b8_] = tLf;
            B3[b8_] = tRf;
            if (--b8_ < 0) b8_ = b1;
            --i14;
            double yL = double(B1[i14 + 1]) * c4_ + tLf;
            const float yRp = float(double(B3[i14 + 1]) * c4_ + tRf);
            if (i14 < 0) i14 = b1;
            --i16;
            yL = double(B1[i16 + 1]) * c8_ + yL;
            const double yR = double(B3[i16 + 1]) * c8_ + yRp;
            if (i16 < 0) i16 = b1;
            const float zLf = float(((double(a_.s0) * k98_ + a_.s2) + a_.s1) + yL);
            {
                const double n80 = double(a_.s0) - zLf;  // original omits the *f here
                a_.s0 = float(n80);
                const double v84 = n80 * k94_ + a_.s1;
                a_.s1 = float(v84);
                a_.s2 = float(yL);
                const double v8c = double(a_.s3) - ((v84 + a_.s4) + a_.s3) * k9c_;
                a_.s3 = float(v8c);
                a_.s4 = float(v8c * k9c_ + a_.s4);
            }
            const float oLp = a_.s4;
            const float zRf = float(((double(k98_) * b_.s0 + yR) + b_.s2) + b_.s1);
            double v110;
            {
                const double n100 = double(b_.s0) - zRf;
                b_.s0 = float(n100);
                const float v104 = float(n100 * k94_ + b_.s1);
                b_.s1 = v104;
                b_.s2 = float(yR);
                const double v10c = double(b_.s3) - ((double(v104) + b_.s4) + b_.s3) * k9c_;
                b_.s3 = float(v10c);
                v110 = v10c * k9c_ + b_.s4;
                b_.s4 = float(v110);
            }
            outLf = float(double(oLp) * double(0.05f) - double(zLf) * f8_);
            outR = v110 * double(0.05f) - double(zRf) * f8_;
            mixIn = (double(outLf) + outR) * 0.5;
            const double vd0 = (mixIn - d0_) * f0_ + d0_;
            d0_ = float(vd0);
            B2[d8_] = float(vd0 * fbk - double(fb) * f4_);
            if (--d8_ < 0) {
                d8_ = L1;
                B2[L] = B2[0];
            }
            io[2 * i] = float((double(outLf) - inL) * mix_ + inL);
            io[2 * i + 1] = float((outR - inR) * mix_ + inR);
        }
    }

private:
    struct Lp {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;  // 0x80..0x90 (L/mono), 0x100..0x110 (R)
    };
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: model(int(double(v) * double(7.9f))); return;
            case 3: drive_ = float(std::exp(double(v) * 6.0)); return;
            case 4: e8_ = float(double(v) * v * double(0.06f)); return;
            case 5: {
                const double d = double(v) - 0.5;
                f8_ = float(std::fabs(d) * d * double(0.1f));
                return;
            }
            case 6: stereo_ = v > 0.5f; return;
            default: return;
        }
    }
    void model(int m) {
        const double sr = sr_, inv = invSr_;
        auto set = [&](unsigned k9c, unsigned k98, unsigned kac, float f94, unsigned kc4, unsigned kc8, float fa8,
                       float tbc, float tc0) {
            k9c_ = bitsf(k9c);
            k98_ = bitsf(k98);
            ac_ = bitsf(kac);
            k94_ = float(inv * f94);
            c4_ = bitsf(kc4);
            c8_ = bitsf(kc8);
            a8_ = float(inv * fa8);
            bc_ = int(sr * double(tbc));
            c0_ = int(sr * double(tc0));
        };
        switch (m) {
            case 0: set(0x3f000000, 0x3f4ccccd, 0x3f800000, 22000.f, 0x3fb47ae1, 0x3f83d70a, 628.f, 0.00034f, 0.00056f); break;
            case 1: set(0x3f800000, 0x3f570a3d, 0x3ec28f5c, 17200.f, 0x3fc8f5c3, 0xbe3851ec, 208.f, 0.0004f, 0.00036f); break;
            case 2: set(0x3ed70a3d, 0x3ef5c28f, 0x3f0ccccd, 22000.f, 0x3f73b646, 0x3f73b646, 424.f, 0.0004f, 0.00036f); break;
            case 3: set(0x3f000000, 0x3f000000, 0x3e8f5c29, 22000.f, 0x3f428f5c, 0x3fb47ae1, 126.f, 0.00088f, 0.00048f); break;
            case 4: set(0x3f800000, 0x3edc28f6, 0x3e6147ae, 15000.f, 0x3f4ccccd, 0x3ecccccd, 628.f, 0.0018f, 0.001f); break;
            case 5: set(0x3e800000, 0x3f07ae14, 0x3f547ae1, 20200.f, 0x3fc28f5c, 0x00000000, 1500.f, 0.f, 0.f); break;
            case 6: set(0x3ea8f5c3, 0x3f3ae148, 0x3ee147ae, 22000.f, 0x3f4ccccd, 0x3f800000, 733.f, 0.0011f, 8e-05f); break;
            case 7: set(0x3f800000, 0x3f3d70a4, 0x3f0a3d71, 17200.f, 0x00000000, 0xbf800000, 5600.f, 0.00072f, 0.00048f); break;
            default: break;
        }
    }
    const float* pitch_ = nullptr;
    std::vector<float> buf1_, buf2_, buf3_;
    int b4_ = 0, b8_ = 0, bc_ = 0, c0_ = 0, d4_ = 0, d8_ = 0, dc_ = 0;
    bool stereo_ = false;
    Lp a_, b_;
    float mix_ = 0.5f, drive_ = 1, e4_ = 0, e8_ = 0, ec_ = 0, f0_ = 0, f4_ = 0, f8_ = 0;
    float k94_ = 0, k98_ = 0, k9c_ = 0, ac_ = 0, a8_ = 0, c4_ = 0, c8_ = 0;
    float r0_ = 0, r1_ = 0, s0_ = 0, s1_ = 0, d0_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_103() { return std::make_unique<AmpSimulator>(); }

}  // namespace hs2
