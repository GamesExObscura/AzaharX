// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <string>
#include <vector>
#include <boost/crc.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include <fmt/ranges.h>
#include "common/archives.h"
#include "common/swap.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/ir/extra_hid.h"
#include "core/hle/service/ir/ir_portal.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/memory.h"

// IRInfinityBase forward declared in your ir_user.h. Include the real header here so
// the unique_ptr destructor and method calls compile. If your project uses a different
// path, adjust this one line.
// NOTE: If your build fails on this include, comment it out and remove the
// IRInfinityBase references at the bottom of the constructor / destructor.
#include "core/hle/service/ir/ir_infinity_base.h"

SERIALIZE_EXPORT_IMPL(Service::IR::IR_USER)
SERVICE_CONSTRUCT_IMPL(Service::IR::IR_USER)

namespace Service::IR {

// --- SSA-DIAG: dispatch-object state dumper ------------------------------
// Captured at IR_USER construction so SendIrNop can reach into guest memory
// and log [r4+0x1C] (dispatch state) without changing the class header.
//
// Pointer chain (from real-hardware Rosalina / GDB capture):
//   fixed VA 0x080B305C  ->  base ptr
//                   +0x18 ->  dispatch object ptr (heap)
//   dispatch_object + 0x1C   = state field (u32)
//   dispatch_object + 0x40   = last-observed state (u32)
//   dispatch_object + 0x44   = poll counter (u32)
//
// If the heap address shifts each run that's fine; we re-resolve the chain
// every call.
namespace {
Core::System* g_ssa_diag_system = nullptr;
// SSA Binary Patcher
// ==================
//
// On real hardware, when an IR portal packet arrives the IR controller
// DMA's the bytes into the receive buffer and fires an IRQ. That IRQ ends
// up invoking SSA's broadcast chain which calls FUN_001fd470 (the response
// parser), which advances the portal state machine.
//
// Azahar doesn't emulate the IR controller hardware or its IRQ, so the
// broadcast chain never fires and FUN_001fd470 never runs. SSA's polling
// thread keeps re-sending the same query forever.
//
// The fix: at load time, patch SSA's code.bin in memory so that its OWN
// polling thread invokes FUN_001fd470 after each IR receive — bypassing
// the missing IRQ-driven broadcast trigger. Patches run in SSA's natural
// thread context with correct stack/locks (this is why a pure emulator-
// side PC redirect doesn't work: wrong thread, wrong locks, wrong stack).
//
// All patches are title-ID gated to SSA (USA / EUR / JPN), and each patch
// entry includes the ORIGINAL bytes — if they don't match (different game
// version, different region, modded binary), we refuse to apply. Zero
// regression risk for other titles.
//
// (No `namespace {` opener here — we are already inside the SSADiag
// anonymous namespace opened above.)

struct SSAPatchEntry {
    VAddr address;              // where to write
    std::vector<u8> original;   // expected current bytes (safety match)
    std::vector<u8> replacement;// what to write
    const char* description;
};

// Known SSA title IDs across regions. Title ID gating means anything not
// in this list never gets patched. Confirmed entries are noted with the
// source of confirmation; unconfirmed entries are guesses based on
// neighboring title-ID conventions and are safe (worst case: no patch
// applied, title gate filters out).
//
// User confirmed running USA SSA on new3DS. The exact USA title ID will
// be verified from the next GDB session (`x/2wx 0x1FF80000`) — if our
// guess is wrong, the patcher will log title=0x... and "no SSA match",
// and we'll add the real ID.
constexpr std::array<u64, 3> kSSATitleIds = {
    0x0004000000036E00ULL, // USA (CONFIRMED via runtime log)
    0x0004000000036F00ULL, // EUR candidate (untested)
    0x0004000000037000ULL, // JPN candidate (untested)
};

// Packet-content router helpers. Matches D:\...\Disney Portal\src\...\ir_user.cpp
// (the user's working DI build). Used by SendIrNop to dispatch each outbound
// packet to the right device class based on its byte signature, NOT by the
// connected_portal / connected_infinity_base flags (since AutoConnection sets
// both for any game that calls it).
//
// Disney Infinity Layer 2 packets: byte[0] = 0xFF, byte[2] = command in
// {0x80, 0x81, 0x83, 0x84, 0x85, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
//  0xA1, 0xA2, 0xA3, 0xB4, 0xB5}.
//
// Skylanders packets (all titles including SSA): byte[3] = ASCII command
// letter in {A, C, J, L, M, Q, R, S, V, W}. SSA's packets are
// "ff ff ff <cmd>" so byte[2] is 0xFF (not in DI command set) and byte[3]
// is the ASCII letter — disambiguated cleanly from DI.
inline bool IsDisneyInfinityPacket(std::span<const u8> data) {
    if (data.size() < 4 || data[0] != 0xFF) return false;
    switch (data[2]) {
    case 0x80: case 0x81: case 0x83: case 0x84: case 0x85:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
    case 0xA1: case 0xA2: case 0xA3:
    case 0xB4: case 0xB5:
        return true;
    default:
        return false;
    }
}

inline bool IsSkylandersPacket(std::span<const u8> data) {
    if (data.size() < 4) return false;
    switch (data[3]) {
    case 'A': case 'C': case 'J': case 'L': case 'M':
    case 'Q': case 'R': case 'S': case 'V': case 'W':
        return true;
    default:
        return false;
    }
}

// Cheap title gate. Called from hot paths (every IR poll), so the body is
// inlined and consists of one process-pointer read + one program_id read
// + 3 compare-and-branch — under 20 ns on a modern x86. Required because
// the SSA diag/patch logic does expensive work (memory scans, mapped buffer
// readbacks, heavy logging) that would otherwise run for ALL games and
// slow Trap Team to crawl + corrupt Swap Force's portal detection.
//
// Use Core::System::GetInstance() (rather than g_ssa_diag_system) so the
// gate works even before SendIrNop has captured the system reference — a
// few early IR commands fire during boot before that capture happens.
inline bool IsSSATitleRunning() {
    auto& system = Core::System::GetInstance();
    auto process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset) return false;
    const u64 tid = process->codeset->program_id;
    for (u64 id : kSSATitleIds) {
        if (tid == id) return true;
    }
    return false;
}

// Patch table. Initially empty — populated once we have raw ARM bytes
// from the user's GDB capture of FUN_00201bec. Each entry describes one
// instruction-level patch to SSA's code.bin in memory.
//
// Patch design (to be filled in):
//   1. Find a free spot in code.bin large enough for ~10 ARM instructions
//      (a trampoline that calls FUN_001fd470).
//   2. Patch the return path of FUN_00201bec (receive dispatcher) to BL
//      into the trampoline before returning to caller.
//   3. Trampoline: resolves dispatch_ptr dynamically (via 0x0053C340),
//      sets up FUN_001fd470's args from saved state, calls FUN_001fd470,
//      restores caller state, returns.
const std::vector<SSAPatchEntry>& GetSSAPatchTable() {
    static const std::vector<SSAPatchEntry> table = {
        // PATCH #1: NOP the vtable[2] call inside FUN_00212480.
        //
        // Disassembly context (verified by GDB capture from user, USA new3DS):
        //   0x212518: mov  r0, #0
        //   0x21251c: strb r0, [r5, #13]    ; clear "data ready" flag (default
        //                                   ; path, when GetConnectionStatus
        //                                   ; status byte is NOT 2)
        //   0x212520: ldr  r0, [r9]         ; r0 = *dispatch_ptr = vtable
        //   0x212524: ldr  r1, [r0, #8]     ; r1 = vtable[2]  (byte offset 8)
        //   0x212528: mov  r0, r9           ; r0 = dispatch_ptr (this)
        //   0x21252c: blx  r1               ; ★ call vtable[2]
        //   0x212530: ldrb r0, [r5, #13]    ; fast path entry — reads flag
        //   0x212534-3c: cmp + beq 0x2129f8 ; if flag == 0, idle
        //
        // The vtable[2] function ("LAB_001fe534" per earlier reverse work)
        // resets internal state INCLUDING r5+13 = 0. So even though the
        // status=2 branch at 0x212644 sets r5+13 = 1 and then branches to
        // 0x212520, vtable[2] immediately clears it again. The flag check
        // at 0x21253c then idles, the `bl 0x201bec` (ReceiveIrnopLarge)
        // never fires from this thread, and FUN_001fd470 is never called.
        //
        // The previous "fast path" hack (returning Result=0x3FE from
        // GetConnectionStatus) tried to skip this whole region by hitting
        // the beq at 0x2124fc — but that ALSO skipped the flag-setting
        // path at 0x212644, so the flag was permanently 0 and the same
        // idle branch was taken.
        //
        // The clean fix: NOP the blx r1 at 0x21252c. The slow path's
        // flag-set at 0x212644 survives, fast path's flag check passes,
        // processing continues to ReceiveIrnopLarge + 'S' byte check +
        // FUN_001fd470 call. State machine advances naturally.
        //
        // Original bytes (ARM little-endian): 31 FF 2F E1  (= 0xE12FFF31 = "blx r1")
        // Replacement (ARM little-endian):    00 F0 20 E3  (= 0xE320F000 = "nop {0}")
        SSAPatchEntry{
            /*.address     =*/ 0x0021252C,
            /*.original    =*/ {0x31, 0xFF, 0x2F, 0xE1},
            /*.replacement =*/ {0x00, 0xF0, 0x20, 0xE3},
            /*.description =*/ "FUN_00212480 0x21252c: NOP blx r1 (skip vtable[2] state reset)",
        },

        // PATCH #2: Preserve InjectSSASlotState's slot+0x67 = 1 across the
        // 'S' processor's LAB_001fd660 path.
        //
        // GDB watchpoint on slot+0x67 on real 3DS confirmed that real HW
        // sets slot+0x67 = 1 (Q-direct mode) when the figure is detected
        // (PC 0x001f915c inside FUN_001f90c8). On Azahar, our inject does
        // the same write at figure-add time. BUT the very next 'S' response
        // triggers FUN_001fd470's LAB_001fd660, which has this code:
        //
        //   0x1fd660: cmp   r10, #0          ; r10 = OLD slot+0x67
        //   0x1fd664: movne r0, #3            ; load 3
        //   0x1fd668: strbne r0, [r6, #3]    ; slot+0x67 = 3
        //
        // Since our inject already set slot+0x67 = 1, the next poll sees
        // r10 = 1 (non-zero), so the strbne fires and overwrites our 1
        // with 3. FUN_001fd2dc's `cmp r2, #1` then fails (sees 3, not 1),
        // returns 0, no Q query is generated.
        //
        // The strbne behavior is "if OLD value was non-zero, mark slot as
        // ADDED (= 3)". On real HW that's correct because real HW's path
        // doesn't go through inject — FUN_001f90c8 sets slot+0x67 = 1
        // AFTER the 'S' processor runs the first time (when r10 was 0 and
        // strbne was skipped). We can't easily replicate that ordering
        // from Azahar's IR thread context, so we patch the value:
        //
        //   movne r0, #3  →  movne r0, #1
        //
        // Now if r10 was 1 (inject) the strbne writes 1 (no change). If
        // r10 was 0 (idle), strbne is skipped (preserves 0).
        //
        // Original (ARM LE, `movne r0, #3`):    03 00 A0 13
        // Replacement (ARM LE, `movne r0, #1`): 01 00 A0 13
        SSAPatchEntry{
            /*.address     =*/ 0x001FD664,
            /*.original    =*/ {0x03, 0x00, 0xA0, 0x13},
            /*.replacement =*/ {0x01, 0x00, 0xA0, 0x13},
            /*.description =*/ "FUN_001fd470 0x1fd664: movne r0,#3 -> movne r0,#1 "
                               "(preserve inject's slot+0x67=1 for natural Q-cycle)",
        },

        // PATCH #3: SECOND overwrite site, post-LAB_001fd660. Same logic
        // as PATCH #2.
        //
        //   0x1fd6a8: movne r0, #3
        //   0x1fd6b0: strbne r0, [r6, #3]
        //
        // Same change: write 1 instead of 3.
        //
        // Original (ARM LE, `movne r0, #3`):    03 00 A0 13
        // Replacement (ARM LE, `movne r0, #1`): 01 00 A0 13
        SSAPatchEntry{
            /*.address     =*/ 0x001FD6A8,
            /*.original    =*/ {0x03, 0x00, 0xA0, 0x13},
            /*.replacement =*/ {0x01, 0x00, 0xA0, 0x13},
            /*.description =*/ "FUN_001fd470 0x1fd6a8: movne r0,#3 -> movne r0,#1 "
                               "(second overwrite site, post-LAB_001fd660)",
        },

        // PATCH #4 and #5 REMOVED after PATCH #6 unblocked case 12.
        //
        // They were workarounds for when state[1c]=12 dispatch was being
        // hijacked by FUN_001fdfc4's pre-switch (PATCH #6 fixed that root
        // cause). With case 12 now reaching FUN_001fd2dc naturally, the
        // function's own slot search and state[24] write are what advances
        // the bitmap walk through blocks 0..15.
        //
        // PATCH #4 was force-picking slot 0 every call (`mov r1,#0`),
        // which made FUN_001fd2dc emit the same Q query repeatedly — log
        // showed 156x `ff ff ff 51` (identical bytes), and slot[68] froze
        // at 1 after block 0 was read. The natural `cmp r1,#0` is now
        // correct because our inject sets slot+0x67=1 (= slot 0 is the
        // Q-mode slot the search is looking for).
        //
        // PATCH #5 was NOPping `str r2, [r0, #36]` to preserve state[24].
        // But state[24] is supposed to hold the CURRENT block index the
        // Q query is asking for; the Q response handler in FUN_001fd470
        // reads it to know which slot+0x80 offset to write the block
        // data into. NOPping it meant block 0's data was repeatedly
        // overwritten by each new (= same) response.
        //
        // Reverting these lets the natural Q-cycle progress:
        //   block 0 read → slot[78] bit 0 set → FUN_001fd2dc finds bit 1
        //   clear → asks for block 1 → block 1 read → bit 1 set → ...
        //   → all 16 blocks read → slot[6c] hits 0 → SSA marks figure
        //   loaded → character appears.

        // PATCH #6: Skip the pre-switch in FUN_001fdfc4 so state[1c]=12
        // actually reaches the case-12 body (which calls FUN_001fd2dc and
        // emits the 'W' read command).
        //
        // SAMPLER PROOF (from log [SSA-SAMPLE] at 200Hz):
        //
        //   state[1c]=11 always paired with state[3c]=528000  (idle wait)
        //   state[1c]=12 always paired with state[3c]=0      (post-S-response)
        //   state[1c]=16 NEVER appears                       (no 'W' emit)
        //   state[24] never leaves 0                         (FUN_001fd2dc unreached)
        //
        // The pre-switch at 0x1fe014–0x1fe034 in FUN_001fdfc4 fires every
        // time state[1c]=12 + state[3c]=0 + state[55]=1, transitioning
        // state[1c]=10 (case-10 re-sends 'S'). Because the natural S-response
        // handler zeroes state[3c] whenever it transitions state[1c] to 12,
        // the pre-switch *always* fires and case 12 body is unreachable.
        //
        // On real HW this pre-switch is a TIMEOUT — restart S send if no
        // response. Our InjectSSASlotState guarantees the case-12 conditions
        // succeed, so the timeout fallback is dead code for us.
        //
        // Patch: 0x1fe014 BNE -> B unconditional. Skips pre-switch every
        // time, falls straight through to case dispatch with state[1c]
        // unchanged. Case 12 then runs FUN_001fd2dc at 0x1fe21c.
        //
        //   ARM encoding:
        //     bne 0x1fe038 = 0x1A000007  (cond NE, opcode B)
        //     b   0x1fe038 = 0xEA000007  (cond AL, opcode B)
        //
        //   Memory bytes (LE):
        //     original    = 07 00 00 1A
        //     replacement = 07 00 00 EA   (single byte +3 changes)
        SSAPatchEntry{
            /*.address     =*/ 0x001FE014,
            /*.original    =*/ {0x07, 0x00, 0x00, 0x1A},
            /*.replacement =*/ {0x07, 0x00, 0x00, 0xEA},
            /*.description =*/ "FUN_001fdfc4 0x1fe014: bne -> b "
                               "(skip pre-switch so state[1c]=12 reaches "
                               "case 12 body and emits 'W' read command)",
        },

        // ============================================================
        // PATCH #2-#6 REMOVED.
        // ============================================================
        //
        // After GDB watchpoint reverse engineering on real 3DS hardware
        // confirmed that the natural SSA portal state machine uses:
        //   slot+0x64 = 1   (figure type)
        //   slot+0x65 = 1   (ready flag)
        //   slot+0x67 = 1   (Q-direct mode — set by FUN_001f90c8)
        //   state[1c] = 15  (Q response wait state)
        //   state[48] = 2   (wireless mode flag set during IR init)
        //
        // ...the earlier patches chasing slot+0x67=2/3 were all symptoms.
        // Real HW transitions slot+0x67 0 → 1 directly (caught at PC
        // 0x001f915c in the writer function 0x001f90c8, LR 0x001fb49c).
        //
        // The proper fix is to inject the correct state into SSA's
        // dispatch struct at figure-add time (see InjectSSASlotState in
        // ir_portal.cpp) and let SSA's natural code drive the read cycle.
        // No binary patches in the response-processing path are needed —
        // the response handlers FUN_001fd2dc (cmp r2, #1) and FUN_001f8e0c
        // (cmp r0, #1) BOTH accept the natural value of 1.
        //
        // PATCH #1 (the vtable NOP at 0x21252c) is kept because it
        // addresses a different problem: a function call that resets
        // SSA's broadcast-thread data-ready flag, which Azahar doesn't
        // emulate correctly. Skipping it lets the broadcast thread make
        // progress past the boot handshake.
        // ============================================================

        // (Sentinel block — intentionally empty. Future patches can be
        // added here following the same SSAPatchEntry format.)
    };
    return table;
}

// Tracks the last title ID we logged the check for, so we don't spam the
// log every IR call with "ssa_match=yes/no". This is purely cosmetic —
// the actual title check still runs every call so title switches in the
// same Azahar process (e.g., user tests Giants then SSA without restart)
// don't get stuck with a stale cached result.
u64 g_ssa_patcher_last_logged_title = 0;

void ApplySSAIRPatchesOnce(Core::System& system) {
    // Run the title check every call. Cheap (one struct field read + small
    // loop) and guarantees correct behavior across title switches. The
    // previous static-cache approach failed when the user ran Giants first
    // in the same Azahar process: g_ssa_patcher_is_ssa got set false and
    // never re-evaluated when SSA booted, so PATCH #1 never applied for SSA.

    // Title ID gate. Only proceed if the running process is SSA.
    auto process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset) {
        return;
    }
    const u64 title_id = process->codeset->program_id;
    bool is_ssa = false;
    for (u64 id : kSSATitleIds) {
        if (title_id == id) {
            is_ssa = true;
            break;
        }
    }
    // Log the title check ONCE per distinct title we see. Most calls hit a
    // title we've already logged, so this stays quiet. When user switches
    // titles (Giants → SSA in same Azahar process), we log the new title.
    if (title_id != g_ssa_patcher_last_logged_title) {
        LOG_DEBUG(Service_IR,
                  "[SSA-PATCHER] running process title=0x{:016x}, ssa_match={}",
                  title_id, is_ssa ? "yes" : "no");
        g_ssa_patcher_last_logged_title = title_id;
    }
    if (!is_ssa) return;

