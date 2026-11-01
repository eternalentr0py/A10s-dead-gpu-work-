#!/system/bin/sh
# Collect the files needed for the gralloc/composer/boot-wiring work into ONE archive in /tmp.
# Run in TWRP terminal:   sh /tmp/softgfx/collect_diag.sh
# Read-only: mounts system/vendor read-only if needed and copies files; changes nothing on the phone.
# For useful last_kmsg/pstore: boot the phone normally ONCE (let it crash-loop), then boot back to TWRP and run this.
R="${ROOT:-}"                       # test hook; leave empty on the phone
OUT=/tmp/diag_bundle
LOG="$OUT/_collect_log.txt"
rm -rf "$OUT" /tmp/diag_bundle.zip /tmp/diag_bundle.tar.gz
mkdir -p "$OUT"

mnt() {  # $1 device  $2 mount point
  [ -n "$R" ] && return 0
  mkdir -p "$2" 2>/dev/null
  grep -q " $2 " /proc/mounts && return 0
  mount -t ext4 -o ro "$1" "$2" 2>/dev/null || mount -o ro "$1" "$2" 2>/dev/null
}
mnt /dev/block/mmcblk0p35 /system_root
mnt /dev/block/mmcblk0p33 /vendor

grab() {  # $1 = source path (file or dir); keeps the path structure under $OUT
  src="$R$1"
  if [ -e "$src" ]; then
    mkdir -p "$OUT$(dirname "$1")"
    cp -r "$src" "$OUT$(dirname "$1")/" 2>/dev/null && echo "ok      $1" >> "$LOG" || echo "FAILED  $1" >> "$LOG"
  else
    echo "MISSING $1" >> "$LOG"
  fi
}

echo "collect_diag $(date 2>/dev/null)" > "$LOG"

# --- requested ---
grab /vendor/etc/init
grab /vendor/etc/selinux
for f in "$R"/vendor/etc/fstab.*; do [ -e "$f" ] && grab "${f#$R}"; done
grab /system_root/system/etc/vintf/compatibility_matrix.device.xml
grab /system_root/system/etc/vintf/manifest.xml
grab /vendor/etc/vintf
# --- small extras that help the same work ---
grab /vendor/build.prop
grab /vendor/default.prop
grab /system_root/system/build.prop
grab /system_root/system/etc/init
grab /system_root/system/etc/selinux/plat_sepolicy.cil
grab /system_root/system/etc/selinux/plat_file_contexts
grab /system_root/system/etc/selinux/plat_property_contexts
grab /vendor/etc/selinux/vendor_file_contexts
grab /vendor/etc/selinux/vendor_property_contexts

# --- runtime evidence ---
cat /proc/cmdline > "$OUT/proc_cmdline.txt" 2>&1
[ -e "$R/proc/last_kmsg" ] && cp "$R/proc/last_kmsg" "$OUT/last_kmsg.txt" 2>/dev/null || echo "MISSING /proc/last_kmsg" >> "$LOG"
if [ -d "$R/sys/fs/pstore" ]; then mkdir -p "$OUT/pstore"; cp -r "$R"/sys/fs/pstore/* "$OUT/pstore/" 2>/dev/null; echo "ok      /sys/fs/pstore ($(ls "$R/sys/fs/pstore" 2>/dev/null | wc -l) files)" >> "$LOG"; else echo "MISSING /sys/fs/pstore" >> "$LOG"; fi
dmesg > "$OUT/dmesg_recovery.txt" 2>&1
ls -l /dev/pvr* /dev/mali* /dev/ion /dev/graphics /dev/dri /dev/ged /dev/fb* > "$OUT/dev_nodes.txt" 2>&1
cat /proc/modules > "$OUT/modules.txt" 2>&1
ls -l "$R/vendor/lib/modules" "$R/vendor/lib/hw" "$R/vendor/lib/egl" "$R/vendor/bin/hw" > "$OUT/vendor_listing.txt" 2>&1

# --- pack: zip if available, else tar.gz ---
cd /tmp || exit 1
if command -v zip >/dev/null 2>&1; then
  zip -qr diag_bundle.zip diag_bundle && A=/tmp/diag_bundle.zip
else
  tar -czf diag_bundle.tar.gz diag_bundle && A=/tmp/diag_bundle.tar.gz
fi
echo; echo "--- collected:"; grep -c "^ok" "$LOG" | sed 's/$/ items ok/'; grep -E "MISSING|FAILED" "$LOG"
echo; echo "Archive: $A  ($(ls -l "$A" | awk '{print $5}') bytes)"
echo "Pull it with:  adb pull $A ."
