// 4 Reverse Reverb (ctor FUN_10027bd0, setSampleRate FUN_10027c00, tempo FUN_10027d20, process FUN_10027d50,
// setParam FUN_10028460; vtable 0x100a8b70).
// One 1 s line of 12 series allpasses in a single buffer: 8 stages with gain g = 0.05 + 0.45*p3 (lengths
// T*{0.15, 0.1} * 0.78^k), whose output (doubled) plus the input delayed by 0.85*T is scaled by a slowly
// rising gain (0.0001/sample toward p4*p0, reset to 0 when T changes, at most every sr/4 samples) and fed to
// 4 stages with gain 0.5 (T*0.035*0.78^k). Output: dry*(1-p0^2) + input delayed by T-1 * p5*p0 + reverb.
// T = (0.02 + 0.98*p2)*sr, or tempo-synced when p1 > 0.5 (beat from the tempo hook; 0 when never called, which
// reads the line one frame before its start on the first block: the oracle's heap holds zeros there).
// The x87 code keeps several values in 53-bit registers; mirrored with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class ReverseReverb : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        w_ = 0;
        L_ = int(sr);
        x_.assign(size_t(L_) * 2, 0.f);
        b_.assign(L_, 0.f);
        reapplyParams();
        Tcur_ = T_;
        gain_ = target_;
        counter_ = 1;
    }
    // vtable +0xc (FUN_10027d20): beat = 1/b, then re-applies param 2. Never called by the oracle.
    void setTempo(float, float b) override {
        beat_ = b != 0.f ? 1.f / b : 0.f;
        setParam(2, p_[2]);
    }
    // FUN_10027d50 takes only (buffer, frames): no silence gating, it always processes and never clears the flag.
    void processGated(float* io, int n, bool&) override { process(io, n); }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const int L = L_, L1 = L - 1;
        if (T_ != Tcur_ && counter_ > 0) {
            gain_ = 0;
            counter_ = -(int(sr_) / 4);
            Tcur_ = T_;
        }
        counter_ += n;
        int w = w_;
        auto wrap = [&](int x) { return L1 < x ? x - L : x; };
        const double T = Tcur_;
        int tr = wrap(w - int(T * double(-0.85f)));
        int tc = wrap(int(T) + w - 1);
        int a[12];
        int prev = w;
        const float bases[3] = {0.15f, 0.1f, 0.035f};
        for (int grp = 0; grp < 3; ++grp) {
            double f = T * double(bases[grp]);
            for (int k = 0; k < 4; ++k) {
                const double fk = k < 3 ? f : f * double(0.78f);
                prev = wrap(int(fk) + prev);
                a[grp * 4 + k] = prev;
                if (k < 2) f *= double(0.78f);
            }
        }
        const double g = g_;
        const double dry = dry_, wd = wetDry_;
        float* B = b_.data();
        float* X = x_.data();
        auto dec = [&](int& t) {
            if (--t < 0) t = L1;
        };
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            X[2 * w] = inL;
            X[2 * w + 1] = inR;
            const double s = double(inR) + inL;
            // 8 stages, gain g
            const double v1 = B[a[0]] - g * s;
            const double gv1 = g * v1;
            B[w] = float(gv1 + s);
            const float v2f = float(B[a[1]] - gv1);
            const float gp = float(v2f * g);
            B[a[0]] = float(v2f * g + v1);
            dec(a[0]);
            const double v3 = double(B[a[2]]) - gp;
            const float v3f = float(v3);
            double gv = v3 * g;
            B[a[1]] = float(double(v2f) + gv);
            dec(a[1]);
            float prevf = v3f;
            for (int k = 3; k < 7; ++k) {  // stages 4..7: value rounded to float, then scaled
                const float vf = float(B[a[k]] - gv);
                gv = vf * g;
                B[a[k - 1]] = float(double(prevf) + gv);
                dec(a[k - 1]);
                prevf = vf;
            }
            const double v8 = B[a[7]] - gv;
            B[a[6]] = float(g * v8 + prevf);
            dec(a[6]);
            // feed of the 0.5 stages: (2*v8 + input delayed by 0.85T) * rising gain
            const float* xr = X + 2 * tr;
            const float yf = float(((v8 + v8) + xr[1] + xr[0]) * gain_);
            dec(tr);
            gain_ = float((double(target_) - gain_) * double(9.999999747378752e-05f) + gain_);
            const double u1 = B[a[8]] - 0.5 * yf;
            const float u1f = float(u1);
            B[a[7]] = float(double(yf) + 0.5 * u1);
            dec(a[7]);
            const float u2f = float(B[a[9]] - 0.5 * u1);
            B[a[8]] = float(double(u1f) + 0.5 * u2f);
            dec(a[8]);
            const float u3f = float(B[a[10]] - 0.5 * u2f);
            B[a[9]] = float(double(u2f) + 0.5 * u3f);
            dec(a[9]);
            const double u4 = B[a[11]] - 0.5 * u3f;
            B[a[10]] = float(0.5 * u4 + u3f);
            dec(a[10]);
            dec(a[11]);
            // dry tap; tc can be -1 (T == 0) on the first block: the original then reads zeros before the line
            const double dl = tc >= 0 ? X[2 * tc] : 0.f, dr = tc >= 0 ? X[2 * tc + 1] : 0.f;
            io[2 * i] = float((dl * wd + inL * dry) + u4);
            io[2 * i + 1] = float((dr * wd + inR * dry) + u4);
            dec(tc);
            dec(w);
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0:
                dry_ = float(1.0 - double(p_[0]) * p_[0]);
                [[fallthrough]];
            case 4:
                target_ = float(double(p_[4]) * p_[0] + double(9.99999993922529e-09f));
                [[fallthrough]];
            case 5: wetDry_ = float(double(p_[5]) * p_[0]); break;
            case 1:
            case 2: {
                double t;
                if (!(p_[1] <= 0.5f)) {
                    t = double(beat_) / double(noteValue(1.f - p_[2]));
                    while (double(L_ - 1) < t) t *= 0.5;
                } else {
                    t = (double(p_[2]) * double(0.98f) + double(0.02f)) * double(sr_);
                }
                T_ = float(t);
                break;
            }
            case 3: g_ = float(double(v) * double(0.45f) + double(0.05f)); break;
            default: break;
        }
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    int L_ = 0, w_ = 0, counter_ = 0;
    std::vector<float> x_, b_;
    float T_ = 0, Tcur_ = 0, g_ = 0, dry_ = 0, target_ = 0, wetDry_ = 0, gain_ = 0, beat_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_4() { return std::make_unique<ReverseReverb>(); }

}  // namespace hs2
