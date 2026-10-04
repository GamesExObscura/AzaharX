// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <fmt/format.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/ir/ir_portal.h"
#include "core/memory.h"
#include "core/movie.h"

namespace Service::IR {

namespace {
// Returns true if the running title is SSA (USA / EUR / JPN). Used to gate
// SSA-specific portal protocol differences so Giants/TT/SC/I/Disney
// Infinity get exactly the stock behavior they always had.
bool IsRunningSSA() {
    auto& system = Core::System::GetInstance();
    auto process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset) return false;
    const u64 title_id = process->codeset->program_id;
    return title_id == 0x0004000000036E00ULL || // USA (CONFIRMED)
           title_id == 0x0004000000036F00ULL || // EUR candidate
           title_id == 0x0004000000037000ULL;   // JPN candidate
}

// SSA-only: inject the full state that real hardware reaches AFTER its
// natural code processes the first Q response cycle. Lets SSA's natural
// portal-manager code drive the figure read from there.
//
// What real HW does (verified via GDB watchpoint on real 3DS, slot+0x67
// write site captured at PC 0x001f915c inside FUN_001f90c8):
//
//   1. 'S' response shows figure presence
//   2. 'S' processor's LAB_001fd660 sets slot+0x65 = 1, slot+0x66 = 1
//      (slot+0x67 stays 0 on first detection because of cmp r10, #0 gate)
//   3. FUN_001f90c8 is called (via 0x1fb35c → 0x1fb494). It validates
//      slot+0x64 ∈ {1, 3}, slot+0x65 != 0, slot+0x67 ∉ {1, 2}, then writes:
//        slot+0x68 = block offset / 16
//        slot+0x6C = block count
//        slot+0x70 = data buffer pointer
//        slot+0x67 = 1 (Q-direct mode)
//   4. State machine transitions state[1c] = 15 (Q response wait)
//   5. SSA sends 'Q' query, gets response, FUN_001f8e0c processes (requires
//      slot+0x67 == 1), block data lands in slot+0x80 + block*16
//   6. Cycle continues until all blocks read
//
// On Azahar, steps 3-4 never happen because the broadcast-thread code
// path that triggers FUN_001f90c8 isn't being exercised (some upstream
// event/SVC/service interaction we don't yet emulate). The simple,
// reliable fix is to perform step 3-4's writes ourselves at figure-add
// time, then let SSA's natural code take over from step 5.
//
// Fields written here exactly mirror what FUN_001f90c8 would write on
// real HW PLUS the state[1c]/state[20]/state[24]/state[48] header fields
// that the natural code path sets in adjacent functions:
//
//   slot+0x64 = 1   figure type indicator (1 = regular Skylander chip)
//   slot+0x65 = 1   ready flag (LAB_001fd660 sets this on real HW too,
//                   but Azahar's 'S' processor path doesn't reliably hit
//                   it — we set it ourselves to remove any ambiguity)
//   slot+0x67 = 1   Q-direct active state (the value FUN_001fd2dc and
//                   FUN_001f8e0c both gate on with `cmp ?, #1`)
//   slot+0x68 = 0   current block cursor (start at block 0)
//   slot+0x6C = 16  blocks to read (Mifare 1K has 16 readable blocks)
//   state[1c] = 15  Q response wait state (case 0x0f dispatches to the
//                   Q handler at 0x001fdb04 → 0x001fdc48 → FUN_001f8e0c)
//   state[20] = slot_index (slot iterator)
//   state[24] = 0   current block number (matches slot+0x68)
//   state[48] = 2   wireless mode flag (set during IR init on real HW —
//                   real HW snapshot showed state[48]=2 vs Azahar's 0)
//
// The dispatch_ptr (= FUN_001fd470's param_1 / state base) is reached via
// the static pointer chain SSA installs at boot:
//   parent_ptr   = *(u32*)0x0053C340
//   dispatch_ptr = *(u32*)(parent_ptr + 0x18)
// Both real HW and Azahar use dispatch_ptr = 0x09778ff4 — confirmed by
// snapshot comparison on the user's 3DS.
void InjectSSASlotState(u8 slot_index, const u8* figure_data, std::size_t figure_data_size) {
    if (!IsRunningSSA()) return;
    if (slot_index >= 16) return; // SSA's dispatch has 16 slots

    auto& system = Core::System::GetInstance();
    auto& mem = system.Memory();

    constexpr VAddr kParentPtrVA = 0x0053C340;
    const u32 parent_ptr = mem.Read32(kParentPtrVA);
    if (parent_ptr < 0x08000000 || parent_ptr >= 0x40000000) {
        LOG_WARNING(Service_IR,
                    "[SSA-INJECT] parent_ptr=0x{:08x} not in heap range, "
                    "skipping inject for slot {}",
                    parent_ptr, slot_index);
        return;
    }

    const u32 dispatch_ptr = mem.Read32(parent_ptr + 0x18);
    if (dispatch_ptr < 0x08000000 || dispatch_ptr >= 0x40000000) {
        LOG_WARNING(Service_IR,
                    "[SSA-INJECT] dispatch_ptr=0x{:08x} not in heap range "
                    "(parent=0x{:08x}), skipping inject for slot {}",
                    dispatch_ptr, parent_ptr, slot_index);
        return;
    }

    const u32 slot_base = dispatch_ptr + static_cast<u32>(slot_index) * 0x41C;

    // Capture pre-inject values so the log line confirms exactly what
    // changed (helpful when comparing against real-HW GDB snapshots).
    const u8 before_64 = mem.Read8(slot_base + 0x64);
    const u8 before_65 = mem.Read8(slot_base + 0x65);
    const u8 before_67 = mem.Read8(slot_base + 0x67);
    const u32 before_6C = mem.Read32(slot_base + 0x6C);
    const u32 before_48 = mem.Read32(dispatch_ptr + 0x48);

    // --- Strategy: present "all reads done" state with figure data ---
    //
    // FUN_001fd2dc returns 0 despite all visible conditions being satisfied
    // (verified via SSA-DIAG: slot+0x67=1, slot+0x6C=16, slot+0x78=0,
    // slot+0x74=0). The actual reason is buried somewhere we can't see
    // without runtime debugging that has proven unstable. Bypass the Q-cycle
    // entirely by presenting the post-read final state directly.
    //
    //   slot+0x64 = 1    figure type indicator
    //   slot+0x65 = 1    ready flag
    //   slot+0x66 = 0
    //   slot+0x67 = 0    idle (= read complete on real HW)
    //   slot+0x68 = 16   cursor past end of blocks
    //   slot+0x6C = 0    no blocks remaining (read done)
    //   slot+0x78 = 0xFFFFFFFF  all bits set (= all blocks marked processed)
    //   slot+0x80..0x17F figure data (16 blocks × 16 bytes = 256 bytes)
    mem.Write8(slot_base + 0x64, 1);
    mem.Write8(slot_base + 0x65, 1);
    mem.Write8(slot_base + 0x66, 0);
    mem.Write8(slot_base + 0x67, 0);    // idle / read done
    mem.Write32(slot_base + 0x68, 16);  // cursor at end
    mem.Write32(slot_base + 0x6C, 0);   // no blocks remaining
    mem.Write32(slot_base + 0x70, 0);
    mem.Write32(slot_base + 0x74, 7);   // GDB capture showed real HW = 7 at FUN_001fd2dc entry
    mem.Write32(slot_base + 0x78, 0);   // ★ real HW = 0 at FUN_001fd2dc entry — bitmap walk needs clear bits
    mem.Write32(slot_base + 0x7C, 0);   // default

    // Populate slot+0x80 with figure data (the buffer where Q responses
    // accumulate on real HW). Mifare 1K has 64 blocks × 16 bytes = 1024
    // bytes total. The slot in SSA's dispatch struct allocates 0x400
    // (= 1024) bytes at +0x80, so we can safely write the entire figure.
    //
    // First test with 256 bytes (16 blocks) got SSA to accept the figure
    // and start the load countdown — but the character didn't appear in
    // game because SSA tried to read blocks past 16 (snapshot showed
    // slot+0x68 advancing to 36). Skylander figures use blocks beyond
    // 16 for character profile (XP, hats, unlocks, save data).
    const std::size_t copy_size = std::min<std::size_t>(figure_data_size, 1024);
    if (figure_data && copy_size > 0) {
        for (std::size_t i = 0; i < copy_size; ++i) {
            mem.Write8(slot_base + 0x80 + static_cast<u32>(i), figure_data[i]);
        }
    }

    // --- LOG THE FIGURE DATA FOR INSPECTION ---
    //
    // Skylander figure format (from Brandon Wilson's RE):
    //   Block 0 (bytes 0..15):  UID, manufacturer, product info
    //   Block 1 (bytes 16..31): Character ID + variant + other metadata
    //     bytes 16-17 (LE): Character/Toy ID
    //                        SSA characters: 0..31  (Spyro=0, Gill Grunt=6, etc.)
    //                        Giants:        100..199
    //                        SuperChargers: 1000+
    //     bytes 18-19 (LE): Variant ID
    //     other bytes: misc metadata
    //
    // If the logged character ID is outside SSA's range (0-31), the figure
    // is NOT compatible with SSA — that's the "wrong toy" error source.
    // If it IS in range, SSA's validation is doing more than ID checking
    // (probably crypto signature verification on the chip data).
    if (figure_data && figure_data_size >= 32) {
        const u16 char_id = static_cast<u16>(figure_data[16]) |
                            (static_cast<u16>(figure_data[17]) << 8);
        const u16 variant = static_cast<u16>(figure_data[18]) |
                            (static_cast<u16>(figure_data[19]) << 8);
        LOG_DEBUG(Service_IR,
                  "[SSA-FIGURE] char_id={} variant={}",
                  char_id, variant);
    }

    // --- Wireless-mode state flag ---
    mem.Write32(dispatch_ptr + 0x48, 2);

    // --- state[24] = 0 so PATCH #5 (NOP at 0x1fd418) preserves a valid
    // block number. Without this, state[24] = -1 from FUN_001fe480 init,
    // and the Q query SSA emits would request block 0xFF.
    mem.Write32(dispatch_ptr + 0x24, 0);

    // --- Reset state[3c] timer ---
    //
    // state[3c] is the pre-switch timer that gates the natural state
    // transition state[1c] = 12 → 16 (which kicks the state machine into
    // the figure-read cycle). On real HW it's decremented by the broadcast
    // thread or a timer ISR until it reaches 0, at which point FUN_001fdfc4's
    // pre-switch fires and state advances.
    //
    // SSA-DIAG captures show state[3c] = 528000 STABLE on Azahar (= 528ms,
    // the cfg_poll period × 1000). Nothing on the emulator side decrements
    // it, so the state machine sits at state[1c] = 11 forever, never
    // generating Q queries even though slot+0x67 = 1.
    //
    // Forcing state[3c] = 0 here lets the next FUN_001fdfc4 call see the
    // timer expired, fire the pre-switch, and progress.
    mem.Write32(dispatch_ptr + 0x3C, 0);

    LOG_DEBUG(Service_IR,
              "[SSA-INJECT] slot={} dispatch=0x{:08x}",
              slot_index, dispatch_ptr);
    // Silence the "before" capture-warnings since the corresponding INFO log
    // was downgraded to DEBUG and they're now unused outside that path.
    (void)before_64;
    (void)before_65;
    (void)before_67;
    (void)before_6C;
    (void)before_48;
    (void)slot_base;

}
} // namespace
} // namespace Service::IR

