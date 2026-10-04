// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Common::Hacks {

enum class HackType : int {
    RIGHT_EYE_DISABLE,
    ACCURATE_MULTIPLICATION,
    DECRYPTION_AUTHORIZED,
    ONLINE_LLE_REQUIRED,
    REGION_FROM_SECURE,
    REQUIRES_SHADER_FIXUP,
    SPOOF_FRIEND_CODE_SEED,
    DELAY_TEXTURE_COPY_COMPLETION,
    // ---- Azahar 100 additions below ----
    // Forces PICA UBYTE/BYTE vertex attributes to be normalized at fetch
    // (glVertexAttribPointer with normalized=GL_TRUE). Default Citra/Azahar
    // behavior passes them raw 0..255, which destroys per-vertex color ratios
    // for games whose VS assumes normalized input. Disney Princess: My
    // Fairytale Adventure's dress/skin/hair color comes through this path.
    NORMALIZE_VERTEX_COLORS,
    // When an ETC1 surface uploads from all-zero source memory, substitute
    // mid-gray ETC1 data instead. Workaround for the Disney Princess: My
    // Fairytale Adventure character-creator textures whose data flow Citra
    // doesn't observe (game writes appear to bypass MemorySystem::Write,
    // probably via dynarmic JIT-optimized memcpy). Without this, the TEV
    // multiply chain (vertex × tex0 × tex1 × 2) collapses to zero and the
    // dress/skin renders as `const_color - 0.5` = dark olive. Mid-gray = 0.5
    // makes the multiply produce ~0.5, then the AddSigned with const_color
    // gives the full chosen color. Loses fabric shading detail but recovers
    // user-picked dress/skin colors.
    MID_GRAY_ETC1_FALLBACK,
    // Puzzler Mind Gym 3D — force tex0 sampling at base mip (textureLod = 0)
    // in the generated FS. PMG3D's intro logo ETC1 textures never have their
    // higher mip levels populated in Azahar's upload path, so the standard
    // computed-LOD sample returns (0,0,0,0) and the logos render black.
    // Forcing LOD 0 reads only the populated base mip. The legacy enum
    // name is preserved for source compatibility with prior builds; the
    // actual effect is LOD-0 sampling.
    PMG3D_FORCE_WHITE_AMBIENT,
    // Puzzler Mind Gym 3D — make the VS / trivial-GS clip sites emit
    // `gl_ClipDistance[0] = 1.0` instead of `-vtx_pos.z` so PICA's z<=0
    // clip plane never discards the unlit 2D draws (logos, UI, signs,
    // backgrounds) whose shaders write negative z. As of 2026-07-04
    // the fixed-GS path is ALWAYS strict (not affected by this hack):
    // the lit 3D meshes (building, host, doors) render correctly with
    // natural clipping and broke under the old whole-pipeline relax.
    PMG3D_RELAX_CLIP_PLANE,
    // Disable dynarmic's inline page-table fast path so every CPU
    // memory access goes through MemorySystem callbacks, which respect
    // RasterizerCachedMemory page attributes. General accuracy tool for
    // titles whose CPU writes to rasterizer-cached pages are lost
    // through the inline fast path. Costs CPU performance. NOTE: was
    // trialed as an MFA fix and ruled out — MFA's real bug was the GX
    // completion-IRQ ordering (see GX_COMPLETION_IRQ_DELAY below); no
    // titles currently force this, but the mechanism is kept for
    // future use.
    ACCURATE_JIT_MEMORY,
    // Defer GX completion interrupts (DMA/P3D/PPF/PSC0/PSC1) by a
    // sliver of emulated time instead of signaling them synchronously
    // inside command execution. Restores real-hardware ordering where
    // a command's completion interrupt always arrives AFTER the game's
    // submit path finishes its post-submit bookkeeping. Disney
    // Princess MFA's software command pump halts itself after
    // submitting a stop-flagged entry and relies on that entry's
    // completion interrupt to clear the halt; synchronous delivery
    // clears the halt BEFORE it is set, permanently stalling the
    // asset loader (missing arms / dress sections / wardrobe
    // textures — broken in every Citra-family emulator to date).
    GX_COMPLETION_IRQ_DELAY,
    // Instead of crashing (stock UNREACHABLE) when dynarmic hits an
    // instruction it can't JIT, interpret/skip it and keep running.
    // Fixed Skylanders Trap Team crashing mid-game. Title-gated
    // because silently skipping unknown instructions is unsafe as a
    // global default: any other title reaching this path would corrupt
    // quietly instead of failing loudly.
    GRACEFUL_JIT_FALLBACK,
    // Raise the pre-interpolation vertex-color clamp from PICA's
    // saturate-to-1.0 to 2.0 of headroom. Disney Princess: My Fairytale
    // Adventure needs the headroom for its PrimaryColor * Texture0
    // overbright multiply (pink fabric turns burgundy without it).
    // Title-gated: the extra headroom subtly shifts colors in games
    // that rely on the hardware saturate.
    OVERBRIGHT_VERTEX_COLORS,
    // DIAGNOSTIC (Frogger 3D): force secondary_fragment_color.b = 1.0. Its
    // material lerps white->texture using specular BLUE as the blend factor;
    // spec.b reads ~0 so the model renders pure white. Confirms or kills that.
    FROGGER_SPECULAR_BLUE,
    // Display-present fallback for games that program a ZERO left-eye
    // framebuffer address and put the real frame in the right-eye
    // address slot (observed: Disney Infinity — without this the
    // screen flashes gray/black and the log spams
    // "Unknown virtual address @ 0x00000000" once per frame). When the
    // left address is 0, present the right-eye buffer instead, with
    // the RGB8/BGR + stride-0 quirks that path carries. Title-gated:
    // this exact fallback ran globally in an earlier build and caused
    // the "VHS tape" corruption in Luigi's Mansion / MH3U / RE:R.
    RIGHT_EYE_DISPLAY_FALLBACK,
    // Restore pre-#1218 GX MemoryFill interrupt signaling: buffer-0
    // fills raise PSC0 and buffer-1 fills raise PSC1 even when both
    // are set in one command (upstream 0453e4463 dispatches only PSC0
    // for dual fills). Mobiclip video engines fill both buffers per
    // frame and wait on BOTH interrupts; without PSC1 their pump
    // livelocks (DI: black after intros; Skylanders titles: videos
    // don't render). Bisect-confirmed root cause.
    MEMORYFILL_PER_BUFFER_IRQ,
    // Skip core-1 CPU-time preemption (upstream #1934) for the title.
    // The limiter force-sleeps over-budget core-1 app threads; Terraria's
    // world-gen worker gets parked mid-computation and never rescheduled
    // (GDB: runnable thread frozen at a leaf function, registers static).
    SKIP_CORE1_PREEMPTION,
    // When a screen's framebuffer address is programmed to 0, keep
    // presenting the previous image instead of attempting to present
    // from address 0 (which reads nothing and shows black). Skylanders
    // Giants / Swap Force zero the bottom-screen address during their
    // full-screen videos; the pre-port fork kept the last frame there
    // via its (then-global) zero-address early return.
    KEEP_LAST_FRAME_ON_ZERO_FB,
    // Zero-fill the output buffer in the (stubbed) ArchiveBackend::Control
    // path. The stock stub returns ResultSuccess without ever writing
    // output[], so games that call FS ControlArchive expecting an output
    // byte read garbage and loop/hang on it (Madagascar 3: corrupted-save
    // retry -> 0fps "Initializing data" stall; Imagine Fashion Life: stuck
    // on the extdata-create prompt; Croods / Rise of the Guardians:
    // corrupted-save prompt at boot; RollerCoaster Tycoon 3D: "extra data
    // save disabled" until a relaunch). Zeroed output = deterministic
    // "success/no-pending-state" byte for the observed action=0,
    // output_size=1 calls. Title-gated pending a real per-action
    // implementation.
    FS_CONTROL_ZERO_OUTPUT,
    // Signal the PDC0/PDC1 vblank interrupts BEFORE presenting the frame
    // in GPU::VBlankCallback (upstream #2273 ordering) instead of after.
    // Cars 2 stalls at boot forever on the legacy order (draws null-texture
    // frames endlessly, never progresses) and was confirmed fixed by the
    // upstream GSP commit; this is the smallest gateable slice of it.
    // Title-gated while the exact mechanism is confirmed per-title.
    VBLANK_SIGNAL_BEFORE_PRESENT,
    // Same as FS_CONTROL_ZERO_OUTPUT but fills the output buffer with 1.
    // The Torus Games engine (Madagascar 3 / Croods / Rise of the
    // Guardians — save files all named "TSG_*") creates its save file but
    // never writes a byte under zero output: it reads Control action=0's
    // output byte as a commit/ready status and stalls or re-prompts until
    // it sees nonzero. Empirically separated from the zero-output class
    // (RollerCoaster Tycoon needs 0). Takes precedence over ZERO if both
    // are gated for a title.
    FS_CONTROL_ONE_OUTPUT,
    // Diagnostic (no behavior change): log every save-file write, resize
    // and close at the disk backend, plus the Control input bytes the
    // guest supplies. Answers the one question the Torus save cluster
    // turns on: when a title's save file exists but stays 0 bytes, is the
    // guest issuing writes that we drop, or is it never writing at all?
    // Pair with log_filter "Service.FS:Debug" so the OpenFile lines that
    // carry the path bracket each write burst.
    // Restore Citra 2104's unconditional GSP PDC interrupt delivery.
    // Azahar drops PDC0/PDC1 when the guest sets ignore_pdc, and again
    // once the relay queue passes stop_queuing_pdc_threeshold (0x20) —
    // and in BOTH cases skips interrupt_event->Signal(). Citra queues and
    // signals every time. A title that waits on that event before setting
    // up its render targets then never advances: it loops drawing a few
    // primitives per frame into a framebuffer whose colour-buffer address
    // is never programmed. Measured on LEGO Harry Potter 5-7, which runs
    // clean on Citra 2104 with zero framebuffer errors.
    // Disable Azahar's simulated 3DS GPU timings for this title, matching
    // Citra 2104, which signals GPU completion interrupts immediately with no
    // delay at all. Azahar delays P3D/PPF completion by a computed amount
    // (gsp_gpu.cpp, guarded by Settings::values.simulate_3ds_gpu_timings).
    // Titles that submit a small command list, wait for completion, and then
    // decide what to set up next can lose that race: measured on LEGO Harry
    // Potter 5-7, which issued only 8 GX submissions during boot where Citra
    // received 16, never programmed its colour-buffer register, and rendered
    // 100% of draws into an attachment-less framebuffer.
    DISABLE_GPU_TIMING_SIM,
    // Present every host frame for this title even when "Skip Presenting
    // Duplicate Frames" (use_skip_duplicate_frames, default ON since upstream
    // #1867) is enabled. That feature presents only when the game completes a
    // new GPU frame (PerfStats::game_frames_updated). Titles that draw their
    // intro videos with the CPU never complete one ("App: 0 FPS"), so every
    // frame is judged a duplicate and the screen freezes on the first image
    // it showed. Found by git bisect between stock 2125 (good) and A100 (bad).
    FORCE_PRESENT_EVERY_FRAME,
    // Keep the previous OpenGL cull state when the game writes the invalid cull mode 3, as
    // Citra 2104 and pre-#1059 Azahar did (their SyncCullMode ignored unknown values). Upstream
    // #1059 ("video_core: Refactor state tracking", 5e2161d90) recomputes cull state every draw
    // as `enabled = mode != KeepAll`, front face CW only for mode 1 - so mode 3 culls with the
    // opposite winding and every triangle disappears. Found by bisect (Citra 2104 -> stock) plus
    // an A/B probe of the two adjacent builds: 87/96 GPU-state keys identical, the only draw-
    // state difference was cull `true,GL_BACK,GL_CCW`; Reel Fishing writes mode 3 ~16,500 times
    // in 50 s. Vulkan already maps mode 3 to no culling (pica_to_vk.h), so this is OpenGL-only.
    CULL_MODE_INVALID_KEEPS_STATE,
    // Restore the pre-#69 rasterizer-cache validation-skip rule. Upstream 878bbf530
    // ("rasterizer_cache: Improve validation skip heuristic", #69, April 2024) skips validating a
    // texture when ANY part of its range is owned by a GPU-modified surface of a different stride
    // ("texture aliasing"). Girls' Fashion Shoot writes its salon background (CPU) over memory
    // that earlier screens rendered with the GPU at another stride, so the cache keeps the old
    // pixels (clothing / calendar fragments); a save-state load, which rebuilds the cache, shows
    // the correct image. Found by a 9-step bisect Citra 2104 -> stock with the user judging each
    // build. The pre-#69 rule skipped only when the WHOLE interval was GPU-modified.
    LEGACY_VALIDATION_SKIP,
    // Restore PICA's vertex-color saturate (abs, then clamp to [0,1]) in the HARDWARE vertex
    // shader. A100 raised that clamp to 2.0 for Disney Princess (OVERBRIGHT_VERTEX_COLORS) but
    // gated only the CPU copy in output_vertex.cpp; the generated GLSL kept 2.0 for every title.
    // Ace Combat: Assault Horizon Legacy draws its distance haze with vertex colors, and at 2.0
    // the haze is replaced by a hard cyan band with sharp islands behind it. Found by a second
    // bisect with upstream #2159 reverted at every step (first bad = the upstream merge that
    // brought the A100 payload in), then proven on that build: the stock clamp alone restores
    // the haze.
    HW_VERTEX_COLOR_SATURATE,
    // Shadow-texture comparison: treat stored depth EQUAL to the fragment depth as LIT (shadowed
    // only when stored < fragment), as the software renderer does (sw_rasterizer.cpp: lit when
    // z_ref >= z_int). The GPU shader generators use <=, so a tie counts as shadow. Petz Beach /
    // Countryside keep every shadow-map depth at the clear value 0xFFFFFF and carry their shadows
    // in the intensity byte; the floor's depth clamps to 0xFFFFFF too, so under <= it flickers
    // between shadowed and lit per pixel (black/white static). Measured by reading the shadow map
    // back: cleared to 0xFFFFFFFF before each pass, and the pass wrote only 0xFFFFFFB2 texels.
    SHADOW_EQUAL_DEPTH_LIT,
    // Clear textures through a framebuffer (glClearBuffer*) instead of glClearTexSubImage, as the
    // driver-bug path BrokenClearTexture already does for Haswell/Broadwell Intel. DKC Returns 3D
    // renders both stereo eyes into one 240x800 target and clears each half separately; on
    // NVIDIA (RTX 5060, driver 610.88) the glClearTexSubImage half-clear intermittently loses the
    // left eye's rendering, so the title sky flickers to black (also on stock 2125 and Citra
    // 2104). Measured: FBO clear path 20/20 frames correct, glClearTexSubImage 0/20, same build.
    FORCE_FBO_CLEAR,
    // Report a SaveData archive's free space as the size the game formatted it to
    // (FormatSaveData total_size), like a real card save, instead of Azahar's 32 MiB stub. The
    // Torus Games engine (Madagascar 3, The Croods, Rise of the Guardians) stores save capacity as
    // a 16-bit count of 512-byte blocks computed as (free_bytes << 7) >> 16. 32 MiB overflows that
    // to 0, so every save fails its "needed > capacity" check (status 17, "data corrupted") and
    // nothing is written. Decompiled: capacity at [state+0x6A] set at 0x1531A4, checked at
    // 0x143654; the game formats 0x40000 (256 KiB).
    SAVEDATA_FREE_BYTES_FROM_FORMAT,
    // When a thread acquires the GPU right, run any commands still pending in its GX command
    // queue, and ignore TriggerCmdReqQueue while no thread holds the right (instead of reading
    // the out-of-range command buffer for thread id 0xFFFFFFFF). Terraria re-submits GX commands
    // ~3 ms BEFORE it re-acquires the right after the software keyboard closes; the trigger is
    // lost, the game only triggers again when the queue goes from 0 to 1, so its 4 commands never
    // run, their completion never arrives, and the main thread waits forever on the command-list
    // busy flag (0x835219, loop at 0x10E7F0). Measured: AcquireRight found n=4 pending.
    GSP_DRAIN_QUEUE_ON_ACQUIRE,
    // JAWS: Ultimate Predator only: unlink a destroyed effect entity (vtable 0x3E4384, destructor
    // 0x0021EAB8) from every child list the main loop walks (0x00204B50) before it is freed.
    // When the game's effect pool is full, its steal path (0x2A3EE8 -> 0x2A0568 ->
    // 0x2A0674) deletes the stolen slot's entities without removing them from that list, and
    // the next walk calls vt[8] on freed memory (crash at pc 0 / heap, lr 0x00204B88).
    // Measured: every crash follows a steal within 2 ms; happens under HLE and LLE audio and
    // with GPU timing off, and on stock 2125 / Citra 2104, so it is game logic, not a setting.
    // Implemented as two SVC trampolines patched into the guest code (original words checked).
    JAWS_ENTITY_UNLINK,
    // Emulation > Stop must not let a still-running HLE async task (HLERequestContext::RunAsync)
    // wake its guest thread after Shutdown has destroyed the kernel and timing. RollerCoaster
    // Tycoon 3D keeps a listening socket and a SOC Poll (cmd 0x0014) blocking on a pool thread;
    // Stop destroyed kernel + timing at 24.81 s, the poll timed out at 24.995 s and its
    // WakeAfterDelay hit freed memory (crash on the thread-pool worker; also on stock 2125 and
    // Citra 2104). Shutdown clears a shared guard first; tasks check it under its mutex.
    ASYNC_WAKE_SHUTDOWN_GUARD,
    // Apply a render-thread delay (the "Delay game render thread" setting, 5770 us = 5.77 ms)
    // automatically when the user has left that setting at 0. Each GX SubmitCmdList then holds
    // the submitting thread for that long, standing in for real GPU busy time. Without it,
    // per the user: WWE All Stars matches slow down, Captain America: Super Soldier runs too
    // fast (huge jumps), Carnival Games: Wild West 3D slows down. WWE gets the 5.77 ms the user
    // runs it with; Captain America gets 2.25 ms, which holds its 30 fps (it moves characters
    // per frame: 60 fps runs/jumps too far, 5.77 ms gave a slow-motion 12-14 fps); Carnival
    // gets 1 us, since it only needs the thread to yield (29 fps at 100% speed, where 5.77 ms
    // cut it to 14-22 fps). A non-zero user setting always wins.
    AUTO_RENDER_THREAD_DELAY,
    // Cut the per-draw cost of floods of tiny draws (OpenGL only). WWE All Stars wrestler
    // entrances draw their wrestler and set meshes as ~26,000 strips of 4-6 vertices a frame;
    // the entrances ran at 58-73% speed, with the GPU driver choking on the draw-call count.
    // 1) Lookup reuse: skip GetFramebufferSurfaces / SyncTextureUnits / the framebuffer
    //    invalidation when the framebuffer, viewport, scissor and texture-unit registers are
    //    unchanged AND the rasterizer cache has not changed (MutationGeneration).
    // 2) Zero-base vertex layout: single-loader attribute pointers stay at offset 0; draws reach
    //    their vertices through first/base vertex, so equal layouts make no VAO change.
    // 3) Draw merge: consecutive non-indexed draws whose GL state is provably identical (no
    //    render-register change per PicaCore::draw_merge_breaks, no VS uniform/code/swizzle or
    //    cache change) are issued as one glMultiDrawArrays; every other rasterizer entry point
    //    and SwapBuffers flushes them first. ~6 draws per GL call: entrances 100% / 60 fps.
    DRAW_LOOKUP_REUSE,
    // Store 0 for NaN components of float32-mode shader uniform writes. Resident Evil: The
    // Mercenaries 3D writes its boot "Loading / Data loaded" message box layer with c7 =
    // (1, -16, NaN, 0); the NaN is the ARM default NaN (0x7FC00000) from the game's own VFP math.
    // Its GUI vertex shader takes depth = dp4(-c10, c7) / dp4(c11, c7) where c10.z = c11.z = 0,
    // so on hardware (box visible, per the user) the NaN never reaches the depth; here it did,
    // every vertex got z = NaN and the whole box (frame and text) was clipped, leaving a black
    // screen until A. With 0 the depth clamps to -0.001 like the rest of the UI.
    FLOAT_UNIFORM_NAN_TO_ZERO,
    GSP_LEGACY_PDC_QUEUE,
    FS_SAVE_DIAG,
    // Diagnostic (no behavior change): write the decompressed .code section
    // to dump/code/<TID>_code.bin at load, for static analysis. Preferable
    // to external extraction because the loader has already applied LZSS
    // decompression. Arm a title, launch it once, then disarm.
    DUMP_CODE_BIN,
    // Diagnostic (no behavior change): log every BLOCKING kernel wait —
    // WaitSynchronization1/N with the waited object's type and name, and
    // ArbitrateAddress with the futex address — so a stalled title's log
    // names the exact kernel object its parked threads sleep on. Wired for
    // the splash-stall trio (Brunswick / Finding Nemo / Happy Feet Two)
    // whose game logic parks after N splashes while the GX loop stays
    // healthy; every service-level log line is identical to a working run,
    // so the missing wake has to be identified at the kernel-object level.
    KERNEL_WAIT_DIAG,
    // Run the DSP under LLE (real dumped firmware) instead of Azahar's
    // HLE reimplementation. The TT Games / LEGO engine's audio skips and
    // drifts out of alignment under HLE while the pipeline itself is
    // provably healthy (cubeb negotiates the correct 32728Hz rate, zero
    // underruns, no DSP errors) — and the same titles play correctly with
    // DSP set to LLE (user-verified 2026-07-24 across the cluster). So
    // the defect is in the HLE mixer, and LLE is both the workaround and
    // the ground-truth oracle for eventually fixing HLE properly.
    // Title-gated because LLE emulates the actual DSP chip and costs CPU
    // the other ~212 working titles should not have to pay.
    FORCE_DSP_LLE,
    // Preserve APT's ApplicationJumpParameters across a title reset.
    // Compilations (Atooi Collection, Sega 3D Classics) launch a sub-game
    // by application-jumping to their OWN title id with an 8-byte deliver
    // arg naming the game. Azahar can't find the title installed (it was
    // launched from a .cci), so RebootToTitle falls back to "reset current
    // title" — which is the right outcome for a self-jump. System::Reset
    // already carries deliver_arg across that reset, but NOT
    // app_jump_parameters; a fresh AppletManager has next/current title id
    // = ~0ULL, so ApplicationJumpParameters::Valid() is false and
    // GetStartupArgument never reads the restored arg. The relaunched
    // collection therefore sees no selection and shows its carousel again
    // — the "select loops back to menu" bug. Carrying the parameters over
    // completes the existing workaround. Citra 2104 has the identical
    // defect, so this is not an Azahar regression.
    PRESERVE_APP_JUMP_PARAMS,
    // Diagnostic (no behavior change): log each screen's framebuffer
    // config — both eye addresses, stride and color-fill flag — a few
    // times a second. For titles where one screen renders and the other
    // stays blank (Ultimate NES Remix bottom screen, The Smurfs), this
    // says whether the blank screen is being handed a null address, a
    // colour fill, or a valid buffer we then fail to draw.
    FB_CONFIG_DIAG,
    // Present the BOTTOM screen via the CPU-upload path instead of
    // RasterizerOpenGL::AccelerateDisplay. Measured on Ultimate NES Remix
    // (FB_CONFIG_DIAG): the bottom screen is handed a perfectly valid
    // double-buffered VRAM framebuffer (240x320, stride 720, no colour
    // fill) yet renders black, while the top screen — same code path —
    // is fine. AccelerateDisplay is byte-identical to Citra 2104, so the
    // stale match happens inside the (heavily modified) rasterizer cache
    // surface lookup. Same failure the Giants/Swap Force gate already
    // works around via force_cpu_bottom; this exposes that mechanism on
    // its own, without dragging in the zero-address behaviour.
    FORCE_CPU_BOTTOM_SCREEN,
    // Skip the viewport/scissor VerticalMirror that Azahar PR #699
    // ("Implement framebuffer vertical flip flag") applies when the PICA
    // framebuffer sets IsFlipped. The mirror is computed within the
    // SURFACE rect height, so a full-height viewport is positionally
    // unaffected — but a partial-height viewport into a taller shared
    // render target gets RELOCATED by the height difference. 7th Dragon
    // III and Metroid: Samus Returns render both screens into one
    // 240x400 target and draw the second screen with a 320-tall
    // viewport: the mirror moves those draws 80 rows (= a quarter of the
    // 320-wide screen), while the display blit still reads the original
    // window — the measured quarter-screen shift. Citra 2104 has no flip
    // handling at all and renders both titles correctly, so for this
    // configuration no-relocation is the empirically correct behaviour.
    FB_FLIP_RECT_DISABLE,
    // Zeusiota/Citra-MMJ "Core DownCount Hack": run each core for only a
    // fraction of its scheduled timing slice (per-core right-shifts
    // {1,4,2,2}, applied in Timing::Timer::SetNextSlice) while emulated
    // time still advances at the full slice rate. Net effect: per-core CPU
    // throttle (core0 1/2, core1 1/16, cores2/3 1/4 speed) that fixes
    // cross-core sync races. MMJ calls the same mechanism
    // SetCpuUsageLimit; Zeusiota Final exposes it as the toggle that (with
    // the two hacks below) makes Mario Tennis Open / Mario Golf World Tour
    // playable. Same problem class the user's manual ms-delay tuning
    // addresses.
    CORE_DOWNCOUNT_HACK,
    // Zeusiota "Ignore Format Reinterpretation Hack" (also an old
    // citra-canary hack, removed upstream in Canary 1041): when surface
    // validation finds the data owned by a GPU surface of a different
    // format, keep the destination surface's current contents instead of
    // running the format reinterpreters (D24S8<->RGBA8 etc.). Fixes
    // texture/asset corruption in titles whose aliasing pattern the
    // reinterpreters mishandle.
    IGNORE_FORMAT_REINTERPRETATION,
    // Zeusiota "Force Validate Indexed PICA Registers": the PICA indexed
    // write paths (VS/GS shader program upload offset, swizzle upload
    // offset, VS default attribute index) drop writes whose index register
    // ran out of range; hardware wraps instead. Dropped shader-program
    // words are the classic cause of shimmering / flickering assets.
    // When gated, wrap the index into the valid range and perform the
    // write ([ZPICA] log per occurrence).
    FORCE_VALIDATED_PICA_INDEX,
};

