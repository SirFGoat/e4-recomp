#!/bin/bash
# Build the E4 recomp on macOS (Vulkan via MoltenVK).
#
# Expects the same folder layout as on the PC:
#   E4/e4recomp                  <- this project (this script lives here)
#   E4/tools/rexglue-sdk-src     <- the modified ReXGlue SDK (texture replacement, glow variants)
#   E4/game                      <- extracted game files (default.xex ...)
#   E4/textures                  <- HD / glow replacement PNGs
#
# Needs: Xcode 16+ (or its Command Line Tools: xcode-select --install), CMake 3.25+, Ninja.
#   brew install cmake ninja
# The first build takes a while (SDK + MoltenVK + the recompiled game) and downloads a few
# MoltenVK dependencies (SPIRV-Cross etc.).
#
# Usage: ./build_mac.sh [relwithdebinfo|release|debug]
set -euo pipefail

CONFIG="${1:-relwithdebinfo}"
case "$CONFIG" in
  relwithdebinfo) CMAKE_CONFIG=RelWithDebInfo ;;
  release) CMAKE_CONFIG=Release ;;
  debug) CMAKE_CONFIG=Debug ;;
  *) echo "Unknown config '$CONFIG' (use relwithdebinfo, release or debug)"; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK_SRC="$(cd "$ROOT/../tools/rexglue-sdk-src" && pwd)"

case "$(uname -m)" in
  arm64) ARCH=arm64 ;;
  x86_64) ARCH=amd64 ;;
  *) echo "Unsupported CPU $(uname -m)"; exit 1 ;;
esac
SDK_PRESET="mac-$ARCH"
SDK_BUILD="$SDK_SRC/out/build/$SDK_PRESET"
SDK_INSTALL="$SDK_SRC/out/install/$SDK_PRESET"
GAME_PRESET="mac-$ARCH-$CONFIG"

for tool in cmake ninja clang++; do
  command -v "$tool" >/dev/null || { echo "Missing $tool - see the top of this script."; exit 1; }
done
JOBS="$(sysctl -n hw.ncpu)"

# Third-party sources (git submodules) - only needed in a fresh clone of the public repo.
if [ ! -f "$SDK_SRC/thirdparty/fmt/CMakeLists.txt" ]; then
  echo "=== Fetching third-party sources (git submodules) ==="
  git -C "$ROOT/.." submodule update --init --recursive
fi
# MoltenVK changes for E4 (shipped as a patch in the public repo).
MVK_PATCH="$SDK_SRC/patches/moltenvk.patch"
if [ -s "$MVK_PATCH" ] && git -C "$SDK_SRC/thirdparty/moltenvk" apply --check "$MVK_PATCH" 2>/dev/null; then
  echo "=== Applying MoltenVK patch ==="
  git -C "$SDK_SRC/thirdparty/moltenvk" apply "$MVK_PATCH"
fi

echo "=== 1/3  ReXGlue SDK ($SDK_PRESET) ==="
cmake --preset "$SDK_PRESET" -S "$SDK_SRC" -DREXGLUE_BUILD_TESTS=OFF
# The SDK package needs its Release config even for RelWithDebInfo game builds (the loader
# and MoltenVK dylibs are installed from Release).
for c in Release "$CMAKE_CONFIG"; do
  cmake --build "$SDK_BUILD" --config "$c" --parallel "$JOBS"
  cmake --install "$SDK_BUILD" --config "$c"
done

# Recompile the game's code from your own copy of the game (../game/default.xex).
if [ ! -f "$ROOT/generated/default/e4_init.h" ]; then
  [ -f "$ROOT/../game/default.xex" ] || { echo "Put the game files in E4/game first (see E4/game/README.md)."; exit 1; }
  echo "=== 2/3  Generating code from default.xex ==="
  (cd "$ROOT" && "$SDK_INSTALL/bin/rexglue" codegen e4_manifest.toml)
fi

echo "=== 3/3  E4 ($GAME_PRESET) ==="
cmake --preset "$GAME_PRESET" -S "$ROOT" -Drexglue_DIR="$SDK_INSTALL/lib/cmake/rexglue"
cmake --build "$ROOT/out/build/$GAME_PRESET" --parallel "$JOBS"

echo
echo "Built: $ROOT/out/build/$GAME_PRESET/e4  (+ e4_launcher)"
# Package into E4.app + a drag-to-install E4.dmg (contains your game files: own Macs only).
bash "$ROOT/make_mac_app.sh" "$CONFIG"
echo "Run:   $ROOT/run_e4.sh   or   $ROOT/out/build/$GAME_PRESET/e4_launcher"
