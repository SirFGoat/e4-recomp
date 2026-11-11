#!/bin/bash
# Packages a macOS build into E4.app and a drag-to-install E4.dmg (out/E4.dmg).
#
# The app contains the launcher, the game, its libraries, YOUR game files (../game) and
# your HD textures (../textures/replace, if any) - so it is for your own Macs only. Do not
# share the .app or .dmg publicly: it contains the game.
#
# Run by build_mac.sh at the end of a build, or on its own:
#   ./make_mac_app.sh [relwithdebinfo|release|debug]
set -euo pipefail

CONFIG="${1:-relwithdebinfo}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -m)" in
  arm64) ARCH=arm64 ;;
  x86_64) ARCH=amd64 ;;
  *) echo "Unsupported CPU $(uname -m)"; exit 1 ;;
esac
BUILD="$ROOT/out/build/mac-$ARCH-$CONFIG"
GAME="$ROOT/../game"
TEXTURES="$ROOT/../textures/replace"
OUT="$ROOT/out"
APP="$OUT/E4.app"
DMG="$OUT/E4.dmg"

[ -x "$BUILD/e4" ] && [ -x "$BUILD/e4_launcher" ] || { echo "Build first: ./build_mac.sh $CONFIG"; exit 1; }
[ -f "$GAME/default.xex" ] || { echo "No game files in $GAME"; exit 1; }

echo "=== Making E4.app ==="
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# Binaries and their libraries (the game finds librexruntime next to itself). The
# Vulkan/MoltenVK runtime goes in Contents/Resources/vulkan, which the game also searches:
# codesign rejects non-code folders (its share/ ICD json) inside Contents/MacOS.
cp "$BUILD/e4" "$BUILD/e4_launcher" "$APP/Contents/MacOS/"
find "$BUILD" -maxdepth 1 -name '*.dylib' -exec cp {} "$APP/Contents/MacOS/" \;
[ -d "$BUILD/vulkan" ] && cp -R "$BUILD/vulkan" "$APP/Contents/Resources/vulkan"

# Game files and HD textures (the launcher looks in Contents/Resources first).
cp -R "$GAME" "$APP/Contents/Resources/game"
if [ -d "$TEXTURES" ]; then
  mkdir -p "$APP/Contents/Resources/textures"
  cp -R "$TEXTURES" "$APP/Contents/Resources/textures/replace"
fi

# Icon from the game's own artwork (largest square PNG it ships), if the tools are there.
ICON_SRC=""
for f in "$GAME/E4_Large.png" "$GAME/Charaicon1.png"; do
  [ -f "$f" ] && { ICON_SRC="$f"; break; }
done
HAS_ICON=0
if [ -n "$ICON_SRC" ] && command -v sips >/dev/null && command -v iconutil >/dev/null; then
  SET="$OUT/E4.iconset"
  rm -rf "$SET"; mkdir -p "$SET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$ICON_SRC" --out "$SET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$ICON_SRC" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$SET" -o "$APP/Contents/Resources/E4.icns" && HAS_ICON=1
  rm -rf "$SET"
fi

VERSION="$(date +%Y.%m.%d)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>E4</string>
  <key>CFBundleDisplayName</key><string>E4: Every Extend Extra Extreme</string>
  <key>CFBundleIdentifier</key><string>local.e4recomp.e4</string>
  <key>CFBundleExecutable</key><string>e4_launcher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
$( [ $HAS_ICON = 1 ] && echo '  <key>CFBundleIconFile</key><string>E4</string>' || true )
  <key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST

# Ad-hoc signature: required for Apple Silicon. Libraries first (inside out), then the
# executables and the bundle.
find "$APP/Contents" -name '*.dylib' -type f -exec codesign --force --sign - {} \; 2>/dev/null
codesign --force --sign - "$APP/Contents/MacOS/e4"
codesign --force --sign - "$APP"
echo "Made $APP"

echo "=== Making E4.dmg ==="
STAGE="$OUT/dmg_stage"
RW="$OUT/E4_rw.dmg"
rm -rf "$STAGE" "$RW" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
SIZE_MB=$(( $(du -sm "$STAGE" | cut -f1) + 50 ))
hdiutil create -volname "E4" -srcfolder "$STAGE" -fs HFS+ -format UDRW -size ${SIZE_MB}m "$RW" >/dev/null
rm -rf "$STAGE"

# Lay out the window: E4 on the left, Applications on the right, nothing else. Needs
# Finder automation (macOS may ask once to allow Terminal to control Finder); if that's
# refused the DMG still works, just without the tidy layout.
MOUNT_DIR="$(hdiutil attach -readwrite -noverify -noautoopen "$RW" | awk -F'\t' '/\/Volumes\//{print $NF; exit}')"
if [ -n "$MOUNT_DIR" ]; then
  osascript >/dev/null 2>&1 <<OSA || echo "(Finder layout skipped - the DMG still works)"
tell application "Finder"
  tell disk "E4"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 760, 460}
    set opts to the icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set position of item "E4.app" of container window to {150, 160}
    set position of item "Applications" of container window to {410, 160}
    update without registering applications
    delay 1
    close
  end tell
end tell
OSA
  sync
  hdiutil detach "$MOUNT_DIR" -quiet || hdiutil detach "$MOUNT_DIR" -force -quiet
fi
hdiutil convert "$RW" -format UDZO -imagekey zlib-level=9 -o "$DMG" >/dev/null
rm -f "$RW"

echo
echo "Done: $DMG"
echo "Open it and drag E4 onto Applications. It contains your game files - keep it to"
echo "your own Macs."
