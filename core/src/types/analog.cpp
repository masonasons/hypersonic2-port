// Analog element (type 5): three band-limited oscillators (integrated minimum-phase BLEPs), sub oscillator, noise,
// optional wavetable oscillators, the shared SVF or the "Analog" ladder filter.
//
// Original code:
//   - Element setter FUN_100221f0, Analog cases (docs/engine/element_setter.c; element idx 0x11..0x25):
//       0x11 Filter (+0x8a0, v > 0.5: "Analog" filter), 0x12 Osc1 Phase (+0x8bc = -1 / 1), 0x13 Wave (+0x89c osc 1
//       wave, trunc(v*7.9)), 0x14 Highpass (+0x8a8 = 1 - exp(7v + ln(1/sr) + 3): leak of the integrators),
//       0x15 Osc1 Mix (+0x8ac = (v^2 + 1e-8) * phase sign), 0x16/0x17 Semi/Cent (+0x894 = exp((semi+cent)*ln2/12),
//       osc 1 ratio), 0x18 Min PW (+0x8b0 = 0.1v + 0.001), 0x19/0x1a Osc2 Semi/Cent (+0x8d0 osc 2 ratio),
//       0x1b Osc2 Wave (+0x8a4), 0x1c Osc3 Wave (+0x880), 0x1d Osc3 Mix (+0x898 = v + 1e-8), 0x1e Sub Osc
//       (+0x8b4 = 0.7079458 v^2 + 1e-8), 0x1f Noise (+0x8b8 = v^2), 0x20 Osc3 Filter Mod (+0x8c0 = 0.68 v^2),
//       0x21 Osc2 Mix (+0x8d4 = v + 1e-8), 0x22 Wavetable (+0xbb8 = trunc(v*128.9), wavetable bank 0 entry),
//       0x23 Filt Dist (+0x8c4 = v/2), 0x24 Wavetable Aliasing (+0x888 = (short)trunc(v^2*16383), XOR mask),
//       0x25 Osc3 Mute (raw > 0.5). Common 0x66 (+0x5e0) is the base of osc 2's pulse width (mod dest 9).
//   - Patch loader FUN_10016f70: unless the patch type (patch 0x44, trunc(v*15.9), 0 -> 5) is 1 or 5, the Analog
//     params 0x19..0x25 and Note-on Click are reset before the setter runs; old patches scale Note-on Click by 0.3.
//     Both are applied to the stored params in PatchPlayer::load (synth.cpp).
//   - Voice note-on FUN_10014910 case 5 (@100152d5): render mode, oscillator start state (startOsc). The shared
//     Voice does the rest of the note-on (incl. the mono unison stack terms of param_7 and the Note-on Click, whose
//     Analog part seeds the filter states: onClick), the x87-exact control update, envelopes and LFO 3.
//   - Render (part render switch FUN_100139c0): 5 = 1004cc00 (osc 3 + osc 1 + SVF), -2 = 1004d7a0 (same + ladder),
//     -3 = 1004e400 (osc 3 + osc 1 + osc 2 + wavetable oscs + SVF with osc-3 cutoff FM and drive; also the "Analog"
//     filter for modes 3/6), -4 = 1004fc10 (the -3 oscillators + ladder with osc-3 FM and drive).
//     Mode choice: the Analog filter with modes 1/2/8 gives -2; any of dist/osc3 FM > 1e-4, osc 2 mix > 0.01, osc 1
//     wave > 5 or Osc3 Mute gives the full path (-3, or -4 with the ladder); the Analog filter with modes 3/6 gives -3.
//   - Control update FUN_1004ab60 (shared; Analog: v[0x13] = osc 2 PW base, its own filter words, a = 3.25 for
//     -2/-4: filterControl), then the per-mode oscillator/ladder coefficient block of each render function.
//   - BLEP insert FUN_1004a0c0: 32 taps x 64 sub-sample phases (table @103bfea8, linear interpolation), added to a
//     32-sample ring indexed by the control-block counter (one ring per oscillator: +0x538, +0x5b8, +0x638).
//
// Oscillators (render functions above): osc 3 (+0x40 phase, master pitch = 2x the note frequency) produces impulses at
// every wrap, its counter (0..3) builds the sub octave; waves 0 saw, 1 square (half), 2 "integrated" (feeds the
// osc-1 integrator, leak 1 - inc), others square. Osc 1/2 (+0x164 / +0x1cc, ratio * note frequency) waves 0 pulse
// (PW from the Wave dest), 1 PWM square, 2 pulse with fixed width, 3 saw, 4 square, 5 7-voice "unison" saw,
// 6/7 wavetable (-3/-4 only). All impulses are integrated by leaky integrators (+0x158 osc 3, +0x170 osc 1+2).
// Noise: a 16-bit LCG (x91) on the low half of a float near 128, plus one per block.
//
// Precision: the x87 runs at 53 bits; register values are double here, float stores at the original's store points,
// `fst` keeps the unrounded register. Verified against the emulated original with tools/emu/part_oracle.py (with the
// Wavetables bank opened): all four render modes, all osc waves incl. wavetable, unison saw, sub, noise, osc-3 FM,
// drive, click, mono unison stacks reach -95..-124 dB per note when the shared parts agree (patch LFOs; see NOTES).
// Debug: HS2_ANTRACE=<file> dumps the modelled voice words after each render call (see trace()).
// Not modelled: the host tempo scale on envelope rates (tempo factor fixed at 1).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "hs2/bank.h"
#include "hs2/content.h"
#include "hs2/element_types.h"

namespace hs2 {

namespace {

float asFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint32_t asBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// fistp with RC = chop (_ftol): out of range gives the integer indefinite 0x80000000.
int32_t chop(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0)) return INT32_MIN;
    return int32_t(d);
}

// The original's float clamp: a value with exponent bit 30 set (|x| >= 2) becomes +-2.
float clamp2(float f) {
    const uint32_t u = asBits(f);
    return (u & 0x40000000u) ? asFloat(u & 0xc0000000u) : f;
}

