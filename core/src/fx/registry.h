#pragma once
// Generated from docs/engine/effects_table.json. One factory per effect type (core/src/fx/fx_NNN_*.cpp).
#include <memory>

#include "hs2/effects.h"

namespace hs2 {

std::unique_ptr<Effect> make_fx_0();  // Reverb
std::unique_ptr<Effect> make_fx_1();  // Hall Reverb
std::unique_ptr<Effect> make_fx_2();  // Plate Reverb
std::unique_ptr<Effect> make_fx_3();  // Room Reverb
std::unique_ptr<Effect> make_fx_4();  // Reverse Reverb
std::unique_ptr<Effect> make_fx_5();  // Non-Linear Reverb
std::unique_ptr<Effect> make_fx_6();  // Early Reflections
std::unique_ptr<Effect> make_fx_7();  // Tuned Resonators
std::unique_ptr<Effect> make_fx_8();  // Overheads
std::unique_ptr<Effect> make_fx_10();  // Delay
std::unique_ptr<Effect> make_fx_11();  // Stereo Delay
std::unique_ptr<Effect> make_fx_12();  // Long Delay
std::unique_ptr<Effect> make_fx_13();  // Tape Delay
std::unique_ptr<Effect> make_fx_14();  // Ducking Delay
std::unique_ptr<Effect> make_fx_15();  // Grain Delay
std::unique_ptr<Effect> make_fx_16();  // Diffuser Delay
std::unique_ptr<Effect> make_fx_19();  // Chorus + Reverb
std::unique_ptr<Effect> make_fx_20();  // Chorus
std::unique_ptr<Effect> make_fx_21();  // Quad Chorus
std::unique_ptr<Effect> make_fx_22();  // Space Chorus
std::unique_ptr<Effect> make_fx_23();  // Ensemble
std::unique_ptr<Effect> make_fx_30();  // Flanger
std::unique_ptr<Effect> make_fx_31();  // Step Flanger
std::unique_ptr<Effect> make_fx_40();  // Phaser
std::unique_ptr<Effect> make_fx_41();  // Deep Phaser
std::unique_ptr<Effect> make_fx_42();  // Bi-Phaser
std::unique_ptr<Effect> make_fx_50();  // Pan / Tremolo
std::unique_ptr<Effect> make_fx_51();  // Stereo Width
std::unique_ptr<Effect> make_fx_52();  // Stereo Enhancer
std::unique_ptr<Effect> make_fx_54();  // Scanner Vibrato
std::unique_ptr<Effect> make_fx_55();  // Rotary Speaker
std::unique_ptr<Effect> make_fx_56();  // Rotary Speaker 2
std::unique_ptr<Effect> make_fx_60();  // Pitch Shift
std::unique_ptr<Effect> make_fx_61();  // Detune
std::unique_ptr<Effect> make_fx_70();  // Vintage Filter
std::unique_ptr<Effect> make_fx_71();  // Wah Pedal
std::unique_ptr<Effect> make_fx_72();  // TalkBox
std::unique_ptr<Effect> make_fx_80();  // Shelf EQ
std::unique_ptr<Effect> make_fx_81();  // Parametric EQ
std::unique_ptr<Effect> make_fx_85();  // Enhancer
std::unique_ptr<Effect> make_fx_90();  // Limiter
std::unique_ptr<Effect> make_fx_91();  // Compressor
std::unique_ptr<Effect> make_fx_92();  // Multiband
std::unique_ptr<Effect> make_fx_93();  // Punch
std::unique_ptr<Effect> make_fx_95();  // Gate
std::unique_ptr<Effect> make_fx_100();  // Distortion
std::unique_ptr<Effect> make_fx_101();  // Overdrive
std::unique_ptr<Effect> make_fx_102();  // Bit Reduction
std::unique_ptr<Effect> make_fx_103();  // Amp Simulator
std::unique_ptr<Effect> make_fx_104();  // Tube Drive
std::unique_ptr<Effect> make_fx_105();  // Waveshaper
std::unique_ptr<Effect> make_fx_110();  // Modulate L/R

}  // namespace hs2
