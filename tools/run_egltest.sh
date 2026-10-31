#!/system/bin/sh
# Run the EGL capability test on the device (TWRP terminal or adb shell, recovery or normal).
# Usage: sh run_egltest.sh <dir containing egltest and vendor_root/>   (default: dir of this script)
D="${1:-$(cd "$(dirname "$0")" && pwd)}"
V="$D/vendor_root/vendor/lib"
chmod 755 "$D/egltest" 2>/dev/null
# Platform libs (libcutils, libhardware, libnativewindow, libsync, liblog): borrow from the mounted system if the
# current environment (e.g. recovery) lacks them.
EXTRA=""
for p in /system_root/system/lib /system/lib /vendor/lib; do [ -d "$p" ] && EXTRA="$EXTRA:$p"; done
export LD_LIBRARY_PATH="$V:$V/egl$EXTRA"
export LIBGL_DRIVERS_PATH="$V/dri"       # where Mesa looks for swrast_dri.so (overrides the /vendor/lib/dri default)
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=softpipe
export EGL_LOG_LEVEL=debug
export LIBGL_DEBUG=verbose
OUT="${2:-$D/egltest_result.txt}"
"$D/egltest" "$V/egl/libEGL_soft.so" "$V/egl/libGLESv2_soft.so" > "$OUT" 2>&1
echo "exit=$?" >> "$OUT"
echo "wrote $OUT"; tail -n 5 "$OUT"