// BLEP table @0x103bfea8: 32 taps x 64 phases (+2 guard values for the interpolation at phase 64).
const uint32_t kBlepBits[2050] = {
    0xb3b650f9, 0xb3bf5aef, 0xb39b802d, 0xb2be2bda, 0x33a6c318, 0x348469e9, 0x35088d81, 0x356f9132,
    0x35c01e50, 0x36114122, 0x365281d3, 0x3693a736, 0x36c9d28b, 0x3707043c, 0x37316dac, 0x37659885,
    0x37929214, 0x37b8f00b, 0x37e6e7e8, 0x380ecbb1, 0x382f1d13, 0x385515f7, 0x3880baa9, 0x389a862a,
    0x38b85fb5, 0x38dac3b9, 0x39011cbe, 0x3917a9da, 0x39315894, 0x394e7eaf, 0x396f789b, 0x398a54e7,
    0x399f3e8b, 0x39b6b278, 0x39d0edf0, 0x39ee3265, 0x3a0762d4, 0x3a19790a, 0x3a2d835d, 0x3a43abc7,
    0x3a5c1ecb, 0x3a770b89, 0x3a8a51eb, 0x3a9a8e28, 0x3aac563b, 0x3abfc75d, 0x3ad5004a, 0x3aec214e,
    0x3b02a627, 0x3b105266, 0x3b1f27fb, 0x3b2f3a58, 0x3b409dc9, 0x3b53677d, 0x3b67ad87, 0x3b7d86e1,
    0x3b8a85b8, 0x3b972a02, 0x3ba4bd2f, 0x3bb34c97, 0x3bc2e60c, 0x3bd397d7, 0x3be570bd, 0x3bf87ffc,
    0x3c066aa5, 0x3c11406b, 0x3c1cc9a3, 0x3c290ede, 0x3c3618e3, 0x3c43f0b7, 0x3c529f96, 0x3c622ef3,
    0x3c72a879, 0x3c820b04, 0x3c8b40d8, 0x3c94fada, 0x3c9f3e45, 0x3caa1068, 0x3cb576a8, 0x3cc1767f,
    0x3cce157a, 0x3cdb5934, 0x3ce9475c, 0x3cf7e5ac, 0x3d039cf6, 0x3d0ba4f7, 0x3d140dc5, 0x3d1cda51,
    0x3d260d8e, 0x3d2faa6e, 0x3d39b3e4, 0x3d442ce3, 0x3d4f1859, 0x3d5a7932, 0x3d665253, 0x3d72a69b,
    0x3d7f78e1, 0x3d8665f9, 0x3d8d5148, 0x3d947fb9, 0x3d9bf29e, 0x3da3ab44, 0x3dabaaed, 0x3db3f2d2,
    0x3dbc8422, 0x3dc56001, 0x3dce8784, 0x3dd7fbb6, 0x3de1bd94, 0x3debce08, 0x3df62df2, 0x3e006f0e,
    0x3e05efa1, 0x3e0b9906, 0x3e116b86, 0x3e176762, 0x3e1d8ccf, 0x3e23dbf7, 0x3e2a54f9, 0x3e30f7e6,
    0x3e37c4c5, 0x3e3ebb8f, 0x3e45dc30, 0x3e4d2685, 0x3e549a5f, 0x3e5c377d, 0x3e63fd93, 0x3e6bec44,
    0x3e740322, 0x3e7c41b1, 0x3e8253b1, 0x3e8699cd, 0x3e8af2d5, 0x3e8f5e68, 0x3e93dc1d, 0x3e986b84,
    0x3e9d0c21, 0x3ea1bd70, 0x3ea67ee6, 0x3eab4fed, 0x3eb02fe4, 0x3eb51e23, 0x3eba19fa, 0x3ebf22ac,
    0x3ec43775, 0x3ec95787, 0x3ece820c, 0x3ed3b622, 0x3ed8f2e2, 0x3ede3757, 0x3ee38287, 0x3ee8d36d,
    0x3eee28fb, 0x3ef3821e, 0x3ef8ddb5, 0x3efe3a9b, 0x3f01cbd1, 0x3f0479c9, 0x3f072697, 0x3f09d199,
    0x3f0c7a29, 0x3f0f1f9c, 0x3f11c147, 0x3f145e79, 0x3f16f680, 0x3f1988a7, 0x3f1c1435, 0x3f1e9871,
    0x3f21149e, 0x3f2387fe, 0x3f25f1d1, 0x3f285155, 0x3f2aa5c9, 0x3f2cee68, 0x3f2f2a6e, 0x3f315916,
    0x3f33799c, 0x3f358b3b, 0x3f378d2d, 0x3f397eb0, 0x3f3b5eff, 0x3f3d2d5a, 0x3f3ee8ff, 0x3f409130,
    0x3f422531, 0x3f43a447, 0x3f450dba, 0x3f4660d8, 0x3f479ced, 0x3f48c14d, 0x3f49cd4e, 0x3f4ac04a,
    0x3f4b99a0, 0x3f4c58b5, 0x3f4cfcf2, 0x3f4d85c4, 0x3f4df2a0, 0x3f4e4300, 0x3f4e7665, 0x3f4e8c57,
    0x3f4e8462, 0x3f4e5e1c, 0x3f4e1923, 0x3f4db51a, 0x3f4d31ae, 0x3f4c8e93, 0x3f4bcb86, 0x3f4ae84d,
    0x3f49e4b7, 0x3f48c09a, 0x3f477bd9, 0x3f46165d, 0x3f44901b, 0x3f42e910, 0x3f412144, 0x3f3f38ca,
    0x3f3d2fbd, 0x3f3b0644, 0x3f38bc90, 0x3f3652dd, 0x3f33c971, 0x3f31209b, 0x3f2e58b9, 0x3f2b7230,
    0x3f286d70, 0x3f254af6, 0x3f220b47, 0x3f1eaef3, 0x3f1b3697, 0x3f17a2d8, 0x3f13f465, 0x3f102bf9,
    0x3f0c4a59, 0x3f085052, 0x3f043ebc, 0x3f001679, 0x3ef7b0e8, 0x3eef0b41, 0x3ee63df9, 0x3edd4b19,
    0x3ed434bd, 0x3ecafd13, 0x3ec1a654, 0x3eb832cd, 0x3eaea4d5, 0x3ea4fed3, 0x3e9b433a, 0x3e917489,
    0x3e87954a, 0x3e7b5025, 0x3e675eff, 0x3e535c72, 0x3e3f4dda, 0x3e2b38a4, 0x3e172246, 0x3e031041,
    0x3dde1040, 0x3db61ee2, 0x3d8e5790, 0x3d4d8aeb, 0x3cfdcef7, 0x3c436c82, 0xbbe41f41, 0xbcd23ef2,
    0xbd34e18f, 0xbd7fb450, 0xbda4c10a, 0xbdc91ab7, 0xbdecdc9b, 0xbe07fe23, 0xbe1937ba, 0xbe2a1607,
    0xbe3a9418, 0xbe4aad17, 0xbe5a5c48, 0xbe699d12, 0xbe786afc, 0xbe8360d8, 0xbe8a4e7d, 0xbe90fc69,
    0xbe9768ab, 0xbe9d9166, 0xbea374d5, 0xbea91149, 0xbeae652a, 0xbeb36ef8, 0xbeb82d4d, 0xbebc9edd,
    0xbec0c275, 0xbec496fe, 0xbec81b7c, 0xbecb4f0e, 0xbece30f1, 0xbed0c07e, 0xbed2fd2c, 0xbed4e68f,
    0xbed67c59, 0xbed7be59, 0xbed8ac80, 0xbed946d8, 0xbed98d8f, 0xbed980ee, 0xbed9215f, 0xbed86f67,
    0xbed76baf, 0xbed616f9, 0xbed47228, 0xbed27e3d, 0xbed03c55, 0xbecdadac, 0xbecad39a, 0xbec7af92,
    0xbec44325, 0xbec08ffe, 0xbebc97e4, 0xbeb85cb7, 0xbeb3e070, 0xbeaf2521, 0xbeaa2cf5, 0xbea4fa2c,
    0xbe9f8f1d, 0xbe99ee36, 0xbe9419f6, 0xbe8e14f1, 0xbe87e1cd, 0xbe818342, 0xbe75f82e, 0xbe689e45,
    0xbe5afe90, 0xbe4d1ef1, 0xbe3f0560, 0xbe30b7ea, 0xbe223caf, 0xbe1399da, 0xbe04d5a7, 0xbdebecb5,
    0xbdce0484, 0xbdafff59, 0xbd91e9dd, 0xbd67a167, 0xbd2b80fa, 0xbcdf1731, 0xbc4f691d, 0x3aef444a,
    0x3c84b209, 0x3cf95402, 0x3d365507, 0x3d6f425c, 0x3d93ad53, 0x3daf437d, 0x3dca586f, 0x3de4e12b,
    0x3dfed2fa, 0x3e0c11b6, 0x3e18642f, 0x3e245c00, 0x3e2ff46c, 0x3e3b28e3, 0x3e45f503, 0x3e50549c,
    0x3e5a43b2, 0x3e63be7b, 0x3e6cc165, 0x3e754914, 0x3e7d5268, 0x3e826d3c, 0x3e85ef4c, 0x3e892e2e,
    0x3e8c28c9, 0x3e8ede22, 0x3e914d63, 0x3e9375d0, 0x3e9556d2, 0x3e96eff1, 0x3e9840d4, 0x3e994947,
    0x3e9a0933, 0x3e9a80a4, 0x3e9aafc7, 0x3e9a96e8, 0x3e9a3677, 0x3e998f01, 0x3e98a133, 0x3e976ddd,
    0x3e95f5eb, 0x3e943a69, 0x3e923c80, 0x3e8ffd79, 0x3e8d7eb7, 0x3e8ac1bd, 0x3e87c825, 0x3e8493a7,
    0x3e812613, 0x3e7b02a7, 0x3e734ed4, 0x3e6b34dd, 0x3e62b921, 0x3e59e025, 0x3e50ae98, 0x3e47294e,
    0x3e3d553c, 0x3e33377a, 0x3e28d53c, 0x3e1e33d2, 0x3e1358a5, 0x3e084933, 0x3dfa1620, 0x3de347b8,
    0x3dcc3295, 0x3db4e22d, 0x3d9d6209, 0x3d85bdbd, 0x3d5c01c7, 0x3d2c6e2b, 0x3cf9afa0, 0x3c9aab8e,
    0x3beff802, 0xbb88ae07, 0xbc7f47c0, 0xbcdc3e88, 0xbd1be7c0, 0xbd4915bf, 0xbd7593fe, 0xbd90a6d4,
    0xbda6172d, 0xbdbb111a, 0xbdcf8aef, 0xbde37b4a, 0xbdf6d914, 0xbe04cdc6, 0xbe0ddd21, 0xbe169691,
    0xbe1ef63c, 0xbe26f877, 0xbe2e99ca, 0xbe35d6ef, 0xbe3cacd8, 0xbe4318ac, 0xbe4917cb, 0xbe4ea7cc,
    0xbe53c681, 0xbe5871f8, 0xbe5ca87a, 0xbe60688d, 0xbe63b0f2, 0xbe6680a9, 0xbe68d6f0, 0xbe6ab342,
    0xbe6c1558, 0xbe6cfd2a, 0xbe6d6aed, 0xbe6d5f12, 0xbe6cda49, 0xbe6bdd7e, 0xbe6a69d9, 0xbe6880bb,
    0xbe6623c1, 0xbe6354c1, 0xbe6015c8, 0xbe5c691b, 0xbe585133, 0xbe53d0bd, 0xbe4eea9a, 0xbe49a1d9,
    0xbe43f9ba, 0xbe3df5aa, 0xbe379941, 0xbe30e83f, 0xbe29e68d, 0xbe229838, 0xbe1b0170, 0xbe132686,
    0xbe0b0be7, 0xbe02b61f, 0xbdf453a0, 0xbde2d76b, 0xbdd1013b, 0xbdbedad2, 0xbdac6e0f, 0xbd99c4e8,
    0xbd86e96a, 0xbd67cb5c, 0xbd4187af, 0xbd1b1c19, 0xbce939ca, 0xbc9c3c9e, 0xbc1ed1e9, 0xb9b9b429,
    0x3c12444c, 0x3c948ab6, 0x3cdf2c29, 0x3d147030, 0x3d38c0f2, 0x3d5c7600, 0x3d7f7d65, 0x3d90e2cf,
    0x3da19ed1, 0x3db1ea74, 0x3dc1bdbb, 0x3dd110f0, 0x3ddfdcae, 0x3dee19db, 0x3dfbc1b2, 0x3e0466e5,
    0x3e0a9c08, 0x3e107d69, 0x3e16085d, 0x3e1b3a6b, 0x3e20114c, 0x3e248aed, 0x3e28a570, 0x3e2c5f29,
    0x3e2fb6a5, 0x3e32aaa5, 0x3e353a22, 0x3e37644b, 0x3e392887, 0x3e3a8674, 0x3e3b7de7, 0x3e3c0eed,
    0x3e3c39ca, 0x3e3bfef7, 0x3e3b5f24, 0x3e3a5b36, 0x3e38f448, 0x3e372ba9, 0x3e3502d8, 0x3e327b89,
    0x3e2f97a1, 0x3e2c5932, 0x3e28c27f, 0x3e24d5f6, 0x3e209632, 0x3e1c05f6, 0x3e17282f, 0x3e11ffee,
    0x3e0c906b, 0x3e06dcff, 0x3e00e925, 0x3df570e8, 0x3de89d47, 0x3ddb5f00, 0x3dcdbde8, 0x3dbfc1f8,
    0x3db17353, 0x3da2da3e, 0x3d93ff1d, 0x3d84ea6d, 0x3d6b497e, 0x3d4c6d67, 0x3d2d51ee, 0x3d0e0877,
    0x3cdd44d4, 0x3c9e624e, 0x3c3f17f6, 0x3b839120, 0xbb6b9523, 0xbc36adfa, 0xbc98a75f, 0xbcd542e5,
    0xbd08849c, 0xbd25ed4f, 0xbd42cc09, 0xbd5f11a7, 0xbd7aaf6d, 0xbd8acb88, 0xbd97dd5e, 0xbda4868f,
    0xbdb0c0b1, 0xbdbc85a4, 0xbdc7cf8d, 0xbdd298df, 0xbddcdc5c, 0xbde69515, 0xbdefbe71, 0xbdf8542d,
    0xbe00292c, 0xbe03dab0, 0xbe073d07, 0xbe0a4ec3, 0xbe0d0ea4, 0xbe0f7b9b, 0xbe1194c6, 0xbe135975,
    0xbe14c928, 0xbe15e38d, 0xbe16a885, 0xbe171820, 0xbe17329b, 0xbe16f865, 0xbe166a1b, 0xbe158889,
    0xbe1454a7, 0xbe12cf9b, 0xbe10fab6, 0xbe0ed775, 0xbe0c677e, 0xbe09aca0, 0xbe06a8d3, 0xbe035e32,
    0xbdff9e01, 0xbdf7fb47, 0xbdefd945, 0xbde73d4a, 0xbdde2ce3, 0xbdd4add6, 0xbdcac622, 0xbdc07bf5,
    0xbdb5d5af, 0xbdaad9db, 0xbd9f8f28, 0xbd93fc6d, 0xbd88289d, 0xbd78358e, 0xbd5fb423, 0xbd46db6b,
    0xbd2db9f5, 0xbd145e65, 0xbcf5aedb, 0xbcc26784, 0xbc8f0428, 0xbc37441d, 0xbba1795d, 0x3aaaa196,
    0x3bf56aa3, 0x3c5f2e2c, 0x3ca1438b, 0x3cd244a8, 0x3d013fcc, 0x3d18ed0f, 0x3d301d4d, 0x3d46c414,
    0x3d5cd550, 0x3d724552, 0x3d83846c, 0x3d8d8a8b, 0x3d972fd9, 0x3da06f6a, 0x3da94492, 0x3db1aae8,
    0x3db99e49, 0x3dc11adc, 0x3dc81d0d, 0x3dcea198, 0x3dd4a580, 0x3dda261b, 0x3ddf210c, 0x3de39446,
    0x3de77e0b, 0x3deadcf4, 0x3dedafe4, 0x3deff617, 0x3df1af15, 0x3df2dabd, 0x3df3793b, 0x3df38b0d,
    0x3df31104, 0x3df20c3e, 0x3df07e25, 0x3dee6876, 0x3debcd33, 0x3de8aeac, 0x3de50f78, 0x3de0f275,
    0x3ddc5ac4, 0x3dd74bc9, 0x3dd1c92a, 0x3dcbd6c5, 0x3dc578b8, 0x3dbeb357, 0x3db78b2c, 0x3db004f2,
    0x3da82594, 0x3d9ff22a, 0x3d976ff6, 0x3d8ea45c, 0x3d8594e7, 0x3d788e7c, 0x3d658249, 0x3d5210ed,
    0x3d3e4648, 0x3d2a2e5d, 0x3d15d54a, 0x3d01473f, 0x3cd920f2, 0x3caf7a79, 0x3c85b392, 0x3c37c95c,
    0x3bc8986e, 0x3a88ffc9, 0xbb831728, 0xbc138739, 0xbc64a46c, 0xbc9a5ab6, 0xbcc1c6ad, 0xbce88028,
    0xbd0738df, 0xbd19c34d, 0xbd2bd545, 0xbd3d6503, 0xbd4e691d, 0xbd5ed88a, 0xbd6eaaa6, 0xbd7dd736,
    0xbd862b35, 0xbd8d1073, 0xbd9397e2, 0xbd99be49, 0xbd9f80ae, 0xbda4dc57, 0xbda9cec9, 0xbdae55cb,
    0xbdb26f65, 0xbdb619e5, 0xbdb953da, 0xbdbc1c1a, 0xbdbe71bb, 0xbdc0541c, 0xbdc1c2e0, 0xbdc2bdeb,
    0xbdc34569, 0xbdc359c8, 0xbdc2fbb8, 0xbdc22c2d, 0xbdc0ec5c, 0xbdbf3db7, 0xbdbd21f3, 0xbdba9b01,
    0xbdb7ab0d, 0xbdb4547e, 0xbdb099f3, 0xbdac7e44, 0xbda8047a, 0xbda32fd5, 0xbd9e03c1, 0xbd9883db,
    0xbd92b3ea, 0xbd8c97df, 0xbd8633d3, 0xbd7f17fe, 0xbd714982, 0xbd630527, 0xbd54541b, 0xbd453fbd,
    0xbd35d195, 0xbd261355, 0xbd160ecc, 0xbd05cde3, 0xbceab52e, 0xbcc97de5, 0xbca80a10, 0xbc866dd6,
    0xbc497aa9, 0xbc06192b, 0xbb85be0b, 0x3644e3ff, 0x3b85038c, 0x3c045add, 0x3c4571a8, 0x3c82d07a,
    0x3ca26227, 0x3cc15c0f, 0x3cdfacdf, 0x3cfd43c5, 0x3d0d083c, 0x3d1b01a1, 0x3d288684, 0x3d358fab,
    0x3d421630, 0x3d4e1386, 0x3d59817c, 0x3d645a44, 0x3d6e986d, 0x3d7836f0, 0x3d809896, 0x3d84c173,
    0x3d88942a, 0x3d8c0f0b, 0x3d8f309d, 0x3d91f7a0, 0x3d94630a, 0x3d96720b, 0x3d98240b, 0x3d9978ac,
    0x3d9a6fc4, 0x3d9b0966, 0x3d9b45db, 0x3d9b25a1, 0x3d9aa96f, 0x3d99d232, 0x3d98a10a, 0x3d97174b,
    0x3d95367e, 0x3d93005b, 0x3d9076ca, 0x3d8d9be3, 0x3d8a71ec, 0x3d86fb53, 0x3d833ab2, 0x3d7e6597,
    0x3d75cd0d, 0x3d6cb1dc, 0x3d631a60, 0x3d590d31, 0x3d4e9126, 0x3d43ad49, 0x3d3868d9, 0x3d2ccb40,
    0x3d20dc12, 0x3d14a306, 0x3d0827f3, 0x3cf6e590, 0x3cdd1715, 0x3cc2f49f, 0x3ca88e6e, 0x3c8df4cf,
    0x3c66701f, 0x3c30d0ed, 0x3bf658e8, 0x3b8b45cf, 0x3a82a1a0, 0xbb12805d, 0xbbb235b3, 0xbc0cfd71,
    0xbc4029d0, 0xbc72828b, 0xbc91f575, 0xbcaa237d, 0xbcc1bdce, 0xbcd8b749, 0xbcef0347, 0xbd024ad1,
    0xbd0cb15d, 0xbd16afbc, 0xbd2040ad, 0xbd295f3a, 0xbd3206b9, 0xbd3a32d4, 0xbd41df84, 0xbd490919,
    0xbd4fac37, 0xbd55c5da, 0xbd5b5356, 0xbd60525a, 0xbd64c0ed, 0xbd689d74, 0xbd6be6ac, 0xbd6e9bb1,
    0xbd70bbf6, 0xbd72474d, 0xbd733de1, 0xbd73a036, 0xbd736f29, 0xbd72abf0, 0xbd715816, 0xbd6f757c,
    0xbd6d0653, 0xbd6a0d22, 0xbd668cbb, 0xbd62883e, 0xbd5e0317, 0xbd5900f6, 0xbd5385d2, 0xbd4d95e6,
    0xbd4735a9, 0xbd4069cf, 0xbd393744, 0xbd31a32a, 0xbd29b2d4, 0xbd216bc3, 0xbd18d3a2, 0xbd0ff045,
    0xbd06c79f, 0xbcfabf87, 0xbce77dc5, 0xbcd3d682, 0xbcbfd66e, 0xbcab8a59, 0xbc96ff28, 0xbc8241ce,
    0xbc5abe83, 0xbc30c8f6, 0xbc06bccc, 0xbbb96774, 0xbb4b1d08, 0xba110a33, 0x3b015cd2, 0x3b92af6f,
    0x3be3b18b, 0x3c19c2bc, 0x3c40fe82, 0x3c677595, 0x3c86890b, 0x3c98df6b, 0x3caab3b0, 0x3cbbfc06,
    0x3cccaefe, 0x3cdcc398, 0x3cec3146, 0x3cfaeff2, 0x3d047c00, 0x3d0b2128, 0x3d116425, 0x3d1741ea,
    0x3d1cb7b1, 0x3d21c2f7, 0x3d266180, 0x3d2a9156, 0x3d2e50ce, 0x3d319e80, 0x3d347950, 0x3d36e069,
    0x3d38d340, 0x3d3a5191, 0x3d3b5b61, 0x3d3bf0fb, 0x3d3c12f1, 0x3d3bc21c, 0x3d3aff99, 0x3d39ccc8,
    0x3d382b4b, 0x3d361d06, 0x3d33a41a, 0x3d30c2e8, 0x3d2d7c09, 0x3d29d251, 0x3d25c8ca, 0x3d2162b5,
    0x3d1ca382, 0x3d178ed4, 0x3d122877, 0x3d0c7467, 0x3d0676c2, 0x3d0033cf, 0x3cf35fea, 0x3ce5df74,
    0x3cd7ef81, 0x3cc99986, 0x3cbae726, 0x3cabe225, 0x3c9c9469, 0x3c8d07f0, 0x3c7a8d93, 0x3c5ab61e,
    0x3c3a9dc5, 0x3c1a58c4, 0x3bf3f69a, 0x3bb332e9, 0x3b651ca5, 0x3ac8c12d, 0xb9dc033f, 0xbb1a3990,
    0xbb8bbfb9, 0xbbc98412, 0xbc0322d9, 0xbc20f0b4, 0xbc3e1a80, 0xbc5a8fab, 0xbc764038, 0xbc888e62,
    0xbc958b48, 0xbca20fc4, 0xbcae1525, 0xbcb99518, 0xbcc489a8, 0xbcceed45, 0xbcd8bac3, 0xbce1ed5e,
    0xbcea80be, 0xbcf270f3, 0xbcf9ba7f, 0xbd002d28, 0xbd0326e1, 0xbd05c953, 0xbd08139d, 0xbd0a0518,
    0xbd0b9d54, 0xbd0cdc1a, 0xbd0dc16b, 0xbd0e4d7f, 0xbd0e80c5, 0xbd0e5be3, 0xbd0ddfb4, 0xbd0d0d45,
    0xbd0be5da, 0xbd0a6ae7, 0xbd089e0f, 0xbd068128, 0xbd041633, 0xbd015f5d, 0xbcfcbe00, 0xbcf62f36,
    0xbcef17ad, 0xbce77cfd, 0xbcdf6501, 0xbcd6d5d9, 0xbccdd5e0, 0xbcc46ba7, 0xbcba9df5, 0xbcb073c0,
    0xbca5f427, 0xbc9b266d, 0xbc9011f9, 0xbc84be4a, 0xbc7265ee, 0xbc5aef53, 0xbc43282d, 0xbc2b1ff7,
    0xbc12e631, 0xbbf514ac, 0xbbc437ab, 0xbb93540e, 0xbb451081, 0xbac7c976, 0xb8580c32, 0x3ab8854a,
    0x3b3aca60, 0x3b8bff66, 0x3bb9d485, 0x3be6c9d7, 0x3c0962b7, 0x3c1ed71a, 0x3c33b5fc, 0x3c47f3bd,
    0x3c5b853e, 0x3c6e5fe4, 0x3c803cd0, 0x3c88e477, 0x3c912273, 0x3c98f29b, 0x3ca05112, 0x3ca73a4e,
    0x3cadab15, 0x3cb3a081, 0x3cb91800, 0x3cbe0f53, 0x3cc28492, 0x3cc6762b, 0x3cc9e2e3, 0x3cccc9d2,
    0x3ccf2a69, 0x3cd1046d, 0x3cd257f9, 0x3cd3257c, 0x3cd36db6, 0x3cd331bb, 0x3cd272f0, 0x3cd13306,
    0x3ccf73fe, 0x3ccd3821, 0x3cca8204, 0x3cc75482, 0x3cc3b2b7, 0x3cbfa005, 0x3cbb200b, 0x3cb636a4,
    0x3cb0e7e3, 0x3cab3813, 0x3ca52bb1, 0x3c9ec768, 0x3c981012, 0x3c910aaf, 0x3c89bc65, 0x3c822a7b,
    0x3c74b4af, 0x3c64a2f0, 0x3c542ae8, 0x3c4357e6, 0x3c32355a, 0x3c20cec7, 0x3c0f2fc3, 0x3bfac7d1,
    0x3bd6eda5, 0x3bb2e829, 0x3b8ece68, 0x3b556e7f, 0x3b0d72aa, 0x3a8bac16, 0xb79da6f7, 0xba8f2c29,
    0xbb0d0fdb, 0xbb517f2b, 0xbb8a5dce, 0xbbab4ef6, 0xbbcb8009, 0xbbeadeaf, 0xbc04ac9f, 0xbc136f65,
    0xbc21af96, 0xbc2f6586, 0xbc3c89f2, 0xbc491608, 0xbc550363, 0xbc604c16, 0xbc6aeaab, 0xbc74da25,
    0xbc7e1600, 0xbc834d1c, 0xbc8731a3, 0xbc8ab713, 0xbc8ddc27, 0xbc909fdc, 0xbc930171, 0xbc950061,
    0xbc969c6a, 0xbc97d589, 0xbc98abfa, 0xbc992034, 0xbc9932f0, 0xbc98e520, 0xbc9837f0, 0xbc972cc9,
    0xbc95c549, 0xbc940345, 0xbc91e8c9, 0xbc8f7811, 0xbc8cb38b, 0xbc899dd5, 0xbc8639ba, 0xbc828a2e,
    0xbc7d249b, 0xbc74aab8, 0xbc6bad81, 0xbc623404, 0xbc58458d, 0xbc4de99f, 0xbc4327ef, 0xbc38085f,
    0xbc2c92f7, 0xbc20cfe2, 0xbc14c767, 0xbc0881e3, 0xbbf80f86, 0xbbdec302, 0xbbc52f37, 0xbbab6525,
    0xbb9175be, 0xbb6ee3c6, 0xbb3ad4a4, 0xbb06df40, 0xbaa648b9, 0xb9fe21ef, 0x39990b39, 0x3a8adb02,
    0x3aee0365, 0x3b27c067, 0x3b578ce1, 0x3b8325a5, 0x3b99f062, 0x3bb019bc, 0x3bc59551, 0x3bda5750,
    0x3bee547f, 0x3c00c120, 0x3c09eb4b, 0x3c12a415, 0x3c1ae72a, 0x3c22b08a, 0x3c29fc91, 0x3c30c7f2,
    0x3c370fbe, 0x3c3cd162, 0x3c420aa8, 0x3c46b9b6, 0x3c4add13, 0x3c4e73a1, 0x3c517c9f, 0x3c53f7ac,
    0x3c55e4c3, 0x3c574435, 0x3c5816b4, 0x3c585d47, 0x3c58194b, 0x3c574c77, 0x3c55f8d1, 0x3c5420b1,
    0x3c51c6bd, 0x3c4eeded, 0x3c4b9979, 0x3c47cce7, 0x3c438bfc, 0x3c3edac1, 0x3c39bd79, 0x3c3438a4,
    0x3c2e50f3, 0x3c280b50, 0x3c216cce, 0x3c1a7aae, 0x3c133a58, 0x3c0bb156, 0x3c03e553, 0x3bf7b826,
    0x3be736e5, 0x3bd652c3, 0x3bc517bb, 0x3bb391e1, 0x3ba1cd56, 0x3b8fd641, 0x3b7b718b, 0x3b5701fe,
    0x3b3275ee, 0x3b0de538, 0x3ad2cefa, 0x3a8a2812, 0x3a04072e, 0xb82e2e12, 0xba1815e5, 0xba919dea,
    0xbad60078, 0xbb0c84ff, 0xbb2d4999, 0xbb4d3b1b, 0xbb6c4755, 0xbb852e71, 0xbb93b596, 0xbba1b140,
    0xbbaf1a04, 0xbbbbe8ec, 0xbbc8177b, 0xbbd39fb1, 0xbbde7c08, 0xbbe8a77d, 0xbbf21d8e, 0xbbfada3b,
    0xbc016d05, 0xbc050d03, 0xbc084be0, 0xbc0b28a6, 0xbc0da2a2, 0xbc0fb966, 0xbc116cc3, 0xbc12bccc,
    0xbc13a9d5, 0xbc143472, 0xbc145d75, 0xbc1425eb, 0xbc138f22, 0xbc129a9d, 0xbc114a1b, 0xbc0f9f90,
    0xbc0d9d26, 0xbc0b453a, 0xbc089a58, 0xbc059f3e, 0xbc0256d2, 0xbbfd8850, 0xbbf5d4ee, 0xbbed9a3d,
    0xbbe4df3c, 0xbbdbab2a, 0xbbd2057e, 0xbbc7f5e5, 0xbbbd8439, 0xbbb2b87d, 0xbba79ad3, 0xbb9c3380,
    0xbb908adc, 0xbb84a950, 0xbb712ea7, 0xbb58bac3, 0xbb4007ed, 0xbb272711, 0xbb0e2908, 0xbaea3d0f,
    0xbab83038, 0xba864c38, 0xba296276, 0xb98dfb69, 0x38d2bf75, 0x39f4c3fe, 0x3a58e59d, 0x3a9ad4d1,
    0x3ac83b5f, 0x3af48bcd, 0x3b0fd63b, 0x3b24c26a, 0x3b38febe, 0x3b4c801e, 0x3b5f3c0e, 0x3b7128b3,
    0x3b811e6e, 0x3b893800, 0x3b90dd26, 0x3b980a4b, 0x3b9ebc39, 0x3ba4f013, 0x3baaa358, 0x3bafd3e5,
    0x3bb47ff3, 0x3bb8a619, 0x3bbc454c, 0x3bbf5cdb, 0x3bc1ec73, 0x3bc3f41c, 0x3bc57435, 0x3bc66d7a,
    0x3bc6e0fb, 0x3bc6d01d, 0x3bc63c9a, 0x3bc5287c, 0x3bc3961d, 0x3bc18821, 0x3bbf0177, 0x3bbc0554,
    0x3bb89732, 0x3bb4bacb, 0x3bb07415, 0x3babc740, 0x3ba6b8b5, 0x3ba14d0d, 0x3b9b8910, 0x3b9571b4,
    0x3b8f0c13, 0x3b885d6e, 0x3b816b23, 0x3b747557, 0x3b65a336, 0x3b566b2e, 0x3b46d8aa, 0x3b36f72b,
    0x3b26d243, 0x3b16758a, 0x3b05ec98, 0x3aea85fb, 0x3ac90873, 0x3aa7776a, 0x3a85e972, 0x3a48e9a5,
    0x3a065ef3, 0x3988bbb5, 0x37441f76, 0xb9758006, 0xb9f9973a, 0xba3b0b81, 0xba77fa21, 0xba99b9a7,
    0xbab6aa24, 0xbad2bdec, 0xbaede52b, 0xbb040871, 0xbb109975, 0xbb1c9eff, 0xbb2812e0, 0xbb32ef5f,
    0xbb3d2f3a, 0xbb46cdad, 0xbb4fc66b, 0xbb5815aa, 0xbb5fb81a, 0xbb66aaec, 0xbb6cebd0, 0xbb7278f5,
    0xbb775108, 0xbb7b7335, 0xbb7edf25, 0xbb80ca7e, 0xbb81caac, 0xbb8270a9, 0xbb82bd38, 0xbb82b15b,
    0xbb824e49, 0xbb81956f, 0xbb808871, 0xbb7e524a, 0xbb7af325, 0xbb76f7e0, 0xbb726543, 0xbb6d406a,
    0xbb678ec4, 0xbb61560c, 0xbb5a9c44, 0xbb5367b2, 0xbb4bbed9, 0xbb43a873, 0xbb3b2b6e, 0xbb324ee6,
    0xbb291a20, 0xbb1f9480, 0xbb15c58d, 0xbb0bb4e1, 0xbb016a2a, 0xbaedda49, 0xbad88b24, 0xbac2f66f,
    0xbaad2bad, 0xba973a51, 0xba8131b1, 0xba564200, 0xba2a2e81, 0xb9fc8cf6, 0xb9a54dff, 0xb91db227,
    0x3753445b, 0x3935aad6, 0x39ada41a, 0x39fed336, 0x3a2718c7, 0x3a4dc7ec, 0x3a736065, 0x3a8be64d,
    0x3a9d7c04, 0x3aae67a9, 0x3abea021, 0x3ace1cec, 0x3adcd625, 0x3aeac48a, 0x3af7e17c, 0x3b021381,
    0x3b07c7e5, 0x3b0d0b95, 0x3b11dc92, 0x3b163933, 0x3b1a2020, 0x3b1d905c, 0x3b20893a, 0x3b230a61,
    0x3b2513ce, 0x3b26a5cd, 0x3b27c0fb, 0x3b286644, 0x3b2896e2, 0x3b285459, 0x3b27a078, 0x3b267d55,
    0x3b24ed47, 0x3b22f2ed, 0x3b209120, 0x3b1dcaf9, 0x3b1aa3ca, 0x3b171f1a, 0x3b1340a6, 0x3b0f0c58,
    0x3b0a8649, 0x3b05b2bc, 0x3b009617, 0x3af669cb, 0x3aeb279e, 0x3adf6f2f, 0x3ad34a36, 0x3ac6c28e,
    0x3ab9e238, 0x3aacb349, 0x3a9f3fea, 0x3a91924f, 0x3a83b4ac, 0x3a6b626a, 0x3a4f2420, 0x3a32c2a5,
    0x3a1651f4, 0x39f3cb90, 0x39bb2324, 0x3982d0d8, 0x3915f432, 0x381e1bbd, 0xb88abdc2, 0xb9307818,
    0xb98cc0e4, 0xb9c01db2, 0xb9f23373, 0xba11723b, 0xba290a36, 0xba3fd43d, 0xba55c3a1, 0xba6acc79,
    0xba7ee3a8, 0xba88ff6f, 0xba920a52, 0xba9a8e2b, 0xbaa28716, 0xbaa9f19f, 0xbab0cabe, 0xbab70fdf,
    0xbabcbedb, 0xbac1d5fc, 0xbac653fc, 0xbaca3802, 0xbacd81a6, 0xbad030ea, 0xbad2463d, 0xbad3c274,
    0xbad4a6cf, 0xbad4f4f1, 0xbad4aedf, 0xbad3d6fe, 0xbad2700f, 0xbad07d2c, 0xbace01c6, 0xbacb019f,
    0xbac780c7, 0xbac38399, 0xbabf0eb6, 0xbaba2701, 0xbab4d19b, 0xbaaf13dd, 0xbaa8f356, 0xbaa275c5,
    0xba9ba112, 0xba947b4e, 0xba8d0aac, 0xba855578, 0xba7ac436, 0xba6a6e1c, 0xba59b5b2, 0xba48a81d,
    0xba37528b, 0xba25c228, 0xba14041b, 0xba022575, 0xb9e06661, 0xb9bc7441, 0xb9988ddb, 0xb9699831,
    0xb9228ecf, 0xb8b85ec1, 0xb7b5442f, 0x383761a9, 0x38e24efc, 0x39331a44, 0x397387df, 0x39992492,
    0x39b79c53, 0x39d5196b, 0x39f18b04, 0x3a0670a3, 0x3a1386af, 0x3a2000c1, 0x3a2bd87e, 0x3a370815,
    0x3a418a40, 0x3a4b5a4c, 0x3a547411, 0x3a5cd3fb, 0x3a647705, 0x3a6b5abd, 0x3a717d3f, 0x3a76dd39,
    0x3a7b79e6, 0x3a7f530f, 0x3a813484, 0x3a825e56, 0x3a8327b0, 0x3a839184, 0x3a839d05, 0x3a834ba4,
    0x3a829f0e, 0x3a81992a, 0x3a803c17, 0x3a7d1454, 0x3a790bd6, 0x3a746422, 0x3a6f2306, 0x3a694ea3,
    0x3a62ed6d, 0x3a5c061f, 0x3a549fbe, 0x3a4cc18a, 0x3a4472fd, 0x3a3bbbc3, 0x3a32a3b5, 0x3a2932d1,
    0x3a1f7135, 0x3a15671a, 0x3a0b1cca, 0x3a009a9d, 0x39ebd1e4, 0x39d62051, 0x39c03134, 0x39aa152d,
    0x3993dcb6, 0x397b303e, 0x394eaefe, 0x39225553, 0x38ec8493, 0x38952881, 0x37fb46b1, 0xb73232d2,
    0xb853cf93, 0xb8bbdcc5, 0xb905f8be, 0xb92cfa22, 0xb952dadc, 0xb9778468, 0xb98d70be, 0xb99e6f08,
    0xb9aeb3b8, 0xb9be361b, 0xb9ccee32, 0xb9dad4ab, 0xb9e7e2eb, 0xb9f4130f, 0xb9ff5fe8, 0xba04e282,
    0xba099f54, 0xba0de4e9, 0xba11b21d, 0xba150628, 0xba17e09a, 0xba1a4161, 0xba1c28bf, 0xba1d9751,
    0xba1e8e07, 0xba1f0e23, 0xba1f193a, 0xba1eb12f, 0xba1dd82f, 0xba1c90b4, 0xba1add7a, 0xba18c183,
    0xba16400f, 0xba135c9d, 0xba101ae3, 0xba0c7ece, 0xba088c7a, 0xba044835, 0xb9ff6ce8, 0xb9f5b7a9,
    0xb9eb7a28, 0xb9e0be21, 0xb9d58d82, 0xb9c9f266, 0xb9bdf70d, 0xb9b1a5d1, 0xb9a50925, 0xb9982b83,
    0xb98b176c, 0xb97baeba, 0xb960eb93, 0xb945fa21, 0xb92aeeef, 0xb90fde48, 0xb8e9b84a, 0xb8b3f842,
    0xb87d45be, 0xb813bb2b, 0xb72e66ab, 0x376b606b, 0x381f55e3, 0x3880cefc, 0x38b0b8ad, 0x38df49ec,
    0x39063312, 0x391bf922, 0x3930ea6b, 0x3944fb07, 0x39581fe7, 0x396a4ed7, 0x397b7e88, 0x3985d346,
    0x398d5fb0, 0x39946134, 0x399ad4f9, 0x39a0b89c, 0x39a60a2f, 0x39aac83a, 0x39aef1b8, 0x39b28619,
    0x39b5853c, 0x39b7ef73, 0x39b9c57c, 0x39bb0882, 0x39bbba19, 0x39bbdc3b, 0x39bb7145, 0x39ba7bf4,
    0x39b8ff63, 0x39b6ff03, 0x39b47e9c, 0x39b18244, 0x39ae0e5e, 0x39aa2795, 0x39a5d2d6, 0x39a1154e,
    0x399bf460, 0x399675a5, 0x39909ee6, 0x398a7615, 0x39840148, 0x397a8d6c, 0x396c9960, 0x395e3337,
    0x394f67df, 0x39404459, 0x3930d5b3, 0x392128fc, 0x39114b3a, 0x39014960, 0x38e2608d, 0x38c21941,
    0x38a1d5e4, 0x3881af0f, 0x384379b8, 0x38042dac, 0x378b4f9c, 0x3601495f, 0xb75183fa, 0xb7df1f0d,
    0xb8294f48, 0xb8617a4d, 0xb88bf63f, 0xb8a641d9, 0xb8bf8fe2, 0xb8d7d14e, 0xb8eef827, 0xb9027bc4,
    0xb90ce1d7, 0xb916a8fb, 0xb91fcc71, 0xb928480b, 0xb9301834, 0xb93739ea, 0xb93daabe, 0xb94368da,
    0xb94872f7, 0xb94cc862, 0xb95068fa, 0xb9535528, 0xb9558de5, 0xb95714ae, 0xb957eb8a, 0xb95814fd,
    0xb957940a, 0xb9566c2e, 0xb954a158, 0xb95237e9, 0xb94f34a8, 0xb94b9cc5, 0xb94775cb, 0xb942c5a1,
    0xb93d927e, 0xb937e2e8, 0xb931bda9, 0xb92b29cc, 0xb9242e94, 0xb91cd378, 0xb9152017, 0xb90d1c39,
    0xb904cfc1, 0xb8f88553, 0xb8e6f9fe, 0xb8d50dab, 0xb8c2d08a, 0xb8b052c6, 0xb89da475, 0xb88ad58a,
    0xb86feb9d, 0xb84a29a2, 0xb82483bb, 0xb7fe2fbe, 0xb7b4069f, 0xb7558a16, 0xb68a88e1, 0x3690b20b,
    0x3752e7a7, 0x37ad028c, 0x37eea9c4, 0x38171e79, 0x3835c905, 0x385340c9, 0x386f735e, 0x388527dc,
    0x3891e31a, 0x389de44d, 0x38a92514, 0x38b39fc8, 0x38bd4f80, 0x38c63013, 0x38ce3e13, 0x38d576d4,
    0x38dbd864, 0x38e16191, 0x38e611de, 0x38e9e989, 0x38ece982, 0x38ef136b, 0x38f06990, 0x38f0eee6,
    0x38f0a704, 0x38ef961c, 0x38edc0f7, 0x38eb2cee, 0x38e7dfe4, 0x38e3e039, 0x38df34d0, 0x38d9e4f7,
    0x38d3f866, 0x38cd773d, 0x38c669f1, 0x38bed949, 0x38b6ce54, 0x38ae525e, 0x38a56eee, 0x389c2db3,
    0x38929883, 0x3888b94d, 0x387d3429, 0x386889c6, 0x38538787, 0x383e4173, 0x3828cb6c, 0x38133922,
    0x37fb3c07, 0x37d01a53, 0x37a53299, 0x377552ce, 0x37214498, 0x369cff01, 0xb42e7c23, 0xb6a44f91,
    0xb71f8cd8, 0xb76aaedf, 0xb799ab59, 0xb7bca82e, 0xb7de35a2, 0xb7fe3d24, 0xb80e54f5, 0xb81cb47d,
    0xb82a349d, 0xb836cdbb, 0xb842792f, 0xb84d3148, 0xb856f14e, 0xb85fb580, 0xb8677b14, 0xb86e4034,
    0xb8740400, 0xb878c685, 0xb87c88be, 0xb87f4c8c, 0xb8808a59, 0xb880f266, 0xb880e0a6, 0xb88057b8,
    0xb87eb53b, 0xb87bd969, 0xb878236c, 0xb8739b5a, 0xb86e49e9, 0xb8683863, 0xb8617097, 0xb859fcd2,
    0xb851e7d1, 0xb8493cb5, 0xb84006fb, 0xb8365268, 0xb82c2b02, 0xb8219d03, 0xb816b4c9, 0xb80b7ece,
    0xb8000799, 0xb7e8b766, 0xb7d10f35, 0xb7b92f6c, 0xb7a13097, 0xb7892ae5, 0xb7626c28, 0xb732d2b5,
    0xb703b6aa, 0xb6aa87c6, 0xb61e92ef, 0x349fc2b8, 0x3641f68b, 0x36b569c5, 0x370380cf, 0x372ac07f,
    0x3750553e, 0x377422a1, 0x378b074a, 0x379b00bb, 0x37a9f30e, 0x37b7d4ef, 0x37c49e52, 0x37d04877,
    0x37dacde3, 0x37e42a69, 0x37ec5b23, 0x37f35e72, 0x37f933f9, 0x37fddc9b, 0x3800ad38, 0x3801d862,
    0x38027203, 0x38027ce3, 0x3801fc51, 0x3800f423, 0x37fed151, 0x37fabd55, 0x37f5b6c1, 0x37efc8d6,
    0x37e8ff95, 0x37e167ae, 0x37d90e6f, 0x37d001b7, 0x37c64fe0, 0x37bc07ae, 0x37b13840, 0x37a5f0fc,
    0x379a417c, 0x378e397c, 0x3781e8c6, 0x376abe49, 0x37515895, 0x3737bf93, 0x371e11f7, 0x37046dec,
    0x36d5e1da, 0x36a36f52, 0x3663778f, 0x3601f964, 0x350b2a4f, 0xb566cd60, 0xb6132416, 0xb66930e6,
    0xb69dc47a, 0xb6c4f0cf, 0xb6e9fb5b, 0xb70662ad, 0xb71699b7, 0xb72596da, 0xb7334fe6, 0xb73fbc70,
    0xb74ad5da, 0xb7549752, 0xb75cfdd2, 0xb764081b, 0xb769b6ac, 0xb76e0bbc, 0xb7710b31, 0xb772ba8a,
    0xb77320d9, 0xb77246b9, 0xb7703631, 0xb76cfab0, 0xb768a0e7, 0xb76336c3, 0xb75ccb41, 0xb7556e64,
    0xb74d3114, 0xb744250b, 0xb73a5cb9, 0xb72feb26, 0xb724e3d5, 0xb7195aa9, 0xb70d63c3, 0xb7011363,
    0xb6e8fb98, 0xb6cf6e64, 0xb6b5a72e, 0xb69bcd62, 0xb682079d, 0xb650f6ec, 0xb61e9a94, 0xb5da7ffb,
    0xb574a6fe, 0xb4696fce, 0x34f1e77a, 0x3592357c, 0x35e39f08, 0x36182cec, 0x363c0970, 0x365d40b0,
    0x367bb36f, 0x368ba3d1, 0x3697f441, 0x36a2c34c, 0x36ac0be5, 0x36b3cba9, 0x36ba02d9, 0x36beb46a,
    0x36c1e5e1, 0x36c39f34, 0x36c3eab0, 0x36c2d4e4, 0x36c06c7f, 0x36bcc231, 0x36b7e887, 0x36b1f3de,
    0x36aafa23, 0x36a31299, 0x369a55a4, 0x3690dc9b, 0x3686c191, 0x36783e4e, 0x366220a5, 0x364b609f,
    0x3634349d, 0x361cd25f, 0x36056e8f, 0x35dc78b7, 0x35aeda2c, 0x35825f95, 0x352ec3ae, 0x34b8d288,
    0x3364acd8, 0xb46c2d23, 0xb4fe385c, 0xb53d7b9b, 0xb575ce9d, 0xb593e621, 0xb5a99f06, 0xb5bbff31,
    0xb5cafcdc, 0xb5d697c0, 0xb5ded90f, 0xb5e3d34a, 0xb5e5a1f6, 0xb5e46944, 0xb5e055a0, 0xb5d999cf,
    0xb5d06ef3, 0xb5c51466, 0xb5b7cf11, 0xb5a8e894, 0xb598ae54, 0xb587708f, 0xb56b02a6, 0xb546631f,
    0xb521a17c, 0xb4fab241, 0xb4b43ff2, 0xb4621149, 0xb3c855e1, 0x3274f911, 0x33ed618d, 0x34509eee,
    0x348e092e, 0x34ac28c6, 0x34c2799d, 0x34d0f042, 0x34d7aad1, 0x34d6f016, 0x34cf2dba, 0x34c1091c,
    0x34ad55f1, 0x349509fa, 0x347269b1, 0x3435ef48, 0x33edeb14, 0x335f0ba8, 0xb1b13400, 0xb37b0118,
    0xb3e3addb, 0xb41d2dab, 0xb43fc01b, 0xb4595591, 0xb46ab5b9, 0xb475e4ae, 0xb47e65a1, 0xb482dbeb,
    0x00000000, 0x00000000,
};

