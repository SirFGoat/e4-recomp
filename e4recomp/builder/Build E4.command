#!/bin/bash
# E4 builder for macOS (Apple Silicon): turns your E4 Xbox Live Arcade zip into E4.app.
# Put the zip next to this file and double-click it (the first time: right-click > Open).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KIT="$HERE/kit"
GAME="$KIT/game"
PROJ="$KIT/e4recomp"
SDK="$KIT/sdk"
TOOLS="$KIT/tools"
EXPECTED_XEX="c4157358ace8664504edc95348e20b4588c294a8fe32821ef4b2f2a9461a3fd9"

step() { echo; echo "=== $1 ==="; }
fail() { echo; echo "$1"; echo; read -r -p "Press Return to close." _ || true; exit 1; }
trap 'fail "Something went wrong (see above)."' ERR

latest_asset() {  # <owner/repo> <regex>: download URL of the latest release asset
  curl -fsSL -H "User-Agent: E4-builder" "https://api.github.com/repos/$1/releases/latest" |
    python3 -c 'import json,re,sys; r=json.load(sys.stdin); print(next(a["browser_download_url"] for a in r["assets"] if re.search(sys.argv[1], a["name"])))' "$2"
}

echo "E4: Every Extend Extra Extreme - builder"
[ "$(uname -m)" = "arm64" ] || fail "This builder is for Apple Silicon Macs. On an Intel Mac, build from the source code (see the GitHub page)."
# Files from a downloaded zip are quarantined by macOS; clear it on our own files.
xattr -dr com.apple.quarantine "$HERE" 2>/dev/null || true

# --- 1. Compiler (Xcode Command Line Tools, installed once) ---------------------
step "1/5  Compiler"
if ! xcode-select -p >/dev/null 2>&1; then
  echo "  macOS will now offer to install the Command Line Tools - click Install."
  xcode-select --install 2>/dev/null || true
  for _ in $(seq 1 720); do  # wait up to an hour
    xcode-select -p >/dev/null 2>&1 && break
    sleep 5
  done
  xcode-select -p >/dev/null 2>&1 || fail "The Command Line Tools aren't installed yet. Run the builder again once they are."
fi
echo "  compiler OK"

# CMake and Ninja: portable copies inside the kit (nothing installed).
mkdir -p "$TOOLS"
if ! command -v cmake >/dev/null; then
  if [ ! -x "$TOOLS/cmake/CMake.app/Contents/bin/cmake" ]; then
    echo "  downloading CMake ..."
    curl -fsSL "$(latest_asset Kitware/CMake '^cmake-[0-9.]+-macos-universal\.tar\.gz$')" -o "$TOOLS/cmake.tgz"
    mkdir -p "$TOOLS/cmake"
    tar -xzf "$TOOLS/cmake.tgz" -C "$TOOLS/cmake" --strip-components 1
    rm "$TOOLS/cmake.tgz"
  fi
  export PATH="$TOOLS/cmake/CMake.app/Contents/bin:$PATH"
fi
if ! command -v ninja >/dev/null; then
  if [ ! -x "$TOOLS/ninja" ]; then
    echo "  downloading Ninja ..."
    curl -fsSL "$(latest_asset ninja-build/ninja '^ninja-mac\.zip$')" -o "$TOOLS/ninja.zip"
    unzip -oq "$TOOLS/ninja.zip" -d "$TOOLS"
    rm "$TOOLS/ninja.zip"
  fi
  export PATH="$TOOLS:$PATH"
fi

# --- 2. Game files ------------------------------------------------------------------
step "2/5  Game files"
if [ ! -f "$GAME/default.xex" ]; then
  TMP="$KIT/extract_tmp"
  rm -rf "$TMP"; mkdir -p "$TMP"
  ZIP="$(find "$HERE" -maxdepth 1 -type f -iname '*.zip' ! -name 'E4-Builder*' | head -n 1)"
  if [ -n "$ZIP" ]; then
    echo "  unzipping $(basename "$ZIP") ..."
    ditto -x -k "$ZIP" "$TMP"
  fi
  PKG=""
  while IFS= read -r -d '' f; do
    [ "$(stat -f %z "$f")" -gt 1048576 ] || continue
    case "$(head -c 4 "$f" | tr -d '\0')" in LIVE|"CON "|PIRS) PKG="$f"; break ;; esac
  done < <(find "$TMP" "$HERE" -maxdepth 4 -type f ! -path "$KIT/sdk/*" -print0)
  [ -n "$PKG" ] || fail "No game found. Put your E4 Xbox Live Arcade zip (or its package file) next to 'Build E4.command', then run it again."
  echo "  extracting $(basename "$PKG") ..."
  "$TOOLS/e4_extract" "$PKG" "$GAME"
  rm -rf "$TMP"
fi
[ -f "$GAME/default.xex" ] || fail "That package has no default.xex - is it E4?"
HASH="$(shasum -a 256 "$GAME/default.xex" | cut -d' ' -f1)"
if [ "$HASH" != "$EXPECTED_XEX" ]; then
  echo "  WARNING: this default.xex is a different version than E4 supports ($HASH)."
  read -r -p "  Continue anyway? (y/n) " a
  [ "$a" = "y" ] || exit 1
else
  echo "  game files OK"
fi

# --- 3. Recompile ----------------------------------------------------------------------
step "3/5  Recompiling the game code from your default.xex (a few minutes)"
if [ ! -f "$PROJ/generated/default/e4_init.h" ]; then
  (cd "$PROJ" && "$SDK/bin/rexglue" codegen e4_manifest.toml)
else
  echo "  already done"
fi

# --- 4. Build ----------------------------------------------------------------------------
step "4/5  Building E4 (10-20 minutes the first time)"
(cd "$PROJ" && cmake --preset mac-arm64-release -Drexglue_DIR="$SDK/lib/cmake/rexglue" &&
  cmake --build "out/build/mac-arm64-release" --parallel "$(sysctl -n hw.ncpu)")

# --- 5. App + installer ------------------------------------------------------------------
step "5/5  Making E4.app"
bash "$PROJ/make_mac_app.sh" release
rm -rf "$HERE/E4.dmg"
cp "$PROJ/out/E4.dmg" "$HERE/E4.dmg"
trap - ERR

echo
echo "Done! Opening E4.dmg - drag E4 onto Applications."
echo "(It contains your game files: keep it to your own Macs.)"
open "$HERE/E4.dmg"
