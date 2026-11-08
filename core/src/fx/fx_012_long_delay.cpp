// 12 Long Delay (ctor FUN_10043790, setSampleRate FUN_1002c860, vtable 0x100a9cf4, paramName 0x1002c870,
// paramDisplay 0x1002c730). Identical to 10 Delay (shares its process FUN_1002c0a0, setParam FUN_1002c4e0 and
// tempo FUN_1002d850): FUN_1002c860 only sets the line-length factor (this+0x78) to 4.01 s instead of 1.51 s and
// jumps to the Delay's setSampleRate FUN_1002bfd0. Implementation: fx_010_delay.cpp.
// Oracle-verified: bit-exact (-240 dB floor).
#include "effect_base.h"

namespace hs2 {

std::unique_ptr<Effect> makeDelay(float seconds);  // fx_010_delay.cpp

std::unique_ptr<Effect> make_fx_12() { return makeDelay(4.01f); }

}  // namespace hs2