// FUN_1004a0c0: add the band-limited step residual for a step at fractional position `x` (0..1, of the current
// sample) and size `amp` into the 32-sample ring `buf`, starting at the current position `pos` (counting down).
void addBlep(float* buf, int pos, float x, float amp) {
    if (x < 0.f) x = 0.f;
    else if (x > 1.f) x = 1.f;
    const double s = double(x) * 64.0;
    int t = int(s);
    const float fr = float(s - double(t));
    auto tap = [&](float& b) {
        const double t0 = asFloat(kBlepBits[t]), t1 = asFloat(kBlepBits[t + 1]);
        b = float(((t1 - t0) * double(fr) + t0) * double(amp) + double(b));
        t += 64;
    };
    for (int p = pos; p >= 0; --p) tap(buf[p]);
    for (int p = 31; p > pos; --p) tap(buf[p]);
}

// Wavetable content (the Wavetable element's tables; Analog uses bank 0, entry trunc(v*128.9)). Slot 1 raw:
// 8-byte header, then 8 mip levels of 64 waves of (256 >> k) + 1 int16 samples.
constexpr uint32_t kWtEntrySize = 66312;
const std::vector<uint8_t>* wavetableEntry(const Content* content, int bank, int wave) {
    // Shared by every engine in the process (plugin instances load on their own threads): look up under the lock,
    // read the file outside it, keep the first bank stored. Entries are never removed, so pointers stay valid.
    static std::mutex mu;
    static std::map<std::pair<std::string, int>, std::unique_ptr<std::vector<std::vector<uint8_t>>>> cache;
    if (!content) return nullptr;
    const auto key = std::make_pair(content->root().string(), bank);
    const std::vector<std::vector<uint8_t>>* found = nullptr;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) found = it->second.get(), have = true;
    }
    if (!have) {
        std::unique_ptr<std::vector<std::vector<uint8_t>>> wb;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(content->root() / "Wavetables", ec)) {
            const auto name = f.path().filename().string();
            if (!f.is_regular_file() || f.path().has_extension() || name.size() < 4 || name[3] != ' ') continue;
            if (!std::isdigit(uint8_t(name[0])) || !std::isdigit(uint8_t(name[1])) || !std::isdigit(uint8_t(name[2])))
                continue;
            if (std::stoi(name.substr(0, 3)) != bank) continue;
            try {
                auto mb = MilanBank::open(f.path());
                if (mb->kind != 3) break;
                wb = std::make_unique<std::vector<std::vector<uint8_t>>>();
                for (const auto& e : mb->entries) {
                    auto s = mb->read(e.slot1);
                    wb->emplace_back(s.begin(), s.end());
                }
            } catch (...) {
                wb.reset();
            }
            break;
        }
        std::lock_guard<std::mutex> lk(mu);
        found = cache.emplace(key, std::move(wb)).first->second.get();  // emplace keeps an existing entry
    }
    if (!found || wave < 0 || wave >= int(found->size())) return nullptr;
    const auto& v = (*found)[size_t(wave)];
    return v.size() >= kWtEntrySize ? &v : nullptr;
}

