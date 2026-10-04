// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/hacks/hack_manager.h"

namespace Common::Hacks {

std::atomic<bool> g_accurate_jit_memory{false};
std::atomic<bool> g_graceful_jit_fallback{false};
std::atomic<bool> g_overbright_vertex_colors{false};
std::atomic<bool> g_frogger_specular_blue{false};
std::atomic<bool> g_right_eye_display_fallback{false};
std::atomic<bool> g_memoryfill_per_buffer_irq{false};
std::atomic<bool> g_keep_last_frame_on_zero_fb{false};
std::atomic<bool> g_skip_core1_preemption{false};
std::atomic<bool> g_fs_control_zero_output{false};
std::atomic<bool> g_fs_control_one_output{false};
std::atomic<bool> g_fs_save_diag{false};
std::atomic<bool> g_gsp_legacy_pdc_queue{false};
std::atomic<bool> g_disable_gpu_timing_sim{false};
std::atomic<bool> g_force_present_every_frame{false};
std::atomic<bool> g_cull_mode_invalid_keeps_state{false};
std::atomic<bool> g_legacy_validation_skip{false};
std::atomic<bool> g_hw_vertex_color_saturate{false};
std::atomic<bool> g_shadow_equal_depth_lit{false};
std::atomic<bool> g_force_fbo_clear{false};
std::atomic<bool> g_savedata_free_bytes_from_format{false};
std::atomic<bool> g_gsp_drain_queue_on_acquire{false};
std::atomic<bool> g_jaws_entity_unlink{false};
std::atomic<bool> g_async_wake_shutdown_guard{false};
std::atomic<u32> g_auto_render_thread_delay_us{0};
std::atomic<bool> g_draw_lookup_reuse{false};
std::atomic<bool> g_float_uniform_nan_to_zero{false};
std::atomic<bool> g_vblank_signal_before_present{false};
std::atomic<bool> g_kernel_wait_diag{false};
std::atomic<bool> g_core_downcount_hack{false};
std::atomic<bool> g_fb_config_diag{false};
std::atomic<bool> g_force_cpu_bottom_screen{false};
std::atomic<bool> g_fb_flip_rect_disable{false};
std::atomic<bool> g_ignore_format_reinterpretation{false};
std::atomic<bool> g_force_validated_pica_index{false};

HackManager hack_manager = {
    .entries = {

        // The following games cannot use the right eye disable hack due to the way they
        // handle rendering.
        {HackType::RIGHT_EYE_DISABLE,
         HackEntry{
             .mode = HackAllowMode::DISALLOW,
             .affected_title_ids =
                 {
                     // Luigi's Mansion
                     0x00040000001D1900,
                     0x00040000001D1A00,

                     // Luigi's Mansion 2
                     0x0004000000055F00,
                     0x0004000000076500,
                     0x0004000000076400,
                     0x00040000000D0000,

                     // Rayman Origins
                     0x000400000005A500,
                     0x0004000000084400,
                     0x0004000000057600,

                     // Azahar 100 additions 2026-07-09: bottom-screen
                     // flicker with top-screen bleed-through (right-eye
                     // disabler false positives; all title IDs read from
                     // the user's own cci headers).
                     // FIFA Soccer 12
                     0x0004000000047A00, // USA
                     // FIFA Soccer 13
                     0x00040000000A2900, // USA
                     // FIFA 14 - Legacy Edition
                     0x00040000000DEA00, // USA
                     // FIFA 15 - Legacy Edition
                     0x000400000013C700, // USA
                     // Captain America: Super Soldier
                     0x0004000000040600, // USA
                     // Castlevania: Lords of Shadow - Mirror of Fate
                     0x0004000000096600, // USA
                     // Phineas and Ferb: Quest for Cool Stuff
                     0x00040000000E5B00, // USA
                     // Shin Megami Tensei: Devil Survivor Overclocked
                     0x0004000000038800, // USA
                     // Real Heroes: Firefighter 3D (flicker otherwise needs
                     // a ~10ms render-thread delay that costs half the fps)
                     0x000400000004E500, // USA
                     // Sonic & All-Stars Racing Transformed (same
                     // signature: top-screen flicker otherwise needs a
                     // 5.668ms render delay that caps it at 22fps)
                     0x00040000000B3500, // USA

                     // Skylanders: the disabler blocks the top-screen
                     // display transfers of their full-screen intro/story
                     // videos, so decoded frames never reach the
                     // framebuffer ("videos do not render"). The pre-port
                     // fork disabled the RightEyeDisabler globally; this
                     // is the title-gated equivalent.
                     0x0004000000036E00, // Spyro's Adventure USA
                     0x0004000000091D00, // Giants USA
                     0x00040000000E6500, // Swap Force USA
                     0x0004000000131200, // Trap Team USA
                     0x000400000013BE00, // Trap Team EUR

                     // Azahar 100 additions 2026-07-15: verification-campaign
                     // flicker cluster (top-screen bleed onto bottom and/or
                     // screen flicker; several otherwise need a render-thread
                     // ms delay that costs fps). All title IDs read from the
                     // user's own cci headers (USA unless noted).
                     // Carnival Games: Wild West 3D (else needs 7.616ms delay)
                     0x0004000000061400,
                     // Bratz: Fashion Boutique
                     0x00040000000A7300,
                     // Farming Simulator 18
                     0x00040000001B7900,
                     // Horses 3D
                     0x000400000004C200,
                     // Ice Age: Continental Drift - Arctic Games
                     0x0004000000086900,
                     // Imagine: Fashion Designer
                     0x0004000000044A00,
                     // Mahjong 3D: Warriors of the Emperor (else ~5ms delay)
                     0x000400000007A700,
                     // Naruto Powerful Shippuden
                     0x000400000003CF00,
                     // Nickelodeon Teenage Mutant Ninja Turtles (base game)
                     0x00040000000D5B00,
                     // The Peanuts Movie: Snoopy's Grand Adventure
                     0x0004000000169900,
                     // SpongeBob HeroPants (else needs 5.313ms delay)
                     0x0004000000154800,
                     // SpongeBob: Plankton's Robotic Revenge
                     0x00040000000F2400,
                     // Transformers: Dark of the Moon - Stealth Force Edition
                     0x000400000004A500,
                     // Wipeout: Create & Crash
                     0x00040000000ED200,
                     // Wipeout 2
                     0x000400000004DD00,
                     // Wipeout 3
                     0x000400000009F300,
                     // Battleship
                     0x0004000000071700,
                     // Madden NFL Football (top screen unrenderable, FIFA-like)
                     0x0004000000035500,
                     // Style Savvy: Styling Star
                     0x00040000001C2500,
                     // Centipede: Infestation (slight flicker)
                     0x000400000004AD00,
                     // Fire Emblem Warriors (N3DS; slight flicker)
                     0x000400000F70CC00,
                     // Metroid Prime: Federation Force (slight flicker)
                     0x000400000016E300,
                     // Shin Megami Tensei IV (slight flicker)
                     0x00040000000E5C00,
                     // Style Savvy: Trendsetters (intro flicker)
                     0x00040000000A9100,
                     // Hyrule Warriors Legends (loading flicker; also has a
                     // separate bottom-screen-missing issue — this addresses
                     // the flicker component only)
                     0x000400000017EA00,
                     // TMNT: The Movie — EXPERIMENTAL: compound-broken title
                     // (no bottom screen + corrupt top + choppy); the disabler
                     // may be blocking its display transfers like Skylanders.
                     // Gated, so worst case is no change.
                     0x000400000012D400,

                     // Azahar 100 additions 2026-07-15 (batch 2): the
                     // bottom-screen-missing cluster. TMNT: The Movie's full
                     // recovery above proved the disabler can block a screen's
                     // display transfers outright (not just flicker) — these
                     // titles show the same missing-bottom symptom.
                     // Cartoon Universe: Adventure (bottom missing at menu,
                     // blocks entry — was BROKEN)
                     0x000400000012EE00,
                     // Generator Rex: Agent of Providence (bottom black in
                     // gameplay)
                     0x0004000000036C00,
                     // Pokemon Super Mystery Dungeon (bottom missing)
                     0x0004000000174600,
                     // Persona Q: Shadow of the Labyrinth (bottom missing
                     // during story videos + logo overlay on top)
                     0x0004000000123400,
                     // Persona Q2: New Cinema Labyrinth (same as Persona Q)
                     0x00040000001D7100,

                     // Azahar 100 additions 2026-07-15 (batch 3): the
                     // top-screen-black cluster — same disabler symptom on
                     // the opposite screen (batches 1+2 all user-confirmed).
                     // The Adventures of Tintin: The Game (top screen black,
                     // bottom + gameplay fine)
                     0x000400000004A000,
                     // Disney Planes: Fire & Rescue (top screen black)
                     0x0004000000137400,
                     // Disney Violetta: Rhythm & Music (top screen black)
                     0x000400000012F400,

                     // Azahar 100 addition 2026-07-15 (batch 4): Imagine
                     // Fashion Life — top screen over bottom, same disabler
                     // false-positive class (its extdata prompt is a
                     // separate, still-open issue).
                     0x0004000000047800,

                     // Azahar 100 additions 2026-07-18 (batch 7): the
                     // ex-stall trio renders normally post-vblank-fix (30fps
                     // GX loops confirmed by TRIGGER-DIAG) but the screens
                     // never show it; Brunswick routes splash frames to the
                     // BOTTOM screen — disabler-misroute signature. Gated
                     // experiment alongside the FBSWAP/HWREG probes.
                     // Brunswick Pro Bowling
                     0x000400000004C400,
                     // Finding Nemo: Escape to the Big Blue SE
                     0x00040000000A2100,
                     // Happy Feet Two
                     0x0004000000044300,

                     // Azahar 100 addition 2026-07-24 (measured, not guessed):
                     // Ultimate NES Remix blits its BOTTOM screen from the
                     // same source buffer the disabler has tagged as the top
                     // screen's, so ShouldAllowDisplayTransfer returns false
                     // and the blit is dropped by BOTH the accelerated and
                     // software paths — the bottom framebuffer is valid but
                     // never written (FBXFER probe: 0x1839F600 -> 0x18038400,
                     // RGBA8->RGB8, crop_input_lines). Citra 2104 has no such
                     // guard and renders it; stock Azahar fails identically.
                     0x0004000000132000, // Ultimate NES Remix

                     // Screen-shift pair — CONFIRMED FIXED 2026-07-25.
                     // 7th Dragon III shifted its TOP screen a quarter
                     // right, Samus Returns its BOTTOM. Proven the hard
                     // way: instrumented Citra 2104 was built from source
                     // and runtime-diffed against A100 — draw placement,
                     // blit windows, sub-rect resolution, presentation,
                     // framebuffer contents and layout rects were ALL
                     // identical. The disabler is the one display feature
                     // A100 has that Citra lacks entirely, and disabling
                     // it fixes both games. Its damage here is positional
                     // rather than the dropped-blit blackout seen on
                     // Ultimate NES Remix / The Smurfs.
                     0x000400000018F800, // 7th Dragon III Code: VFD
                     0x00040000001BB200, // Metroid: Samus Returns

                     // The Smurfs — CONFIRMED FIXED 2026-07-24 (was
                     // BROKEN: ran to the main menu with only a white box
                     // on the top screen). Note for future triage: its
                     // observable transfers all targeted the BOTTOM screen
                     // while the visible fault was on the TOP, and its
                     // top-screen left/right eye addresses are identical.
                     // So the disabler's damage is NOT confined to the
                     // screen whose transfers you can see — do not rule
                     // this class out on that basis.
                     0x0004000000160900,

                     // (Reel Fishing Paradise 3D 0x0004000000048D00 was
                     // trialed here 2026-07-24 and removed: no change —
                     // still no render, audio plays choppily. Its blank
                     // screens are NOT the disabler-drop class.)

                     // Azahar 100 additions 2026-07-16 (batch 5): Great-tier
                     // sweep — flicker / screen-missing / screen-bleed class
                     // (user-verified symptoms; TIDs from user's cci headers).
                     // Cartoon Network: Battle Crashers (top flicker; rotated
                     // level screen tracked separately)
                     0x0004000000192000,
                     // Crush 3D (top covering bottom)
                     0x0004000000037100,
                     // Etrian Mystery Dungeon (bottom not rendering)
                     0x000400000015B200,
                     // Ever Oasis (top no-render in intro, flicker in game)
                     0x00040000001A4800,
                     // Fantasy Life (top doesn't render)
                     0x0004000000113200,
                     // Fire Emblem Awakening (bottom menu misrender)
                     0x00040000000A0500,
                     // Fire Emblem Echoes: Shadows of Valentia (top flicker
                     // blocks gameplay)
                     0x00040000001B4000,
                     // Fossil Fighters: Frontier (bottom missing everywhere)
                     0x000400000012DB00,
                     // Harvest Moon 3D: The Lost Valley (bottom flicker)
                     0x000400000010F600,
                     // Harvest Moon: Skytree Village (bottom flicker)
                     0x00040000001A3500,
                     // Kingdom Hearts 3D: Dream Drop Distance (no bottom)
                     0x000400000008D300,
                     // LEGO Jurassic World (top missing at main menu; audio
                     // issue tracked in the LEGO cluster)
                     0x000400000015B000,
                     // LEGO Star Wars: The Force Awakens (confirm/cancel
                     // popup routed to wrong screen + bottom corruption)
                     0x000400000017F900,
                     // LEGO Star Wars III: The Clone Wars — EXPERIMENTAL:
                     // both screens black (TMNT-Movie-style total blockage?)
                     0x0004000000035400,
                     // Mario Sports Superstars (no bottom)
                     0x0004000000188C00,
                     // Paper Mario: Sticker Star (no top at main menu; peel
                     // animations invisible)
                     0x00040000000A5E00,
                     // Reel Fishing Paradise 3D — EXPERIMENTAL: both black
                     0x0004000000048D00,
                     // Scribblenauts Unlimited (slight top flicker)
                     0x0004000000038600,
                     // Shin Megami Tensei IV: Apocalypse (half-screen top
                     // flicker; now user-verified — was deliberately left
                     // out of batch 1)
                     0x000400000019A200,
                     // Tom Clancy's Ghost Recon: Shadow Wars (top flicker)
                     0x0004000000035A00,
                     // Xenoblade Chronicles 3D (N3DS; top flicker)
                     0x000400000F700100,

                     // Azahar 100 additions 2026-07-16 (batch 6): final
                     // Great-tier pass (user-verified; TIDs from cci headers).
                     // Azure Striker Gunvolt: Striker Pack (bottom flicker +
                     // top overlay)
                     0x00040000001A5600,
                     // Donkey Kong Country Returns 3D (assets misrender +
                     // flicker)
                     0x00040000000CCE00,
                     // Myst (no top screen)
                     0x000400000007C200,
                     // Pinball Hall of Fame: The Williams Collection (intro
                     // doesn't render — Skylanders-video-style blockage)
                     0x000400000004C600,
                     // Scribblenauts Unmasked (top flicker)
                     0x00040000000D2300,
                     // Shin Megami Tensei: Strange Journey Redux (slight top
                     // flicker)
                     0x00040000001CD200,
                     // Tekken 3D Prime Edition — EXPERIMENTAL: both screens
                     // black
                     0x0004000000080300,
                     // The Legend of Zelda: Majora's Mask 3D — EXPERIMENTAL:
                     // no top screen at menu intro + gameplay window solid
                     // white (TMNT-Movie-style blockage suspected)
                     0x0004000000125500,
                 },
         }},

        // Zero-fill FS ControlArchive output for games that hang/misbehave
        // reading the never-written stub output — see
        // HackType::FS_CONTROL_ZERO_OUTPUT. All were verified calling
        // Control with action=0, output_size=1 (Madagascar 3 log) or show
        // the matching save/extdata symptom family.
        {HackType::FS_CONTROL_ZERO_OUTPUT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // RollerCoaster Tycoon 3D (extra-data save disabled
                     // until relaunch) — CONFIRMED FIXED by zero output
                     // 2026-07-15.
                     0x0004000000051200, // USA
                     // Imagine Fashion Life (extdata/"online data" prompt) —
                     // zero output did not change it; kept gated while the
                     // real cause (likely extdata/BOSS side) is investigated.
                     0x0004000000047800, // USA
                 },
         }},

