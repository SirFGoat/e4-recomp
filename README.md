# E4: Every Extend Extra Extreme: PC & Mac recompilation

A native Windows and macOS port of **E4: Every Extend Extra Extreme** (Xbox Live Arcade, 2007),
built by statically recompiling the original Xbox 360 executable with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**This repository contains no game data.** It contains no game files, no recompiled game
code, no textures, music or other assets. You need your own legally obtained copy of the
Xbox Live Arcade game. The build recompiles the code from your copy on your machine.

## Features

- Runs natively at any resolution (up to 8K internal rendering), 60 FPS, D3D12 or Vulkan,
  Metal via MoltenVK on macOS.
- Launcher with display, anti-aliasing, texture filtering and v-sync settings.
- Online leaderboards (shared, per stage), with Friends filter and your online best in the
  in-game HI-SCORE.
- Beat timing tuned for PC audio/video latency (the beat bar, beat lamps and controller
  vibration land on the music), with a fine-tune slider and a tap-along calibration.
- Optional "HD Remaster" extras: explosion sparks, neon glow on sprites, and support for
  high-resolution replacement textures that you make yourself (see below).

## 1. Get the game files

You need the E4 XBLA package (title ID `58410872`) from your own Xbox 360. Extract it into
`game/`:

```
python tools/stfs_extract.py "<path to the package file>" game
```

Check that `game/default.xex` and `game/Media/` exist. The build checks `default.xex`
against the supported version (SHA-256 `c4157358…3fd9`).

## 2. Build

Clone with git (the SDK's third-party libraries are git submodules):

```
git clone --recurse-submodules <this repo> E4
```

### Windows

Install [Git](https://git-scm.com/), [CMake](https://cmake.org/) 3.25+,
[Ninja](https://ninja-build.org/), [LLVM](https://releases.llvm.org/) 20+ (clang on your
PATH) and the Visual Studio 2022 Build Tools (C++ workload, for the Windows SDK). Then:

```
E4\e4recomp\setup.bat
```

This builds the SDK, recompiles the game's code from your `default.xex` and builds
`e4recomp\out\build\win-amd64-relwithdebinfo\e4_launcher.exe`. The first build takes a while.

### macOS (Apple Silicon or Intel)

Install Xcode 16+ (or its Command Line Tools), then `brew install cmake ninja`, then:

```
bash E4/e4recomp/build_mac.sh
```

On Apple Silicon, resolution scale 2 is recommended.

## 3. Play

Start `e4_launcher`, enter a name for the leaderboards and press **PLAY**. `run_e4.bat` /
`run_e4.sh` start the game directly with the settings listed at the top of the script.

Beat timing: if the beat bar or vibration feels early or late on your TV or speakers, use
**Beat fine-tune** in the launcher.

## HD textures (optional, make your own)

Replacement textures aren't included, because they are derived from the game's art. To
make your own:

1. `e4recomp\dump_textures.bat`: play, and every texture the game shows is saved to
   `textures/dump/`.
2. Upscale the ones you want (for example with
   [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN); `upscale_textures.bat` shows how)
   into `textures/replace/`, keeping the file names. Each name ends in the texture's hash.
3. Optional neon glow: `e4recomp\glow\make_glow.bat`.

Select **HD Remaster** in the launcher to use them.

## Layout

| Folder | What |
|---|---|
| `e4recomp/` | The E4 project: game hooks and patches (`src/`), launcher, build/run scripts, recompiler manifest |
| `tools/rexglue-sdk-src/` | ReXGlue SDK 0.10.1 with this project's changes (texture replacement, audio/beat-sync latency fixes, Xbox LIVE stats/leaderboards, XMA decoder fixes, macOS fixes) |
| `tools/stfs_extract.py` | Extracts Xbox 360 STFS packages (CON/LIVE/PIRS) |
| `game/` | Your game files (not included) |
| `textures/` | Your texture dumps / replacements (not included) |

## Legal

E4: Every Extend Extra Extreme and its assets belong to their respective owners (Q
Entertainment). This project is not affiliated with or endorsed by them. No copyrighted game data is distributed here.
Please support the original game.

The project code is MIT-licensed (see `LICENSE`). The ReXGlue SDK is BSD-3-Clause (see
`tools/rexglue-sdk-src/LICENSE`). Third-party components keep their own licences (see
`THIRD_PARTY_NOTICES.md`).
