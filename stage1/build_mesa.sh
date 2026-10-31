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
PREFIX="$WORK/prefix"
mkdir -p "$WORK" "$OUT" "$PREFIX"

mkcross() { # $1 = meson host system name
  sed -e "s#@TC@#$TC#g" -e "s#@API@#$API#g" -e "s#@SYSTEM@#$1#g" \
      -e "s#@PKGPATH@#$PREFIX/lib/pkgconfig#g" "$HERE/android-armv7.cross.in" > "$WORK/cross-$1.txt"
}
mkcross linux; mkcross android

[ -d "$WORK/mesa" ] || git clone --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$WORK/mesa"

# Diagnostics: how this Mesa decides with_dri / EGL.
{
  for r in 195,215 262,276 360,396 440,482; do echo "=== meson.build lines $r"; sed -n "${r}p" "$WORK/mesa/meson.build"; done
  echo "=== subprojects"; ls "$WORK/mesa/subprojects" 2>&1
} > "$OUT/meson-dri-lines.txt" 2>&1 || true

# libdrm (Mesa's android/dri2 path needs it; pkg-config is not available for the target)
if [ ! -f "$PREFIX/lib/pkgconfig/libdrm.pc" ]; then
  git clone --depth 1 --branch libdrm-2.4.110 https://gitlab.freedesktop.org/mesa/drm.git "$WORK/drm" >> "$OUT/libdrm.log" 2>&1 || true
  ( meson setup "$WORK/drm-build" "$WORK/drm" --cross-file "$WORK/cross-linux.txt" --prefix "$PREFIX" --libdir lib \
      --buildtype=release -Dintel=false -Dradeon=false -Damdgpu=false -Dnouveau=false -Dvmwgfx=false \
      -Dfreedreno=false -Dvc4=false -Detnaviv=false -Dexynos=false -Dcairo-tests=false \
      -Dvalgrind=false -Dman-pages=false -Dudev=false -Dinstall-test-programs=false \
    && ninja -C "$WORK/drm-build" install ) >> "$OUT/libdrm.log" 2>&1 \
    && echo "libdrm OK" | tee -a "$OUT/libdrm.log" || echo "libdrm FAILED (continuing)" | tee -a "$OUT/libdrm.log"
fi

COMMON=(
  --buildtype=release -Db_ndebug=true -Db_lto=false
  -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true
  -Dglx=disabled -Dgbm=disabled -Degl-native-platform=android
  -Dgallium-drivers=swrast -Dvulkan-drivers= -Ddri-drivers=
  -Dllvm=disabled -Dshared-glapi=enabled -Dglvnd=false
  -Degl-lib-suffix=_soft
  -Dlmsensors=disabled -Dlibunwind=disabled -Dvalgrind=disabled
  -Dbuild-tests=false -Dshader-cache=disabled -Dzstd=disabled
  -Dosmesa=false -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled
  --wrap-mode=default
)
# host system name seen by meson, then extra options; first that configures wins
CANDIDATES=(
  "linux|"
  "linux|-Dplatform-sdk-version=29"
)
ok=0; i=0
for c in "${CANDIDATES[@]}"; do
  sys="${c%%|*}"; extra="${c#*|}"; i=$((i+1)); rm -rf "$WORK/build"
  echo "### candidate $i: system=$sys extra=$extra" | tee -a "$OUT/meson-setup.log"
  # shellcheck disable=SC2086
  if meson setup "$WORK/build" "$WORK/mesa" --cross-file "$WORK/cross-$sys.txt" "${COMMON[@]}" $extra >> "$OUT/meson-setup.log" 2>&1; then
    echo "### candidate $i configured OK" | tee -a "$OUT/meson-setup.log"; ok=1; break
  else
    echo "### candidate $i FAILED" | tee -a "$OUT/meson-setup.log"
    { echo "=== candidate $i (system=$sys extra=$extra) errors/last lines"; grep -nE "ERROR|Problem encountered|Dependency .* not found|NO \(tried" "$WORK/build/meson-logs/meson-log.txt" | tail -15; tail -n 12 "$WORK/build/meson-logs/meson-log.txt"; } >> "$OUT/errors.txt" 2>&1 || true
  fi
done
[ "$ok" = 1 ] || { echo "no candidate configured"; exit 1; }

ninja -C "$WORK/build" 2>&1 | tee "$OUT/ninja.log"
DESTDIR="$WORK/stage" ninja -C "$WORK/build" install >/dev/null
find "$WORK/stage" \( -name '*.so' -o -name '*.so.*' \) -print | tee "$OUT/installed-files.txt"
mkdir -p "$OUT/vendor_lib"
find "$WORK/stage" -name '*.so' -exec cp -v {} "$OUT/vendor_lib/" \;
[ -f "$PREFIX/lib/libdrm.so" ] && cp -v "$PREFIX/lib/libdrm.so" "$OUT/vendor_lib/" || true
for f in "$OUT"/vendor_lib/*.so; do
  "$TC/bin/llvm-strip" --strip-unneeded "$f" || true
  { echo "# $(basename "$f")"; "$TC/bin/llvm-readelf" -h "$f" | grep -E 'Class|Machine'; "$TC/bin/llvm-readelf" -d "$f" | grep NEEDED; } >> "$OUT/elf-report.txt"
done
echo "done -> $OUT/vendor_lib"
