// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include "common/logging/log.h"
#include "core/hle/service/ir/ir_infinity_base.h"

namespace Service::IR {

// Global instance (mirrors g_skyportal pattern)
InfinityBase g_infinity_base;

// ============================================================================
// InfinityFigure Implementation
// ============================================================================

void InfinityFigure::Save() {
    if (!fig_file) {
        return;
    }
    fig_file.Seek(0, SEEK_SET);
    fig_file.WriteBytes(data.data(), data.size());
}

// ============================================================================
// InfinityBase Implementation (like SkylanderPortal)
// ============================================================================

void InfinityBase::Activate() {
    std::lock_guard lock(mutex);
    if (activated) {
        return;
    }
    
    // Queue status updates for present figures
    for (auto& fig : figures) {
        if (fig.present) {
            fig.queued_status = 0x00;  // Added
            fig.status_pending = true;
        }
    }
    
    activated = true;
    LOG_INFO(Service_IR, "Disney Infinity base activated");
}

void InfinityBase::Deactivate() {
    std::lock_guard lock(mutex);
    activated = false;
    LOG_INFO(Service_IR, "Disney Infinity base deactivated");
}

bool InfinityBase::IsActivated() const {
    return activated;
}

void InfinityBase::SetFigure(u8 pad, std::span<const u8> figure_data, const std::string& path) {
    std::lock_guard lock(mutex);
    
    // pad: 0x01 = Hexagon (index 0), 0x02 = Round (index 1)
    size_t index = (pad == 0x01) ? 0 : 1;
    
    if (index >= figures.size()) {
        LOG_WARNING(Service_IR, "Infinity: Invalid pad {:02X}", pad);
        return;
    }
    
    auto& figure = figures[index];
    
    // Copy figure data (up to 320 bytes)
    size_t copy_size = std::min(figure_data.size(), figure.data.size());
    std::memcpy(figure.data.data(), figure_data.data(), copy_size);
    
    // Open file for saving changes
    if (!path.empty()) {
        figure.fig_file = FileUtil::IOFile(path, "rb+");
    }
    
    figure.present = true;
    figure.order = current_order++;
    figure.queued_status = 0x00;  // Added
    figure.status_pending = true;  // Will be sent on next ConnectionAlive
    
    LOG_INFO(Service_IR, "Infinity: Figure added to pad {:02X} with order {}", pad, figure.order);
}

bool InfinityBase::RemoveFigure(u8 pad) {
    std::lock_guard lock(mutex);
    
    size_t index = (pad == 0x01) ? 0 : 1;
    
    if (index >= figures.size()) {
        LOG_WARNING(Service_IR, "Infinity: Invalid pad {:02X}", pad);
        return false;
    }
    
    auto& figure = figures[index];
    
    if (!figure.present) {
        LOG_WARNING(Service_IR, "Infinity: No figure on pad {:02X}", pad);
        return false;
    }
    
    // Save before removing
    figure.Save();
    
    figure.queued_status = 0x01;  // Removed
    figure.status_pending = true;  // Will be sent on next ConnectionAlive
    figure.present = false;
    
    LOG_INFO(Service_IR, "Infinity: Figure removed from pad {:02X}", pad);
    
    RecalculateOrders();
    return true;
}

bool InfinityBase::HasFigure(u8 pad) const {
    std::lock_guard lock(mutex);
    size_t index = (pad == 0x01) ? 0 : 1;
    if (index >= figures.size()) {
        return false;
    }
    return figures[index].present;
}

std::array<u8, 320> InfinityBase::GetFigureData(u8 pad) const {
    std::lock_guard lock(mutex);
    size_t index = (pad == 0x01) ? 0 : 1;
    if (index >= figures.size()) {
        return {};
    }
    return figures[index].data;
}

void InfinityBase::QueryBlock(u8 order, u8 sector, u8 block, u8* reply_buf) {
    std::lock_guard lock(mutex);
    
    InfinityFigure* figure = nullptr;
    for (auto& fig : figures) {
        if (fig.present && fig.order == order) {
            figure = &fig;
            break;
        }
    }
    
    reply_buf[0] = order;  // Counter/order
    
    if (!figure || !figure->present) {
        reply_buf[1] = 0x80;  // Error
        return;
    }
    
    // Validate sector/block
    if (sector > 4 || block > 3) {
        reply_buf[1] = 0x80;  // Error
        return;
    }
    
    // Calculate offset: each sector has 4 blocks of 16 bytes
    size_t offset = (sector * 4 * 16) + (block * 16);
    
    reply_buf[1] = 0x00;  // Success
    std::memcpy(&reply_buf[2], &figure->data[offset], 16);
}

void InfinityBase::WriteBlock(u8 order, u8 sector, u8 block, const u8* data, u8* reply_buf) {
    std::lock_guard lock(mutex);
    
    InfinityFigure* figure = nullptr;
    for (auto& fig : figures) {
        if (fig.present && fig.order == order) {
            figure = &fig;
            break;
        }
    }
    
    reply_buf[0] = order;
    
    if (!figure || !figure->present) {
        reply_buf[1] = 0x80;  // Error
        return;
    }
    
    // Validate sector/block
    if (sector > 4 || block > 3) {
        reply_buf[1] = 0x80;  // Error
        return;
    }
    
    // Calculate offset
    size_t offset = (sector * 4 * 16) + (block * 16);
    
    // Write data
    std::memcpy(&figure->data[offset], data, 16);
    
    // Save to file
    figure->Save();
    
    reply_buf[1] = 0x00;  // Success
}

void InfinityBase::GetUid(u8 order, u8* reply_buf) {
    std::lock_guard lock(mutex);
    
    InfinityFigure* figure = nullptr;
    for (auto& fig : figures) {
        if (fig.present && fig.order == order) {
            figure = &fig;
            break;
        }
    }
    
    reply_buf[0] = order;
    
    if (!figure || !figure->present) {
        reply_buf[1] = 0x80;  // Error
        std::memset(&reply_buf[2], 0, 7);
        return;
    }
    
    reply_buf[1] = 0x00;  // Success
    // UID is first 7 bytes of figure data
    std::memcpy(&reply_buf[2], figure->data.data(), 7);
}

std::array<u8, 32> InfinityBase::GetStatus() {
    std::lock_guard lock(mutex);
    
    std::array<u8, 32> response{};
    
    response[0] = 0x53;  // Status response
    response[5] = interrupt_counter++;
    response[6] = activated ? 0x01 : 0x00;
    
    // Add figure presence info
    u8 presence = 0;
    for (size_t i = 0; i < figures.size(); i++) {
        if (figures[i].present) {
            presence |= (1 << i);
        }
    }
    response[1] = presence;
    
    return response;
}

InfinityFigure* InfinityBase::GetFigureByOrder(u8 order) {
    // Note: caller should hold mutex if needed
    for (auto& figure : figures) {
        if (figure.present && figure.order == order) {
            return &figure;
        }
    }
    return nullptr;
}

InfinityFigure* InfinityBase::GetFigureByPad(u8 pad) {
    // Note: caller should hold mutex if needed
    size_t index = (pad == 0x01) ? 0 : 1;
    if (index >= figures.size()) {
        return nullptr;
    }
    return &figures[index];
}

u8 InfinityBase::GetPadFromOrder(u8 order) const {
    std::lock_guard lock(mutex);
    for (size_t i = 0; i < figures.size(); i++) {
        if (figures[i].present && figures[i].order == order) {
            return (i == 0) ? 0x01 : 0x02;
        }
    }
    return 0x00;
}

void InfinityBase::Reset() {
    std::lock_guard lock(mutex);
    
    for (auto& figure : figures) {
        figure.present = false;
        figure.data.fill(0);
        figure.order = 0;
        figure.queued_status = 0;
        figure.status_pending = false;
    }
    
    current_order = 0;
    activated = false;
}

void InfinityBase::RecalculateOrders() {
    u8 new_order = 0;
    for (auto& figure : figures) {
        if (figure.present) {
            figure.order = new_order++;
        }
    }
    current_order = new_order;
}

// ============================================================================
// IRInfinityBase Implementation (like IRPortal)
// ============================================================================

IRInfinityBase::IRInfinityBase(SendFunc send_func) : IRDevice(send_func) {}

IRInfinityBase::~IRInfinityBase() {
    OnDisconnect();
}

void IRInfinityBase::OnConnect() {
    LOG_INFO(Service_IR, "IRInfinityBase connected");
}

void IRInfinityBase::OnDisconnect() {
    LOG_INFO(Service_IR, "IRInfinityBase disconnected");
}

void IRInfinityBase::OnReceive(std::span<const u8> data) {
    HandlePacket(data);
}

void IRInfinityBase::HandlePacket(std::span<const u8> data) {
    // Disney Infinity packet format (NO ir:USER header):
    // [0xFF] [Length] [Command] [Counter] [Data...] [Checksum]
    // 
    // Offsets:
    // data[0] = 0xFF (Disney prefix)
    // data[1] = Length of payload
    // data[2] = Command
    // data[3] = Counter
    // data[4+] = Command-specific data
    
    if (data.size() < 4) {
        LOG_WARNING(Service_IR, "Infinity packet too short: {} bytes", data.size());
        return;
    }
    
    if (data[0] != 0xFF) {
        LOG_WARNING(Service_IR, "Infinity packet missing 0xFF prefix: {:02X}", data[0]);
        return;
    }
    
    const u8 length = data[1];
    const u8 command = data[2];
    last_counter = data[3];
    
    LOG_INFO(Service_IR, "Infinity cmd: {:02X}, counter: {:02X}, length: {}", 
             command, last_counter, length);
    
    switch (static_cast<InfinityCommand>(command)) {
    case InfinityCommand::Handshake:
        HandleHandshake(data);
        break;
    case InfinityCommand::DeviceChallenge:
        HandleDeviceChallenge(data);
        break;
    case InfinityCommand::ChallengeResponse:
        HandleChallengeResponse(data);
        break;
    case InfinityCommand::Disconnect:
        HandleDisconnect(data);
        break;
    case InfinityCommand::BatteryRequest:
        HandleBatteryRequest(data);
        break;
    case InfinityCommand::LedColor:
        HandleLedColor(data);
        break;
    case InfinityCommand::ConnectionAlive:
        HandleConnectionAlive(data);
        break;
    case InfinityCommand::LedFade:
        HandleLedFade(data);
        break;
    case InfinityCommand::LedPulse:
        HandleLedPulse(data);
        break;
    case InfinityCommand::LedStrobe:
        HandleLedStrobe(data);
        break;
    case InfinityCommand::LedAll:
        HandleLedAll(data);
        break;
    case InfinityCommand::NfcPosition:
        HandleNfcPosition(data);
        break;
    case InfinityCommand::ReadNfc:
        HandleReadNfc(data);
        break;
    case InfinityCommand::WriteNfc:
        HandleWriteNfc(data);
        break;
    case InfinityCommand::UidRequest:
        HandleUidRequest(data);
        break;
    case InfinityCommand::Status:
        HandleStatus(data);
        break;
    default:
        LOG_WARNING(Service_IR, "Unknown Infinity command: {:02X}", command);
        Send(BuildResponse({last_counter}));
        break;
    }
}

void IRInfinityBase::HandleHandshake(std::span<const u8> data) {
    // Handshake packet: [0xFF] [0x11] [0x80] [Counter] ["(c) Disney 2013"...] [Checksum]
    LOG_INFO(Service_IR, "Infinity: Handshake received");
    
    g_infinity_base.Activate();
    
    // Response format for 3DS Disney Infinity base
    // Based on real hardware captures
    std::vector<u8> payload = {
        last_counter,
        0x00,           // Status OK
        0x09, 0x87, 0x04, 0x32,  // Base identifier bytes
        0x01, 0x00,     // Version info
        0x02, 0x08, 0x00, 0x00,
        0x0F, 0x40, 0x00, 0x07,
        0x5D
    };
    
    Send(BuildResponse(payload));
}

void IRInfinityBase::HandleDeviceChallenge(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: Device Challenge");
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleChallengeResponse(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: Challenge Response");
    
    std::vector<u8> payload = {last_counter};
    for (int i = 0; i < 8; i++) {
        payload.push_back(0x00);
    }
    
    Send(BuildResponse(payload));
}

void IRInfinityBase::HandleDisconnect(std::span<const u8> data) {
    LOG_INFO(Service_IR, "Infinity: Disconnect");
    g_infinity_base.Deactivate();
}

void IRInfinityBase::HandleBatteryRequest(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: Battery Request");
    Send(BuildResponse({last_counter, battery_level}));
}

void IRInfinityBase::HandleLedColor(std::span<const u8> data) {
    // Format: [0xFF] [len] [0x90] [Counter] [Position] [R] [G] [B]
    // Offsets:  0      1     2       3          4       5   6   7
    if (data.size() >= 8) {
        LOG_DEBUG(Service_IR, "Infinity: LED Color pos={:02X} RGB=({},{},{})", 
                 data[4], data[5], data[6], data[7]);
    }
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleConnectionAlive(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: Connection Alive");
    
    // Check for pending figure notifications and send them FIRST
    // This is how the game learns about newly placed figures
    for (size_t i = 0; i < 2; i++) {
        u8 pad = (i == 0) ? 0x01 : 0x02;
        auto* fig = g_infinity_base.GetFigureByPad(pad);
        if (fig && fig->status_pending) {
            // Send figure added/removed notification
            auto notification = BuildFigureNotification(pad, fig->order, fig->queued_status);
            Send(notification);
            fig->status_pending = false;
            LOG_INFO(Service_IR, "Sent figure notification: pad={:02X}, order={}, status={}", 
                     pad, fig->order, fig->queued_status);
            // Only send one notification per ConnectionAlive to avoid flooding
            return;
        }
    }
    
    // Normal response if no pending notifications
    u8 random_byte = 0x42;
    Send(BuildResponse({last_counter, random_byte, random_byte, random_byte}));
}

void IRInfinityBase::HandleLedFade(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: LED Fade");
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleLedPulse(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: LED Pulse");
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleLedStrobe(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: LED Strobe");
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleLedAll(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: LED All");
    Send(BuildResponse({last_counter}));
}

void IRInfinityBase::HandleNfcPosition(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: NFC Position Request");
    
    // Response: [Counter] [Figure positions...]
    std::vector<u8> payload = {last_counter};
    
    // Report each pad's figure order, or 0xFF if empty
    for (size_t i = 0; i < 2; i++) {
        u8 pad = (i == 0) ? 0x01 : 0x02;
        auto* fig = g_infinity_base.GetFigureByPad(pad);
        if (fig && fig->present) {
            payload.push_back(fig->order);
        } else {
            payload.push_back(0xFF);  // No figure
        }
    }
    
    Send(BuildResponse(payload));
}

void IRInfinityBase::HandleReadNfc(std::span<const u8> data) {
    // Format: [0xFF] [len] [0xA2] [Counter] [Order] [Sector] [Block]
    // Offsets:  0      1     2       3         4       5        6
    if (data.size() < 7) {
        Send(BuildResponse({last_counter, 0x80}));
        return;
    }
    
    const u8 order = data[4];
    const u8 sector = data[5];
    const u8 block = data[6];
    
    LOG_DEBUG(Service_IR, "Infinity: Read NFC order={} sector={} block={}", order, sector, block);
    
    std::array<u8, 20> reply{};
    g_infinity_base.QueryBlock(order, sector, block, reply.data());
    
    std::vector<u8> payload = {last_counter, reply[1]};  // Counter, status
    if (reply[1] == 0x00) {
        // Success - add the 16 bytes of data
        for (int i = 0; i < 16; i++) {
            payload.push_back(reply[2 + i]);
        }
    }
    
    Send(BuildResponse(payload));
}

void IRInfinityBase::HandleWriteNfc(std::span<const u8> data) {
    // Format: [0xFF] [len] [0xA3] [Counter] [Order] [Sector] [Block] [16 bytes data]
    // Offsets:  0      1     2       3         4       5        6       7-22
    if (data.size() < 23) {
        Send(BuildResponse({last_counter, 0x80}));
        return;
    }
    
    const u8 order = data[4];
    const u8 sector = data[5];
    const u8 block = data[6];
    
    LOG_DEBUG(Service_IR, "Infinity: Write NFC order={} sector={} block={}", order, sector, block);
    
    std::array<u8, 4> reply{};
    g_infinity_base.WriteBlock(order, sector, block, &data[7], reply.data());
    
    Send(BuildResponse({last_counter, reply[1]}));
}

void IRInfinityBase::HandleUidRequest(std::span<const u8> data) {
    // Format: [0xFF] [len] [0xB4] [Counter] [Order]
    // Offsets:  0      1     2       3         4
    if (data.size() < 5) {
        Send(BuildResponse({last_counter, 0x80, 0, 0, 0, 0, 0, 0, 0}));
        return;
    }
    
    const u8 order = data[4];
    
    LOG_DEBUG(Service_IR, "Infinity: UID Request order={}", order);
    
    std::array<u8, 10> reply{};
    g_infinity_base.GetUid(order, reply.data());
    
    std::vector<u8> payload = {last_counter, reply[1]};
    for (int i = 0; i < 7; i++) {
        payload.push_back(reply[2 + i]);
    }
    
    Send(BuildResponse(payload));
}

void IRInfinityBase::HandleStatus(std::span<const u8> data) {
    LOG_DEBUG(Service_IR, "Infinity: Status");
    Send(BuildResponse({last_counter}));
}

std::vector<u8> IRInfinityBase::BuildResponse(const std::vector<u8>& payload) {
    // Disney Layer 2 response: [0xAA] [Length] [Payload...] [Checksum] [Padding to 32]
    std::vector<u8> response;
    response.push_back(0xAA);
    response.push_back(static_cast<u8>(payload.size()));
    
    for (u8 byte : payload) {
        response.push_back(byte);
    }
    
    // Calculate checksum (sum of all bytes)
    u8 checksum = 0;
    for (u8 byte : response) {
        checksum += byte;
    }
    response.push_back(checksum);
    
    // Pad to 32 bytes
    while (response.size() < 32) {
        response.push_back(0x00);
    }
    
    return response;
}

std::vector<u8> IRInfinityBase::BuildFigureNotification(u8 position, u8 order, u8 status) {
    // 0xAB packet for figure add/remove notifications
    // Format: [0xAB] [0x04] [Position] [0x09] [Order] [Status] [Checksum] [Padding]
    std::vector<u8> response;
    response.push_back(0xAB);
    response.push_back(0x04);      // Length
    response.push_back(position);  // 0x01 = hexagon, 0x02 = round
    response.push_back(0x09);      // Tag type constant
    response.push_back(order);     // Order number
    response.push_back(status);    // 0x00 = added, 0x01 = removed
    
    u8 checksum = 0;
    for (u8 byte : response) {
        checksum += byte;
    }
    response.push_back(checksum);
    
    while (response.size() < 32) {
        response.push_back(0x00);
    }
    
    return response;
}

} // namespace Service::IR