namespace Service::IR {

SkylanderPortal g_skyportal;

IRPortal::IRPortal(SendFunc send_func) : IRDevice(send_func) {}

IRPortal::~IRPortal() {
    OnDisconnect();
}

void IRPortal::OnConnect() {}

void IRPortal::OnDisconnect() {}

void IRPortal::OnReceive(std::span<const u8> data) {
    HandlePortalCommand(data);
}

void IRPortal::HandlePortalCommand(std::span<const u8> data) {
    // Data to be queued to be sent back via the Interrupt Transfer (if needed)
    std::array<u8, 32> response = {};

    // The first byte of the Control Request is always a char for Skylanders (offset by 3 for
    // infrared commands)
    switch (data[3]) {
    case 'A': {
        response = {0x41, data[4], 0xFF, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00,    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00,    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (data[4] == 0x01) {
            g_skyportal.Activate();
        }
        // 'A 00' behavior is title-gated.
        //
        // For Giants / Trap Team / SuperChargers / Imaginators / Disney
        // Infinity (the games that have always worked): treat 'A 00' as
        // Deactivate exactly like before. Those games rely on this Deactivate
        // to clear figure ADDED bits and reset their portal state machine
        // between cycles. Removing it breaks them.
        //
        // For SSA only: don't Deactivate on 'A 00'. SSA's broadcast-thread
        // parser FUN_001fd470 'S' handler checks `(param_2[6] & 1) == 0`
        // (= active flag). SSA's natural handshake cycles A 01 -> A 00 -> S,
        // so if we Deactivate on A 00, the very next 'S' response has
        // byte[6] = 0 and FUN_001fd470 early-returns, never processing the
        // figure bitmap. Skipping Deactivate keeps m_activated = 1, byte[6]
        // = 1, and figure detection proceeds.
        if (data[4] == 0x00 && !IsRunningSSA()) {
            g_skyportal.Deactivate();
        }
        break;
    }
    case 'C': {
        g_skyportal.SetLEDs(0x01, data[4], data[5], data[6]);
        response = g_skyportal.GetStatus();
        break;
    }
    case 'J': {
        response = {data[3]};
        g_skyportal.SetLEDs(data[4], data[5], data[6], data[7]);
        break;
    }
    case 'L': {
        u8 side = data[4];
        if (side == 0x02) {
            side = 0x04;
        }
        g_skyportal.SetLEDs(side, data[5], data[6], data[7]);
        break;
    }
    case 'M': {
        response = {data[3], data[4], 0x00, 0x19};
        break;
    }
    case 'Q': {
        const u8 sky_num = data[4] & 0xF;
        const u8 block = data[5];
        g_skyportal.QueryBlock(sky_num, block, response.data());
        break;
    }
    case 'R': {
        // Firmware version bytes [1..2] = major.minor.
        //   0x02 0x02 = v2.2 (matches user's Disney Portal source — works for
        //   DI, SF handshake, and all other games tested).
        //   Previous value 0x01 0x34 (v1.52) satisfied SSA's >=0x1F check but
        //   was below what SF expects. v2.2 satisfies SSA (still >=0x1F),
        //   matches the user's working DI build, and gets SF past the initial
        //   R-response gate. SF still rejects later (downstream issue, not
        //   the firmware version), but no game is harmed by the upgrade.
        response = {0x52, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        break;
    }
    case 'S': {
        response = g_skyportal.GetStatus();
        break;
    }
    case 'V': {
        response = g_skyportal.GetStatus();
        break;
    }
    case 'W': {
        const u8 sky_num = data[4] & 0xF;
        const u8 block = data[5];
        g_skyportal.WriteBlock(sky_num, block, &data[6], response.data());
        break;
    }
    default:
        LOG_ERROR(Service_IR, "Unhandled Skylander Portal Query: {}", data[3]);
        break;
    }

    Send(response);
}

void Skylander::Save() {
    if (!sky_file) {
        LOG_ERROR(Service_IR, "Tried to save a Skylander but no file was open");
        return;
    }
    sky_file.Seek(0, SEEK_SET);
    sky_file.WriteBytes(data.data(), 0x40 * 0x10);
    sky_file.Close();
}

bool SkylanderPortal::IsActivated() {
    return m_activated;
}

void SkylanderPortal::Activate() {
    std::lock_guard lock(sky_mutex);
    if (m_activated) {
        // If the portal was already active no change is needed
        return;
    }

    // If not we need to advertise change to all the figures present on the portal
    for (auto& s : skylanders) {
        if (s.status & 1) {
            s.queued_status.push(Skylander::ADDED);
            s.queued_status.push(Skylander::READY);
        }
    }

    m_activated = true;
}

void SkylanderPortal::Deactivate() {
    std::lock_guard lock(sky_mutex);

    for (auto& s : skylanders) {
        // check if at the end of the updates there would be a figure on the portal
        if (!s.queued_status.empty()) {
            s.status = s.queued_status.back();
            s.queued_status = std::queue<u8>();
        }

        s.status &= 1;
    }

    m_activated = false;
}

void SkylanderPortal::SetLEDs(u8 side, u8 red, u8 green, u8 blue) {
    std::lock_guard lock(sky_mutex);
    if (side == 0x00) {
        m_color_right.red = red;
        m_color_right.green = green;
        m_color_right.blue = blue;
    } else if (side == 0x01) {
        m_color_right.red = red;
        m_color_right.green = green;
        m_color_right.blue = blue;

        m_color_left.red = red;
        m_color_left.green = green;
        m_color_left.blue = blue;
    } else if (side == 0x02) {
        m_color_left.red = red;
        m_color_left.green = green;
        m_color_left.blue = blue;
    } else if (side == 0x03) {
        m_color_trap.red = red;
        m_color_trap.green = green;
        m_color_trap.blue = blue;
    }
}

std::array<u8, 32> SkylanderPortal::GetStatus() {
    std::lock_guard lock(sky_mutex);

    u32 status = 0;
    u8 active = 0x00;

    if (m_activated) {
        active = 0x01;
    }

    for (int i = MAX_SKYLANDERS - 1; i >= 0; i--) {
        auto& s = skylanders[i];

        if (!s.queued_status.empty()) {
            s.status = s.queued_status.front();
            s.queued_status.pop();
        }
        status <<= 2;
        status |= s.status;
    }

    std::array<u8, 32> response = {0x53,   0x00, 0x00, 0x00, 0x00, m_interrupt_counter++,
                                   active, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00,   0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00,   0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00,   0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00,   0x00};
    memcpy(&response[1], &status, sizeof(status));
    return response;
}

void SkylanderPortal::QueryBlock(u8 sky_num, u8 block, u8* reply_buf) {
    if (!IsSkylanderNumberValid(sky_num) || !IsBlockNumberValid(block))
        return;

    std::lock_guard lock(sky_mutex);

    const auto& skylander = skylanders[sky_num];

    reply_buf[0] = 'Q';
    reply_buf[2] = block;
    if (skylander.status & Skylander::READY) {
        reply_buf[1] = (0x10 | sky_num);
        memcpy(&reply_buf[3], skylander.data.data() + (block * 0x10), 0x10);
    } else {
        reply_buf[1] = 0x01;
    }
}

void SkylanderPortal::WriteBlock(u8 sky_num, u8 block, const u8* to_write_buf, u8* reply_buf) {
    if (!IsSkylanderNumberValid(sky_num) || !IsBlockNumberValid(block))
        return;

    std::lock_guard lock(sky_mutex);

    auto& skylander = skylanders[sky_num];

    reply_buf[0] = 'W';
    reply_buf[2] = block;

    if (skylander.status & 1) {
        reply_buf[1] = (0x10 | sky_num);
        memcpy(skylander.data.data() + (block * 0x10), to_write_buf, 0x10);
        skylander.Save();
    } else {
        reply_buf[1] = 0x01;
    }
}

bool SkylanderPortal::RemoveSkylander(u8 sky_num) {
    if (!IsSkylanderNumberValid(sky_num))
        return false;

    LOG_DEBUG(Service_IR, "Cleared Skylander from slot {}", sky_num);
    std::lock_guard lock(sky_mutex);
    auto& skylander = skylanders[sky_num];

    skylander.Save();

    if (skylander.status & Skylander::READY) {
        skylander.status = Skylander::REMOVING;
        skylander.queued_status.push(Skylander::REMOVING);
        skylander.queued_status.push(Skylander::REMOVED);
        return true;
    }

    return false;
}

u8 SkylanderPortal::LoadSkylander(u8* buf, FileUtil::IOFile in_file) {
    std::lock_guard lock(sky_mutex);

    u32 sky_serial = 0;
    for (int i = 3; i > -1; i--) {
        sky_serial <<= 8;
        sky_serial |= buf[i];
    }
    u8 found_slot = 0xFF;

    // mimics spot retaining on the portal
    for (u8 i = 0; i < 8; i++) {
        if ((skylanders[i].status & 1) == 0) {
            if (skylanders[i].last_id == sky_serial) {
                found_slot = i;
                break;
            }

            if (i < found_slot) {
                found_slot = i;
            }
        }
    }

    if (found_slot != 0xff) {
        Skylander& thesky = skylanders[found_slot];
        memcpy(thesky.data.data(), buf, thesky.data.size());
        thesky.sky_file = std::move(in_file);
        thesky.last_id = sky_serial;

        if (IsRunningSSA()) {
            // SSA-specific: skip the ADDED→READY transition. Go straight to
            // READY so the 'S' bitmap stays steady at READY (0x01) on every
            // poll. Real HW behaves this way — the portal hardware sees the
            // figure and reports it as READY immediately. The two-step
            // ADDED→READY transition on Azahar causes bitmap 0→3→1, which
            // triggers SSA's 'S' processor LAB_001fd660 (only runs when
            // bitmap changes) and overwrites our injected slot+0x67=1 back
            // to 3 via the strbne at 0x1fd6b0.
            thesky.status = 1;
            // No queued_status pushes — current status stays at 1.

            // Prime the per-slot state machine AND populate slot+0x80 with
            // the figure data so SSA's game-layer code sees a complete
            // post-read state. See InjectSSASlotState() for details.
            InjectSSASlotState(found_slot, thesky.data.data(), thesky.data.size());
        } else {
            // All other Skylanders games (Giants/TT/SC/SW) — preserve the
            // original ADDED→READY flow that they expect.
            thesky.status = 3;
            thesky.queued_status.push(3);
            thesky.queued_status.push(1);
        }
    }

    return found_slot;
}

bool SkylanderPortal::IsSkylanderNumberValid(u8 sky_num) {
    return sky_num < MAX_SKYLANDERS;
}

bool SkylanderPortal::IsBlockNumberValid(u8 block) {
    return block < 64;
}

} // namespace Service::IR