    const auto& table = GetSSAPatchTable();
    if (table.empty()) return;

    auto& mem = system.Memory();
    int applied = 0;
    int already_patched = 0;
    int skipped_mismatch = 0;

    for (const auto& entry : table) {
        // Read current bytes at the patch site.
        std::vector<u8> current(entry.original.size());
        for (std::size_t i = 0; i < current.size(); ++i) {
            current[i] = mem.Read8(entry.address + static_cast<VAddr>(i));
        }

        // Fast path: already patched. This is the common case after the
        // first apply — every subsequent IR delivery just reads bytes and
        // confirms they still match. Skip the write and the (expensive)
        // cache clear.
        if (current == entry.replacement) {
            already_patched++;
            continue;
        }

        // Safety: bytes are neither original nor replacement — refuse to
        // patch a game version we don't recognize.
        if (current != entry.original) {
            LOG_WARNING(Service_IR,
                        "[SSA-PATCHER] SKIP {} @ 0x{:08x}: bytes don't match "
                        "original or replacement (got 0x{:02x}{:02x}{:02x}{:02x})",
                        entry.description, entry.address,
                        current.size() >= 4 ? current[0] : 0,
                        current.size() >= 4 ? current[1] : 0,
                        current.size() >= 4 ? current[2] : 0,
                        current.size() >= 4 ? current[3] : 0);
            skipped_mismatch++;
            continue;
        }

        // Write the replacement bytes.
        for (std::size_t i = 0; i < entry.replacement.size(); ++i) {
            mem.Write8(entry.address + static_cast<VAddr>(i),
                       entry.replacement[i]);
        }

        // Invalidate the JIT cache for this region across ALL cores so
        // dynarmic recompiles the patched instructions next time PC enters
        // this range. The previous code used `system.GetRunningCore()` —
        // that only invalidated the IR thread's JIT cache, but the 'S'
        // processor (FUN_001fd470) runs on a different core (the broadcast
        // thread spawned by FUN_00212480), whose JIT had already compiled
        // the un-patched instructions during boot. Result: patches were in
        // memory but the broadcast thread kept executing cached pre-patch
        // code. system.InvalidateCacheRange iterates all cpu_cores.
        system.InvalidateCacheRange(entry.address, entry.replacement.size());

        LOG_DEBUG(Service_IR,
                  "[SSA-PATCHER] APPLIED {} @ 0x{:08x} ({} bytes)",
                  entry.description, entry.address, entry.replacement.size());
        applied++;
    }

