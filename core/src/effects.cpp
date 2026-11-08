#include "hs2/effects.h"

#include "fx/registry.h"

namespace hs2 {

// Factory index = the original's FUN_10043e20 type number (element param 0x13 = trunc(v*128.9)).
std::unique_ptr<Effect> createEffect(int type) {
    switch (type) {
        case 0: return make_fx_0();
        case 1: return make_fx_1();
        case 2: return make_fx_2();
        case 3: return make_fx_3();
        case 4: return make_fx_4();
        case 5: return make_fx_5();
        case 6: return make_fx_6();
        case 7: return make_fx_7();
        case 8: return make_fx_8();
        case 10: return make_fx_10();
        case 11: return make_fx_11();
        case 12: return make_fx_12();
        case 13: return make_fx_13();
        case 14: return make_fx_14();
        case 15: return make_fx_15();
        case 16: return make_fx_16();
        case 19: return make_fx_19();
        case 20: return make_fx_20();
        case 21: return make_fx_21();
        case 22: return make_fx_22();
        case 23: return make_fx_23();
        case 30: return make_fx_30();
        case 31: return make_fx_31();
        case 40: return make_fx_40();
        case 41: return make_fx_41();
        case 42: return make_fx_42();
        case 50: return make_fx_50();
        case 51: return make_fx_51();
        case 52: return make_fx_52();
        case 54: return make_fx_54();
        case 55: return make_fx_55();
        case 56: return make_fx_56();
        case 60: return make_fx_60();
        case 61: return make_fx_61();
        case 70: return make_fx_70();
        case 71: return make_fx_71();
        case 72: return make_fx_72();
        case 80: return make_fx_80();
        case 81: return make_fx_81();
        case 85: return make_fx_85();
        case 90: return make_fx_90();
        case 91: return make_fx_91();
        case 92: return make_fx_92();
        case 93: return make_fx_93();
        case 95: return make_fx_95();
        case 100: return make_fx_100();
        case 101: return make_fx_101();
        case 102: return make_fx_102();
        case 103: return make_fx_103();
        case 104: return make_fx_104();
        case 105: return make_fx_105();
        case 110: return make_fx_110();
        default: return nullptr;  // 128 = no effect (group bus)
    }
}

const char* effectName(int type) {
    switch (type) {
        case 0: return "Reverb";
        case 1: return "Hall Reverb";
        case 2: return "Plate Reverb";
        case 3: return "Room Reverb";
        case 4: return "Reverse Reverb";
        case 5: return "Non-Linear Reverb";
        case 6: return "Early Reflections";
        case 7: return "Tuned Resonators";
        case 8: return "Overheads";
        case 10: return "Delay";
        case 11: return "Stereo Delay";
        case 12: return "Long Delay";
        case 13: return "Tape Delay";
        case 14: return "Ducking Delay";
        case 15: return "Grain Delay";
        case 16: return "Diffuser Delay";
        case 19: return "Chorus + Reverb";
        case 20: return "Chorus";
        case 21: return "Quad Chorus";
        case 22: return "Space Chorus";
        case 23: return "Ensemble";
        case 30: return "Flanger";
        case 31: return "Step Flanger";
        case 40: return "Phaser";
        case 41: return "Deep Phaser";
        case 42: return "Bi-Phaser";
        case 50: return "Pan / Tremolo";
        case 51: return "Stereo Width";
        case 52: return "Stereo Enhancer";
        case 54: return "Scanner Vibrato";
        case 55: return "Rotary Speaker";
        case 56: return "Rotary Speaker 2";
        case 60: return "Pitch Shift";
        case 61: return "Detune";
        case 70: return "Vintage Filter";
        case 71: return "Wah Pedal";
        case 72: return "TalkBox";
        case 80: return "Shelf EQ";
        case 81: return "Parametric EQ";
        case 85: return "Enhancer";
        case 90: return "Limiter";
        case 91: return "Compressor";
        case 92: return "Multiband";
        case 93: return "Punch";
        case 95: return "Gate";
        case 100: return "Distortion";
        case 101: return "Overdrive";
        case 102: return "Bit Reduction";
        case 103: return "Amp Simulator";
        case 104: return "Tube Drive";
        case 105: return "Waveshaper";
        case 110: return "Modulate L/R";
        case 128: return "(none)";
        default: return "?";
    }
}

}  // namespace hs2
