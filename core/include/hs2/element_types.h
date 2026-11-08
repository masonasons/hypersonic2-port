#pragma once
// Element types other than Sample: registry of per-type parameter computation and voice factories.
// Each type lives in core/src/types/<type>.cpp and defines its three functions below. Not implemented =
// supported() false (the element is skipped with a warning).
#include <memory>

#include "hs2/element.h"
#include "hs2/voice.h"

namespace hs2 {

// p = element params after quick knobs and macros (as computeElement uses them), raw = stored params,
// pp = patch params, e = the common element params computed so far (type-specific code may read or adjust them).
using TypeParamsFn = std::shared_ptr<const TypeParams> (*)(const ParamArray& p, const ParamArray& raw,
                                                           const ParamArray& pp, const EngineConsts& c,
                                                           ElementParams& e);

#define HS2_ELEMENT_TYPE(Name)                                                                                          bool Name##Supported();                                                                                             std::shared_ptr<const TypeParams> compute##Name##Params(const ParamArray& p, const ParamArray& raw,                                                                         const ParamArray& pp, const EngineConsts& c,                                                                        ElementParams& e);                                          std::unique_ptr<Voice> make##Name##Voice();
HS2_ELEMENT_TYPE(Drawbar)
HS2_ELEMENT_TYPE(Wavetable)
HS2_ELEMENT_TYPE(FM)
HS2_ELEMENT_TYPE(Analog)
#undef HS2_ELEMENT_TYPE

inline bool elementTypeSupported(ElementType t) {
    switch (t) {
        case ElementType::Sample: return true;
        case ElementType::Drawbar: return DrawbarSupported();
        case ElementType::Wavetable: return WavetableSupported();
        case ElementType::FM: return FMSupported();
        case ElementType::Analog: return AnalogSupported();
        default: return false;
    }
}

inline std::shared_ptr<const TypeParams> computeTypeParams(ElementType t, const ParamArray& p, const ParamArray& raw,
                                                           const ParamArray& pp, const EngineConsts& c,
                                                           ElementParams& e) {
    switch (t) {
        case ElementType::Drawbar: return computeDrawbarParams(p, raw, pp, c, e);
        case ElementType::Wavetable: return computeWavetableParams(p, raw, pp, c, e);
        case ElementType::FM: return computeFMParams(p, raw, pp, c, e);
        case ElementType::Analog: return computeAnalogParams(p, raw, pp, c, e);
        default: return nullptr;
    }
}

inline std::unique_ptr<Voice> makeVoice(ElementType t) {
    switch (t) {
        case ElementType::Drawbar: return makeDrawbarVoice();
        case ElementType::Wavetable: return makeWavetableVoice();
        case ElementType::FM: return makeFMVoice();
        case ElementType::Analog: return makeAnalogVoice();
        default: return std::make_unique<Voice>();  // Sample
    }
}

}  // namespace hs2
