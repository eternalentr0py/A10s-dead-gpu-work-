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

# egltest.c dlopen()s libEGL/libGLESv2 at runtime (so this test binary doesn't need them
# present at build time). dlopen() CANNOT work from a statically-linked executable on
# Android/Bionic - the static libdl.a is only a stub that aborts with "libdl.a is a stub
# --- use libdl.so instead" the moment you call it. So the STATIC binary can only ever prove
# "this ELF executes on this device"; it will always fail the actual EGL/GLES load step.
# The DYNAMIC binary is what actually exercises dlopen(), and is what we need here - but it
# needs a real linker present on-device (recovery often doesn't mount /system by default).
# Prefer dynamic whenever a linker is available; only fall back to static (smoke-test only,
# documented as such in its own output) if no linker can be found at all.
LNK=""
if [ -e /system/bin/linker ]; then
  LNK=""   # default system path works, no explicit interpreter override needed
  HAVE_LINKER=1
else
  HAVE_LINKER=0
  for c in /system_root/system/bin/linker /apex/com.android.runtime/bin/linker; do
    [ -e "$c" ] && LNK="$c" && HAVE_LINKER=1 && break
  done
fi

if [ "$HAVE_LINKER" = "1" ] && [ -x "$D/egltest" ]; then
  EGLTEST="$D/egltest"
  echo "using dynamic egltest (linker available) - this is the real test" >> "$OUT"
elif [ -x "$D/egltest_static" ]; then
  EGLTEST="$D/egltest_static"
  echo "WARNING: no linker found; mount System in TWRP (Mount menu) first." >> "$OUT"
  echo "WARNING: falling back to static egltest - this can only prove the ELF executes on" >> "$OUT"
  echo "WARNING: this device; it will always fail at the dlopen step below (expected -" >> "$OUT"
  echo "WARNING: static binaries cannot dlopen() on Android). Mount System and re-run for" >> "$OUT"
  echo "WARNING: the real EGL/GLES test." >> "$OUT"
else
  EGLTEST="$D/egltest"
  [ -z "$LNK" ] && [ "$HAVE_LINKER" = "0" ] && echo "WARNING: no linker found; mount System in TWRP (Mount menu) first" >> "$OUT"
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