    // Belt-and-suspenders: after applying patches, fully clear every core's
    // JIT cache. Forces complete recompilation on next execution so blocks
    // that were compiled from un-patched bytes get re-fetched from our
    // patched memory. Only runs when we actually wrote bytes — the common
    // path (patches already in place) skips this entirely.
    if (applied > 0) {
        const u32 num_cores = system.GetNumCores();
        for (u32 i = 0; i < num_cores; ++i) {
            system.GetCore(i).ClearInstructionCache();
        }
        LOG_DEBUG(Service_IR,
                  "[SSA-PATCHER] applied={} already_patched={} "
                  "cleared JIT cache on {} core(s)",
                  applied, already_patched, num_cores);
    }
    // Suppress per-call success logs; this runs on every IR delivery.
}

// (Previously a one-shot guard for state injection; removed because the
// natural response parser FUN_001fd470 isn't being invoked in our emu, so
// SSA's state machine never advances on its own. The reserved comment is
// kept for historical context.)
//
// Last outbound query bytes captured at SendIrNop entry. SSA's IR send
// payload is "ff ff ff <cmd> <param0> ..." so we capture bytes [3] and
// [4]. ReceiveIrnopLarge's SSA-PATCH block uses this pair to decide
// which state to inject after the response is delivered.
//
// Why we need this: on real hardware the IR firmware DMAs portal bytes
// into the receive buffer and an IRQ chain invokes FUN_001fd470 which
// advances the state machine. Azahar has no DMA/IRQ equivalent, so the
// natural parser never runs and state stays frozen. We emulate the
// transition that FUN_001fd470 *would* have written by keying off the
// last-sent query (which tells us what response we just delivered) and
// matching against the real-hardware GDB-captured state transition map.
u8 g_ssa_last_query_cmd = 0;
u8 g_ssa_last_query_param = 0;
} // namespace

template <class Archive>
void IR_USER::serialize(Archive& ar, const unsigned int) {
    DEBUG_SERIALIZATION_POINT;
    ar& boost::serialization::base_object<Kernel::SessionRequestHandler>(*this);
    ar & conn_status_event;
    ar & send_event;
    ar & receive_event;
    ar & shared_memory;
    ar & connected_circle_pad;
    ar & connected_portal;
    ar & receive_buffer;
    ar&* extra_hid.get();
    ar&* ir_portal.get();
}

// This is a header that will present in the ir:USER shared memory if it is initialized with
// InitializeIrNopShared service function. Otherwise the shared memory doesn't have this header if
// it is initialized with InitializeIrNop service function.
struct SharedMemoryHeader {
    u32_le latest_receive_error_result;
    u32_le latest_send_error_result;
    // TODO(wwylele): for these fields below, make them enum when the meaning of values is known.
    u8 connection_status;
    u8 trying_to_connect_status;
    u8 connection_role;
    u8 machine_id;
    u8 connected;
    u8 network_id;
    u8 initialized;
    u8 unknown;

    // This is not the end of the shared memory. It is followed by a receive buffer and a send
    // buffer. We handle receive buffer in the BufferManager class. For the send buffer, because
    // games usually don't access it, we don't emulate it.
};
static_assert(sizeof(SharedMemoryHeader) == 16, "SharedMemoryHeader has wrong size!");

class BufferManager {
public:
    BufferManager(std::shared_ptr<Kernel::SharedMemory> shared_memory_, u32 info_offset_,
                  u32 buffer_offset_, u32 max_packet_count_, u32 buffer_size)
        : shared_memory(shared_memory_), info_offset(info_offset_), buffer_offset(buffer_offset_),
          max_packet_count(max_packet_count_),
          max_data_size(buffer_size - sizeof(PacketInfo) * max_packet_count_) {
        UpdateBufferInfo();
    }

