# Rumble Roses XX for Android: building

This folder holds the Android (arm64-v8a) port of Rumble Roses XX: the launcher app, the Android
CMake project and JNI glue, the port's overlays on the ReXGlue SDK, Vortek, the game project for
the SDK's code generator (`rrxx/`) and the build scripts. It is built the same way as the WET
Android port. What the port does and what was reverse-engineered is described in
[docs/ANDROID_PORT.md](../docs/ANDROID_PORT.md).

The Windows host in the rest of this repository (`hybrid/`, `runtime/`, `config/`) is not used by
the Android build: Android recompiles the game with ReXGlue's own code generator instead of
XenonRecomp (see "How this differs from the Windows build" below).

## You need your own copy of the game

**This repository contains no game files, no code recompiled from the game and no prebuilt APK.**
To build and play, you need your own legally obtained copy of Rumble Roses XX for Xbox 360
(Title ID `4B4E07D1`), as a disc image (ISO) you made yourself or as an extracted game folder.

- The build reads only `default.xex` from your copy and recompiles it on your computer
  (`rexglue codegen`). The output (`rrxx/generated/default`) and your `default.xex`
  (`rrxx/assets/`) are git-ignored and stay on your machine. The tested build is Media ID
  `1A6D4362`, version 0.0.0.3; the script warns if your `default.xex` differs.
- On the phone, the launcher plays the game from your own ISO or extracted folder, and DLC from
  your own packages.
- Do not share the APK you build: it contains code recompiled from your copy of the game.

## Requirements

- Linux x86_64 with clang 19 or newer (set `CC`/`CXX`), CMake, Ninja, Git and Python 3
- The X11, Wayland, ALSA and PulseAudio development headers for the host code generator's SDL
  build (on Ubuntu: `libx11-xcb-dev libx11-dev libwayland-dev wayland-protocols
  libxkbcommon-dev libasound2-dev libpulse-dev`)
- Android SDK with platform 35 and build-tools 35, and **NDK 27.2.12479018**
- JDK 17

## Build

```sh
git clone https://github.com/yuriaaaaa/RumbleRosesXX-Recomp
cd RumbleRosesXX-Recomp
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
export CC=clang-19 CXX=clang++-19

android/build.sh --xex /path/to/your/default.xex
```

The APK is written to `android/build/out/RumbleRosesXX-android-arm64.apk`. If your game is an ISO,
extract `default.xex` from it first with an Xbox 360 ISO extraction tool. The native build of the
recompiled code takes a long time (about 30 minutes on 2 cores).

`android/build.sh --skip-game` runs only what doesn't need the game: the launcher unit tests, the
Android builds of AdrenoTools, Vortek and the SDK libraries with the port's overlays, and a
launcher-only APK, which has no game library and can't start the game. It is a check that the
sources build.

The steps can also be run one by one:

```sh
git submodule update --init --depth 1 --recursive android/rexglue-sdk android/native/thirdparty/libadrenotools
bash android/scripts/apply_overlays.sh          # port overlays onto android/rexglue-sdk
RRXX_XEX=/path/to/default.xex bash android/scripts/build_host_codegen.sh
bash android/scripts/build_android_native.sh    # librestuff.so, Xenos plugin, AdrenoTools, Vortek
(cd android && ./gradlew :app:assembleRelease)
```

`build_host_codegen.sh` runs `rexglue codegen rrxx/rrxx_manifest.toml` and then
`rrxx/tools/apply_code_patches.py rrxx/generated/default`. That second step is part of the port:
it adds the switches for the Rumble-XX code patches and pairs the game's `setjmp`/`longjmp` with
host ones (the fix for the freeze before a match). It must run again after every codegen; it is
idempotent.

`.github/workflows/android.yml` runs the checks that don't need the game on GitHub Actions
(manual trigger). The game can't be built there: game files are never put in this repository.

### Signing

The release APK is signed with the debug key unless you provide your own:

```sh
export RRXX_KEYSTORE=/path/to/your.keystore   # or put it at android/keystore/release.keystore
export RRXX_STORE_PASSWORD=... RRXX_KEY_ALIAS=... RRXX_KEY_PASSWORD=...
```

