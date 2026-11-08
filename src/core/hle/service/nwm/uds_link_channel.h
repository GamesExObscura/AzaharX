// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Service::NWM {
// The room backend used a dummy channel; a native peer validates the advertised one.
inline constexpr std::uint8_t LinkClientChannel(std::uint8_t advertised, std::uint8_t current) {
    return advertised >= 1 && advertised <= 14 ? advertised : current;
}
} // namespace Service::NWM
