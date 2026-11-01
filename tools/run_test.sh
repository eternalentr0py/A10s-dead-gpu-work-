#!/system/bin/sh
# TWRP terminal:   sh /tmp/softgfx/run_test.sh
# Needs System mounted (loader at /system_root/system/bin/bootstrap/linker). Changes nothing on the phone.
D="$(cd "$(dirname "$0")" && pwd)"
V="$D/vendor_root/vendor/lib"
LNK=/system_root/system/bin/bootstrap/linker
chmod 755 "$D/egltest" 2>/dev/null
[ -e "$LNK" ] || { echo "System not mounted. Mount System in TWRP (Mount menu), then run again."; exit 1; }
OUT="$D/result.txt"
env LD_LIBRARY_PATH="$V:$V/egl:/system_root/system/lib:/system_root/system/apex/com.android.runtime/lib/bionic" \
    LIBGL_DRIVERS_PATH="$V/egl" LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe \
    "$LNK" "$D/egltest" "$V/egl/libEGL_soft.so" "$V/egl/libGLESv2_soft.so" 2>&1 | grep -v "application_versions" > "$OUT"
echo "exit=$?" >> "$OUT"
tail -n 40 "$OUT"
echo; echo "Send me: $OUT"
