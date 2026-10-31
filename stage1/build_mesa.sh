#!/usr/bin/env bash
# Cross-build Mesa softpipe (EGL + GLESv1 + GLESv2) for Android armeabi-v7a, API 30.
# Usage: ANDROID_NDK_HOME=/path/to/ndk stage1/build_mesa.sh [mesa-tag] [out-dir]
set -euo pipefail
MESA_TAG="${1:-mesa-21.3.9}"
OUT="$(realpath -m "${2:-out}")"
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
API=30
TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
WORK="${WORK:-$PWD/work}"
mkdir -p "$WORK" "$OUT"

if [ ! -d "$WORK/mesa" ]; then
  git clone --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$WORK/mesa"
fi

sed -e "s#@TC@#$TC#g" -e "s#@API@#$API#g" "$HERE/android-armv7.cross.in" > "$WORK/android-armv7.cross"

rm -rf "$WORK/build"
meson setup "$WORK/build" "$WORK/mesa" \
  --cross-file "$WORK/android-armv7.cross" \
  --buildtype=release -Db_ndebug=true -Db_lto=false \
  -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true \
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
  -Dglx=disabled -Dgbm=disabled -Degl-native-platform=android \
  -Dgallium-drivers=swrast -Ddri-drivers= -Dvulkan-drivers= \
  -Dllvm=disabled -Dshared-glapi=enabled -Dglvnd=false \
  -Degl-lib-suffix=_soft \
  -Dlmsensors=disabled -Dlibunwind=disabled -Dvalgrind=disabled \
  -Dbuild-tests=false -Dshader-cache=disabled -Dzstd=disabled \
  -Dosmesa=false -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled \
  --wrap-mode=default 2>&1 | tee "$OUT/meson-setup.log"

ninja -C "$WORK/build" 2>&1 | tee "$OUT/ninja.log"

DESTDIR="$WORK/stage" ninja -C "$WORK/build" install >/dev/null
find "$WORK/stage" \( -name '*.so' -o -name '*.so.*' \) -print | tee "$OUT/installed-files.txt"
mkdir -p "$OUT/vendor_lib"
find "$WORK/stage" \( -name '*.so' \) -exec cp -v {} "$OUT/vendor_lib/" \;
for f in "$OUT"/vendor_lib/*.so; do
  "$TC/bin/llvm-strip" --strip-unneeded "$f" || true
  { echo "# $(basename "$f")"; "$TC/bin/llvm-readelf" -h "$f" | grep -E 'Class|Machine'; "$TC/bin/llvm-readelf" -d "$f" | grep NEEDED; } >> "$OUT/elf-report.txt"
done
echo "done -> $OUT/vendor_lib"