struct AnalogParams : TypeParams {
    bool analogFilter = false;  // +0x8a0
    bool full = false;          // the osc-2 / FM / dist / wavetable path (render modes -3 / -4)
    int wave1 = 0;              // +0x89c (v[0x4e])
    int wave2 = 0;              // +0x8a4, 7 -> 6 (v[0x4f])
    int wave3 = 0;              // +0x880, 7 -> 6 (v[0x42])
    bool mute3 = false;         // raw param 0x25 > 0.5 (v[0x32])
    float ratio1 = 1, ratio2 = 1;          // +0x894, +0x8d0
    float mix1 = 0, mix2 = 0, mix3 = 0;    // +0x8ac (signed by Osc1 Phase), +0x8d4, +0x898
    float minPw = 0.001f, sub = 0, noise = 0, o3fm = 0, dist = 0, hp = 1;  // +0x8b0 +0x8b4 +0x8b8 +0x8c0 +0x8c4 +0x8a8
    uint16_t xorMask = 0;       // +0x888
    float pw2Base = 0;          // +0x5e0 (element param 0x66)
    float invSr = 0;            // part+0x4c4
    const std::vector<uint8_t>* table = nullptr;  // elem+0xb94
};

struct BlepOsc {
    float ph = 0;      // +0x164 / +0x1cc
    float incL = 0;    // +0x1f8 / +0x1fc  increment in use (latched at edges)
    float inc = 0;     // +0x168 / +0x1d0  target increment
    float edge = 1;    // +0x148 / +0x1e4  next edge position
    float step = 1;    // +0x144 / +0x1e0
    float lev = 1;     // +0x140 / +0x1dc  sign/level of the next edge
    float slope = 0;   // +0x16c / +0x1d4  DC slope (saw)
    float minPw = 15;  // +0x174 / +0x1e8
    float uni[6] = {};  // +0x180.. / +0x1b0..  unison edge positions
    int wave = 0;      // +0x138 / +0x13c
};

