#!/usr/bin/env bash
# build_swiftshader.sh
#
# Cross-compiles Google SwiftShader (software EGL/GLES implementation) for
# armeabi-v7a (the A10s is 32-bit-userland only: ro.zygote=zygote32,
# ro.bionic.arch=arm, no 2nd_arch - see context/props_graphics.txt).
#
# Usage: build_swiftshader.sh <swiftshader-git-ref> <out-dir>
#   Requires: ANDROID_NDK_HOME set, cmake, ninja, git
#
# Produces (in <out-dir>/vendor_root/vendor/lib/egl/):
#   libEGL_swiftshader.so
#   libGLESv1_CM_swiftshader.so
#   libGLESv2_swiftshader.so
#
# NOTE: SwiftShader's CMake build historically names the android-arm32
# output libraries with a "lib" + name + ".so" pattern matching whatever
# EGL_LIBRARY_NAME etc. is configured; this script normalizes to the
# `_swiftshader` suffix Android's HAL loader expects for ro.hardware.egl=swiftshader.

set -euo pipefail

SS_REF="${1:?usage: build_swiftshader.sh <git-ref> <out-dir>}"
OUT_DIR="${2:?usage: build_swiftshader.sh <git-ref> <out-dir>}"
WORK_DIR="${WORK_DIR:-$PWD/work}"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"

echo "== SwiftShader build =="
echo "ref:       $SS_REF"
echo "out:       $OUT_DIR"
echo "work:      $WORK_DIR"
echo "ndk:       $ANDROID_NDK_HOME"

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

if [ ! -d swiftshader/.git ]; then
    git clone --depth 1 --branch "$SS_REF" https://github.com/google/swiftshader.git swiftshader \
        || git clone --depth 1 https://github.com/google/swiftshader.git swiftshader
fi
cd swiftshader
git fetch --depth 1 origin "$SS_REF" 2>/dev/null || true
git checkout "$SS_REF" 2>/dev/null || echo "staying on default branch HEAD (ref '$SS_REF' not found as a tag/branch)"

BUILD_DIR="$WORK_DIR/swiftshader/build-armeabi-v7a"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "ERROR: NDK CMake toolchain file not found at $TOOLCHAIN_FILE" >&2
    exit 1
fi

cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI=armeabi-v7a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_ARM_NEON=TRUE \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWIFTSHADER_BUILD_TESTS=OFF \
    -DSWIFTSHADER_BUILD_VULKAN=OFF \
    -DSWIFTSHADER_BUILD_EGL=ON \
    -DSWIFTSHADER_BUILD_GLESv2=ON \
    -DSWIFTSHADER_BUILD_GLES_CM=ON \
    -DSWIFTSHADER_BUILD_PVR=OFF \
    -DREACTOR_BACKEND=Subzero \
    .. 2>&1 | tee "$OUT_DIR/cmake-configure.log"

ninja -j"$(nproc)" 2>&1 | tee "$OUT_DIR/ninja-build.log"

echo "== Build finished, locating output .so files =="
find "$BUILD_DIR" -maxdepth 3 -name "*.so" -print

DEST="$OUT_DIR/vendor_root/vendor/lib/egl"
mkdir -p "$DEST"

# SwiftShader's CMake output naming has varied across versions
# (libEGL.so / libEGLd.so / libEGL_translator.so etc). Search broadly
# and normalize to the *_swiftshader.so names Android's EGL loader
# expects when ro.hardware.egl=swiftshader.
copy_lib() {
    local pattern="$1" dest_name="$2"
    local found
    found="$(find "$BUILD_DIR" -maxdepth 4 -iname "$pattern" | head -1)"
    if [ -n "$found" ]; then
        cp -v "$found" "$DEST/$dest_name"
    else
        echo "WARNING: no match for pattern '$pattern' - $dest_name NOT produced" | tee -a "$OUT_DIR/ninja-build.log"
    fi
}

copy_lib "libEGL*.so"        "libEGL_swiftshader.so"
copy_lib "libGLESv2*.so"     "libGLESv2_swiftshader.so"
copy_lib "libGLES_CM*.so"    "libGLESv1_CM_swiftshader.so"
copy_lib "libGLESv1*CM*.so"  "libGLESv1_CM_swiftshader.so"

echo "== vendor_root layout =="
find "$OUT_DIR/vendor_root" -type f -exec ls -la {} \;

echo "== readelf summary (verify armeabi-v7a, no unresolved deps beyond standard Bionic libs) =="
{
  for f in "$DEST"/*.so; do
    [ -f "$f" ] || continue
    echo "--- $f ---"
    readelf -h "$f" | grep -E "Class|Machine" || true
    readelf -d "$f" | grep NEEDED || true
    echo
  done
} | tee "$OUT_DIR/elf-report.txt"

echo "== done =="