// Set at title load (GPU::ReportLoadingProgramID) when the
// ACCURATE_JIT_MEMORY hack applies to the booting title; read by
// ARM_Dynarmic::MakeJit, which runs after title load for the app
// process page table.
extern std::atomic<bool> g_accurate_jit_memory;

// Set at title load like g_accurate_jit_memory; read by
// DynarmicUserCallbacks::InterpreterFallback on the CPU hot path.
extern std::atomic<bool> g_graceful_jit_fallback;

// Set at title load; read by Pica::OutputVertex per vertex.
extern std::atomic<bool> g_overbright_vertex_colors;

// Set at title load; read by FragmentModule::WriteLighting.
extern std::atomic<bool> g_frogger_specular_blue;

// Set at title load; read by RendererOpenGL::LoadFBToScreenInfo per frame.
extern std::atomic<bool> g_right_eye_display_fallback;

// Set at title load; read by GPU::Execute's MemoryFill dispatch.
extern std::atomic<bool> g_memoryfill_per_buffer_irq;

// Set at title load; read by the renderers' LoadFBToScreenInfo per frame.
extern std::atomic<bool> g_keep_last_frame_on_zero_fb;

// Set at title load; read by CpuLimiterMulti::DoTimeLimit.
extern std::atomic<bool> g_skip_core1_preemption;

