// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Transport.h"
#include <array>
#include <span>

namespace WiiU3DSLink {

enum class NativeFrameError {
    None,
    Channel,
    Length,
    Protocol,
    Fields,
    Node,
    Tags,
    MissingNetworkInfo,
    Unsupported,
};

namespace NativeFrameDetail {
using Bytes = std::span<const std::uint8_t>;

inline std::uint16_t BE16(Bytes bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

inline std::uint16_t LE16(Bytes bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

inline bool Node(std::uint16_t value, std::uint8_t maximum = 16) {
    return value >= 1 && value <= maximum;
}

inline NativeFrameError Tags(Bytes tags, bool beacon) {
    bool network_info = false;
    while (!tags.empty()) {
        if (tags.size() < 2 || tags[1] > tags.size() - 2)
            return NativeFrameError::Tags;
        const auto id = tags[0];
        const auto body = tags.subspan(2, tags[1]);
        if (id == 0 && body.size() > 32)
            return NativeFrameError::Tags;
        if (id == 221) {
            if (body.size() < 3)
                return NativeFrameError::Tags;
            if (body[0] == 0 && body[1] == 0x1F && body[2] == 0x32) {
                if (body.size() < 4)
                    return NativeFrameError::Tags;
                if (body[3] == 21) {
                    // OUI/network fields (31), SHA1 (20), app-data length (1).
                    if (network_info || body.size() < 52 || body[51] > 200 ||
                        body.size() != std::size_t{52} + body[51])
                        return NativeFrameError::Tags;
                    if (body[17] < 2 || body[17] > 16 || body[16] < 1 ||
                        body[16] > body[17])
                        return NativeFrameError::Node;
                    network_info = true;
                }
            }
        }
        tags = tags.subspan(2 + body.size());
    }
    return !beacon || network_info ? NativeFrameError::None
                                   : NativeFrameError::MissingNetworkInfo;
}

inline NativeFrameError Data(Bytes bytes, std::uint8_t expected_max_nodes) {
    constexpr std::array<std::uint8_t, 6> llc{0xAA, 0xAA, 3, 0, 0, 0};
    if (bytes.size() < 8)
        return NativeFrameError::Length;
    for (std::size_t i = 0; i < llc.size(); ++i)
        if (bytes[i] != llc[i])
            return NativeFrameError::Protocol;
    const auto protocol = BE16(bytes, 6);
    if (protocol == 0x876D) {
        if (bytes.size() < 22 || bytes.size() - 22 > 1478)
            return NativeFrameError::Length;
        const auto protocol_size = BE16(bytes, 8);
        if (protocol_size != bytes.size() - 8 || BE16(bytes, 12) != protocol_size - 4)
            return NativeFrameError::Length;
        if (bytes[14] != 0)
            return NativeFrameError::Unsupported;
        if (bytes[15] == 0)
            return NativeFrameError::Fields;
        if (!Node(BE16(bytes, 20)) ||
            (!Node(BE16(bytes, 18)) && BE16(bytes, 18) != 0xFFFF))
            return NativeFrameError::Node;
        return NativeFrameError::None;
    }
    if (protocol != 0x888E)
        return NativeFrameError::Protocol;
    if (bytes.size() < 10)
        return NativeFrameError::Length;
    const auto magic = BE16(bytes, 8);
    if (magic == 0x201) {
        if (bytes.size() != 8 + 0x30)
            return NativeFrameError::Length;
        if (BE16(bytes, 10) == 0 || (bytes[12] != 1 && bytes[12] != 2))
            return NativeFrameError::Fields;
        if (BE16(bytes, 48) > 16)
            return NativeFrameError::Node;
        return NativeFrameError::None;
    }
    if (magic != 0x202)
        return NativeFrameError::Unsupported;
    if (bytes.size() != 8 + 0x298)
        return NativeFrameError::Length;
    const auto count = bytes[26];
    const auto maximum = bytes[27];
    if (maximum < 2 || maximum > 16 || count == 0 || count > maximum ||
        (expected_max_nodes != 0 && maximum != expected_max_nodes) ||
        BE16(bytes, 12) > maximum)
        return NativeFrameError::Node;
    std::uint32_t mask = 0;
    unsigned actual = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const auto id = BE16(bytes, 64 + i * 0x28);
        if (id == 0)
            continue;
        if (!Node(id, maximum) || (mask & (1U << (id - 1))) != 0)
            return NativeFrameError::Node;
        mask |= 1U << (id - 1);
        ++actual;
    }
    if (actual != count || (mask & 1) == 0 ||
        (BE16(bytes, 12) != 0 && (mask & (1U << (BE16(bytes, 12) - 1))) == 0))
        return NativeFrameError::Node;
    return NativeFrameError::None;
}
} // namespace NativeFrameDetail

// Structural safety only: not authentication, association-state validation, or
// validation of encrypted beacon contents. Never treat this as a connected peer.
inline NativeFrameError ValidateNativeFrame(const Frame& frame,
                                          std::uint8_t expected_max_nodes = 0) {
    using namespace NativeFrameDetail;
    if (frame.channel < 1 || frame.channel > 14)
        return NativeFrameError::Channel;
    const Bytes bytes(frame.data);
    if (bytes.size() > MaxPayload)
        return NativeFrameError::Length;
    switch (frame.type) {
    case FrameType::Beacon:
        return bytes.size() < 12 ? NativeFrameError::Length : Tags(bytes.subspan(12), true);
    case FrameType::Authentication:
        if (bytes.size() != 6)
            return NativeFrameError::Length;
        return LE16(bytes, 0) == 0 && (LE16(bytes, 2) == 1 || LE16(bytes, 2) == 2) &&
                       LE16(bytes, 4) == 0
                   ? NativeFrameError::None : NativeFrameError::Unsupported;
    case FrameType::AssociationResponse:
        if (bytes.size() < 6)
            return NativeFrameError::Length;
        if (LE16(bytes, 2) != 0 || (LE16(bytes, 4) & 0x3FFF) == 0)
            return NativeFrameError::Unsupported;
        return Tags(bytes.subspan(6), false);
    case FrameType::Data:
        return Data(bytes, expected_max_nodes);
    case FrameType::Deauthentication:
        return bytes.size() == 2 ? NativeFrameError::None : NativeFrameError::Length;
    }
    return NativeFrameError::Unsupported;
}

} // namespace WiiU3DSLink
