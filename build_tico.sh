#!/usr/bin/env bash
set -euo pipefail

# Builds tico-drastic.nro, the Drastic host as the tico frontend's entrypoint,
# and tico-drastic-module.zip, the module tico installs into sdmc:/tico/modules/.
# The Drastic core, game database and post-FX shader sources come from the
# user's Drastic APK (DRASTIC_APK_DIR, same default as build_all.sh). Uses a
# local devkitPro install when there is one, otherwise the switch-dev Docker
# image.

APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$APP")"
APK_DIR=${DRASTIC_APK_DIR:-"$ROOT/com.dsemu.drastic_r2.6.0.4a-109_minAPI14(arm64-v8a)(nodpi)_drasticds.com"}
APK_DIR="$(cd "$APK_DIR" 2>/dev/null && pwd || echo "$APK_DIR")"
JOBS=${JOBS:-8}
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
IMAGE=${SWITCH_DEV_IMAGE:-}

CORE="$APK_DIR/lib/arm64-v8a/libdrastic_arm64.so"
MODULE_SRC="$APP/source/tico/module"
ASSETS="$APK_DIR/assets"
for file in "$CORE" "$ASSETS/game_database.xml" "$ASSETS/shaders/None.dfx"; do
  [[ -f "$file" ]] || {
    echo "Missing build input: $file" >&2
    echo "Point DRASTIC_APK_DIR at the extracted Drastic APK." >&2
    exit 1
  }
done

if [[ ! -f "$DEVKITPRO/cmake/Switch.cmake" ]]; then
  if [[ -z "$IMAGE" ]]; then
    # the switch-dev image is retagged by date, so take the newest one present
    IMAGE="$(docker images --format '{{.Repository}}:{{.Tag}}' ghcr.io/autorunhq/switch-dev | sort -r | head -n 1)"
  fi
  [[ -n "$IMAGE" ]] || {
    echo "No devkitPro install at $DEVKITPRO and no ghcr.io/autorunhq/switch-dev image found." >&2
    exit 1
  }
  exec docker run --rm -v "$APP:$APP" -v "$APK_DIR:$APK_DIR:ro" -w "$APP" \
    -e JOBS="$JOBS" -e DEVKITPRO=/opt/devkitpro -e DRASTIC_APK_DIR="$APK_DIR" \
    "$IMAGE" bash "$APP/build_tico.sh"
fi

export DEVKITPRO
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
MESA_SDK=${MESA_SDK_DIR:-"$DEVKITPRO/portlibs/switch"}
BUILD_CACHE=${DRASTIC_BUILD_CACHE_DIR:-"$APP/.drasticds-nx-cache"}
GLSLANG=${GLSLANG_VALIDATOR:-$(command -v glslangValidator)}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tico-drastic.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "==== Drastic Android post-FX programs ===="
python3 "$APP/tools/build_dfx.py" \
  --source "$ASSETS/shaders" --output "$WORK/dfx" --glslang "$GLSLANG"

echo "==== storage dependencies ===="
DEPS_BUILD="$BUILD_CACHE/storage"
cmake -S "$APP/launcher/dependencies" -B "$DEPS_BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build "$DEPS_BUILD" --parallel "$JOBS"

echo "==== tico romfs ===="
mkdir -p "$WORK/romfs/cores" "$WORK/romfs/res"
cp -f "$CORE" "$WORK/romfs/cores/libdrastic_arm64.so"
python3 "$APP/tools/patch_game_database.py" \
  --input "$ASSETS/game_database.xml" \
  --output "$WORK/romfs/res/game_database.xml"
# the tico overlay's fonts and translations
cp -R "$APP/source/tico/fonts" "$APP/source/tico/lang" "$WORK/romfs/"
# the overlay builds its settings menu from the module's own definition
mkdir -p "$WORK/romfs/module"
cp -f "$MODULE_SRC/settings.json" "$WORK/romfs/module/"

echo "==== tico-drastic host ===="
# Vulkan headers come after the repo's lsfg-vk headers: the image ships its own
# lsfg-vk in portlibs, which must not shadow the copy this host builds against.
make -C "$APP" -j"$JOBS" TICO=1 ELF_LIB= \
  STORAGE_BUILD="$DEPS_BUILD" \
  LIBSMB2_INCLUDE="$DEPS_BUILD/_deps/libsmb2-src/include" \
  LIBUSBHSFS_INCLUDE="$DEPS_BUILD/_deps/libusbhsfs-src/include" \
  MESA_SDK="$MESA_SDK" \
  VULKAN_INCLUDE="$APP/third_party/lsfg-vk/lsfg-vk-common/include" \
  DFX_GENERATED="$WORK/dfx" \
  TICO_ROMFS="$WORK/romfs"

#---------------------------------------------------------------------------------
# Module bundle
#
# A module is a directory, not a bare NRO: tico discovers it by reading
# module.json, and everything the module owns -- its settings definition and
# gamelist -- travels with it. The NRO sits beside module.json, so the bundle
# extracts straight into sdmc:/tico/modules/<id>/.
#---------------------------------------------------------------------------------
echo "==== tico module bundle ===="
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$WORK/module/$MODULE_ID"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp -f "$APP/tico-drastic.nro" "$MODULE_OUT/"
# tico prefers .json.gz when resolving a gamelist
if [[ -d "$MODULE_OUT/gamelists" ]]; then
  gzip -f -9 "$MODULE_OUT"/gamelists/*.json
fi
BUNDLE="$APP/tico-$MODULE_ID-module.zip"
rm -f "$BUNDLE"
( cd "$WORK/module" && zip -qr "$BUNDLE" "$MODULE_ID" )

echo
ls -la "$APP/tico-drastic.nro" "$BUNDLE"
echo "The module extracts to sdmc:/tico/modules/$MODULE_ID/:"
find "$MODULE_OUT" -type f | sed "s|$WORK/module/|    |"