// Set at title load; read by ArchiveBackend::Control (FS service thread).
extern std::atomic<bool> g_fs_control_zero_output;

// Set at title load; read by ArchiveBackend::Control (FS service thread).
extern std::atomic<bool> g_fs_control_one_output;

// Set at title load; read by DiskFile::Write/SetSize/Close and
// ArchiveBackend::Control (FS service thread).
extern std::atomic<bool> g_fs_save_diag;

// Set at title load; read by GSP_GPU::SignalInterruptForThread.
extern std::atomic<bool> g_gsp_legacy_pdc_queue;

// Set at title load; read by GSP_GPU::SignalInterruptForThread.
extern std::atomic<bool> g_disable_gpu_timing_sim;

// Set at title load; read by RendererOpenGL::RenderToMailbox and
// RendererVulkan::RenderToWindow.
extern std::atomic<bool> g_force_present_every_frame;

// Set at title load; read by RasterizerOpenGL::SyncDrawState every draw.
extern std::atomic<bool> g_cull_mode_invalid_keeps_state;

// Set at title load; read by RasterizerCache::ValidateByReinterpretation.
extern std::atomic<bool> g_legacy_validation_skip;

// Set at title load; read by the GLSL vertex-shader generator (GenerateVertexShader and the
// fixed geometry shader) when it emits primary_color.
extern std::atomic<bool> g_hw_vertex_color_saturate;

