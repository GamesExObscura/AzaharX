// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Transport.h"
#include <deque>
#include <functional>

namespace WiiU3DSLink {
using Mac = std::array<std::uint8_t, 6>;
inline constexpr Mac BroadcastMac{255,255,255,255,255,255};

struct UdsNode {
    std::uint64_t friend_code{};
    std::array<std::uint16_t, 10> name{};
    std::uint16_t id{};
    Mac mac{};
};

struct UdsStatus {
    std::uint32_t state{}, reason{};
    std::uint16_t id{}, changed{}, mask{};
    std::uint8_t count{}, maximum{};
};

struct UdsCrypto {
    using Bytes = std::span<const std::uint8_t>;
    std::function<std::vector<std::uint8_t>(Bytes)> md5, sha1;
    std::function<std::vector<std::uint8_t>(Bytes, const std::array<std::uint8_t,16>&)> ctr;
};

enum class UdsResult { Ok, NotInitialized, State, Argument, Resource, NotFound, TooLarge, Busy };
struct UdsDatagram { std::uint16_t source{}; std::vector<std::uint8_t> data; };

// Called only on the owning emulator's scheduler. No guest memory or callbacks
// are accessed from the socket worker. Crypto is supplied by the host emulator.
class UdsHost {
public:
    explicit UdsHost(UdsCrypto crypto);
    UdsResult Initialize(const UdsNode& identity, std::size_t memory_size);
    void Finalize();
    UdsResult CreateNetwork(std::uint8_t id, std::uint8_t maximum, std::uint32_t communication_id,
                            std::uint8_t channel, std::uint32_t network_id,
                            std::span<const std::uint8_t> application_data);
    UdsResult DestroyNetwork(std::uint32_t reason = 2);
    UdsResult Allow(bool allow);
    UdsResult Eject(std::uint16_t node);
    UdsResult SetApplicationData(std::span<const std::uint8_t> data);
    UdsResult CreateEndpoint(std::uint32_t& descriptor);
    UdsResult Attach(std::uint32_t descriptor, std::uint16_t peer, std::uint8_t channel,
                     std::size_t capacity);
    UdsResult DestroyEndpoint(std::uint32_t descriptor);
    UdsResult Receive(std::uint32_t descriptor, std::size_t capacity, UdsDatagram& datagram);
    UdsResult Send(std::uint32_t descriptor, std::uint16_t peer, std::uint8_t channel,
                   std::span<const std::uint8_t> data);
    void Process(const Frame& frame);
    void PeerLost();
    std::optional<Frame> Beacon(std::uint64_t microseconds);
    std::optional<Frame> PopOutgoing();
    const UdsStatus& Status() const { return status; }
    UdsStatus TakeStatus();
    bool HasStateChange() const { return state_change; }
    bool Initialized() const { return initialized; }
    int EndpointIndex(std::uint32_t descriptor) const;
    bool HasPacket(std::size_t index) const;
    const UdsNode* Node(std::uint16_t id) const;
    std::uint8_t Channel() const { return channel; }
    std::span<const std::uint8_t> ApplicationData() const { return app_data; }
    std::uint64_t Received() const { return received; }
    std::uint64_t Transmitted() const { return transmitted; }
    std::uint64_t Rejected() const { return rejected; }
private:
    struct Endpoint {
        std::uint32_t descriptor{};
        std::uint16_t peer{};
        std::uint8_t channel{};
        std::size_t capacity{}, used{};
        std::deque<UdsDatagram> packets;
    };
    UdsCrypto crypto;
    bool initialized{}, allowed{}, state_change{};
    UdsStatus status;
    std::array<UdsNode,16> nodes{};
    std::array<Endpoint,16> endpoints{};
    std::array<Mac,16> authenticating{};
    std::deque<Frame> outgoing;
    std::vector<std::uint8_t> app_data;
    std::size_t memory_size{};
    std::uint32_t next_descriptor{}, communication_id{}, network_id{};
    std::uint16_t sequence{};
    std::uint8_t id{}, channel{};
    std::uint64_t last_beacon{}, received{}, transmitted{}, rejected{};
    void Changed(std::uint16_t mask);
    Frame MakeFrame(FrameType type, const Mac& destination, std::vector<std::uint8_t> data) const;
    void QueueLogoff(const UdsNode& node);
    void Remove(std::size_t index);
};
} // namespace WiiU3DSLink