    bool Put(std::span<const u8> packet) {
        if (info.packet_count == max_packet_count) {
            return false;
        }

        u32 write_offset;

        if (info.packet_count == 0) {
            write_offset = 0;
            if (packet.size() > max_data_size)
                return false;
        } else {
            const u32 last_index = (info.end_index + max_packet_count - 1) % max_packet_count;
            const PacketInfo first = GetPacketInfo(info.begin_index);
            const PacketInfo last = GetPacketInfo(last_index);
            write_offset = (last.offset + last.size) % max_data_size;
            const u32 free_space = (first.offset + max_data_size - write_offset) % max_data_size;
            if (packet.size() > free_space)
                return false;
        }

        PacketInfo packet_info{write_offset, static_cast<u32>(packet.size())};
        SetPacketInfo(info.end_index, packet_info);

        for (std::size_t i = 0; i < packet.size(); ++i) {
            *GetDataBufferPointer((write_offset + i) % max_data_size) = packet[i];
        }

        info.end_index++;
        info.end_index %= max_packet_count;
        info.packet_count++;
        UpdateBufferInfo();
        return true;
    }

    bool Release(u32 count) {
        if (info.packet_count < count)
            return false;

        info.packet_count -= count;
        info.begin_index += count;
        info.begin_index %= max_packet_count;
        UpdateBufferInfo();
        return true;
    }

    bool Get(u32 size, std::vector<u8>& buffer) {
        if (info.packet_count == 0)
            return false;

        PacketInfo packet = GetPacketInfo(info.begin_index);

        u8* buf = GetDataBufferPointer(packet.offset);
        for (u8 i = 0; i < packet.size; i++) {
            buffer.push_back(buf[i]);
        }
        return true;
    }

private:
    struct BufferInfo {
        u32_le begin_index;
        u32_le end_index;
        u32_le packet_count;
        u32_le unknown;

    private:
        template <class Archive>
        void serialize(Archive& ar, const unsigned int) {
            ar & begin_index;
            ar & end_index;
            ar & packet_count;
            ar & unknown;
        }
        friend class boost::serialization::access;
    };
    static_assert(sizeof(BufferInfo) == 16, "BufferInfo has wrong size!");

    struct PacketInfo {
        u32_le offset;
        u32_le size;
    };
    static_assert(sizeof(PacketInfo) == 8, "PacketInfo has wrong size!");

    u8* GetPacketInfoPointer(u32 index) {
        return shared_memory->GetPointer(buffer_offset + sizeof(PacketInfo) * index);
    }

    void SetPacketInfo(u32 index, const PacketInfo& packet_info) {
        std::memcpy(GetPacketInfoPointer(index), &packet_info, sizeof(PacketInfo));
    }

    PacketInfo GetPacketInfo(u32 index) {
        PacketInfo packet_info;
        std::memcpy(&packet_info, GetPacketInfoPointer(index), sizeof(PacketInfo));
        return packet_info;
    }

    u8* GetDataBufferPointer(u32 offset) {
        return shared_memory->GetPointer(buffer_offset + sizeof(PacketInfo) * max_packet_count +
                                         offset);
    }

    void UpdateBufferInfo() {
        if (info_offset) {
            std::memcpy(shared_memory->GetPointer(info_offset), &info, sizeof(info));
        }
    }

    BufferInfo info{0, 0, 0, 0};
    std::shared_ptr<Kernel::SharedMemory> shared_memory;
    u32 info_offset;
    u32 buffer_offset;
    u32 max_packet_count;
    u32 max_data_size;

private:
    BufferManager() = default;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & info;
        ar & shared_memory;
        ar & info_offset;
        ar & buffer_offset;
        ar & max_packet_count;
        ar & max_data_size;
    }
    friend class boost::serialization::access;
};

/// Wraps the payload into packet and puts it to the receive buffer
void IR_USER::PutToReceive(std::span<const u8> payload) {
    LOG_INFO(Service_IR, "called, data={}", fmt::format("{:02x}", fmt::join(payload, " ")));
    std::size_t size = payload.size();

    std::vector<u8> packet;

    // Builds packet header. For the format info:
    // https://www.3dbrew.org/wiki/IRUSER_Shared_Memory#Packet_structure

    // fixed value
    packet.push_back(0xA5);
    // destination network ID
    u8 network_id = *(shared_memory->GetPointer(offsetof(SharedMemoryHeader, network_id)));
    packet.push_back(network_id);

    // puts the size info.
    if (size < 0x40) {
        packet.push_back(static_cast<u8>(size));
    } else if (size < 0x4000) {
        packet.push_back(static_cast<u8>(size >> 8) | 0x40);
        packet.push_back(static_cast<u8>(size));
    } else {
        ASSERT(false);
    }

    // puts the payload
    packet.insert(packet.end(), payload.begin(), payload.end());

    // calculates CRC and puts to the end
    packet.push_back(boost::crc<8, 0x07, 0, 0, false, false>(packet.data(), packet.size()));

    if (receive_buffer->Put(packet)) {
        // ALWAYS signal receive_event — the standard wake-up for IR data.
        // This is what Giants / TT / SC / I have always relied on, and what
        // stock Citra/Azahar has always done. Don't touch this default for
        // any reason.
        receive_event->Signal();

        // SSA additionally has a broadcast-listener thread inside FUN_00212480
        // that polls svcWaitSynchronizationN(timeout=0) on the
        // conn_status_event handle (verified by GDB capture on real
        // hardware — handles[0] resolves to "IR:ConnectionStatusEvent").
        // For SSA we need to ALSO signal that event so the listener wakes
        // up and processes the bytes via FUN_001fd470. Other titles
        // (Giants, TT, SC, Imaginators, Disney Infinity) don't have this
        // broadcast pattern — they read shared memory directly — so
        // signaling conn_status_event on every packet would either be a
        // no-op (no thread waiting on it for data) OR could cause spurious
        // state-change reactions in their connection-status polling code.
        // Title-gate to SSA so other games get exactly the stock behavior.
        if (g_ssa_diag_system) {
            auto process = g_ssa_diag_system->Kernel().GetCurrentProcess();
            if (process && process->codeset) {
                const u64 title_id = process->codeset->program_id;
                // SSA USA confirmed = 0x0004000000036E00; EUR/JPN candidates
                // in the same kSSATitleIds table. Inline the check here
                // since this is a hot path (every packet).
                if (title_id == 0x0004000000036E00ULL ||
                    title_id == 0x0004000000036F00ULL ||
                    title_id == 0x0004000000037000ULL) {
                    conn_status_event->Signal();
                }
            }
        }
    } else {
        LOG_ERROR(Service_IR, "receive buffer is full!");
    }
}

void IR_USER::InitializeIrNopShared(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 shared_buff_size = rp.Pop<u32>();
    const u32 recv_buff_size = rp.Pop<u32>();
    const u32 recv_buff_packet_count = rp.Pop<u32>();
    const u32 send_buff_size = rp.Pop<u32>();
    const u32 send_buff_packet_count = rp.Pop<u32>();
    const u8 baud_rate = rp.Pop<u8>();
    shared_memory = rp.PopObject<Kernel::SharedMemory>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    shared_memory->SetName("IR_USER: shared memory");

    receive_buffer = std::make_unique<BufferManager>(shared_memory, 0x10, 0x20,
                                                     recv_buff_packet_count, recv_buff_size);
    SharedMemoryHeader shared_memory_init{};
    shared_memory_init.initialized = 1;
    std::memcpy(shared_memory->GetPointer(), &shared_memory_init, sizeof(SharedMemoryHeader));

    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR,
             "called, shared_buff_size={}, recv_buff_size={}, "
             "recv_buff_packet_count={}, send_buff_size={}, "
             "send_buff_packet_count={}, baud_rate={}",
             shared_buff_size, recv_buff_size, recv_buff_packet_count, send_buff_size,
             send_buff_packet_count, baud_rate);
}

void IR_USER::InitializeIrNop(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 shared_buff_size = rp.Pop<u32>();
    const u32 recv_buff_size = rp.Pop<u32>();
    const u32 recv_buff_packet_count = rp.Pop<u32>();
    const u32 send_buff_size = rp.Pop<u32>();
    const u32 send_buff_packet_count = rp.Pop<u32>();
    const u8 baud_rate = rp.Pop<u8>();
    shared_memory = rp.PopObject<Kernel::SharedMemory>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    shared_memory->SetName("IR_USER: shared memory");

    // Per deReeperJosh baseline: same offsets as InitializeIrNopShared but no header init.
    // The "no SharedMemoryHeader" comment in the doc applies to the GAME's view, not to the
    // BufferManager layout — both init paths use the same offsets in his code.
    receive_buffer = std::make_unique<BufferManager>(shared_memory, 0x10, 0x20,
                                                     recv_buff_packet_count, recv_buff_size);

    // SSA (Skylanders: Spyro's Adventure) uses InitializeIrNop, NOT the Shared
    // variant. SSADiag observed all three heap-allocated portal-manager
    // dispatch siblings (0x09778ff4 / 0x09779058 / 0x0977d228) sitting inert
    // for 280+ poll cycles — counters and state fields never moved — which
    // is consistent with SSA's receiver thread never starting because it
    // sees SharedMemoryHeader.initialized == 0 and exits during portal
    // bring-up. Real firmware writes the same header for both Init variants;
    // mirroring that here unblocks the receiver thread.
    //
    // TITLE-GATED to SSA: Swap Force was being detected as the "wrong portal"
    // because it also uses InitializeIrNop (not Shared), and our unconditional
    // header write (initialized=1) changed what its device-detection code
    // read. Other games that use InitializeIrNop get the original stock
    // behavior — no header init, header stays zeroed as the doc says it
    // should for the unshared variant.
    if (IsSSATitleRunning()) {
        SharedMemoryHeader shared_memory_init{};
        shared_memory_init.initialized = 1;
        std::memcpy(shared_memory->GetPointer(), &shared_memory_init, sizeof(SharedMemoryHeader));
        }

    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR,
             "called, shared_buff_size={}, recv_buff_size={}, "
             "recv_buff_packet_count={}, send_buff_size={}, "
             "send_buff_packet_count={}, baud_rate={}",
             shared_buff_size, recv_buff_size, recv_buff_packet_count, send_buff_size,
             send_buff_packet_count, baud_rate);
}

