#pragma once
// Small numeric helpers that reproduce Hypersonic's conventions (see docs/engine/sample_voice.md).
#include <cmath>
#include <cstdint>

namespace hs2 {

constexpr float kLn2 = 0.6931471805599453f;
constexpr float kSemitone = 0.057762265f;    // ln2 / 12
constexpr float kCent = 0.0005776227f;       // ln2 / 1200
constexpr float kTenOctaves = 6.931472f;     // 10 * ln2
constexpr int kBlock = 32;                   // control-rate block (samples)

// Env/glide time curve (FUN_10048da0): 4v^2 up to 1 s at v = 0.5, then 32 v^5 (32 s at v = 1).
inline float timeCurve(float v) {
    float v2 = v * v;
    return v > 0.5f ? v2 * v2 * v * 32.f : v2 * 4.f;
}

// FUN_100489b0: tempo note value (cycles or notes per beat) for sync'd LFOs and delays, index trunc(v*20.9).
inline float noteValue(float v) {
    static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                     0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                     2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
    const unsigned i = unsigned(int(v * 20.9f));
    return i < 20 ? kNotes[i] : 16.f;
}

// ---- x87 mirrors. The original runs the FPU at 53-bit precision (control word 0x027F): register values are
// doubles, rounded to float only where the code stores them. These return the unrounded register value.
// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale.
inline double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}
// FUN_1000c5b0: exp of a float argument.
inline double expF(float x) { return x87exp(double(x)); }
// FUN_10048da0: the time curve, 4v^2 or 32v^5 above v = 0.5.
inline double timeCurveD(float v) {
    const double v2 = double(v) * double(v);
    return v > 0.5f ? v2 * v2 * double(v) * 32.0 : v2 * 4.0;
}
// FUN_100499b0 (float args): per-block approach coefficient 1 - exp(T / t * -3.45), 1 when t < 1e-6.
inline double timeCoefD(float time, float blockT) {
    if (time < 1e-6f) return 1.0;
    return 1.0 - x87exp(double(blockT) / double(time) * double(-3.45f));
}

// MSVC rand(): Hypersonic's only random source.
struct MsvcRand {
    uint32_t state = 1;
    int next() {
        state = state * 214013u + 2531011u;
        return int((state >> 16) & 0x7fff);
    }
    float uni() { return float(next()) * 3.0517578e-05f; }        // [0, 1)   FUN_10048dd0
    float bi() { return float(next()) * 6.1035156e-05f - 1.f; }   // [-1, 1)  FUN_10048df0
};

}  // namespace hs2
