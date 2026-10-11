# Rumble Roses XX for Android: port notes

An Android (arm64-v8a) port of [RumbleRosesXX-Recomp](https://github.com/Revan67/RumbleRosesXX-Recomp), running on the same ReXGlue SDK and Vulkan fixes as the WET Android port. Latest build: **0.1.3** (package `com.rexglue.rrxx`). Experimental: it boots, plays the intro, video and sound, and mounts DLC; the match-start freeze fix in 0.1.3 has not been tested on a device yet.

## What was implemented

- **Codegen with ReXGlue instead of upstream's XenonRecomp hybrid.** ReXGlue translates every instruction (0 unimplemented markers versus about 2,400 in XenonRecomp's output) and the jump tables upstream hand-patches, so upstream's four boundary functions aren't needed.
- **126 missing functions recovered:** vtable thunks, empty methods and `this`-adjusting thunks that ReXGlue's GapFill skipped because it doesn't split code regions at `bctr`. This fixed the white screen after the Konami logo.
- **Vblank pacing:** `vsync = true`, since the game paces on vblank (fixed the 900 fps logos).
- **Guest setjmp/longjmp pairing:** libjpeg's error path uses `setjmp`/`longjmp`; translated literally, the jump returned to the wrong function on the wrong stack and caused the null read at `0x1A4` before a match. Each of the game's six `setjmp` call sites is now paired with a host `setjmp` (0.1.3).
- **DLC support:** STFS marketplace packages are listed and mounted in place, read-only, from `4B4E07D1/00000002` next to the game, the ISO or `Internal storage/Rumble Roses XX` (Xenia's `Content/...` layout works too). Confirmed on the device in 0.1.2: 91 packages found and opened.
- **Rumble-XX patches** as launcher settings: blur off, bloom (normal / reduced / off), no weapon icons, no Queen match banner, no background crowds, and 21:9.
- **Launcher:** ISO mounting in place through SAF, Turnip driver import, FPS counter, touch controls, logs to `Internal storage/Rumble Roses XX/logs`.

## Status of the source code

The Android port was developed and built on a temporary Linux build machine. The source changes themselves (the Android launcher app, the native overlays on the SDK, and the build scripts) were not committed anywhere before that machine was recycled, so **they are not in this repository yet**. This document records what was implemented and what was reverse-engineered, in enough detail to rebuild it. Code will be added here if the sources are recovered or rewritten.

No game files and no code recompiled from the game are included in this repository. Builds must be made from your own legally obtained copy of the game.

Reference test device: Samsung Galaxy S25 (Snapdragon 8 Elite, Adreno 830). Results on other devices have not been tested.

---

## Build log: Rumble Roses XX Android port: build notes (8 Oct 2026)

First Android build of [RumbleRosesXX-Recomp](https://github.com/Revan67/RumbleRosesXX-Recomp) (Revan67), made in the same Claude cloud session as the WET and Marathon builds (2 vCPU, 8 GB RAM).

## Current build: 0.1.3 (code 4)
- `RumbleRosesXX-arm64-release.apk`, package `com.rexglue.rrxx`, label "Rumble Roses XX", arm64-v8a, minSdk 26, targetSdk 35.
- APK SHA-256 `71f4bc05801aadf6318e84f3cf318dcec84ce5b753f80ad6b1a8bee7e4830e63`; zip SHA-256 `69fde8db9aa6699af33167cf508cc2f85477a2ac3622c26e6cde2933874affd3`.
- Host-paired guest setjmp/longjmp, the real cause of the match-start freeze (see "0.1.3"). Fault lines now include guest lr/r1/r3/r31/ctr.
- Symbols: `build/symbols-0.1.3/` (0.1.2's are in `build/symbols-0.1.2/`).
- **Earlier builds:**
  - 0.1.2 (code 3, APK `7e9ec646…`): DLC mounts (91 packages found and opened, user 20:04), patches apply. The freeze moved to the same fault, now mapped: `RRXXDBG guest fault: read of guest 000001A4 at librestuff.so+0xc575d4`, followed by 5× `Unregistered guest call 0x00000000` (logs 19:57–20:34, 8 Oct).
  - 0.1.1 (code 2, APK `2e21d53d…`): intro, video and sound work. Both drivers froze at about 1 fps after the arena-entrance loading screen; Turnip also drew lines from the wrestlers (user, 19:33, 8 Oct; logs 19:19:15, 19:21:58, 19:22:47 and a Turnip screen recording).
  - 0.1.0 (code 1, APK `8355ea15…`): boots, and the sound, RR logo and Konami logo play. Then a white screen while "Yankee Rose" plays, and no input works (user, 18:48, 8 Oct; logs 18:42:53, 18:43:38, 18:44:58 and a screen recording).
- **0.1.3 not tested on the device yet.**

## Upstream status (README, Oct 2026)
- Windows only and experimental: "no playable release".
- It renders in-engine scenes with audio and controller input, and Data Save worked twice and a load once. Starting a match then triggered a D3D12 device-hung failure; sustained gameplay and clean shutdown are unverified.
- The user only needs `default.xex` for building. To play, they need the full disc on the phone: the launcher mounts an ISO in place through SAF.

## Input
- The user's `default.xex` (5,124,096 bytes, SHA-256 `04271f1e60b7a5e5be028caa6073f250003e82b055eabf83d65e5496f5699660`): title `4B4E07D1`, media `1A6D4362`, version 0.0.0.3, built 2006-02-18. It is kept in `<build-root>/rrxx/private/` and `rrxx-android/rrxx/assets/`, both private.
- All 8 helper addresses in upstream `config/rrxx.toml` match the image (`savegprlr_14` 0x822CB510 `F9C1FF68`, `restgprlr_14` 0x822CB560, `savefpr_14` 0x822CBEF0, `restfpr_14` 0x822CBF3C, `savevmx_14` 0x822CD900, `restvmx_14` 0x822CDB98, `savevmx_64` 0x822CD994, `restvmx_64` 0x822CDC2C). Checked with `host-tools/xexinfo` (Marathon's tool, rebuilt against XenonUtils).

## Codegen: ReXGlue instead of upstream's hybrid
- **Upstream** uses hedge-dev XenonRecomp `ddd128b` output adapted to the ReXGlue runtime (`legacy_context_compat.h`), plus four regenerated "boundary" functions (0x82062200 fade, 0x8214E948 New Game, 0x82129680 save menu, 0x821D1810 dialog) and a C `memset` for `sub_822CB6A0`.
- **Reproduced locally** (`<build-root>/rrxx/RumbleRosesXX-Recomp`): XenonAnalyse found 588 switch tables. XenonRecomp wrote 87 files (75 MB) but reported:
  - unrecognized instructions: vslh 793, bso 540, vsrah 370, vspltish 251, vsubshs 141, vctuxs 68, vpkswss 64, vaddsws 45, bns 19, vavguh 16, and others; about 874 of them in the embsec sections;
  - "switch case jumps outside function" errors, with 2,121 `// ERROR` markers.
  - For example, `sub_8214E878`'s jump table became `PPC_CALL_INDIRECT_FUNC(ctr)`. That is why upstream registers a function at 0x8214E948, which is really a case label.
- **This port:** `rexglue codegen rrxx/rrxx_manifest.toml` with the WET host CLI (`wet-android/build/rexglue-sdk-host/out/linux-amd64/rexglue`, SDK `bd833a2`).
  - Validate first reported 3 UnresolvedCall errors: `b` targets 0x82344F90, 0x823649B0 and 0x82364958. Declaring them in `rrxx_functions.toml` fixed it.
  - It then took 7.4 s and wrote 149 files (74 MB). It has 0 unimplemented-instruction markers; the 592 `__builtin_trap` are switch-default cases.
  - It translates the fade and New Game switches correctly. `sub_82129680` contains `loc_82129700`, `sub_821D1810` contains the `r8 = load(r9+76)` callback, and `sub_822CB6A0` (memset) includes its tail stores. So none of upstream's boundary fixes are needed.
- Kept from upstream's host: the writable `PHOTO:` volume (`\Device\RrxxPhoto` at `<user_data_root>/../photo`), in `rrxx/src/rrxx_app.h` `OnPostSetup`.
- Not carried over: the D3D12 pipeline-thread cvar, `rrxx_hybrid_indirect`, `RRXXTranslatedBase` (the SDK's generated accessors handle the physical heap) and `compiler_compat.cpp` (x86 only).

## Project layout (`<build-root>/rrxx-android`, copied from wet-android)
- `rexglue-sdk/`: a copy of WET's tree (`bd833a2`, without `out/` and `.git`). All 47 files in `native/overlay/rexglue-sdk` are identical to the tree.
- `rrxx/`:
  - `src/main.cpp`: `REX_DEFINE_APP(rrxx, RrxxApp::Create)`.
  - `src/rrxx_app.h`: `graphics_backend` cvar, `OnPreSetup` GPU plugin, `OnConfigurePaths` (`REX_CONFIG_PATH`), PHOTO mount.
  - `rrxx_manifest.toml`, `rrxx_functions.toml`, `assets/default.xex`, `generated/default/`.
- `native/`:
  - `CMakeLists.txt`: `RRXX_SOURCE_DIR`, an `rrxx_recomp` OBJECT library with the PCH `rrxx_pch.h`, and `restuff` built from `rrxx/src/main.cpp` + `jni/android_main.cpp` + `rex_app.cpp`.
  - `jni/android_main.cpp` (WET's) looks up the creator `"rrxx"`.
- `scripts/build_android_native.sh` uses `RRXX_SRC`. The adrenotools and Vortek builds were copied from WET's `build/`.
- **Launcher** (WET's Kotlin app; internal package `com.deivid22srk.restuff` for the JNI bindings):
  - **Branding** (`PortBrandingConfig`): title "RUMBLE ROSES XX", subtitle "RECOMP · ANDROID", rose accent `#E8458B`, sparks particles, ISO texts.
  - **Credits:** "Android wrapper by Hailgames · Rumble Roses XX recomp by Revan67 · Android port by Sydgaming and Claude AI", with links to YouTube, GitHub, RumbleRosesXX-Recomp and Hailgames.
  - **Icon:** new adaptive icon ("RR / XX" in a pink ring on `#5A0F33`, drawn with Pillow and Poppins Bold).
  - **Removed** the Naughty Bear/WET "Game engine" toggles (60 FPS, Unlock All, texture mods). The tech chip reads "RRXX".
  - **GameActivity:**
    - Logs go to `/storage/emulated/0/Rumble Roses XX/logs/rrxx_<stamp>.log` (+ `.toml`), keeping the newest 10.
    - The WET cvars `wet_fps60_unlock` / `wet_disable_*` were dropped.
    - The overlay paths are redirected: `wet_gpu_workaround_file`, `wet_dump_shaders_dir` and `wet_probe_png_dir` point under `Rumble Roses XX/`.
    - `wet_probe_ps = ""` and `wet_dump_shaders_filter = ""`.
    - **`wet_param_gen_variant = 8`** is pinned. The auto search needs a probe shader (it would keep tracing every 240 frames on Qualcomm). Variant 8 drops the float-controls execution modes on param-gen pixel shaders: WET's Qualcomm fix, harmless on Turnip.
    - The rest of WET's mobile config is kept (e.g. `clear_memory_page_state = false`, `readback_resolve = "none"`, FIFO present).
  - **Input:** `input_merge_controllers` (WET overlay) defaults to true, which sends every pad and the touch overlay to player 1. For local 2-player, set it to false under Engine options.

## Build
- **Native:**
  - `ANDROID_NDK_HOME=/opt/android-sdk/ndk/27.2.12479018 bash scripts/build_android_native.sh`: 858 steps for a full build.
  - The game code compiled for arm64 with no errors. The only fix needed was defining the `graphics_backend` cvar in `rrxx_app.h`, which WET defines in `game_cvars.h`.
  - Total about 30 minutes, including the SDK (and its unneeded `rexcodegen`/`rexglue` host targets).
- **Outputs:**
  - `librestuff.so`: 21.8 MB stripped, 60.6 MB unstripped (in `build/android/arm64-v8a`).
  - `librexruntime.so` 12.0 MB, `librexgpu-xenos.so` 6.3 MB.
  - adrenotools (5 files), Vortek (2), `libc++_shared`.
- **Checks:**
  - All 662 undefined dynamic symbols of `librestuff.so` resolve against its NEEDED libraries (rexruntime, libc++_shared and NDK API-26 stubs).
  - The strings "rrxx" and `\Device\RrxxPhoto` are present.
  - `aapt2`: `com.rexglue.rrxx` 1 / 0.1.0, label "Rumble Roses XX".
  - `apksigner` verifies with the WET cert; `zipalign -c -P 16` passes.
- **APK:** `RESTUFF_VERSION_CODE=1 RESTUFF_VERSION_NAME=0.1.0 /opt/gradle/bin/gradle --no-parallel --offline -q :app:assembleRelease`.

## 0.1.0 device test and 0.1.1 fixes
- **Symptoms (0.1.0, Qualcomm driver, extracted game at `/storage/emulated/0/Rumble Roses XX/game`):**
  - Both pads detected: the Xbox Series X pad in slot 0, the touch pad merged. The headphones' media keys were ignored.
  - `WETDBG perf/60f` showed **~900 fps** during the logos (3 draws/frame), then guest frames **stopped at +3.8 s**. Only presenter repaints followed (the white last frame), while the audio worker kept playing the intro music.
  - `Unregistered guest call 0x821D2918; returning` (t27183, at the moment frames stop) and `0x82389C20` ×5 (GPU/interrupt thread).
- **Cause 1, missing functions:**
  - Both addresses are 4-instruction vtable thunks (`lwz r11,0(r3)` / `lwz r10,N(r11)` / `mtctr r10` / `bctr`), absent from .pdata and reached only through vtables.
  - ReXGlue's GapFill (`phase_gapfill.cpp` `splitRegionOnTerminators`) splits code regions only at `blr` and at `b` to known callables, not at `bctr`. A run of thunks therefore becomes one segment, skipped whole when its first thunk is already known (e.g. `sub_821D2908` from the vtable scan).
  - **Comparison:** XenonRecomp's 20,968 functions against ReXGlue's 14,041. Of 6,694 addresses absent from ReXGlue (not a function or a label), 6,411 are zero padding words after `blr` (XenonRecomp makes 4-byte "functions" of them), 39 are exception data (`82475514 …`), 101 are inline jump tables after `bctr` (first word is a code pointer), 14 are SEH `__finally` handlers (scope records at 0x8204B0xx–0x8204B3xx) and 2 are inside memset.
  - The **126 real ones** (81 empty `blr` methods, `this`-adjusting thunks `addi r3,r3,-16/-4/+32; b`, vtable thunks, and vtable-referenced functions such as 0x8207F3C0, 0x822156E8/F0, 0x823786A0 and 0x824628D8) were added to `rrxx/rrxx_functions.toml`, using the unpatched WET host CLI. Codegen then reported 0 warnings and registered 14,165 functions.
  - **Rejected alternative:** a patched GapFill that also splits at `bctr` (built as `rrxx-android/build/rexglue-sdk-host` + `build/host-codegen`, CLI `out/linux-amd64/rexglue`). It recovered the thunks but produced 76 "Unresolved conditional branch" warnings, splitting functions with mid-function jump-table dispatches. Kept for reference only; worth reporting upstream as a GapFill limitation.
  - Adding inline jump tables as functions also caused cross-function branches (46 warnings) until they were filtered by "first word is a code address".
- **Cause 2, pacing:**
  - The launcher (from WET) wrote `vsync = false`. In this SDK, `graphics_system.cpp`'s vsync worker then fires `MarkVblank` every 1 ms (`no_vsync_interval_ticks = guest_tick_frequency / 1000`) instead of at the video mode's 60 Hz, and `WAIT_REG_MEM` yields instead of sleeping.
  - Rumble Roses paces on vblank, so 0.1.1 sets `vsync = true`.
- **Also seen, left as is:** `CACHE:\PAC\MISC.PAC` / `CACHE:\PAC\ED` fail with "no device" after the game's `StfsCreateDevice` (stub). The SDK deliberately registers no `cache:` device ("let cache: fail cleanly"), so the game reads from the disc.
- **Input:** the pads were detected; the game was hung, so nothing could respond. Re-check on 0.1.1.

## 0.1.1 device test and 0.1.2 changes
- **Symptoms (0.1.1):** video and sound fine on both drivers, then after the wrestler-entrance loading screen the game drops to about 1 fps without crashing (Turnip: before the match starts).
  - The log has 298,021 × `Unhandled guest access violation: read of guest 0x000001A4 (host 0x00000001000001A4) on thread 0xF8000484` (host t8169, `XThread4238D3C0`, the busiest guest thread), starting at 19:24:13.
  - On Android the SIGSEGV handler returns for an unhandled fault, so the faulting load re-runs forever. Each pass takes the global lock and parses `/proc/self/maps` (`QueryProtect`), which starves every other thread.
  - Nothing unusual right before it: `XamContentCreateEnumerator: added 0 items` (DLC query) at 19:23:00, no unresolved-call errors. 92 generated sites load from `+420`, too many to guess from.
- **Mitigation** (`native/overlay/rexglue-sdk/src/system/mmio_handler.cpp`, at the "actual access violation" return):
  - A fault inside the guest's first 64 KB maps that range read/write (`memory::Protect(virtual_membase_, 0x10000, kReadWrite)`) and resumes, so the load reads 0, as in Xenia with `protect_zero = false`. Faults elsewhere still return false.
  - `RrxxReportUnhandledFault` logs each faulting host pc once (up to 64 sites): `RRXXDBG guest fault: read of guest 000001A4 at librestuff.so+0x… lr … < …`, with up to 8 frame-pointer callers. Resolve with `llvm-symbolizer --obj=build/symbols-0.1.2/librestuff.so.unstripped 0x…` to a `sub_XXXXXXXX` and source line.
  - `xmemory.cpp`: the unhandled-AV log is rate-limited (first 8, then every 10,000th, with a running count).
  - Root cause still open. Xenia keeps `protect_zero = true` by default, so on Xenia this read probably doesn't happen: it is likely a difference in this build (codegen, a kernel export, or missing content), not the game.
- **Turnip lines:** lines radiating from the wrestlers, only on Turnip (screen recording, 19:24). Looks like stray vertices in the skinned models on Turnip; not investigated. The stock driver is recommended for now.
- **DLC (in place, nothing copied):**
  - SDK overlay `src/system/xam/content_manager.cpp` (now in `scripts/apply_overlays.sh`): cvar `dlc_package_path`.
    - `ListContent` (marketplace, xuid 0) lists the STFS packages in that folder whose header says marketplace content and whose title is 0 or 4B4E07D1.
    - `ResolvePackagePath` falls back to `<dlc_package_path>/<file name>` when no extracted copy exists.
    - `ContentPackage` mounts a regular file with `StfsContainerDevice`, with the license from the header (`license_bits` where `license_flags` is set, else all bits). Log: `DLC: N package(s) from …`, `DLC: mounted <file> as <root>`.
  - Launcher (`GamePaths.dlcDir`): looks for `4B4E07D1/00000002` (case-insensitive), `Content/0000000000000000/4B4E07D1/00000002` or `0000000000000000/4B4E07D1/00000002` under:
    - the game folder and its parent;
    - the ISO's folder and its parent (a real path, or a `primary:` externalstorage document URI);
    - `Internal storage/Rumble Roses XX`.
    - It picks the first folder with at least one file starting `LIVE`/`PIRS`/`CON `, writes `dlc_package_path`, and toasts "DLC: N packages found".
  - The user's DLC is at `Internal storage/Rumble Roses XX/4B4E07D1/00000002/` (hash-named STFS files).
- **Rumble-XX patches** (github.com/Rumble-XX/Rumble-Roses-XX, `4B4E07D1 - Rumble Roses Double X.patch.toml`). Every original instruction was checked against the user's image:
  - Code patches: Blur Off `li r6,1→0` at 8226BF20 and 8226C0A4, plus a be32 at 8226DE12 that straddles `fmr f1,f31` / `bl 0x821e92c8`. The second word becomes invalid, which Xenia's translator skips, so the net effect is not making that call. Bloom Slight Off 1 at 8226C12C and Off 2 at 8226C150 (`li r6,1→0`). Remove Weapon Icons at 8228A7EC and Remove Queen Match Banner at 822A5B84 (`li r4,1→0`).
    - These are gated in the generated C++ by `rrxx/tools/apply_code_patches.py`, using bits of `g_rrxx_code_patches`. It locates each instruction from the nearest `loc_`/`ctx.lr`/function anchor and checks the disassembly comment.
    - It is idempotent and **must be re-run after every codegen**. It changed `rrxx_recomp.15/39/52/56.cpp`.
  - Data patches, written in `OnPostLoadXexImage` after checking the original bytes: crowds `be8 0x82498DAF = 0` (.data), 21:9 `be32 0x82012D00 = 0x4018E38E` (.rdata, was `0x3FE38E39`, i.e. 16:9).
  - Cvars (`rrxx/src/rrxx_patches.h`): `rrxx_blur_off`, `rrxx_bloom` (0/1/2), `rrxx_no_weapon_icons`, `rrxx_no_queen_banner`, `rrxx_no_crowds`, `rrxx_ultrawide`. The log line is `Rumble-XX patches: …`.
  - Launcher Settings → "Game patches". The defaults follow the patch file: blur off, bloom reduced, no weapon icons, no Queen banner. 21:9 also sets `present_letterbox = false`.
- **Build:** this was an incremental native build (the 4 patched generated files, `main.cpp`, the runtime overlays), then the APK (code 3). `scripts/apply_overlays.sh` lost its stale WetRecomp section.

## 0.1.3: the cause of the freeze (guest setjmp/longjmp)
- Symbolized against `symbols-0.1.2`: the fault is in `sub_822FD9F8` (`lwz r11,420(r31)` at loc_822FDA4C). The chain is `sub_823060B0` (indirect `this->vtable(+444)[1]`) < `sub_822FB878` < `sub_822FBB10` < `sub_822E7FD0` < `sub_822EAC18` < `sub_822E3378` < … < `sub_822076B0`.
- That is **libjpeg**: `sub_822FD9F8` = read_markers (cinfo+420 = `unread_marker`, +444 = marker reader, +24 = src, fill_input_buffer at src+12, JWRN 116 via `err->emit_message`), `sub_822FD788` = next_marker. r31 is valid on loop entry and 0 after the call to `sub_822FD788` returns.
- **Mechanism:** the game's jpeg error_exit `sub_822E5320` calls the CRT `longjmp` `sub_82472470` (the buffer is at err+144). There are 4 longjmp callers (`sub_822FB288`, `sub_822FB238`, `sub_822E5320`, tail `sub_822E54B8`). CRT `setjmp` is `sub_824726C0`, with 6 call sites: `sub_823000D8`, `sub_82300558`, `sub_822E8300`, `sub_822E8FD0`, `sub_822E7FD0` (in the fault chain) and `sub_822E91C8`.
  - Translated literally, longjmp restores r1/r13–r31/f14–f31/cr/lr into ctx and then **returns to its host caller**. libjpeg carries on with the setjmp-time registers on the wrong stack, `__restgprlr_27` reloads garbage, r31 becomes 0, and that gives the null+0x1A4 read and the calls to 0.
  - Not a missing function, helper, kernel export or title patch.
- **Fix** (`rrxx/src/rrxx_setjmp.h` + `apply_code_patches.py` `patch_setjmp`):
  - Each function with a setjmp call gets `RrxxJmpFrame rrxx_jmp_frame;` after its prologue (a thread_local intrusive stack; the destructor unlinks it). The call becomes `RRXX_GUEST_SETJMP(frame)`, which runs `Arm(r3, r1)` and then `_setjmp` in that frame: 0 calls the guest setjmp, non-zero runs `RrxxCtxBarrier(&ctx)` (ctx is `__restrict`).
  - The guest longjmp (main path, before its final `return`) calls `RrxxGuestLongjmp(buf=r7, r1, caller lr, jpeg msg_code at buf-124 when the caller is 0x822E5354)`. It finds the frame by guest buffer, drops newer frames and calls `_longjmp`. If no frame is found, it logs "NO SETJMP FRAME" and returns as before. It logs up to 32 times: `RRXXDBG guest longjmp from jpeg error_exit (JPEG message code N) …`.
  - The SEH path (jmp_buf+312 non-zero → `RtlUnwind`) is untouched.
  - Checked in the disassembly: `__imp__sub_822E7FD0` calls `_setjmp@plt`, then `RrxxCtxBarrier` / `sub_824726C0`.
- **Still open:** why libjpeg errors at match start. The new log line gives the libjpeg 6b message code. Suspects: a file read that comes up short, or the failed `CACHE:` partition.
- The zero-page mapping from 0.1.2 stays as a safety net.

## Next
- 0.1.3 on the device: check whether the match starts. Read the `RRXXDBG guest longjmp` line (JPEG message code) and any new fault lines (symbols-0.1.3).
- If the match plays after the zero-page mapping, check for side effects (missing objects, wrong wrestler).
- Turnip lines: compare a frame on both drivers (`wet_probe_png_dir`); suspect vertex fetch or skinning constants.
- Known WET-overlay behaviour that applies here too: quitting can SIGABRT in `KernelState::TerminateTitle` (harmless), and switching apps isn't supported.