struct WtOsc {
    int32_t pos = 0, inc = 0;  // +0x14 / +0x128, +0x20 / +0x12c
    int shift = 0;             // +0x104 / +0x124
    uint32_t rd = 0;           // +0xfc / +0x130 (byte offset into the table)
};

const uint8_t kZeroTable[8] = {};
const uint32_t kCtorUni[6] = {0x3dcccccd, 0x3e4ccccd, 0x3e99999a, 0x3ecccccd, 0x3f000000, 0x3f19999a};

class AnalogVoice : public Voice {
protected:
    void startOsc(const VoiceStart&, float, MsvcRand& rng) override;
    void controlUpdate() override {  // FUN_1004ab60, then the render function's own per-block code
        Voice::controlUpdate();
        switch (mode_) {
            case 5: postControl<5>(); break;
            case -2: postControl<-2>(); break;
            case -3: postControl<-3>(); break;
            default: postControl<-4>(); break;
        }
    }
    void filterControl(double F) override;
    void onClick(double X) override;
    void legatoClick(double X) override {  // FUN_10016820 @10016b5c: the click level is added to the gain ramp words
        for (int o : {0x60, 0x70, 0x64, 0x74}) fw(o) = float(X + double(fw(o)));
    }
    void renderOsc(float* out, int frames) override {
        switch (mode_) {
            case 5: renderT<5>(out, frames); break;
            case -2: renderT<-2>(out, frames); break;
            case -3: renderT<-3>(out, frames); break;
            default: renderT<-4>(out, frames); break;
        }
        if (traceFile() && uniIdx_ <= 1) trace(frames);
    }
    bool oscFinished() const override { return false; }

private:
    template <int M>
    void renderT(float* out, int frames);
    template <int M>
    void postControl();
    void runEdges(BlepOsc& o, float pw, float* buf, int v2);
    float wtRead(WtOsc& w, float pw);
    float& fw(int off) { return fw_[(off - 0x60) >> 2]; }  // filter words 0x60..0x90
    // Debug: HS2_ANTRACE=<file> writes, after every render call, the frame count and the modelled voice words at
    // their original offsets (0x6b8 bytes; unmodelled words NaN), for comparison with the emulated original.
    static FILE* traceFile() {
        static FILE* f = [] {
            const char* n = std::getenv("HS2_ANTRACE");
            return n ? std::fopen(n, "wb") : nullptr;
        }();
        return f;
    }
    void trace(int frames);
    int64_t traced_ = 0;
    int uniIdx_ = 0;

