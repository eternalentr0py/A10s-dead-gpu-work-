#!/system/bin/sh
# Run the EGL capability test on the device (TWRP terminal or adb shell, recovery or normal).
# Usage: sh run_egltest.sh <dir containing egltest and vendor_root/>   (default: dir of this script)
D="${1:-$(cd "$(dirname "$0")" && pwd)}"
V="$D/vendor_root/vendor/lib"
chmod 755 "$D/egltest" 2>/dev/null
# Platform libs (libcutils, libhardware, libnativewindow, libsync, liblog): borrow from the mounted system if the
# current environment (e.g. recovery) lacks them.
# NOTE: these are NOT exported globally on purpose. TWRP's own shell binaries (tail, chmod, etc.)
# can be a different bitness/build than this device's 32-bit-only Android userland, and an exported
# LD_LIBRARY_PATH pointing at /system_root/system/lib leaks into every command run in this script
# afterwards - including the final `tail` below - and can break them with errors like
# "CANNOT LINK EXECUTABLE ... is 32-bit instead of 64-bit". Instead these are passed only to the
# actual test invocation via `env`, below.
EXTRA=""
for p in /system_root/system/lib /system/lib /vendor/lib; do [ -d "$p" ] && EXTRA="$EXTRA:$p"; done
TEST_LD_LIBRARY_PATH="$V:$V/egl$EXTRA"
OUT="${2:-$D/egltest_result.txt}"
# Prefer static binary (no linker/system mount needed); fall back to dynamic.
if [ -x "$D/egltest_static" ]; then
  EGLTEST="$D/egltest_static"
  echo "using static egltest" >> "$OUT"
else
  EGLTEST="$D/egltest"
fi
# egltest's ELF interpreter is /system/bin/linker; recovery may not have it unless system is mounted.
LNK=""
if [ ! -e /system/bin/linker ]; then
  for c in /system_root/system/bin/linker /apex/com.android.runtime/bin/linker; do [ -e "$c" ] && LNK="$c" && break; done
  [ -z "$LNK" ] && echo "WARNING: no linker found; mount System in TWRP (Mount menu) first" >> "$OUT"
fi
env LD_LIBRARY_PATH="$TEST_LD_LIBRARY_PATH" \
    LIBGL_DRIVERS_PATH="$V/dri" \
    LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=softpipe \
    EGL_LOG_LEVEL=debug \
    LIBGL_DEBUG=verbose \
    $LNK "$EGLTEST" "$V/egl/libEGL_soft.so" "$V/egl/libGLESv2_soft.so" >> "$OUT" 2>&1
echo "exit=$?" >> "$OUT"
echo "wrote $OUT"
# Use this shell's own LD_LIBRARY_PATH (unpolluted) for tail, not the test's.
tail -n 5 "$OUT"
