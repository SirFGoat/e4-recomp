#!/bin/bash
# Launch the E4 recomp on macOS / Linux. Usage: ./run_e4.sh [relwithdebinfo|release|debug] [extra args...]
# Same settings as run_e4.bat; or just use the launcher (out/build/<preset>/e4_launcher).
ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-relwithdebinfo}"
[ $# -gt 0 ] && shift
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PRESET=mac-arm64 ;;
  Darwin-x86_64) PRESET=mac-amd64 ;;
  Linux-aarch64) PRESET=linux-arm64 ;;
  *) PRESET=linux-amd64 ;;
esac
BUILD="$ROOT/out/build/$PRESET-$CONFIG"
if [ ! -x "$BUILD/e4" ]; then
  echo "e4 not found in $BUILD - build it first: ./build_mac.sh $CONFIG"
  exit 1
fi

# --- Graphics -------------------------------------------------------------
# Internal render scale over the 360's native 1280x720 (2 = 2560x1440, 3 = 3840x2160).
# Apple GPUs: 2 is a good start; try 3 on Pro/Max chips.
RES_SCALE=2
# Anisotropic filtering: 0 off, 1=1x, 2=2x, 3=4x, 4=8x, 5=16x.
ANISO=5
# Post-process anti-aliasing: none, fxaa, fxaa_extreme.
POST_AA=none
FULLSCREEN=false

# --- Texture replacement ----------------------------------------------------
TEX_DIR="$ROOT/../textures"
TEX_REPLACE=true
# Neon glow baked into enemy/explosion/title sprites (textures/replace/@glow). Empty = off.
TEX_VARIANTS=glow
# Texture dumping only works on the PC (D3D12) build.

# --- Explosion particles (E4 recomp addition) -------------------------------
PARTICLES=true
PARTICLE_COUNT=12

cd "$BUILD" || exit 1
exec "$BUILD/e4" --game_data_root="$ROOT/../game" --gpu_plugin=xenos --license_mask=1 \
  --resolution_scale=$RES_SCALE --anisotropic_override=$ANISO --swap_post_effect=$POST_AA \
  --fullscreen=$FULLSCREEN \
  --texture_dir="$TEX_DIR" --texture_replace=$TEX_REPLACE --texture_dump=false \
  --texture_variants=$TEX_VARIANTS \
  --e4_extra_particles=$PARTICLES --e4_particle_count=$PARTICLE_COUNT \
  --log_file="$ROOT/e4_last_run.log" \
  "$@"