        // Vblank signal-before-present ordering (upstream #2273 slice) —
        // see HackType::VBLANK_SIGNAL_BEFORE_PRESENT. Round-1 gated
        // bisection of the GSP accuracy commit that fixed Cars 2 globally.
        {HackType::VBLANK_SIGNAL_BEFORE_PRESENT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Cars 2 — CONFIRMED FIXED 2026-07-18 (was black-screen
                     // 0fps stall since campaign start)
                     0x000400000006A400, // USA
                     // Black-screen-stall classmates, same signature:
                     // Brunswick Pro Bowling
                     0x000400000004C400, // USA
                     // Finding Nemo: Escape to the Big Blue SE
                     0x00040000000A2100, // USA
                     // Happy Feet Two
                     0x0004000000044300, // USA
                 },
         }},

        // Kernel blocking-wait diagnostic (logging only, no behavior
        // change) — see HackType::KERNEL_WAIT_DIAG. The splash-stall trio
        // parks its logic threads after N splash screens while the GX loop
        // keeps running; service logs are identical to working runs, so
        // this names the kernel object each parked thread waits on.
        {HackType::KERNEL_WAIT_DIAG,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // EMPTIED 2026-07-25. Brunswick / Finding Nemo / Happy Feet
                     // Two were gated here; the instrument emitted ~20,000 lines
                     // per session and its question is answered: no thread is
                     // parked without a waker. The audio pump (DSP semaphore
                     // event) and render thread (GSP interrupt_event) both cycle
                     // normally during the black video. Re-add a title ID here if
                     // a genuine "what is it waiting on" question comes up again.
                 },
         }},

        // NOTE 2026-07-24: the three Zeusiota-port gate entries
        // (CORE_DOWNCOUNT_HACK / IGNORE_FORMAT_REINTERPRETATION /
        // FORCE_VALIDATED_PICA_INDEX) were REMOVED after user testing:
        // the semantic reconstructions worsened Mario Tennis/Golf and
        // fixed none of the 5-game group. The mechanisms remain in code
        // (inert without entries). Do not re-gate without decoding the
        // real Zeusiota implementations from their binaries first.

        // Screen-shift pair — see HackType::FB_FLIP_RECT_DISABLE.
        {HackType::FB_FLIP_RECT_DISABLE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             // No titles: trialed on 7th Dragon III + Samus Returns
             // 2026-07-25 and FALSIFIED — gate armed (log-verified per
             // title), zero visible change on either. The quarter-screen
             // shift does not come from the PR #699 flip-rect mirror.
             .affected_title_ids = {},
         }},

        // Bottom screen via CPU upload — see
        // HackType::FORCE_CPU_BOTTOM_SCREEN.
        {HackType::FORCE_CPU_BOTTOM_SCREEN,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             // No titles: trialed on Ultimate NES Remix 2026-07-24 and
             // removed — its bottom screen was black because the
             // right-eye disabler dropped the blit entirely, not because
             // the accelerated path presented a stale surface. Mechanism
             // kept for the genuine stale-surface case.
             .affected_title_ids = {},
         }},

        // Per-screen framebuffer config probe — see
        // HackType::FB_CONFIG_DIAG. Log-only.
        {HackType::FB_CONFIG_DIAG,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Arm a title here to log its per-screen framebuffer
                     // config plus every DisplayTransfer/TextureCopy
                     // source->dest — the pair that identified the
                     // Ultimate NES Remix root cause.
                     // No titles armed.
                 },
         }},

        // Sub-game launch in compilations — see
        // HackType::PRESERVE_APP_JUMP_PARAMS.
        {HackType::PRESERVE_APP_JUMP_PARAMS,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     0x00040000001D9300, // Atooi Collection (id from its own jump log)
                     0x0004000000185E00, // Sega 3D Classics Collection
                 },
         }},

        // TT Games / LEGO engine audio — see HackType::FORCE_DSP_LLE.
        // All IDs read from the user's own cci headers (USA).
        {HackType::FORCE_DSP_LLE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     0x000400000011B800, // LEGO The Hobbit
                     0x000400000007A900, // LEGO The Lord of the Rings
                     0x00040000000AD500, // LEGO City Undercover: The Chase Begins
                     0x000400000005A200, // LEGO Batman 2: DC Super Heroes
                     0x000400000011DD00, // LEGO Batman 3: Beyond Gotham
                     0x00040000000D2000, // LEGO Marvel Super Heroes
                     0x0004000000118C00, // LEGO Ninjago: Nindroids
                     0x000400000014EB00, // LEGO Ninjago: Shadow of Ronin
                     0x0004000000037400, // LEGO Pirates of the Caribbean
                     0x0004000000105A00, // The LEGO Movie Videogame
                     0x00040000000AF800, // LEGO Legends of Chima
                     0x0004000000168500, // LEGO Marvel Avengers
                     0x000400000015B000, // LEGO Jurassic World
                     // Carnival Games: Wild West 3D: audio crackles under HLE (user); plain LLE
                     // holds 100% speed / 29-30 fps with the 1 us AUTO_RENDER_THREAD_DELAY.
                     0x0004000000061400, // USA
                     // Not gated (no audio complaint recorded, and LLE
                     // costs CPU): LEGO Friends 0x00040000000EA700,
                     // LEGO SW Force Awakens 0x000400000017F900. The two
                     // black-screen titles (LEGO Harry Potter 5-7
                     // 0x000400000004A400, LEGO Star Wars III
                     // 0x0004000000035400) are moot until they render.
                 },
         }},

        // Torus Games engine (TSG_* save files): reads Control action=0's
        // output byte as a commit/ready status; with 0 it creates its save
        // file but never writes a byte (Croods/RotG: 0-byte TSG_* saves ->
        // "corrupted save data" every boot; Madagascar 3: stalls before the
        // file is even created). Empirical: fill output with 1.
        {HackType::FS_CONTROL_ONE_OUTPUT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Madagascar 3: The Video Game
                     0x0004000000087D00, // USA
                     // The Croods: Prehistoric Party
                     0x00040000000B9B00, // USA
                     // Rise of the Guardians
                     0x00040000000B0400, // USA
                 },
         }},

        // Simulated GPU timings off — see HackType::DISABLE_GPU_TIMING_SIM.
        // CONFIRMED FIX 2026-07-25 (user-verified, all three render): Azahar
        // delays GPU completion interrupts; Citra 2104 signals them
        // immediately. These titles submit a small command list, wait for
        // completion, then decide what to configure — and lose that race.
        // Found by diffing the GX command stream against Citra: the game
        // issued 8 submissions during boot where Citra received 16, and the
        // two missing lists (0x140035E0 size 0xB0, 0x14003690 size 0x30) were
        // the render-target setup. Everything else measured identical:
        // service call sequence AND results, first 220 PICA registers,
        // display-transfer output. See memory lego_black_screen.
        {HackType::DISABLE_GPU_TIMING_SIM,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // LEGO Harry Potter: Years 5-7 (was BROKEN, both black)
                     0x000400000004A400, // USA
                     // LEGO Star Wars III: The Clone Wars (was BROKEN)
                     0x0004000000035400, // USA
                     // Resident Evil: The Mercenaries 3D (was NOBOOT) —
                     // different symptom, same root cause.
                     0x0004000000035900, // USA
                     // Rayman 3D: the silver lum Globox coughs up in the prison-ship
                     // cutscene (and Rayman's energy aura) never render. Found
                     // 2026-09-28 by a 9-step bisect judged at that frame (stock 2125
                     // good -> A100 bad): first bad = upstream 5ddbaeae2 "gsp: Fix GPU
                     // interrupt queue and add GPU timing emulation" (#2095).
                     0x0004000000036400, // USA
                 },
         }},

        // Present every frame — see HackType::FORCE_PRESENT_EVERY_FRAME.
        // Found 2026-09-24 by git bisect (7 steps, judged from az_probe
        // screenshots) between upstream b3ee2d8ac / stock 2125 (good) and A100
        // (bad): first bad commit is upstream 8c4e8b77b "Add Skip Presenting
        // Duplicate Frames Feature (#1867)". Verified on A100 by toggling ONLY
        // use_skip_duplicate_frames: off -> intros play, on -> black. Both
        // titles show "App: 0 FPS" through their intros even on stock - the
        // videos are CPU-drawn, so no new GPU frame is ever counted and the
        // feature treats every frame as a duplicate.
        {HackType::FORCE_PRESENT_EVERY_FRAME,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Brunswick Pro Bowling: Crave logo video missing and the
                     // tap-to-continue screen frozen as a near-black ghost.
                     0x000400000004C400, // USA
                     // Pinball Hall of Fame: The Williams Collection: the
                     // bottom-screen intro video rendered solid black.
                     0x000400000004C600, // USA
                     // Rayman 3D: bottom-screen "running Rayman" load animation
                     // (and "saving" text) never shown. Found 2026-09-25 by its
                     // own 7-step bisect (stock 2125 good -> A100 bad): same
                     // first bad commit, 8c4e8b77b (#1867).
                     0x0004000000036400, // USA
                 },
         }},

        // Invalid cull mode keeps previous state — see
        // HackType::CULL_MODE_INVALID_KEEPS_STATE. Found 2026-09-24 by a 10-step
        // bisect (first bad = upstream 5e2161d90, #1059) and an A/B GPU-state probe
        // of that commit against its parent.
        {HackType::CULL_MODE_INVALID_KEEPS_STATE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Reel Fishing Paradise 3D: intros never rendered, then
                     // black/white screens at 60fps (all geometry culled).
                     0x0004000000048D00, // USA
                 },
         }},

        // Pre-#69 validation-skip rule — see HackType::LEGACY_VALIDATION_SKIP.
        // Found 2026-09-24 by a 9-step bisect (first bad = upstream 878bbf530).
        {HackType::LEGACY_VALIDATION_SKIP,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Girls' Fashion Shoot: salon (and other) backgrounds show
                     // stale fragments of earlier screens' textures.
                     0x00040000000B6300, // USA
                 },
         }},

        // Hardware vertex-color saturate — see HackType::HW_VERTEX_COLOR_SATURATE.
        // Found 2026-09-25 by a second bisect (#2159 reverted at every step).
        {HackType::HW_VERTEX_COLOR_SATURATE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Ace Combat: Assault Horizon Legacy: distance haze replaced by a
                     // hard cyan band; distant islands drawn sharp.
                     0x0004000000064900, // USA
                 },
         }},

        // Shadow tie counts as lit — see HackType::SHADOW_EQUAL_DEPTH_LIT.
        // Found 2026-09-29 without an emulator oracle (stock 2125 and Citra 2104 show the
        // same static): a forced-lit A/B localised it to the shadow term, and a shadow-map
        // readback showed every depth at the clear value.
        {HackType::SHADOW_EQUAL_DEPTH_LIT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Petz Beach: floor shows black/white static (visible behind the
                     // title-screen logo).
                     0x0004000000077C00, // USA
                     // Petz Countryside: same engine, same floor static.
                     0x0004000000077B00, // USA
                 },
         }},

        // Clear through a framebuffer, not glClearTexSubImage — see HackType::FORCE_FBO_CLEAR.
        // Found 2026-09-29 without an emulator oracle (stock 2125 and Citra 2104 flicker too).
        {HackType::FORCE_FBO_CLEAR,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Donkey Kong Country Returns 3D: title-screen sky (and in-game
                     // assets) flicker to black.
                     0x00040000000CCE00, // USA
                     // Xenoblade Chronicles 3D: title screen dropped to black in some
                     // frames and rendered dark/washed-out (grey sky); with the FBO clear
                     // path it is steady with the correct blue sky and green field (20/20).
                     0x000400000F700100, // USA (New 3DS)
                     // Fire Emblem Awakening: during avatar creation the bottom screen's
                     // character portrait and menu did not render (mostly black); with the
                     // FBO clear path the whole bottom screen renders (user-confirmed).
                     0x00040000000A0500, // USA
                 },
         }},

        // SaveData free space = formatted size — see HackType::SAVEDATA_FREE_BYTES_FROM_FORMAT.
        // Found 2026-09-29 by decompiling Madagascar 3's save routine (no emulator oracle:
        // stock 2125 and Citra 2104 fail the same way).
        {HackType::SAVEDATA_FREE_BYTES_FROM_FORMAT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Madagascar 3: The Video Game: every save "corrupted", nothing written.
                     0x0004000000087D00, // USA
                     // The Croods: Prehistoric Party (same Torus engine).
                     0x00040000000B9B00, // USA
                     // Rise of the Guardians (same Torus engine).
                     0x00040000000B0400, // USA
                 },
         }},

        // Run pending GX commands on GPU-right acquire — see HackType::GSP_DRAIN_QUEUE_ON_ACQUIRE.
        // Found 2026-09-29 with a thread-snapshot + GX-queue probe (no emulator oracle: stock
        // 2125 and Citra 2104 freeze the same way).
        {HackType::GSP_DRAIN_QUEUE_ON_ACQUIRE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Terraria: UI freezes after character/world creation (after the
                     // software keyboard closes); saves were written, restart was needed.
                     0x000400000016A900, // USA
                 },
         }},

        // Unlink destroyed effect entities — see HackType::JAWS_ENTITY_UNLINK.
        // Found 2026-09-30 with a destructor hook + scripted play (no emulator oracle: stock
        // 2125 and Citra 2104 crash the same way).
        {HackType::JAWS_ENTITY_UNLINK,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // JAWS: Ultimate Predator: crashes in gameplay (NoExecuteFault).
                     0x0004000000048600, // USA
                 },
         }},

        // Async-task wake guard on Stop — see HackType::ASYNC_WAKE_SHUTDOWN_GUARD.
        // Found 2026-09-30 with an in-process crash stack + RunAsync start/end probe
        // (stock 2125 and Citra 2104 crash the same way on Stop).
        {HackType::ASYNC_WAKE_SHUTDOWN_GUARD,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // RollerCoaster Tycoon 3D: Azahar crashes after Emulation > Stop.
                     0x0004000000051200, // USA
                 },
         }},

        // Automatic render-thread delay — see HackType::AUTO_RENDER_THREAD_DELAY.
        // Titles the user runs with "Delay game render thread" = 5.77 ms.
        {HackType::AUTO_RENDER_THREAD_DELAY,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     0x0004000000052400, // WWE All Stars (USA): matches slow down without it
                     0x0004000000040600, // Captain America: Super Soldier (USA): 2.25 ms
                     0x0004000000061400, // Carnival Games: Wild West 3D (USA): 1 us (yield only)
                 },
         }},

        // Per-draw lookup reuse — see HackType::DRAW_LOOKUP_REUSE.
        {HackType::DRAW_LOOKUP_REUSE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     0x0004000000052400, // WWE All Stars (USA): slow wrestler entrances
                 },
         }},

        // NaN float32 uniforms stored as 0 — see HackType::FLOAT_UNIFORM_NAN_TO_ZERO.
        {HackType::FLOAT_UNIFORM_NAN_TO_ZERO,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Resident Evil: The Mercenaries 3D (USA): invisible boot message box.
                     0x0004000000035900,
                 },
         }},

        // Citra-2104 GSP PDC delivery — see HackType::GSP_LEGACY_PDC_QUEUE.
        // Both titles run with ZERO framebuffer errors on Citra 2104 while
        // A100 logs 2500+ per session, and the rasterizer guard that lets
        // the null framebuffer through is byte-identical between the two.
        // The divergence is Azahar's PDC suppression, which also skips the
        // interrupt signal these titles wait on.
        {HackType::GSP_LEGACY_PDC_QUEUE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // LEGO Harry Potter: Years 5-7
                     0x000400000004A400, // USA
                     // LEGO Star Wars III: The Clone Wars
                     0x0004000000035400, // USA
                 },
         }},

        // Code dump for the Torus save investigation — the game's own
        // ControlArchive caller is the only remaining source of truth for
        // what it expects back from action 0 / input 0xB0.
        {HackType::DUMP_CODE_BIN,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Madagascar 3: The Video Game
                     0x0004000000087D00, // USA
                 },
         }},

        // Diagnostic for the Torus save cluster: all three reach gameplay
        // but leave their TSG_* save files at 0 bytes. Logs guest writes,
        // resizes and closes at the disk backend plus the Control input
        // bytes, to decide whether the guest issues writes we drop or
        // never issues them at all.
        {HackType::FS_SAVE_DIAG,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Madagascar 3: The Video Game
                     0x0004000000087D00, // USA
                     // The Croods: Prehistoric Party
                     0x00040000000B9B00, // USA
                     // Rise of the Guardians
                     0x00040000000B0400, // USA
                 },
         }},

        // The following games require accurate multiplication to render properly.
        {HackType::ACCURATE_MULTIPLICATION,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // The Legend of Zelda: Ocarina of Time 3D
                     0x0004000000033400, // JPN
                     0x0004000000033500, // USA
                     0x0004000000033600, // EUR
                     0x000400000008F800, // KOR
                     0x000400000008F900, // CHI

                     // Mario & Luigi: Superstar Saga + Bowsers Minions
                     0x00040000001B8F00, // USA
                     0x00040000001B9000, // EUR
                     0x0004000000194B00, // JPN

                     // Mario & Luigi: Bowsers Inside Story + Bowser Jrs Journey
                     0x00040000001D1400, // USA
                     0x00040000001D1500, // EUR
                     0x00040000001CA900, // JPN

                     // Mario & Luigi: Paper Jam
                     0x0004000000132600, // JPN
                     0x0004000000132700, // USA
                     0x0004000000132800, // EUR
                     0x000400000018A100, // EUR (Demo)
                 },
         }},

        {HackType::DECRYPTION_AUTHORIZED,
         HackEntry{
             .mode = HackAllowMode::ALLOW,
             .affected_title_ids =
                 {
                     // NIM
                     0x0004013000002C02, // Normal
                     0x0004013000002C03, // Safe mode
                     0x0004013020002C03, // New 3DS safe mode

                     // DLP
                     0x0004013000002802,
                 },
         }},

        // Games whose vertex shader assumes hardware-normalized u8/s8 color
        // attributes. Without this hack the VS sees float(0..255) instead of
        // (0..1), and TEV multiplies saturate to flat ceiling values — the
        // user-picked dress / skin / hair colors come out as uniform dark tints.
        {HackType::NORMALIZE_VERTEX_COLORS,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Princess: My Fairytale Adventure
                     0x0004000000083900, // USA
                 },
         }},

        // Games whose ETC1 texture data writes bypass MemorySystem::Write (likely
        // dynarmic JIT optimization). The cached host pointer the JIT uses may be
        // stale once Citra marks the page RasterizerCachedMemory, so writes land
        // in stale host memory while reads from the surface see zeros. Substitute
        // mid-gray ETC1 content when the source memory is all zeros to recover
        // the dress/skin colors at the cost of fabric shading detail.
        {HackType::MID_GRAY_ETC1_FALLBACK,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Princess: My Fairytale Adventure
                     0x0004000000083900, // USA
                 },
         }},

        // PMG3D — force tex0 FS sampling at base mip (textureLod = 0);
        // the intro logos' higher mips are never populated so the
        // computed-LOD sample returns black.
        {HackType::PMG3D_FORCE_WHITE_AMBIENT,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Puzzler Mind Gym 3D
                     0x000400000004EC00, // USA
                 },
         }},

        // PMG3D — relax the PICA z<=0 clip plane on the VS /
        // trivial-GS paths ONLY (2026-07-04 refinement). The unlit 2D
        // draws (logos, UI, signs, backgrounds) emit negative-z
        // vertices and need the bypass; the lit 3D meshes (building,
        // host, doors) go through the fixed-GS path, which now stays
        // strict unconditionally — verified they render correctly
        // with natural clipping once the relax stops touching them.
        {HackType::PMG3D_RELAX_CLIP_PLANE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Puzzler Mind Gym 3D
                     0x000400000004EC00, // USA
                 },
         }},

        // Defer GX completion interrupts to restore real-hardware
        // ordering — see HackType::GX_COMPLETION_IRQ_DELAY. Proven
        // root-cause fix for MFA's stalled asset loader (the
        // missing-arms bug); candidate fix for PMG3D which links the
        // same SDK GX driver.
        {HackType::GX_COMPLETION_IRQ_DELAY,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Princess: My Fairytale Adventure
                     0x0004000000083900, // USA
                     // Puzzler Mind Gym 3D
                     0x000400000004EC00, // USA
                     // REMOVED 2026-07-25 — LEGO Harry Potter: Years 5-7
                     // (0x000400000004A400) and LEGO Star Wars III: The
                     // Clone Wars (0x0004000000035400) were added here on
                     // 2026-07-16 by pattern-matching a GDB LightLock park
                     // to MFA's lost-wakeup signature. They were never
                     // verified to help and both stayed BROKEN. Measured
                     // 2026-07-25: every draw reaches Draw() with
                     // color_addr=0 AND depth_addr=0 while write_color=true
                     // (mask rgba=1110), producing a framebuffer with no
                     // attachments (GL_FRAMEBUFFER_INCOMPLETE_MISSING_
                     // ATTACHMENT 0x8CD7) and a flood of 1286. The guard
                     // that lets that through is byte-identical to Citra
                     // 2104, so the divergence is not there — it is this
                     // A100-exclusive hack, the one display/GPU-timing
                     // feature these two have that Citra lacks entirely.
                     // Pulled to test causation. If they render, this stays
                     // out; if not, re-add and look elsewhere.
                 },
         }},

        // Overbright vertex-color headroom — see
        // HackType::OVERBRIGHT_VERTEX_COLORS. Was a global clamp change
        // during the MFA color investigation; gated 2026-07-08 because
        // it subtly shifts colors in unrelated games.
        // Frogger 3D white-frog DIAGNOSTIC — see HackType::FROGGER_SPECULAR_BLUE.
        // From a RenderDoc capture of the title screen (frog = prog 490,
        // EID 347-507, 9 draws sharing one material), its TEV collapses to:
        //     out = texture * secondary_fragment_color.b + (1 - secondary_fragment_color.b)
        // a white->texture lerp keyed on SPECULAR BLUE. The frog is PURE white,
        // so spec.b must be ~0. Forcing spec.b = 1.0 should snap it to its
        // texture; that CONFIRMS the chain and narrows the real cause to LUT
        // slot 4 (refl_value.b) / specular_0.b / specular_1.b / clamp_highlights.
        // If it does not change, the reading is wrong. REVERT either way -
        // this is a diagnostic, not the fix.
        {HackType::FROGGER_SPECULAR_BLUE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     0x0004000000036900, // Frogger 3D USA
                 },
         }},

        {HackType::OVERBRIGHT_VERTEX_COLORS,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Princess: My Fairytale Adventure
                     0x0004000000083900, // USA
                     // (Frogger 3D 0x0004000000036900 trialled here 2026-07-25
                     // for its WHITE-instead-of-green frog — NO CHANGE, removed.
                     // The direction concern was correct: this raises the
                     // vertex-colour clamp 1.0 -> 2.0, which brightens, and
                     // Frogger is already too bright. MEASURED RESULT: the
                     // frog's white does NOT come from the pre-interpolation
                     // vertex-colour clamp. Remaining candidates are the TEV
                     // combiner (texture not reaching the multiply) and
                     // lighting. Do not re-add.)
                 },
         }},

        // Right-eye display fallback — see
        // HackType::RIGHT_EYE_DISPLAY_FALLBACK. Disney Infinity
        // programs a zero left-eye framebuffer address; without the
        // fallback the screen flashes gray/black (no logo) and the log
        // spams per-frame address-0 errors.
        {HackType::RIGHT_EYE_DISPLAY_FALLBACK,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Infinity: Toy Box Challenge (runtime-confirmed
                     // 2026-07-08 via recentFiles + session log)
                     0x00040000000B9C00, // USA
                     // (WWE All Stars 0x0004000000052400 was listed here
                     // 2026-07-08 as an "unidentified" title and removed
                     // 2026-10-01: neither the display fallback nor the Y2R
                     // completion branch ever fired in a 4.4-minute run
                     // through boot, menus, entrances and a match.)
                     // (Skylanders titles were trialed here 2026-07-09 and
                     // removed same day: FB-DIAG proved their video phase
                     // presents valid buffers — their video bug is in the
                     // texture path, not the display path.)
                 },
         }},

        // Pre-#1218 per-buffer MemoryFill interrupts — see
        // HackType::MEMORYFILL_PER_BUFFER_IRQ. Mobiclip video engines
        // need PSC1 for buffer-1 fills. All IDs read from the user's
        // cci headers (region-exact).
        {HackType::MEMORYFILL_PER_BUFFER_IRQ,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Disney Infinity: Toy Box Challenge
                     0x00040000000B9C00, // USA
                     // Unidentified DI-symptom title (see fallback entry)
                     0x0004000000052400,
                     // Skylanders: Spyro's Adventure
                     0x0004000000036E00, // USA
                     // Skylanders: Giants
                     0x0004000000091D00, // USA
                     // Skylanders: Swap Force
                     0x00040000000E6500, // USA
                     // Skylanders: Trap Team
                     0x0004000000131200, // USA
                     0x000400000013BE00, // EUR
                     // Shin Megami Tensei: Devil Survivor Overclocked
                     // (intro movie renders as color blocks)
                     0x0004000000038800, // USA
                     // Madagascar 3 (Torus): entering gameplay hard-freezes
                     // with 60Hz zero-address framebuffer presents — the
                     // Mobiclip-style video pump livelock signature
                     // (2026-07-15 log). Game never reaches its first
                     // Control call post-menu.
                     0x0004000000087D00, // USA
                     // Pinball Hall of Fame: The Williams Collection —
                     // EXPERIMENTAL 2026-07-16: bottom-screen intro never
                     // renders; LOD-0 hypothesis already ruled out. If the
                     // intro is a Mobiclip-style video, the missing PSC1
                     // signaling would explain it. (Partial: recovered the
                     // loading screen; intro still missing.)
                     0x000400000004C600, // USA
                     // Finding Nemo: Escape to the Big Blue SE — found
                     // 2026-09-24 by a 10-step git bisect from Citra 2104
                     // (good) to stock Azahar (bad): first bad commit is
                     // upstream 0453e4463 (#1218) itself. Its video player
                     // converts 2 frames through Y2R, then waits on the PSC1
                     // that #1218 stopped raising and never runs again
                     // (37 Y2R calls vs 23,000 on good builds).
                     0x00040000000A2100, // USA
                     // REMOVED 2026-07-25 — LEGO Harry Potter: Years 5-7
                     // (0x000400000004A400) and LEGO Star Wars III: The
                     // Clone Wars (0x0004000000035400) were added here
                     // 2026-07-16 on a symptom guess ("Mobiclip pump
                     // livelock signature"), never validated, and both
                     // stayed BROKEN. Second speculative hack pulled off
                     // this pair the same day as GX_COMPLETION_IRQ_DELAY;
                     // testing whether either was suppressing the colour-
                     // buffer register programming (measured: color_addr
                     // stays 0x0 for 100% of draws). Disney Infinity and
                     // the Skylanders keep this hack — validated there.
                 },
         }},

        // HackType::SKIP_CORE1_PREEMPTION — core-1 CPU-time limiter parks
        // the title's core-1 worker mid-computation and never wakes it.
        {HackType::SKIP_CORE1_PREEMPTION,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Terraria (world generation worker)
                     0x000400000016A900, // USA Rev 1
                     // Madagascar 3 (Torus): save-write freeze. GDB at the
                     // freeze (2026-07-15): main thread live-spins polling a
                     // resource registry for hash 0x79996319 that an async
                     // save job should register; worker threads parked in
                     // ArbitrateAddress/WaitSyncN; the job's FS op never
                     // reaches the FS layer. Same parked-worker signature as
                     // Terraria's world-gen bug.
                     0x0004000000087D00, // USA
                 },
         }},

        // HackType::KEEP_LAST_FRAME_ON_ZERO_FB — these games zero the
        // bottom-screen framebuffer address during full-screen videos;
        // keep the last presented image instead of showing black.
        {HackType::KEEP_LAST_FRAME_ON_ZERO_FB,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Skylanders: Giants
                     0x0004000000091D00, // USA
                     // Skylanders: Swap Force
                     0x00040000000E6500, // USA
                 },
         }},

        // Graceful JIT interpreter fallback — see
        // HackType::GRACEFUL_JIT_FALLBACK. Trap Team hits instructions
        // dynarmic can't JIT mid-game; stock Azahar crashes there
        // (UNREACHABLE). Every other title keeps the stock loud-crash
        // behavior so real bugs are never silently skipped.
        {HackType::GRACEFUL_JIT_FALLBACK,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Skylanders Trap Team
                     0x0004000000131200, // USA (runtime-confirmed from the
                                         // 2026-07-08 fallback crash log)
                     0x000400000013BE00, // EUR
                 },
         }},

        {HackType::ONLINE_LLE_REQUIRED,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // eShop
                     0x0004001000020900, // JPN
                     0x0004001000021900, // USA
                     0x0004001000022900, // EUR
                     0x0004001000027900, // KOR
                     0x0004001000028900, // TWN

                     // System Settings
                     0x0004001000020000, // JPN
                     0x0004001000021000, // USA
                     0x0004001000022000, // EUR
                     0x0004001000026000, // CHN
                     0x0004001000027000, // KOR
                     0x0004001000028000, // TWN

                     // Nintendo Network ID Settings
                     0x000400100002BF00, // JPN
                     0x000400100002C000, // USA
                     0x000400100002C100, // EUR

                     // System Settings
                     0x0004003000008202, // JPN
                     0x0004003000008F02, // USA
                     0x0004003000009802, // EUR
                     0x000400300000A102, // CHN
                     0x000400300000A902, // KOR
                     0x000400300000B102, // TWN

                     // Pretendo Network's Nimbus
                     0x000400000D40D200,
                 },
         }},

        {HackType::REGION_FROM_SECURE,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // eShop
                     0x0004001000020900, // JPN
                     0x0004001000021900, // USA
                     0x0004001000022900, // EUR
                     0x0004001000027900, // KOR
                     0x0004001000028900, // TWN

                     // System Settings
                     0x0004001000020000, // JPN
                     0x0004001000021000, // USA
                     0x0004001000022000, // EUR
                     0x0004001000026000, // CHN
                     0x0004001000027000, // KOR
                     0x0004001000028000, // TWN

                     // Nintendo Network ID Settings
                     0x000400100002BF00, // JPN
                     0x000400100002C000, // USA
                     0x000400100002C100, // EUR

                     // System Settings
                     0x0004003000008202, // JPN
                     0x0004003000008F02, // USA
                     0x0004003000009802, // EUR
                     0x000400300000A102, // CHN
                     0x000400300000A902, // KOR
                     0x000400300000B102, // TWN

                     // NIM
                     0x0004013000002C02, // Normal
                     0x0004013000002C03, // Safe mode
                     0x0004013020002C03, // New 3DS safe mode

                     // ACT
                     0x0004013000003802, // Normal

                     // FRD
                     0x0004013000003202, // Normal
                     0x0004013000003203, // Safe mode
                     0x0004013020003203, // New 3DS safe mode
                 },
         }},
        {HackType::REQUIRES_SHADER_FIXUP,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // 3D Thunder Blade
                     0x0004000000128A00, // JPN
                     0x0004000000158200, // EUR
                     0x0004000000158C00, // USA

                     // 3D After Burner II
                     0x0004000000114200, // JPN
                     0x0004000000157A00, // EUR
                     0x0004000000158900, // USA

                     // 3D Classics
                     0x0004000000154000, // 1 (JPN)
                     0x0004000000180E00, // 2 (JPN)
                     0x000400000019A700, // 2 (EUR)
                     0x0004000000185E00, // 2 (USA)
                     0x00040000001AA300, // 3 (JPN)
                 },
         }},
        {HackType::SPOOF_FRIEND_CODE_SEED,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Luigi's Mansion 3ds
                     0x00040000001D1800, // JPN
                     0x00040000001D1900, // USA
                     0x00040000001D1A00, // EUR
                 },
         }},
        {HackType::DELAY_TEXTURE_COPY_COMPLETION,
         HackEntry{
             .mode = HackAllowMode::FORCE,
             .affected_title_ids =
                 {
                     // Super Mario 3D Land
                     0x0004000000054100, // JPN
                     0x0004000000054000, // USA
                     0x0004000000053F00, // EUR
                     0x0004000000089E00, // CHN
                     0x0004000000089D00, // KOR
                 },
         }},
    }};
}