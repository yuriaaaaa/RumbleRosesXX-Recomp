# Baton: Rumble Roses XX Android port, state at 0.1.3
From: claude · Date: 2026-10-11 · Repo commit: 70f6d37

**TL;DR:** Boots, plays the intro, video and sound, and mounts DLC. 0.1.2 froze before a match; 0.1.3 fixes the cause found in the logs (guest setjmp/longjmp) but hasn't been device-tested. Next for ChatGPT: research why libjpeg would error at match start and what Turnip's "lines from the wrestlers" artefact is in Xenia.

## Goal
A playable arm64 Android build of Rumble Roses XX (Title ID `4B4E07D1`) from this fork, built by players from their own `default.xex`. Source-only releases.

## State of play (verified)
- Android source on `main` under `android/` (game project `android/rrxx/`). Build: `android/build.sh --xex /path/to/default.xex`; `--skip-game` passed on 2026-10-11.
- Codegen is ReXGlue (`bd833a2`), not upstream's XenonRecomp hybrid. 126 vtable-only functions were added by hand (fixed the white screen after the Konami logo).
- 0.1.2 on the S25: DLC mounts (91 packages), patches apply, then the freeze before a match: read of guest `0x1A4` in `sub_822FD9F8` (libjpeg `read_markers`), then calls to 0.
- Cause: the game's libjpeg error path calls the CRT `longjmp` (`sub_82472470`), which, translated literally, returns to its host caller instead of unwinding. 0.1.3 pairs guest setjmp/longjmp with host ones (`rrxx/src/rrxx_setjmp.h`, `apply_code_patches.py` `patch_setjmp`).
- Details: `docs/ANDROID_PORT.md`.

## Assumed / unverified
- That 0.1.3 gets into a match. Not tested on a device.
- Why libjpeg errors at all at match start. Suspects: a short file read, or the `CACHE:` partition failing to mount.
- The Turnip-only line artefacts are suspected to be vertex fetch or skinning constants. Not investigated.

## Findings and dead ends
- Not a missing function, helper, kernel export or title patch (ruled out for the freeze).
- Upstream (Revan67) is Windows-only and also hits a failure at match start (D3D12 device hung), so upstream has no fix to copy.
- Quitting can SIGABRT in `KernelState::TerminateTitle` (harmless). Switching apps isn't supported.

## Next task for ChatGPT
Research only, no code:
1. In Xenia / Xenia Canary issue trackers and compatibility pages for Rumble Roses XX (`4B4E07D1`): any reports of match-start crashes, JPEG/texture loading errors, or the cache partition, and their fixes.
2. Any reports of stray lines / stretched vertices on wrestlers under Vulkan or Mesa Turnip in Xenia, and what fixed them.
3. Check Revan67/RumbleRosesXX-Recomp for commits after this fork's base that touch match start, and summarise them with links.
4. Drop a baton `.baton/YYYY-MM-DD-chatgpt-match-start-research.md`.

## Open questions for David
- Does 0.1.3 reach a match? If not, send the newest log from `Internal storage/Rumble Roses XX/logs` (look for `RRXXDBG guest longjmp ... JPEG message code N`).
