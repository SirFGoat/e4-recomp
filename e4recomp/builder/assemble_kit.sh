#!/bin/bash
# Assembles a builder kit (used by the release workflow; no game data involved).
# Usage: assemble_kit.sh <windows|mac> <SDK install dir> <e4_extract binary> <output dir>
set -euo pipefail
PLATFORM="$1"; SDK_INSTALL="$2"; EXTRACT="$3"; OUT="$4"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/e4recomp"
KIT="$OUT/kit"

rm -rf "$OUT"
mkdir -p "$KIT/e4recomp" "$KIT/tools"
cp -R "$SDK_INSTALL" "$KIT/sdk"
cp "$SRC/CMakeLists.txt" "$SRC/CMakePresets.json" "$SRC/e4_manifest.toml" "$KIT/e4recomp/"
cp -R "$SRC/src" "$SRC/launcher" "$KIT/e4recomp/"
cp "$EXTRACT" "$KIT/tools/"
cp "$SRC/builder/README.txt" "$OUT/"
cp "$REPO/LICENSE" "$REPO/THIRD_PARTY_NOTICES.md" "$KIT/"
cp "$REPO/tools/rexglue-sdk-src/LICENSE" "$KIT/LICENSE-rexglue-sdk.txt"

case "$PLATFORM" in
  windows)
    cp "$SRC/builder/Build E4.bat" "$OUT/"
    cp "$SRC/builder/build_windows.ps1" "$KIT/"
    ;;
  mac)
    cp "$SRC/builder/Build E4.command" "$OUT/"
    cp "$SRC/make_mac_app.sh" "$KIT/e4recomp/"
    chmod +x "$OUT/Build E4.command" "$KIT/e4recomp/make_mac_app.sh" "$KIT/tools/"*
    ;;
  *) echo "unknown platform $PLATFORM"; exit 1 ;;
esac
echo "Kit ready: $OUT"