void IR_USER::RequireConnection(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u8 device_id = rp.Pop<u8>();

    u8* shared_memory_ptr = shared_memory->GetPointer();
    if (device_id == 1) {
        shared_memory_ptr[offsetof(SharedMemoryHeader, connection_status)] = 2;
        shared_memory_ptr[offsetof(SharedMemoryHeader, connection_role)] = 2;
        shared_memory_ptr[offsetof(SharedMemoryHeader, connected)] = 1;

        connected_circle_pad = true;
        extra_hid->OnConnect();
        conn_status_event->Signal();
    } else {
        LOG_WARNING(Service_IR, "unknown device id {}. Won't connect.", device_id);
        shared_memory_ptr[offsetof(SharedMemoryHeader, connection_status)] = 1;
        shared_memory_ptr[offsetof(SharedMemoryHeader, trying_to_connect_status)] = 2;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR, "called, device_id = {}", device_id);
}

void IR_USER::AutoConnection(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 param_one = rp.Pop<u32>();
    const u32 param_two = rp.Pop<u32>();
    const u8 param_three = rp.Pop<u8>();
    const u32 param_four = rp.Pop<u32>();
    const u8 param_five = rp.Pop<u8>();
    const u32 param_six = rp.Pop<u32>();
    const u8 param_seven = rp.Pop<u8>();
    const u32 param_eight = rp.Pop<u32>();
    const u8 param_nine = rp.Pop<u8>();
    const u32 param_ten = rp.Pop<u32>();
    const u8 param_eleven = rp.Pop<u8>();

    u8* shared_memory_ptr = shared_memory->GetPointer();
    shared_memory_ptr[offsetof(SharedMemoryHeader, connection_status)] = 2;
    shared_memory_ptr[offsetof(SharedMemoryHeader, connection_role)] = 2;
    shared_memory_ptr[offsetof(SharedMemoryHeader, connected)] = 1;
    // SSA observed-on-real-3DS values that stock Azahar left at zero:
    //   network_id = 1  (3dbrew TODO note: "should assign a (random?) number";
    //                    real firmware picks a non-zero ID for the portal link)
    //   machine_id = 0  (MACHINE_ID_CTR per nn::ir::CTR::MachineId enum)
    // PutToReceive embeds network_id as byte[1] of every framed packet; if
    // SSA's receiver thread filters incoming packets by network_id and the
    // header reads 0, every response gets discarded. Set 1 to match what
    // real firmware does after a successful AutoConnection.
    //
    // TITLE-GATED to SSA: Swap Force's portal detection reads network_id /
    // initialized fields and was getting confused by our forced writes,
    // resulting in "wrong portal" detection. Stock AutoConnection behavior
    // for all other games (only the 3 connection fields are written).
    if (IsSSATitleRunning()) {
        shared_memory_ptr[offsetof(SharedMemoryHeader, network_id)] = 1;
        shared_memory_ptr[offsetof(SharedMemoryHeader, machine_id)] = 0;
        // Also ensure initialized=1 for the case where AutoConnection is called
        // after InitializeIrNop (SSA's path).
        shared_memory_ptr[offsetof(SharedMemoryHeader, initialized)] = 1;
    }

    // Both Skylanders AND Disney Infinity use AutoConnection. The Disney
    // Portal source sets both flags here and lets SendIrNop's per-packet
    // byte router (IsDisneyInfinityPacket / IsSkylandersPacket above)
    // decide which device receives each call. Without setting
    // connected_infinity_base here too, DI's calls fall through to the
    // portal branch and never reach ir_infinity_base — the game sees no
    // responses and shows "lost connection".
    connected_portal = true;
    connected_infinity_base = true;
    if (ir_portal) {
        ir_portal->OnConnect();
    }
    if (ir_infinity_base) {
        ir_infinity_base->OnConnect();
    }
    conn_status_event->Signal();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR,
             "called, one={}, two={}, "
             "three={}, four={}, "
             "five={}, six={}, "
             "seven={}, eight={}, "
             "nine={}, ten={}, "
             "eleven={}",
             param_one, param_two, param_three, param_four, param_five, param_six, param_seven,
             param_eight, param_nine, param_ten, param_eleven);
}

void IR_USER::GetReceiveEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb(ctx, 0x0A, 1, 2);
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(receive_event);
    LOG_INFO(Service_IR, "called");
}

void IR_USER::GetSendEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb(ctx, 0x0B, 1, 2);
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(send_event);
    LOG_INFO(Service_IR, "called");
}

void IR_USER::Disconnect(Kernel::HLERequestContext& ctx) {
    if (connected_circle_pad) {
        extra_hid->OnDisconnect();
        connected_circle_pad = false;
        conn_status_event->Signal();
    }
    if (connected_portal) {
        ir_portal->OnDisconnect();
        connected_portal = false;
        conn_status_event->Signal();
    }
    if (connected_infinity_base) {
        if (ir_infinity_base) {
            ir_infinity_base->OnDisconnect();
        }
        connected_infinity_base = false;
        conn_status_event->Signal();
    }

    u8* shared_memory_ptr = shared_memory->GetPointer();
    shared_memory_ptr[offsetof(SharedMemoryHeader, connection_status)] = 0;
    shared_memory_ptr[offsetof(SharedMemoryHeader, connected)] = 0;

    IPC::RequestBuilder rb(ctx, 0x09, 1, 0);
    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR, "called");
}

void IR_USER::GetConnectionStatusEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb(ctx, 0x0C, 1, 2);
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(conn_status_event);
    LOG_INFO(Service_IR, "called");
}

void IR_USER::GetConnectionStatus(Kernel::HLERequestContext& ctx) {
    // Header: 2 normal (Result + status), 0 translate.
    IPC::RequestBuilder rb(ctx, 0x13, 2, 0);

    if (connected_portal || connected_infinity_base) {
        conn_status_event->Signal();
        // REVERTED: previous code returned Result=0x000003FE to force the
        // "fast path" at FUN_00212480 PC 0x002124fc (the beq that skips
        // FUN_00201448). That bypassed the slow path's status-check at
        // PC 0x00212504-0x00212514, which is the ONLY code that sets
        // r5+13 (the "data ready" flag) via the status=2 branch at
        // 0x00212644. Without r5+13 = 1, the fast path at 0x00212530
        // always reads flag=0 and branches to idle (0x002129f8) —
        // never reaching the bl 0x201bec that does the ReceiveIrnopLarge
        // + 'S' byte check + FUN_001fd470 call at 0x002126a8.
        //
        // Net effect of the old hack: state[44] never advances (the
        // parser is never invoked), state machine stays frozen.
        //
        // Correct behavior: return ResultSuccess + status=2. The slow
        // path runs every cycle, the status=2 branch at 0x00212644
        // sets r5+13=1, and the subsequent fast-path entry at 0x00212530
        // sees the flag set, does the receive + 'S' check + FUN_001fd470
        // call. State advances naturally exactly as on real hardware.
        //
        // Giants/TT/SC/I aren't affected because they don't use this
        // wait+receive+parse pattern — they read shared memory directly.
        rb.Push(ResultSuccess);
        rb.Push<u8>(2);
    } else {
        LOG_ERROR(Service_IR, "not connected");
        rb.Push(Result(static_cast<ErrorDescription>(0x13), ErrorModule::IR,
                       ErrorSummary::InvalidState, ErrorLevel::Status));
        rb.Push<u8>(0);  // pad the 2nd normal param so header matches body
    }

    LOG_INFO(Service_IR, "called");
}

