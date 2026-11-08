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

enum class Role : std::uint8_t { WiiU = 1, Nintendo3DS = 2 };
enum class FrameType : std::uint8_t { Beacon, Data, Authentication, AssociationResponse, Deauthentication };
enum class MessageType : std::uint8_t { Hello = 1, HelloAck, Frame };

struct Frame {
    FrameType type{};
    std::uint8_t channel{};
    std::array<std::uint8_t, 6> source{}, destination{};
    std::vector<std::uint8_t> data;
};

struct Message {
    MessageType type{};
    Role role{};
    std::uint64_t session{}, peer_session{};
    Frame frame;
};

inline constexpr std::size_t HeaderSize = 44;
inline constexpr std::size_t MaxPayload = 2048;
std::vector<std::uint8_t> Encode(const Message& message);
std::optional<Message> Decode(std::span<const std::uint8_t> bytes);

struct LinkStatus {
    bool enabled{}, peer_present{};
    std::uint64_t received{}, transmitted{}, rejected{}, dropped{};
    std::string error;
    std::string guest_status;
    std::string network;
};

// Transport authentication is separate from the native guest UDS association.
class Transport {
public:
    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    bool Start(Role role, std::uint16_t local_port, std::uint16_t peer_port);
    bool StartDirect(Role role, const std::string& address, std::uint16_t port,
                     std::string passphrase);
    void Stop();
    bool Send(const Frame& frame);
    std::optional<Frame> Receive();
    LinkStatus GetStatus() const;
    void SetGuestStatus(std::string status);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

Transport& GetTransport();
std::string DescribeStatus(const LinkStatus& status);

} // namespace WiiU3DSLink
