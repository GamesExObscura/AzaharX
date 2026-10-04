// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <boost/serialization/array.hpp>
#include "common/common_types.h"
#include "common/file_util.h"
#include "core/hle/service/ir/ir_user.h"

namespace Service::IR {

// Disney Infinity figure structure
struct InfinityFigure {
    FileUtil::IOFile fig_file;
    bool present = false;
    std::array<u8, 320> data{};  // 5 sectors × 4 blocks × 16 bytes
    u8 order = 0;                 // Placement order (dynamic)
    u8 queued_status = 0;         // 0x00 = added, 0x01 = removed
    bool status_pending = false;  // Need to send 0xAB notification
    
    void Save();
};

// Disney Infinity command bytes
enum class InfinityCommand : u8 {
    Handshake = 0x80,
    DeviceChallenge = 0x81,
    ChallengeResponse = 0x83,
    Disconnect = 0x84,
    BatteryRequest = 0x85,
    LedColor = 0x90,
    ConnectionAlive = 0x91,
    LedFade = 0x92,
    LedPulse = 0x93,
    LedStrobe = 0x94,
    LedAll = 0x95,
    NfcPosition = 0xA1,
    ReadNfc = 0xA2,
    WriteNfc = 0xA3,
    UidRequest = 0xB4,
    Status = 0xB5,
};

// Pad positions
enum class InfinityPad : u8 {
    All = 0x00,
    Hexagon = 0x01,  // Play Set pad
    Round = 0x02,    // Character pad
};

// ============================================================================
// InfinityBase - Figure storage and management (like SkylanderPortal)
// This class is used by both the GUI and the IR protocol handler
// ============================================================================
class InfinityBase final {
public:
    void Activate();
    void Deactivate();
    bool IsActivated() const;
    
    // Figure management (called from GUI)
    void SetFigure(u8 pad, std::span<const u8> figure_data, const std::string& path = "");
    bool RemoveFigure(u8 pad);
    bool HasFigure(u8 pad) const;
    
    // Get figure data
    std::array<u8, 320> GetFigureData(u8 pad) const;
    
    // Protocol operations (called from IRInfinityBase)
    void QueryBlock(u8 order, u8 sector, u8 block, u8* reply_buf);
    void WriteBlock(u8 order, u8 sector, u8 block, const u8* data, u8* reply_buf);
    void GetUid(u8 order, u8* reply_buf);
    std::array<u8, 32> GetStatus();
    
    // Get figure by order number or pad
    InfinityFigure* GetFigureByOrder(u8 order);
    InfinityFigure* GetFigureByPad(u8 pad);
    u8 GetPadFromOrder(u8 order) const;
    
    // Reset
    void Reset();
    
private:
    void RecalculateOrders();
    
    mutable std::mutex mutex;
    bool activated = false;
    u8 current_order = 0;
    u8 interrupt_counter = 0;
    
    // Figure slots: Index 0 = Hexagon (0x01), Index 1 = Round (0x02)
    std::array<InfinityFigure, 2> figures{};
};

// Global instance for GUI and protocol access (mirrors g_skyportal pattern)
extern InfinityBase g_infinity_base;

// ============================================================================
// IRInfinityBase - IR protocol handler (like IRPortal)
// Inherits from IRDevice for ir:USER integration
// ============================================================================
class IRInfinityBase final : public IRDevice {
public:
    explicit IRInfinityBase(SendFunc send_func);
    ~IRInfinityBase() override;

    // IRDevice interface
    void OnConnect() override;
    void OnDisconnect() override;
    void OnReceive(std::span<const u8> data) override;

private:
    // Handle incoming packet and generate response
    void HandlePacket(std::span<const u8> data);

    // Command handlers
    void HandleHandshake(std::span<const u8> data);
    void HandleDeviceChallenge(std::span<const u8> data);
    void HandleChallengeResponse(std::span<const u8> data);
    void HandleDisconnect(std::span<const u8> data);
    void HandleBatteryRequest(std::span<const u8> data);
    void HandleLedColor(std::span<const u8> data);
    void HandleConnectionAlive(std::span<const u8> data);
    void HandleLedFade(std::span<const u8> data);
    void HandleLedPulse(std::span<const u8> data);
    void HandleLedStrobe(std::span<const u8> data);
    void HandleLedAll(std::span<const u8> data);
    void HandleNfcPosition(std::span<const u8> data);
    void HandleReadNfc(std::span<const u8> data);
    void HandleWriteNfc(std::span<const u8> data);
    void HandleUidRequest(std::span<const u8> data);
    void HandleStatus(std::span<const u8> data);

    // Build Disney Layer 2 response
    std::vector<u8> BuildResponse(const std::vector<u8>& payload);
    std::vector<u8> BuildFigureNotification(u8 position, u8 order, u8 status);

    // Serialization
    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        // State is stored in g_infinity_base, not here
    }
    friend class boost::serialization::access;

    u8 last_counter = 0;
    u8 battery_level = 0xFF;
};

} // namespace Service::IR

BOOST_CLASS_EXPORT_KEY(Service::IR::IRInfinityBase)