void IR_USER::FinalizeIrNop(Kernel::HLERequestContext& ctx) {
    if (connected_circle_pad) {
        extra_hid->OnDisconnect();
        connected_circle_pad = false;
    }
    if (connected_portal) {
        ir_portal->OnDisconnect();
        connected_portal = false;
    }
    if (connected_infinity_base) {
        if (ir_infinity_base) {
            ir_infinity_base->OnDisconnect();
        }
        connected_infinity_base = false;
    }

    shared_memory = nullptr;
    receive_buffer = nullptr;

    IPC::RequestBuilder rb(ctx, 0x02, 1, 0);
    rb.Push(ResultSuccess);

    LOG_INFO(Service_IR, "called");
}

void IR_USER::SendIrNop(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    std::vector<u8> buffer = rp.PopStaticBuffer();
    ASSERT(size == buffer.size());

    // SSA binary patcher: lazy-fires on first SendIrNop. Internal guards
    // gate by title ID and skip the work on every subsequent call.
    if (g_ssa_diag_system) {
        ApplySSAIRPatchesOnce(*g_ssa_diag_system);
    }

    // SSA-PATCH: capture the query bytes for the state-injection logic in
    // ReceiveIrnopLarge. SSA's portal protocol wraps every outbound query as
    // "ff ff ff <cmd> [<param0> ...]". The 'S' (status) query is only 4
    // bytes total — `ff ff ff 53` — with no param. The 'R' query is 5 bytes
    // (`ff ff ff 52 00`). The 'A' queries are 5 bytes (`ff ff ff 41 01` or
    // `ff ff ff 41 00`). Use >= 4 so we capture 'S' too; if there is no
    // param byte at [4], default to 0.
    if (buffer.size() >= 4) {
        g_ssa_last_query_cmd = buffer[3];
        g_ssa_last_query_param = (buffer.size() >= 5) ? buffer[4] : 0;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    // Packet-content router (matches Disney Portal source). AutoConnection
    // sets both connected_portal and connected_infinity_base, so we can't
    // distinguish DI vs Skylanders by flag — must look at the actual bytes.
    //
    // Order matters for clarity but not correctness: DI and Skylanders
    // packets have disjoint byte signatures (DI byte[2] vs Skylanders
    // byte[3]), so only one helper returns true for any given packet.
    //
    // Skylanders detection comes BEFORE Disney check because Skylanders
    // packets are the more common case (5 games vs 1) and the byte[3]
    // ASCII-letter test is cheaper than the byte[2] command-set lookup.
    if (connected_circle_pad) {
        extra_hid->OnReceive(buffer);
        send_event->Signal();
        rb.Push(ResultSuccess);
    } else if (IsSkylandersPacket(buffer) && connected_portal && ir_portal) {
        ir_portal->OnReceive(buffer);
        send_event->Signal();
        rb.Push(ResultSuccess);
    } else if (IsDisneyInfinityPacket(buffer) && connected_infinity_base && ir_infinity_base) {
        ir_infinity_base->OnReceive(buffer);
        send_event->Signal();
        rb.Push(ResultSuccess);
    } else if (connected_portal && ir_portal) {
        // Fallback for any Skylanders sub-byte we didn't list in the helper
        // (e.g. future protocol additions) when connected via portal.
        ir_portal->OnReceive(buffer);
        send_event->Signal();
        rb.Push(ResultSuccess);
    } else if (connected_infinity_base && ir_infinity_base) {
        // Fallback for DI variant packets not matching the helper.
        ir_infinity_base->OnReceive(buffer);
        send_event->Signal();
        rb.Push(ResultSuccess);
    } else {
        LOG_ERROR(Service_IR, "not connected");
        rb.Push(Result(static_cast<ErrorDescription>(13), ErrorModule::IR,
                       ErrorSummary::InvalidState, ErrorLevel::Status));
    }

    LOG_INFO(Service_IR, "called, data={}", fmt::format("{:02x}", fmt::join(buffer, " ")));
}

void IR_USER::ReceiveIrnopLarge(Kernel::HLERequestContext& ctx) {
    // Command 0x0010.
    //
    // ⚠️  TWO DIFFERENT IPC CONTRACTS — must route by title ⚠️
    //
    // Non-SSA games (Giants / Trap Team / Swap Force / SuperChargers /
    // Imaginators / Disney Infinity): pass a StaticBuffer translate descriptor
    // and expect MakeBuilder(1, 2). These games read portal bytes from the
    // shared memory directly (via BufferManager::Get on the receive_buffer),
    // so the IPC call itself only needs to acknowledge receipt and release
    // the consumed packet. This is the stock deReeperJosh behavior — proven
    // working for all five non-SSA Skylanders titles + DI.
    //
    // SSA (Skylanders: Spyro's Adventure): passes a MappedBuffer translate
    // descriptor (0x0000400C, size 1024) and reads response scalars from
    // TLS[0x88] / TLS[0x8c] — so we must use PopMappedBuffer and
    // MakeBuilder(3, 2). The MappedBuffer path also strips the 3-byte
    // {0xA5, network_id, size} frame header + 1-byte CRC trailer that
    // PutToReceive added, because SSA's caller expects to see the raw
    // portal payload (e.g. 0x52 'R' for firmware response) at byte 0 of
    // its destination buffer.
    //
    // Routing by title (not by translate-descriptor sniffing) is safer:
    // PopStaticBuffer on a MappedBuffer request — or vice versa — would
    // misread the kernel's translate slot and leak the wrong byte count
    // back to the game. Title-gated path keeps each game's contract intact.
    //
    // Note on the 'R' response payload in ir_portal.cpp: bytes 1 and 2 of
    // the unwrapped payload form the portal firmware version (high << 8 | low).
    // SSA (code.bin 0x001fdd78) requires version >= 0x001F = 31; the default
    // {0x52, 0x01, 0x34, ...} response yields 0x0134 = 308 which satisfies
    // SSA, Giants, and the later titles. Do not lower bytes 1/2 below 0x1F
    // or SSA's firmware check will fail and the game will re-poll 'R' forever.

    // ---- Non-SSA path: restore working-source behavior verbatim ----
    if (!IsSSATitleRunning()) {
        IPC::RequestParser rp(ctx);
        const u32 size = rp.Pop<u32>();
        std::vector<u8> buffer = rp.PopStaticBuffer();
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        if (receive_buffer && receive_buffer->Get(size, buffer)) {
            receive_buffer->Release(1);
            rb.Push(ResultSuccess);
        } else {
            rb.Push(Result(static_cast<ErrorDescription>(13), ErrorModule::IR,
                           ErrorSummary::InvalidState, ErrorLevel::Status));
        }
        LOG_INFO(Service_IR, "called, size={}, data={}", size,
                 fmt::format("{:02x}", fmt::join(buffer, " ")));
        return;
    }

    // ---- SSA path: MappedBuffer + (3, 2) + frame strip + state inject ----
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    auto& mapped_buffer = rp.PopMappedBuffer();

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 2);

    // IMPORTANT: BufferManager::Get() uses push_back to append the packet
    // bytes, so the destination vector must START EMPTY. Do NOT pre-allocate
    // it with std::vector<u8>(size) — that creates `size` zeros and Get
    // appends after them, leaving the actual packet at offset `size`.
    std::vector<u8> framed;
    if (receive_buffer && receive_buffer->Get(size, framed)) {
        receive_buffer->Release(1);

        // Strip the 3-byte (or 4-byte) frame header and 1-byte CRC trailer.
        // PutToReceive layout:
        //   [0]   0xA5
        //   [1]   network_id
        //   [2]   size_byte_0    (if bit 6 set, [3] is size_byte_1, header is 4 bytes)
        //   [...] payload
        //   [-1]  crc
        std::size_t header_len = 3;
        if (framed.size() > 2 && (framed[2] & 0x40)) {
            header_len = 4;
        }
        std::size_t payload_len = 0;
        if (framed.size() > header_len + 1) {
            payload_len = framed.size() - header_len - 1;
        }
        const std::size_t copy_size =
            std::min<std::size_t>(payload_len, mapped_buffer.GetSize());
        if (copy_size > 0) {
            mapped_buffer.Write(framed.data() + header_len, 0, copy_size);
        }
        // SSA state-machine driver. On real hardware the IR firmware DMAs
        // portal bytes into the receive buffer and an IRQ chain invokes
        // FUN_001fd470 which advances the state machine. Azahar has no
        // DMA/IRQ equivalent, so the natural parser never runs and state
        // stays frozen at 3. We emulate the transitions FUN_001fd470 would
        // have written by keying off the last-sent query (see the switch
        // on g_ssa_last_query_cmd below).
        //
        // Gated by both the title-ID and a 4-field fingerprint of the
        // dispatch struct's config (poll=528, wireless=2000, resp=300) for
        // defense-in-depth. Non-SSA games early-return.
        if (g_ssa_diag_system && IsSSATitleRunning()) {
            auto& mem = g_ssa_diag_system->Memory();
            const u32 parent_ptr = mem.Read32(0x0053C340);
            u32 disp = 0;
            u32 cfg_poll = 0, cfg_wireless = 0, cfg_resp = 0, cur_state = 0;
            if (parent_ptr > 0x08000000 && parent_ptr < 0x0A000000) {
                disp = mem.Read32(parent_ptr + 0x18);
                if (disp > 0x08000000 && disp < 0x0A000000) {
                    cfg_poll = mem.Read32(disp + 0x08);
                    cfg_wireless = mem.Read32(disp + 0x10);
                    cfg_resp = mem.Read32(disp + 0x18);
                    cur_state = mem.Read32(disp + 0x1C);
                }
            }
            // Staged state injection driven by the last query SSA sent.
            //
            // On real hardware (GDB capture of state[1C] writes), the
            // portal state machine walks through:
            //   3  -> 6 -> 7              after 'R' response
            //   7  -> 7 (no change)       after 'A 01' response (battery OK)
            //   7  -> 8                   after 'C' response (LED ack, ~transient)
            //   8  -> 9                   when FUN_001fdfc4 case 8 emits 'A 00'
            //   9  -> 12 (figure poll)    after 'A 00' response (wireless off)
            //   12 -> 10 -> 11 -> 12      steady-state figure polling loop
            //
            // Because FUN_001fd470 never runs in our emulator, we emulate
            // the relevant `state[1C]` writes here. The query byte we just
            // sent tells us which response we just delivered, and that
            // pairs uniquely with one of the transitions above. We collapse
            // some transient states (6 -> 7, 11 -> 12 -> 10) into a single
            // write to keep the cycle moving.
            //
            // Gate is unchanged — same 4 config fingerprints as before,
            // plus a known-polling cur_state value. Any non-SSA title that
            // somehow matched the config would also need to be in exactly
            // the right state at the right time; the conjunction probability
            // remains effectively zero.
            const bool ssa_fingerprint =
                copy_size > 0 && parent_ptr > 0x08000000 &&
                parent_ptr < 0x0A000000 && disp > 0x08000000 &&
                disp < 0x0A000000 && cfg_poll == 528 &&
                cfg_wireless == 2000 && cfg_resp == 300;

            u32 next_state = 0xFFFFFFFF; // sentinel: no injection
            const char* reason = "";

            if (ssa_fingerprint) {
                const bool sane_state = cur_state <= 0x14;
                if (sane_state) {
                    switch (g_ssa_last_query_cmd) {
                    case 0x52: // 'R' (firmware version)
                        // Real-hw 3 -> 6 -> 7. Collapse to 7.
                        next_state = 7;
                        reason = "post-R -> 7";
                        break;
                    case 0x41: // 'A' (battery query)
                        if (g_ssa_last_query_param == 0x01) {
                            // 'A 01' response. Real-hw stays at 7 here then
                            // does C, then transitions 7 -> 8 separately.
                            // We collapse: jump straight to 8 so SSA's case
                            // 8 emits 'A 00' next iteration.
                            next_state = 8;
                            reason = "post-A01 -> 8 (skip C)";
                        } else if (g_ssa_last_query_param == 0x00) {
                            // 'A 00' response. Real-hw 9 -> 12. Use state 12
                            // (not 10) so FUN_001fdfc4's pre-switch logic
                            // handles 12 -> 10 itself, including setting
                            // state[3c] (poll-period timer) the way real-hw
                            // does. Skipping state 12 entirely caused SSA
                            // to detect "no progress" and restart after
                            // every S query.
                            next_state = 12;
                            reason = "post-A00 -> 12";
                        }
                        break;
                    case 0x43: // 'C' (LED control)
                        next_state = 8;
                        reason = "post-C -> 8";
                        break;
                    case 0x53: // 'S' (status / figure-poll)
                        // Real-hw 11 -> 12. Let SSA's case 12 pre-switch
                        // run 12 -> 10 naturally on the next poll iteration.
                        next_state = 12;
                        reason = "post-S -> 12";
                        break;
                    // 'Q' (figure read) and 'W' (write ack) are handled by
                    // SSA's natural case 0xf / 0x10 / 0x11 paths. Once we
                    // reach figure detection we'll need a separate state-
                    // injection for those too.
                    default:
                        break;
                    }
                }
            }

            if (next_state != 0xFFFFFFFF) {
                LOG_DEBUG(Service_IR,
                          "[SSA-PATCH] last_query=0x{:02x}/0x{:02x} "
                          "cur_state={} -> {} ({})",
                          g_ssa_last_query_cmd, g_ssa_last_query_param,
                          cur_state, next_state, reason);
                mem.Write32(disp + 0x1C, next_state);
                // Refresh response-timeout so SSA's polling thread doesn't
                // immediately fall back to state 2 on timeout.
                mem.Write32(disp + 0x28, 400000);
                // When injecting state 12, also zero state[3c] so the
                // pre-switch check in FUN_001fdfc4 (state[1c]==12 &&
                // state[3c]==0 && ...) fires and transitions to state 10
                // with state[3c] = state[8]*1000. Without this, the
                // pre-switch sees state[3c]!=0 and falls through to case
                // 12 proper, which has many subpaths we don't fully drive.
                if (next_state == 12) {
                    mem.Write32(disp + 0x3C, 0);
                }

                // Mirror the companion-field writes that FUN_001fd470 would
                // have performed for each response type. Without these, SSA's
                // polling thread sees stuck state[44] / state[55] values and
                // restarts the handshake after a single 'S' poll.
                //
                // - After 'A 01' response: real-hw case 7 sets state[0x55] =
                //   byte[1] & 1. Our 'A 01' response echoes 0x01 in byte[1],
                //   so state[0x55] should be 1 (wireless flag set). This is
                //   the gate that case 0xb / 'S' handler checks before
                //   processing figure presence — without it, S handler
                //   returns early and state[44] never updates.
                //
                // - After 'S' response: real-hw 'S' handler does
                //     state[0x44] = byte[5]   (interrupt counter)
                //     state[0x40] = bitmap    (figure presence)
                //   GetStatus() puts a monotonically-incrementing interrupt
                //   counter at byte[5] and the figure bitmap in bytes[1..4].
                //   Mirror those writes so SSA sees "S response processed
                //   and state advancing" instead of "no progress, restart."
                switch (g_ssa_last_query_cmd) {
                case 0x41: // 'A'
                    if (g_ssa_last_query_param == 0x01) {
                        // wireless_connected = byte[1] & 1 = 0x01 & 1 = 1
                        mem.Write8(disp + 0x55, 1);
                        // battery_voltage byte[2] = 0xFF, stash at +0x56
                        mem.Write8(disp + 0x56, 0xFF);
                        // battery_level (set to 0 = high since 0xFF >= 0xc0)
                        mem.Write8(disp + 0x57, 0);
                    }
                    break;
                case 0x53: // 'S'
                    if (copy_size >= 7 && payload_len >= 7) {
                        // payload[1..4] = figure bitmap, payload[5] = irq ctr
                        const u8* payload = framed.data() + header_len;
                        u32 figure_bitmap = static_cast<u32>(payload[1]) |
                                            (static_cast<u32>(payload[2]) << 8) |
                                            (static_cast<u32>(payload[3]) << 16) |
                                            (static_cast<u32>(payload[4]) << 24);
                        mem.Write32(disp + 0x40, figure_bitmap);
                        mem.Write32(disp + 0x44, payload[5]);
                    }
                    break;
                default:
                    break;
                }
            }
        }

        // Per Nintendo SDK contract (nn::ir::CTR::Communicator::Receive):
        //   normal[1] = *pReceiveSize  → BYTE count of the received payload
        //   normal[2] = *pRemainCount  → number of receive packets still queued
        // Stock Azahar pushed 1/0 here, which Heavy Iron's wrapper at SSA
        // code.bin 0x00201a20 stores via *param_3 / *param_4. The caller then
        // validates receiveSize against the expected 'R' response length (32);
        // pushing literal `1` made SSA discard the response as truncated and
        // re-poll forever. Push the real copy_size instead — matches what the
        // SDK guarantees and what real-3DS firmware returns.
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>(copy_size)); // *pReceiveSize  TLS[0x88]
        rb.Push<u32>(0);                           // *pRemainCount  TLS[0x8c]
    } else {
        rb.Push(Result(static_cast<ErrorDescription>(13), ErrorModule::IR,
                       ErrorSummary::InvalidState, ErrorLevel::Status));
        rb.Push<u32>(0);
        rb.Push<u32>(0);
    }
    rb.PushMappedBuffer(mapped_buffer);

    LOG_INFO(Service_IR, "called, size={}, data={}", size,
             fmt::format("{:02x}", fmt::join(framed, " ")));
}

