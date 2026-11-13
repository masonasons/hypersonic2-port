#pragma once

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace hs2port {

inline std::optional<double> keyboardNumber(const std::string& input) {
    std::istringstream text(input);
    text.imbue(std::locale::classic());
    double number = 0;
    if (!(text >> std::noskipws >> number) || !text.eof() || !std::isfinite(number)) return {};
    return number;
}

}
