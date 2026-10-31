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

# Diagnostics: how does this Mesa version decide with_dri / EGL?
{ echo "== with_dri / EGL requirement lines =="; grep -nE "with_dri\b|EGL requires|with_dri2|with_platform_android" "$WORK/mesa/meson.build" | head -60; } > "$OUT/meson-dri-lines.txt" 2>&1 || true

COMMON=(
  --cross-file "$WORK/android-armv7.cross"
  --buildtype=release -Db_ndebug=true -Db_lto=false
  -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true
  -Dglx=disabled -Dgbm=disabled -Degl-native-platform=android
  -Dgallium-drivers=swrast -Dvulkan-drivers=
  -Dllvm=disabled -Dshared-glapi=enabled -Dglvnd=false
  -Degl-lib-suffix=_soft
  -Dlmsensors=disabled -Dlibunwind=disabled -Dvalgrind=disabled
  -Dbuild-tests=false -Dshader-cache=disabled -Dzstd=disabled
  -Dosmesa=false -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled
  --wrap-mode=default
)
# Candidate extra option sets, tried in order until one configures.
CANDIDATES=(
  "-Ddri-drivers="
  "-Ddri-drivers=swrast"
  "-Ddri-drivers=auto"
  "-Ddri-drivers= -Dglx=dri"
  "-Ddri-drivers=swrast -Dglx=dri"
)
ok=0; i=0
for extra in "${CANDIDATES[@]}"; do
  i=$((i+1)); rm -rf "$WORK/build"
  echo "### candidate $i: $extra" | tee -a "$OUT/meson-setup.log"
  # shellcheck disable=SC2086
  if meson setup "$WORK/build" "$WORK/mesa" "${COMMON[@]}" $extra >> "$OUT/meson-setup.log" 2>&1; then
    echo "### candidate $i configured OK" | tee -a "$OUT/meson-setup.log"; ok=1; break
  else
    echo "### candidate $i FAILED:" | tee -a "$OUT/meson-setup.log"
    grep -E "ERROR|Problem encountered" "$OUT/meson-setup.log" | tail -3 | tee -a "$OUT/meson-setup.log"
  fi
done
[ "$ok" = 1 ] || { echo "no candidate configured"; exit 1; }

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