void IR_USER::ReceiveIrnop(Kernel::HLERequestContext& ctx) {
    // Command 0x000F. Older Receive variant used by Skylanders: Spyro's
    // Adventure. The other Skylanders 3DS games (Giants onward) use
    // ReceiveIrnopLarge (0x0010) with a MappedBuffer; SSA's IR wrapper at
    // code.bin 0x0020181c uses a STATIC buffer (descriptor at TLS[0x180] =
    // (size << 14) | 2, buffer ptr at TLS[0x184]) and reads the response
    // scalars from TLS[0x88] / TLS[0x8c]. Stock Azahar had 0x000F = nullptr
    // so the call silently no-op'd; the game saw no portal bytes and
    // re-polled 'R' forever.
    //
    // For all non-SSA games, restore the stock nullptr behavior: just push
    // a default success response and bail. SSA is the only known caller of
    // 0x000F on 3DS; binding the handler unconditionally could change the
    // command's response shape for other titles that we don't know about.
    //
    // Response shape (matches what SSA's wrapper expects):
    //   header MakeBuilder(3, 2) → 3 normal + 2 translate
    //     normal[0] = ResultSuccess
    //     normal[1] = packet count (1) → goes to caller's *param_3 scalar
    //     normal[2] = 0              → goes to caller's *param_4 scalar
    //   translate slot = static buffer with the framed packet bytes
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    [[maybe_unused]] auto in_buffer = rp.PopStaticBuffer();

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 2);

    // Non-SSA games: mirror the stock nullptr behavior (empty response). We
    // do this here rather than at the dispatch table because IPC::MakeBuilder
    // needs to fire to keep the kernel happy regardless of how the handler
    // returns. Push the minimum-valid (3,2) response shape with zeros and
    // bail before the SSA-specific framing/strip work runs.
    if (!IsSSATitleRunning()) {
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
        rb.Push<u32>(0);
        rb.PushStaticBuffer(std::vector<u8>(), 0);
        return;
    }

    // BufferManager::Get() uses push_back; must start empty (see comment
    // in ReceiveIrnopLarge above).
    std::vector<u8> packet;
    if (receive_buffer && receive_buffer->Get(size, packet)) {
        receive_buffer->Release(1);
        // Strip the frame header + CRC the same way ReceiveIrnopLarge does
        // (PutToReceive uses identical framing for both code paths).
        std::size_t header_len = 3;
        if (packet.size() > 2 && (packet[2] & 0x40)) {
            header_len = 4;
        }
        std::vector<u8> unwrapped;
        if (packet.size() > header_len + 1) {
            unwrapped.assign(packet.begin() + header_len, packet.end() - 1);
        }
        // Match the (3,2) Receive contract — see comment in ReceiveIrnopLarge.
        // *pReceiveSize = unwrapped payload byte count, *pRemainCount = 0.
        const u32 receive_size = static_cast<u32>(unwrapped.size());
        rb.Push(ResultSuccess);
        rb.Push<u32>(receive_size);
        rb.Push<u32>(0);
        rb.PushStaticBuffer(std::move(unwrapped), 0);
    } else {
        rb.Push(Result(static_cast<ErrorDescription>(13), ErrorModule::IR,
                       ErrorSummary::InvalidState, ErrorLevel::Status));
        rb.Push<u32>(0);
        rb.Push<u32>(0);
        rb.PushStaticBuffer(std::vector<u8>(), 0);
    }

    LOG_INFO(Service_IR, "ReceiveIrnop called, size={}", size);
}