    const AnalogParams* ap_ = nullptr;
    // FUN_10049eb0 at render entry (the element was set again): FIX: the Analog words are copied again (the type
    // parameters are live, e.g. the drum-kit CCs 20.. of patch type 1)
    void typeRefresh() override {
        if (!e_->typeParams) return;
        ap_ = static_cast<const AnalogParams*>(e_->typeParams.get());
        copyWords(*ap_);
    }
    void copyWords(const AnalogParams& a) {  // FUN_10049eb0: the Analog voice words
        ratio1_ = a.ratio1;                  // +0x110 <- +0x894
        mix3_ = a.mix3;                      // +0x11c <- +0x898
        key_ = int16_t(a.xorMask);           // +0x100 <- +0x888
        mix1_ = a.mix1;                      // +0x154 <- +0x8ac
        sub_ = a.sub;                        // +0x15c <- +0x8b4
        noiseAmt_ = a.noise;                 // +0x160 <- +0x8b8
        o3fm_ = a.o3fm;                      // +0x178 <- +0x8c0
        ratio2_ = a.ratio2;                  // +0x1c8 <- +0x8d0
        mix2_ = a.mix2;                      // +0x1d8 <- +0x8d4
        dist_ = tapIdx_ < 0 ? 0.f : a.dist;  // +0x17c <- +0x8c4 (0 when the tap is the zero word)
        osc1PitchBase_ = a.pw2Base;          // +0x240 <- +0x5e0 (v[0x90])
    }
    int mode_ = 5;
    // filter words: 0x60 lp, 0x64 bp, 0x68 hp, 0x6c xprev, 0x70 u, 0x74 t, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90
    float fw_[13] = {};
    int tapIdx_ = -1;  // index into fw_ (-1 = the zero word v[0xbb])
    bool inv_ = false, freqOn_ = true, ana36_ = false;
    float rLim_ = 0.77f, c2_ = 0.63f;
    // osc 3 / mixing
    float ph3_ = 0, inc3L_ = 0, incMin_ = 0, leak3_ = 1, hpc_ = 1, slope3_ = 0, out3_ = 0, acc_ = 0;
    int cnt3_ = 0, w3_ = 0;
    bool mute3_ = false;
    uint32_t noise_ = 0;  // +0x120 float bits; the low 16 bits are the LCG state
    float ratio1_ = 1, ratio2_ = 1, mix1_ = 0, mix2_ = 0, mix3_ = 0, sub_ = 0, noiseAmt_ = 0, o3fm_ = 0, dist_ = 0;
    float loc14_ = 0, locC_ = 1, l18_ = 1;  // -3: cutoff for phase >= 0.5 / t-stage gain; osc-3 mix gain
    BlepOsc o1_ = ctorOsc(), o2_ = ctorOsc();
    static BlepOsc ctorOsc() {  // FUN_10049c80: unison edge positions 0.1 .. 0.6
        BlepOsc o;
        for (int i = 0; i < 6; ++i) o.uni[i] = asFloat(kCtorUni[i]);
        return o;
    }
    float det_[6] = {};  // +0x198.. unison detune, constants from the voice ctor FUN_10049c80
    float buf_[3][32] = {};
    // wavetable oscillators
    const uint8_t* wt_ = kZeroTable;
    int32_t wtEnd_ = 0x100;
    float wtPer_ = 0;  // +0x114
    int16_t key_ = 0;
    WtOsc wt1_, wt2_;
};

void AnalogVoice::startOsc(const VoiceStart& vs, float, MsvcRand& rng) {
    ap_ = static_cast<const AnalogParams*>(e_->typeParams.get());
    const AnalogParams& a = *ap_;
    uniIdx_ = vs.unisonIndex;
    // filter state (note-on @10014e9a.. and the generic filter-mode switch)
    const bool hiReso = Voice::f_[0].bp == 2.f;  // shared start: reso + velReso > 0.98 set t = bp = 2
    std::fill(std::begin(fw_), std::end(fw_), 0.f);
    if (hiReso) fw(0x74) = fw(0x64) = 2.f;
    rLim_ = 0.77f;
    c2_ = 0.63f;
    freqOn_ = true;
    inv_ = false;
    switch (e_->filterMode) {
        case 1: tapIdx_ = 4; break;
        case 2: tapIdx_ = 0; break;
        case 3: tapIdx_ = 5; break;
        case 4: tapIdx_ = 1; break;
        case 5: tapIdx_ = 2; rLim_ = 0.3f; break;
        case 6: tapIdx_ = 1; inv_ = true; rLim_ = 0.5f; break;
        case 7: tapIdx_ = 2; inv_ = true; rLim_ = 0.5f; break;
        case 8: tapIdx_ = 0; freqOn_ = false; break;
        default: tapIdx_ = -1; inv_ = true; break;
    }
    energy_ = 0;
    mode_ = 5;
    const int fm = e_->filterMode;
    if (a.analogFilter && (fm == 1 || fm == 2 || fm == 8)) {
        mode_ = -2;
        if (hiReso) {
            fw(0x74) = fw(0x64) = 0.f;
            fw(0x8c) = fw(0x80) = 2.f;
        }
        tapIdx_ = fm == 2 ? 4 : fm == 8 ? 5 : 11;
    }
    {  // the oscillator words are not reset at note-on except the ones set below (uni[] persists)
        float u1[6], u2[6];
        std::copy(std::begin(o1_.uni), std::end(o1_.uni), u1);
        std::copy(std::begin(o2_.uni), std::end(o2_.uni), u2);
        o1_ = BlepOsc{};
        o2_ = BlepOsc{};
        std::copy(u1, u1 + 6, o1_.uni);
        std::copy(u2, u2 + 6, o2_.uni);
    }
    o1_.wave = a.wave1;
    w3_ = a.wave3;
    o2_.wave = a.wave2;
    mute3_ = a.mute3;
    wt_ = kZeroTable;
    wtEnd_ = 0x100;
    wtPer_ = 0;
    wt1_ = WtOsc{};
    wt2_ = WtOsc{};
    if (a.full) {
        mode_ = mode_ == 5 ? -3 : -4;
        if (a.table) {
            wt_ = a.table->data();
            wt1_.rd = wt2_.rd = 0x4048;  // level 0, wave 32
            wt1_.pos = wt2_.pos = wtEnd_ = 0x800000;
            wtPer_ = 8388608.f;
        }
    } else if (o2_.wave > 5) {
        o2_.wave = 0;
    }
    ana36_ = false;
    if (a.analogFilter && (fm == 3 || fm == 6)) {
        mode_ = -3;
        tapIdx_ = 6;  // 0x78
        ana36_ = true;
        c2_ = float(double(a.o3fm) * double(1.2f));  // set at every -3 render call
    }
    maxInc_ = 1.f;
    rateLn_ = float(double(c_.lnRef) + double(0.6931471f));
    hpc_ = leak3_ = a.hp;
    for (auto& b : buf_) std::fill(std::begin(b), std::end(b), 0.f);
    // start phase (@10015503): 1 - fmod(randpm*RandStart/2 + Start + 2 - VelToStart*vel, 1)
    double P;
    {
        const double r = double(rng.bi());
        const double x = ((r * double(e_->randStart) * 0.5 + double(e_->start)) + 2.0) -
                         double(e_->velToStart) * double(vel_);
        P = 1.0 - std::fmod(x, 1.0);
    }
    o1_.slope = 0;
    o2_.slope = 0;
    o1_.ph = float(P);
    o2_.ph = float((1.0 + P) * 0.5);
    auto unison = [&](BlepOsc& o) {
        const double f = double(rng.bi()) * double(e_->randStart) * double(0.025f) + double(0.07f);
        const float ff = float(f);
        double s = f;
        o.uni[0] = float(s);
        s = s + s;
        o.uni[1] = float(s);
        for (int k = 2; k < 6; ++k) {
            s += double(ff);
            o.uni[k] = float(s);
        }
        return s + double(ff);
    };
    if (o1_.wave == 5) {
        P = unison(o1_);
        o1_.ph = 1.f;
        acc_ = float(double(a.mix1) * -1.0);
    } else if (o1_.wave == 6) {
        acc_ = 0.f;
    } else {
        acc_ = float(double(a.mix1) * -0.5);
    }
    double f5;
    if (o2_.wave == 5) {
        P = unison(o2_);
        o2_.ph = 1.f;
        f5 = double(a.mix2) * -1.0;
    } else if (o2_.wave == 6) {
        f5 = 0.0;
    } else {
        f5 = double(a.mix2) * -0.5;
    }
    o1_.lev = o1_.wave == 0 ? 1.f : -0.5f;
    if (o1_.wave != 0) acc_ = float(-(double(o1_.lev) * double(acc_)));
    o2_.lev = o2_.wave == 0 ? 1.f : -0.5f;
    if (o2_.wave != 0) f5 = -(double(o2_.lev) * f5);
    const float acc0 = float(f5 + double(acc_));
    acc_ = acc0;
    o1_.incL = o2_.incL = inc3L_ = 0;
    incMin_ = float(double(a.invSr) * 250.0);
    ph3_ = float(P);
    {
        const double s = double(a.sub) + double(a.mix3);
        if (w3_ == 0) out3_ = float(s * -0.5);
        else if (w3_ == 1) out3_ = float(s * -0.25);
        else {
            out3_ = float(s * -0.75);
            acc_ = float(s * -0.75 * double(0.3f) + double(acc0));
        }
    }
    o1_.minPw = o1_.wave > 1 ? a.minPw : 15.f;
    o2_.minPw = o2_.wave > 1 ? a.minPw : 15.f;
    o1_.step = o1_.edge = o2_.step = o2_.edge = 1.f;
    cnt3_ = 0;
    slope3_ = 0;
    noise_ = asBits(float(double(rng.uni()) * double(0.2f) + 128.0));
    copyWords(a);  // FUN_10049eb0
    loc14_ = 0;
    locC_ = 1.f;
    l18_ = 1.f;
    {  // FUN_10049c80: {-3, 0.9, 2.07, -1.1, 3, -1.93} * 0.0925
        static const uint32_t kDet[6] = {0xbe8e147b, 0x3daa7efa, 0x3e441205, 0xbdd0624e, 0x3e8e147b, 0xbe36cf42};
        for (int i = 0; i < 6; ++i) det_[i] = asFloat(kDet[i]);
    }
}

// FUN_1004ab60 @1004ae1b with the Analog filter words (render modes -2/-4 fix a = 3.25).
void AnalogVoice::filterControl(double F) {
    b_ = float(F);
    {
        const double c = F * double(c2_);
        cc_ = float(c);
        if (c > 1.0) cc_ = 1.f;
    }
    const double BPu = double(fw(0x64)) + double(1e-08f);
    fw(0x64) = float(BPu);
    fw(0x74) = float(double(fw(0x74)) + double(1e-08f));
    fw(0x80) = float(double(fw(0x80)) + double(1e-08f));
    fw(0x90) = float(double(fw(0x90)) + double(1e-08f));
    {
        const double K = double(0.95f) - double(k_);
        kk_ = float(K);
        if (K < double(0.15f)) {
            const double m = F > 1.0 ? 0.0 : (F - 1.0) * double(0.05f);
            if (double(kk_) < m) kk_ = float(m);
            const double lp = fw(0x60);
            const double E = (lp * lp + BPu * BPu) * double(0.005f);
            const double en = (E - double(energy_)) * double(0.05f) + double(energy_);
            energy_ = float(en);
            kk_ = float(en + double(kk_));
        }
        if (kk_ > 1.f) kk_ = 1.f;
    }
    if (mode_ == -2 || mode_ == -4) {
        a_ = 3.25f;
        if (F > 3.25) b_ = 3.25f;
    } else {
        const double C = double(1.92f) - double(kk_) * double(0.85f);
        a_ = float(C);
        if (F > C) {
            double t = F - C;
            if (t > 1.0) t = 1.0;
            const double K2 = (double(rLim_) - double(kk_)) * t + double(kk_);
            kk_ = float(K2);
            const float cap2 = float(double(1.92f) - K2 * double(0.85f));
            a_ = cap2;
            if (F > double(cap2)) b_ = cap2;
        } else if (freqOn_) {
            a_ = float(F);
        }
    }
}

