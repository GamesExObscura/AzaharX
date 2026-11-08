// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace WiiU3DSLink {
struct LanOptions {
    std::string address;
    std::uint16_t port{};
    std::array<std::uint8_t,32> key{};
    bool passphrase{};
};
std::optional<LanOptions> ParseLanInvite(const std::string& invite);
std::optional<LanOptions> LanOptionsFromEnvironment();
LanOptions DirectOptions(bool host, const std::string& address, std::uint16_t port,
                         std::string passphrase);
std::string GenerateDirectPassphrase();
enum class LanRead { Rejected, Pending, Complete };
struct LanResult { LanRead state{LanRead::Rejected}; std::vector<std::uint8_t> bytes; };

// An authenticated, MTU-bounded envelope around the unchanged native link frames.
// Owned exclusively by the transport's I/O worker; no guest state is accessed here.
class LanDatagram {
public:
    explicit LanDatagram(const std::array<std::uint8_t,32>& key, bool passphrase = false);
    ~LanDatagram();
    LanDatagram(const LanDatagram&)=delete;
    LanDatagram& operator=(const LanDatagram&)=delete;
    std::vector<std::vector<std::uint8_t>> Seal(std::span<const std::uint8_t> bytes);
    LanResult Open(std::span<const std::uint8_t> bytes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