void IR_USER::GetLatestReceiveErrorResult(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb(ctx, 0x11, 2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
    LOG_INFO(Service_IR, "called");
}

void IR_USER::GetLatestSendErrorResult(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb(ctx, 0x12, 2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
    LOG_INFO(Service_IR, "called");
}

void IR_USER::ReleaseReceivedData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 count = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    if (receive_buffer && receive_buffer->Release(count)) {
        rb.Push(ResultSuccess);
    } else {
        LOG_ERROR(Service_IR, "failed to release {} packets", count);
        rb.Push(Result(ErrorDescription::NoData, ErrorModule::IR, ErrorSummary::NotFound,
                       ErrorLevel::Status));
    }

    LOG_INFO(Service_IR, "called, count={}", count);
}

IR_USER::IR_USER(Core::System& system) : ServiceFramework("ir:USER", 1) {
    // SSA-DIAG: capture system reference for SendIrNop's dispatch-state dump.
    g_ssa_diag_system = &system;

    const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &IR_USER::InitializeIrNop, "InitializeIrNop"},
        {0x0002, &IR_USER::FinalizeIrNop, "FinalizeIrNop"},
        {0x0003, nullptr, "ClearReceiveBuffer"},
        {0x0004, nullptr, "ClearSendBuffer"},
        {0x0005, nullptr, "WaitConnection"},
        {0x0006, &IR_USER::RequireConnection, "RequireConnection"},
        {0x0007, &IR_USER::AutoConnection, "AutoConnection"},
        {0x0008, nullptr, "AnyConnection"},
        {0x0009, &IR_USER::Disconnect, "Disconnect"},
        {0x000A, &IR_USER::GetReceiveEvent, "GetReceiveEvent"},
        {0x000B, &IR_USER::GetSendEvent, "GetSendEvent"},
        {0x000C, &IR_USER::GetConnectionStatusEvent, "GetConnectionStatusEvent"},
        {0x000D, &IR_USER::SendIrNop, "SendIrNop"},
        {0x000E, nullptr, "SendIrNopLarge"},
        {0x000F, &IR_USER::ReceiveIrnop, "ReceiveIrnop"},
        {0x0010, &IR_USER::ReceiveIrnopLarge, "ReceiveIrnopLarge"},
        {0x0011, &IR_USER::GetLatestReceiveErrorResult, "GetLatestReceiveErrorResult"},
        {0x0012, &IR_USER::GetLatestSendErrorResult, "GetLatestSendErrorResult"},
        {0x0013, &IR_USER::GetConnectionStatus, "GetConnectionStatus"},
        {0x0014, nullptr, "GetTryingToConnectStatus"},
        {0x0015, nullptr, "GetReceiveSizeFreeAndUsed"},
        {0x0016, nullptr, "GetSendSizeFreeAndUsed"},
        {0x0017, nullptr, "GetConnectionRole"},
        {0x0018, &IR_USER::InitializeIrNopShared, "InitializeIrNopShared"},
        {0x0019, &IR_USER::ReleaseReceivedData, "ReleaseReceivedData"},
        {0x001A, nullptr, "SetOwnMachineId"},
        // clang-format on
    };
    RegisterHandlers(functions);

    using namespace Kernel;

    connected_circle_pad = false;
    connected_portal = false;
    connected_infinity_base = false;
    conn_status_event = system.Kernel().CreateEvent(ResetType::OneShot, "IR:ConnectionStatusEvent");
    send_event = system.Kernel().CreateEvent(ResetType::OneShot, "IR:SendEvent");
    receive_event = system.Kernel().CreateEvent(ResetType::OneShot, "IR:ReceiveEvent");

    extra_hid = std::make_unique<ExtraHID>([this](std::span<const u8> data) { PutToReceive(data); },
                                           system.CoreTiming(), system.Movie());
    ir_portal =
        std::make_unique<IRPortal>([this](std::span<const u8> data) { PutToReceive(data); });
    // Disney Infinity Base — matching D:\...\Disney Portal\src\...\ir_user.cpp
    // constructor. Without this, DI's PutToReceive→OnReceive chain has no
    // device to deliver bytes to, so the game sees no portal responses and
    // shows "lost connection". Constructed for every game (no title gate)
    // because the per-packet router below dispatches by byte content, not
    // by title.
    ir_infinity_base =
        std::make_unique<IRInfinityBase>([this](std::span<const u8> data) { PutToReceive(data); });
}

IR_USER::~IR_USER() {
    if (connected_circle_pad) {
        extra_hid->OnDisconnect();
    }
    if (connected_portal) {
        ir_portal->OnDisconnect();
    }
    if (connected_infinity_base && ir_infinity_base) {
        ir_infinity_base->OnDisconnect();
    }
}

void IR_USER::ReloadInputDevices() {
    extra_hid->RequestInputDevicesReload();
}

void IR_USER::UseArticController(const std::shared_ptr<Service::HID::ArticBaseController>& ac) {
    if (extra_hid.get()) {
        extra_hid->UseArticController(ac);
    }
}

IRDevice::IRDevice(SendFunc send_func_) : send_func(send_func_) {}
IRDevice::~IRDevice() = default;

void IRDevice::Send(std::span<const u8> data) {
    send_func(data);
}

} // namespace Service::IR