void AnalogVoice::trace(int frames) {
    traced_ += frames;
    uint32_t w[0x6b8 / 4];
    std::fill(std::begin(w), std::end(w), 0x7fc00000u);
    auto F = [&](int off, float v) { w[off / 4] = asBits(v); };
    auto I = [&](int off, int32_t v) { w[off / 4] = uint32_t(v); };
    I(0x00, mode_);
    I(0x08, blockCount_);
    F(0x40, ph3_); F(0x44, inc_); F(0x48, wave_); F(0x4c, osc1Pitch_); F(0x50, a_); F(0x54, b_); F(0x58, cc_);
    F(0x5c, kk_);
    for (int i = 0; i < 13; ++i) F(0x60 + 4 * i, fw_[i]);
    F(0xa8, amp_); F(0xac, pan_); F(0xb0, gainL_); F(0xb4, incL_); F(0xb8, gainR_); F(0xbc, incR_); F(0xc0, energy_);
    F(0x110, ratio1_); F(0x11c, mix3_); I(0x120, int32_t(noise_)); I(0x134, cnt3_);
    F(0x140, o1_.lev); F(0x144, o1_.step); F(0x148, o1_.edge); F(0x14c, hpc_); F(0x150, slope3_); F(0x154, mix1_);
    F(0x158, out3_); F(0x15c, sub_); F(0x160, noiseAmt_); F(0x164, o1_.ph); F(0x168, o1_.inc); F(0x16c, o1_.slope);
    F(0x170, acc_); F(0x174, o1_.minPw); F(0x178, o3fm_); F(0x17c, dist_);
    for (int i = 0; i < 6; ++i) {
        F(0x180 + 4 * i, o1_.uni[i]);
        F(0x198 + 4 * i, det_[i]);
        F(0x1b0 + 4 * i, o2_.uni[i]);
    }
    F(0x1c8, ratio2_); F(0x1cc, o2_.ph); F(0x1d0, o2_.inc); F(0x1d4, o2_.slope); F(0x1d8, mix2_); F(0x1dc, o2_.lev);
    F(0x1e0, o2_.step); F(0x1e4, o2_.edge); F(0x1e8, o2_.minPw); F(0x1ec, leak3_); F(0x1f0, inc3L_); F(0x1f4, incMin_);
    F(0x1f8, o1_.incL); F(0x1fc, o2_.incL); F(0x218, maxInc_); F(0x21c, rateLn_); F(0x220, pitchBase_);
    F(0x224, waveBase_); F(0x228, resoBase_); F(0x22c, cutoffBase_); F(0x238, levelBase_); F(0x23c, panBase_);
    F(0x240, osc1PitchBase_); F(0x244, glide_); F(0x248, glideCoef_); F(0x250, glideTarget_);
    F(0x268, env1Level_); F(0x26c, env1Slope_); F(0x270, env1Time_); F(0x274, env1Len_);
    F(0x280, fenv_.level); F(0x284, fenv_.coef); F(0x288, fenv_.target); F(0x28c, fenv_.hi); F(0x290, fenv_.lo);
    F(0x29c, aenv_.level); F(0x2a0, aenv_.coef); F(0x2a4, aenv_.target); F(0x2a8, aenv_.hi); F(0x2ac, aenv_.lo);
    F(0x2b0, aenv_.peak); F(0x2b8, lfo1Val_); F(0x2bc, lfo2Val_); F(0x2c0, lfo3Val_); F(0x2f4, vel_); F(0x2f8, notePitch_); F(0x2fc, lastPitch_);
    for (int i = 0; i < modCount_; ++i) {
        F(0x318 + 0x20 * i + 0x10, mods_[i].last);
        F(0x318 + 0x20 * i + 0x18, mods_[i].state);
    }
    for (int b = 0; b < 3; ++b)
        for (int i = 0; i < 32; ++i) F(0x538 + 0x80 * b + 4 * i, buf_[b][i]);
    const int64_t t = traced_;
    std::fwrite(&t, 8, 1, traceFile());
    std::fwrite(w, 4, 0x6b8 / 4, traceFile());
    std::fflush(traceFile());
}

// Note-on Click (@1001674f): for Analog the filter states start at the click level X too.
void AnalogVoice::onClick(double X) {
    fw(0x70) = float(X);
    fw(0x60) = float(X);
    fw(0x64) = float(X + double(fw(0x64)));
    fw(0x74) = float(X + double(fw(0x74)));
}

// Per-block code of the render functions, after FUN_1004ab60.
template <int M>
void AnalogVoice::postControl() {
    constexpr bool kFull = M == -3 || M == -4;
    if (M == -3) {
        if (ana36_) {
            loc14_ = b_;
            locC_ = float(double(kk_) * double(0.8f) + double(0.08f));
        } else {
            loc14_ = float(double(b_) - double(b_) * double(o3fm_));
        }
    }
    auto incTarget = [&](float ratio, float& dst) {
        const double v = double(ratio) * double(inc_) * 0.5;
        dst = float(v);
        if (v > double(maxInc_)) dst = maxInc_;
    };
    incTarget(ratio1_, o1_.inc);
    if (kFull) incTarget(ratio2_, o2_.inc);
    if (inc3L_ < incMin_) inc3L_ = inc_;
    if (kFull) {
        l18_ = w3_ == 2 ? inc_ : 1.f;
        if (mute3_) l18_ = 0.f;
    }
    if (std::fabs(out3_) < 1e-06f) out3_ = asFloat(0x358637bd);
    if (std::fabs(acc_) < 1e-06f) acc_ = asFloat(0x358637bd);
    if (M == -2 || M == -4) {  // ladder coefficients
        const double G = double(b_) * double(0.28f);
        const double D = G - double(0.366f);
        cc_ = float(D * D);
        b_ = float(((1.0 - G) * double(0.8f) + 1.0) * G);
        const double Q = (double(cc_) * 20.0 + 7.0) * double(cc_) + 2.5;
        kk_ = float(Q * (1.0 - double(kk_)) * (1.0 - G));
    }
    // per-oscillator pulse width / rate shaping (osc 2: the original's case-1 clamp writes osc 1's width)
    auto shape = [&](BlepOsc& o, float& pw, float& pwClampDst, int32_t& wtInc) {
        switch (o.wave) {
            case 1:
                if (pw < 0.f) pw = 1.f;
                else pw = float(double(o.minPw) * double(pw) * double(pw) + 1.0);
                if (double(o.inc) * double(pw) > double(maxInc_)) pwClampDst = float(double(maxInc_) / double(o.inc));
                o.step = float(0.5 / double(pw));
                break;
            case 2: {
                const float m = o.minPw;
                if (m > pw) {
                    pw = m;
                } else {
                    const double m2 = 1.0 - double(m);
                    if (m2 < double(pw)) pw = float(m2);
                }
                break;
            }
            case 3:
            case 4:
                o.inc = float(x87exp((double(pw) - double(0.5f)) * double(0.057762265f)) * double(o.inc));
                break;
            case 5:
                if (pw < 0.f) pw = 0.f;
                else if (pw > 1.f) pw = 1.f;
                pw = float(double(pw) * double(pw));
                break;
            default: {
                const int32_t v = chop(double(wtPer_) * double(o.inc));
                wtInc = v > 0x400000 ? 0x400000 : v;
                break;
            }
        }
        if (o.incL < incMin_) o.incL = o.inc;
    };
    shape(o1_, wave_, wave_, wt1_.inc);
    if (kFull) shape(o2_, osc1Pitch_, wave_, wt2_.inc);
    noise_ = (noise_ & 0xffff0000u) | ((noise_ + 1) & 0xffffu);
}

// Osc 1 / osc 2 edge loop (identical in all four render functions).
void AnalogVoice::runEdges(BlepOsc& o, float pw, float* buf, int v2) {
    const double S = double(o.incL) + double(o.ph);
    o.ph = float(S);
    if (!(S > double(o.edge))) return;
    do {
        switch (o.wave) {
            case 0:
                if (o.edge < 1.f) {
                    addBlep(buf, v2, float((double(o.ph) - double(o.edge)) / double(o.incL)), 1.f);
                    const double E = double(o.edge) + double(o.step);
                    o.edge = float(E);
                    if (!(E < 1.0)) {
                        o.edge = 1.f;
                        o.lev = float(1.0 - (E - 1.0) / double(o.step));
                    }
                } else {
                    const double T = double(o.ph) - 1.0;
                    o.ph = float(T);
                    const float amp = o.lev;
                    o.incL = o.inc;
                    addBlep(buf, v2, float(T / double(o.inc)), amp);
                    double P = pw < 0.f ? 1.0 : double(o.minPw) * double(pw) * double(pw) + 1.0;
                    if (P * double(o.inc) > double(maxInc_)) P = double(maxInc_) / double(o.inc);
                    o.lev = 1.f;
                    const double R = 1.0 / P;
                    o.step = float(R);
                    o.edge = float(R);
                    o.slope = float(-(P * double(o.inc)));
                }
                break;
            case 1:
                o.incL = o.inc;
                if (o.edge < 1.f) {
                    const float a = -o.lev;
                    o.lev = a;
                    addBlep(buf, v2, float((double(o.ph) - double(o.edge)) / double(o.inc)), a);
                    const double E = double(o.step) + double(o.edge);
                    o.edge = float(E);
                    if (E > 1.0) o.edge = 1.f;
                } else {
                    const double T = double(o.ph) - 1.0;
                    o.ph = float(T);
                    if (o.lev < 0.f) {
                        const float a = -o.lev;
                        o.lev = a;
                        addBlep(buf, v2, float(T / double(o.inc)), a);
                    }
                    o.edge = o.step;
                }
                break;
            case 2:
            case 4:
                o.incL = o.inc;
                if (o.edge < 1.f) {
                    const float a = -o.lev;
                    o.lev = a;
                    addBlep(buf, v2, float((double(o.ph) - double(o.edge)) / double(o.inc)), a);
                    o.edge = 1.f;
                } else {
                    const double T = double(o.ph) - 1.0;
                    o.ph = float(T);
                    if (o.lev < 0.f) {
                        const float a = -o.lev;
                        o.lev = a;
                        addBlep(buf, v2, float(T / double(o.inc)), a);
                    }
                    o.edge = o.wave == 2 ? pw : 0.5f;
                }
                break;
            case 3: {
                const double T = double(o.ph) - 1.0;
                o.ph = float(T);
                o.incL = o.inc;
                addBlep(buf, v2, float(T / double(o.inc)), 1.f);
                o.slope = -o.inc;
                break;
            }
            case 5: {
                const float amp = asFloat(0x3e924925);  // 2/7
                if (o.edge < 1.f) {
                    addBlep(buf, v2, float((double(o.ph) - double(o.edge)) / double(o.incL)), amp);
                } else {
                    const double T = double(o.ph) - 1.0;
                    o.ph = float(T);
                    o.incL = o.inc;
                    o.slope = float(-((double(pw) * double(pw) * double(0.072f) + 2.0) * double(o.inc)));
                    addBlep(buf, v2, float(T / double(o.inc)), amp);
                    for (float& u : o.uni)
                        if (!(u < 1.f)) u = float(double(u) - 1.0);
                }
                o.edge = 1.f;
                int idx = -1;
                if (o.uni[0] < 1.f) {
                    idx = 0;
                    o.edge = o.uni[0];
                }
                for (int k = 1; k < 6; ++k)
                    if (o.uni[k] < o.edge) {
                        idx = k;
                        o.edge = o.uni[k];
                    }
                if (idx >= 0) o.uni[idx] = float(double(det_[idx]) * double(pw) + double(o.uni[idx]) + 1.0);
                break;
            }
            default: break;
        }
    } while (o.ph > o.edge);
}