// Set at title load; read by the GLSL and SPIR-V fragment-shader generators (CompareShadow).
extern std::atomic<bool> g_shadow_equal_depth_lit;

// Set at title load; read by OpenGL TextureRuntime::ClearTextureWithoutFbo.
extern std::atomic<bool> g_force_fbo_clear;

// Set at title load; read by FileSys::SaveDataArchive::GetFreeBytes.
extern std::atomic<bool> g_savedata_free_bytes_from_format;

// Set at title load; read by GSP_GPU::TriggerCmdReqQueue and AcquireRight/TryAcquireRight.
extern std::atomic<bool> g_gsp_drain_queue_on_acquire;

// Set at title load; read by Kernel::SVC::CallSVC (JAWS guest hooks).
extern std::atomic<bool> g_jaws_entity_unlink;

// Set at title load; read by HLERequestContext::RunAsync and Core::System::Shutdown.
extern std::atomic<bool> g_async_wake_shutdown_guard;

// Set at title load (0 = off); read by GSP_GPU::TriggerCmdReqQueue.
extern std::atomic<u32> g_auto_render_thread_delay_us;

// Set at title load; read by RasterizerOpenGL::Draw per draw.
extern std::atomic<bool> g_draw_lookup_reuse;

// Set at title load; read by Pica::ShaderSetup::WriteUniformFloatReg per float32 uniform write.
extern std::atomic<bool> g_float_uniform_nan_to_zero;

// Set at title load; read by GPU::VBlankCallback every frame.
extern std::atomic<bool> g_vblank_signal_before_present;

// Set at title load; read by SVC::WaitSynchronization1/N and
// SVC::ArbitrateAddress on every blocking wait (diagnostic logging only).
extern std::atomic<bool> g_kernel_wait_diag;

// Set at title load; read by Core::Timing::Timer::SetNextSlice per slice.
extern std::atomic<bool> g_core_downcount_hack;

// Set at title load; read by RendererOpenGL::DrawScreens (log only).
extern std::atomic<bool> g_fb_config_diag;

// Set at title load; read by RendererOpenGL::LoadFBToScreenInfo.
extern std::atomic<bool> g_force_cpu_bottom_screen;

// Set at title load; read by RasterizerCache::GetFramebufferSurfaces.
extern std::atomic<bool> g_fb_flip_rect_disable;

// Set at title load; read by RasterizerCache::ValidateByReinterpretation.
extern std::atomic<bool> g_ignore_format_reinterpretation;

// Set at title load; read by PicaCore::WriteInternalReg indexed paths.
extern std::atomic<bool> g_force_validated_pica_index;

class UserHackData {};

} // namespace Common::Hacks