An APK signed with a different key can't be installed over an earlier build; uninstall the old one
first.

## On the phone

Install the APK, open **Rumble Roses XX**, tap **Select ISO** and pick your own disc image or
extracted folder (it is read in place; nothing is copied). Logs go to
`Internal storage/Rumble Roses XX/logs` once "All files access" is granted.

- **DLC:** put your own marketplace packages (the STFS files) in `4B4E07D1/00000002/` next to the
  game folder or ISO, or in `Internal storage/Rumble Roses XX/4B4E07D1/00000002/`. Xenia's
  `Content/0000000000000000/4B4E07D1/00000002/` layout works too. They are mounted in place,
  read-only, and the launcher shows how many it found.
- **Settings:** graphics driver (system or an imported Turnip driver), FPS counter, touch
  controls, Engine options (advanced), and "Game patches" (the Rumble-XX patches: blur off, bloom,
  no weapon icons, no Queen match banner, no background crowds, 21:9).
- Every pad and the touch overlay control player 1. For local 2-player, add
  `input_merge_controllers = false` under Engine options (advanced).

## How this differs from the Windows build

The Windows host uses hedge-dev XenonRecomp output plus four hand-regenerated "boundary" functions
and a `memset` replacement. On this game that XenonRecomp revision leaves about 2,400 instructions
untranslated and breaks several jump tables. ReXGlue's code generator translates all of them, so
the Android build doesn't need the boundaries or the `memset` shim. It needs two things instead,
both in `rrxx/`:

- `rrxx_functions.toml`: 129 function starts the generator doesn't find on its own (3 tail-call
  targets and 126 functions reached only through vtables or function-pointer tables).
- `tools/apply_code_patches.py`: the code-patch switches and the host-paired `setjmp`/`longjmp`.

From the Windows host only the writable `PHOTO:` volume is kept (`rrxx/src/rrxx_app.h`).

## Layout

```
android/
  app/            Kotlin launcher (internal package com.deivid22srk.restuff, kept because the JNI
                  symbols bind to it; installed app id com.rexglue.rrxx) and SDL3's Java layer
  native/         Android CMake project, jni/android_main.cpp
    overlay/rexglue-sdk/   the port's changes to the SDK, copied over android/rexglue-sdk by
                           scripts/apply_overlays.sh (Android surface, fibers, file system, input,
                           Vulkan fixes for Adreno, GPU plugin loading, custom drivers, DLC
                           packages, guest fault handling)
    thirdparty/vortek, vortekrenderer   Vortek (brunodev85, LGPL-2.1), with local changes
    thirdparty/libadrenotools           submodule, bylaws/libadrenotools @ 8fae8ce
  rexglue-sdk/    submodule, rexglue/rexglue-sdk @ bd833a2 (the WET port's pin)
  rrxx/           the game project for the code generator
    src/          app class (main.cpp, rrxx_app.h), Rumble-XX patches (rrxx_patches.h),
                  guest setjmp/longjmp pairing (rrxx_setjmp.h)
    rrxx_manifest.toml, rrxx_functions.toml
    tools/apply_code_patches.py
    assets/       put your default.xex here if you don't use --xex (git-ignored)
  scripts/        apply_overlays.sh, build_host_codegen.sh, build_android_native.sh
  build.sh        runs everything
```

## Credits

- RumbleRosesXX-Recomp: Revan67
- ReXGlue SDK: Tom Clay and contributors (BSD-3-Clause, Xenia-derived)
- Android wrapper, SDK overlays and the AdrenoTools/Vortek integration this port starts from:
  Hailgames, [naughtybear-restuff-android](https://github.com/deivid22srk/naughtybear-restuff-android),
  by way of the WET Android port
- Rumble-XX patches: [Rumble-XX/Rumble-Roses-XX](https://github.com/Rumble-XX/Rumble-Roses-XX)
- Vortek: brunodev85 (LGPL-2.1); libadrenotools: bylaws (BSD-2-Clause)