// Wavetable oscillator sample (osc wave > 5 in modes -3/-4): returns the raw float (3.0 + sample/32768).
float AnalogVoice::wtRead(WtOsc& w, float pw) {
    const int32_t a = w.pos >> w.shift;
    const uint8_t* p = wt_ + w.rd + size_t(int64_t(a >> 15) * 2);
    int16_t r0, r1;
    std::memcpy(&r0, p, 2);
    std::memcpy(&r1, p + 2, 2);
    const int32_t s0 = int32_t(r0) ^ int32_t(key_), s1 = int32_t(r1) ^ int32_t(key_);
    const int32_t d = int32_t(uint32_t(s1 - s0) * uint32_t(a & 0x7fff)) >> 8;
    const uint32_t v = uint32_t(d) + (uint32_t(s0 + 0x808000) << 7);
    const float f = asFloat(v);
    const int32_t np = int32_t(uint32_t(w.pos) + uint32_t(w.inc));
    w.pos = np;
    if (np >= wtEnd_) {
        w.rd = 8;
        w.pos = np - wtEnd_;
        w.shift = 0;
        int L = 0x101;
        while (w.inc >> w.shift & int32_t(0xffff0000)) {
            ++w.shift;
            w.rd += uint32_t(L) * 0x80;
            L = (L >> 1) + 1;
        }
        int idx = chop(double(pw) * 64.0);
        if (idx < 0) idx = 0;
        else if (idx > 63) idx = 63;
        w.rd += uint32_t(idx * L * 2);
    }
    return f;
}

template <int M>
void AnalogVoice::renderT(float* out, int frames) {
    constexpr bool kFull = M == -3 || M == -4;
    for (int n = 0; n < frames; ++n) {
        if (--blockCount_ < 0) controlUpdate();
        const int v2 = blockCount_;
        // osc 3
        {
            const double S = double(inc3L_) + double(ph3_);
            ph3_ = float(S);
            if (S > 1.0) {
                ph3_ = float(S - 1.0);
                inc3L_ = inc_;
                const float frac = float(double(ph3_) / double(inc_));
                slope3_ = 0;
                float amp;
                if (w3_ == 0) {
                    slope3_ = float(double(mix3_) * double(inc_) * -0.5);
                    amp = cnt3_ == 0 ? float(double(sub_) + double(mix3_))
                        : cnt3_ == 2 ? float(double(mix3_) - double(sub_)) : 0.f;
                } else {
                    if (w3_ == 2) {
                        const double L = 1.0 - double(inc_);
                        leak3_ = float(L);
                        if (L > double(hpc_)) leak3_ = hpc_;
                    }
                    double v = mix3_;
                    if (w3_ == 1) v *= 0.5;
                    if (cnt3_ == 0) v = v + double(sub_);
                    else if (cnt3_ == 2) v = v - double(sub_);
                    else v = -v;
                    amp = float(v);
                    if (w3_ == 2) amp = float(double(amp) * 1.5);
                }
                addBlep(buf_[0], v2, frac, amp);
                cnt3_ = (cnt3_ + 1) & 3;
            }
            out3_ = float(double(out3_) * double(leak3_) + double(slope3_) + double(buf_[0][v2]));
            buf_[0][v2] = 0;
        }
        const float noiseF = asFloat(noise_);
        noise_ = (noise_ & 0xffff0000u) | ((noise_ * 0x5bu) & 0xffffu);
        double y;
        if constexpr (kFull) {
            float l20 = 0.f;
            if (o1_.wave > 5) {
                l20 = float((double(wtRead(wt1_, wave_)) - 3.0) * double(mix1_));
            } else {
                runEdges(o1_, wave_, buf_[1], v2);
                acc_ = float((double(buf_[1][v2]) + double(o1_.slope)) * double(mix1_) + double(acc_));
            }
            if (o2_.wave > 5) {
                l20 = float((double(wtRead(wt2_, osc1Pitch_)) - 3.0) * double(mix2_) + double(l20));
            } else {
                runEdges(o2_, osc1Pitch_, buf_[2], v2);
                acc_ = float((double(buf_[2][v2]) + double(o2_.slope)) * double(mix2_) + double(acc_));
            }
            float x = float(((double(noiseF) - 128.5) * double(noiseAmt_) + double(acc_)) + double(l20));
            acc_ = float(double(acc_) * double(hpc_));
            buf_[1][v2] = 0;
            buf_[2][v2] = 0;
            if (w3_ == 2) acc_ = float(double(l18_) * double(out3_) + double(acc_));
            else x = float(double(l18_) * double(out3_) + double(x));
            if constexpr (M == -3) {  // SVF with osc-3 phase switched cutoff and drive (1004e400)
                float &lp = fw(0x60), &bp = fw(0x64), &hp = fw(0x68), &xp = fw(0x6c), &u = fw(0x70), &t = fw(0x74);
                const double H = double(kk_) * double(bp) - double(x) - double(xp) + double(lp);
                hp = float(H);
                const double BP = double(bp) - H * double(a_);
                bp = float(BP);
                const float B = ph3_ < 0.5f ? b_ : loc14_;
                xp = x;
                lp = float(BP * double(B) + double(lp));
                const double V = double(locC_) * double(t) - double(lp) + double(u);
                fw(0x78) = float(V);
                const double T = double(t) - V * double(cc_);
                t = float(T);
                u = float(T * double(cc_) + double(u));
                y = tapIdx_ < 0 ? 0.0 : double(fw_[tapIdx_]);
                const double D = double(0.8f) - double(b_);
                if (D > 0.0) {
                    const float m = clamp2(float((double(dist_) * 10.0 + double(bp)) * double(bp) * D));
                    y = double(m) * double(dist_) + y;
                }
                if (inv_) y = double(xp) * 2.0 - y;
            } else {  // ladder with osc-3 cutoff FM and drive (1004fc10)
                const float s = clamp2(fw(0x8c));
                const double ds = s;
                const float in = float((double(x) - (ds - ds * ds * ds * double(0.083333f)) * double(1.0576f)) *
                                           double(kk_) +
                                       double(x) * 2.0);
                double g = (double(o3fm_) * double(out3_) + 1.0) * double(b_);
                if (g > 1.0) g = 1.0;
                else if (g < 0.0) g = 0.0;
                float &xp = fw(0x6c), &t = fw(0x74), &u = fw(0x70), &s3 = fw(0x90), &s4 = fw(0x8c);
                const double oldT = t, oldU = u, oldS3 = s3;
                t = float(((double(in) + double(xp)) - double(t) - double(t)) * g + double(t));
                xp = in;
                u = float(((oldT + double(t)) - double(u) - double(u)) * g + double(u));
                const float n3 = float(((oldU + double(u)) - double(s3) - double(s3)) * g + double(s3));
                s3 = n3;
                const double S4 = ((double(n3) + oldS3) - double(s4) - double(s4)) * g + double(s4);
                s4 = float(S4);
                const float m = clamp2(float((S4 - double(n3)) * double(dist_) * 16.0));
                y = double(fw_[tapIdx_]) - double(m) * double(dist_);
            }
        } else {
            runEdges(o1_, wave_, buf_[1], v2);
            double X = (double(noiseF) - 128.5) * double(noiseAmt_) + double(acc_);
            acc_ = float((double(buf_[1][v2]) + double(o1_.slope)) * double(mix1_) + double(hpc_) * double(acc_));
            buf_[1][v2] = 0;
            if (w3_ == 2) acc_ = float(double(inc_) * double(out3_) + double(acc_));
            else X = X + double(out3_);
            if constexpr (M == 5) {  // SVF (1004cc00)
                float &lp = fw(0x60), &bp = fw(0x64), &hp = fw(0x68), &xp = fw(0x6c), &u = fw(0x70), &t = fw(0x74);
                const double H = double(bp) * double(kk_) - X - double(xp) + double(lp);
                hp = float(H);
                const double BP = double(bp) - H * double(a_);
                bp = float(BP);
                lp = float(BP * double(b_) + double(lp));
                xp = float(X);
                const double T = double(t) - ((double(t) - double(lp)) + double(u)) * double(cc_);
                t = float(T);
                u = float(T * double(cc_) + double(u));
                y = tapIdx_ < 0 ? 0.0 : double(fw_[tapIdx_]);
                if (inv_) y = double(xp) * 2.0 - y;
            } else {  // ladder (1004d7a0)
                const float s = clamp2(fw(0x8c));
                const double ds = s;
                const double IN = (X - (ds - ds * ds * ds * double(0.083333f)) * double(1.0576f)) * double(kk_) + X * 2.0;
                const double g = b_;
                float &xp = fw(0x6c), &t = fw(0x74), &u = fw(0x70), &s3 = fw(0x90), &s4 = fw(0x8c);
                const double oldT = t, oldU = u, oldS3 = s3;
                t = float(((IN + double(xp)) - double(t) - double(t)) * g + double(t));
                xp = float(IN);
                u = float(((oldT + double(t)) - double(u) - double(u)) * g + double(u));
                const double N3 = ((oldU + double(u)) - double(s3) - double(s3)) * g + double(s3);
                s3 = float(N3);
                s4 = float(((N3 + oldS3) - double(s4) - double(s4)) * g + double(s4));
                y = double(fw_[tapIdx_]);
            }
        }
        const double gl = double(incL_) + double(gainL_);
        gainL_ = float(gl);
        out[2 * n] = float(gl * y + double(out[2 * n]));
        const double gr = double(incR_) + double(gainR_);
        gainR_ = float(gr);
        out[2 * n + 1] = float(gr * y + double(out[2 * n + 1]));
    }
}

}  // namespace

bool AnalogSupported() { return true; }

std::shared_ptr<const TypeParams> computeAnalogParams(const ParamArray& p, const ParamArray& raw, const ParamArray& pp,
                                                      const EngineConsts& c, ElementParams& e) {
    auto a = std::make_shared<AnalogParams>();
    // (the loader's Analog param resets, FUN_10016f70, are applied to the stored params in PatchPlayer::load)
    const ParamArray& q = p;
    const double invSr = 1.0 / double(c.sr);
    a->invSr = float(invSr);
    a->analogFilter = q[0x11] > 0.5f;
    // FIX: case 0xa9 (Osc1 Mix) multiplies by the Osc1 Phase sign word (+0x8bc) as it is when 0x15 is set, i.e. in the
    // element's load pass with its own knob values; the loader's final Hyperknob calls may re-route 0x12 afterwards
    // without setting 0x15 again (37/15: Phase routed by knob B). Not modelled: a later runtime set of 0x15.
    const float sign = elementOwnKnobParams(raw, pp)[0x12] > 0.5f ? -1.f : 1.f;
    a->wave1 = quant(q[0x13], 7.9f);
    a->hp = float(1.0 - x87exp(double(float(double(q[0x14]) * 7.0 + std::log(double(a->invSr)) + 3.0))));
    a->mix1 = float((double(q[0x15]) * double(q[0x15]) + double(1e-08f)) * double(sign));
    auto ratio = [](float semiP, float centP) {
        const double semi = std::floor(double(float(double(semiP) * 48.0 + 0.5))) - 24.0;
        const double d = double(centP) - 0.5;
        const double cent = std::fabs(d) * d * 2.0;
        return float(x87exp(double(float((cent + semi) * double(0.057762265f)))));
    };
    a->ratio1 = ratio(q[0x16], q[0x17]);
    a->minPw = float(double(q[0x18]) * double(0.1f) + double(0.001f));
    a->ratio2 = ratio(q[0x19], q[0x1a]);
    a->wave2 = quant(q[0x1b], 7.9f);
    if (a->wave2 == 7) a->wave2 = 6;
    a->wave3 = quant(q[0x1c], 7.9f);
    if (a->wave3 == 7) a->wave3 = 6;
    a->mix3 = float(double(q[0x1d]) + double(1e-08f));
    a->sub = float(double(q[0x1e]) * double(q[0x1e]) * double(0.7079458f) + double(1e-08f));
    a->noise = float(double(q[0x1f]) * double(q[0x1f]));
    a->o3fm = float(double(q[0x20]) * double(q[0x20]) * double(0.68f));
    a->mix2 = float(double(q[0x21]) + double(1e-08f));
    a->dist = float(double(q[0x23]) * 0.5);
    a->xorMask = uint16_t(chop(double(q[0x24]) * double(q[0x24]) * double(16383.f)));
    a->mute3 = q[0x25] > 0.5f;
    a->pw2Base = q[0x66];
    a->full = (double(a->dist) + double(a->o3fm) > double(0.0001f)) || a->mix2 > 0.01f || a->wave1 > 5 || a->mute3;
    if (a->full) a->table = wavetableEntry(c.content, 0, quant(q[0x22], 128.9f));
    return a;
}

std::unique_ptr<Voice> makeAnalogVoice() { return std::make_unique<AnalogVoice>(); }

}  // namespace hs2
