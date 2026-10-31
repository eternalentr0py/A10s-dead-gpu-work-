#!/system/bin/sh
# Run from TWRP terminal (or adb shell in recovery) AFTER one failed normal boot attempt.
# Collects evidence we still lack. Output: /tmp/boot_diag.tar.gz  (copy to SD card / pull it).
O=/tmp/boot_diag; rm -rf $O; mkdir -p $O
mount -o ro /dev/block/mmcblk0p33 /vendor 2>/dev/null
mount -o ro /dev/block/mmcblk0p35 /system_root 2>/dev/null
mount -o ro /dev/block/mmcblk0p34 /product 2>/dev/null
cp /proc/last_kmsg $O/ 2>/dev/null
cp -r /sys/fs/pstore $O/pstore 2>/dev/null
dmesg > $O/dmesg_recovery.txt 2>&1
ls -l /dev/pvr* /dev/mali* /dev/ion /dev/graphics /dev/dri /dev/ged 2>&1 > $O/dev_nodes.txt
cat /proc/cmdline > $O/cmdline.txt
cat /proc/modules > $O/modules.txt 2>&1
ls -lR /vendor/lib/modules /vendor/lib/hw /vendor/lib/egl /vendor/lib /vendor/bin/hw > $O/vendor_listing.txt 2>&1
mkdir -p $O/rc; for d in /vendor/etc/init /system_root/system/etc/init /system_root/system/system_ext/etc/init /product/etc/init; do cp -r $d $O/rc/$(echo $d | tr / _) 2>/dev/null; done
cp /vendor/etc/fstab.* $O/ 2>/dev/null
cp /vendor/etc/vintf/manifest.xml $O/vendor_manifest.xml 2>/dev/null
cp /system_root/system/etc/vintf/compatibility_matrix*.xml $O/ 2>/dev/null
cp /vendor/etc/selinux/precompiled_sepolicy* /vendor/etc/selinux/*_sepolicy* $O/ 2>/dev/null
ls -l /vendor/etc/selinux /system_root/system/etc/selinux > $O/selinux_listing.txt 2>&1
grep -aE "^ro\.(hardware|board|opengles|sf|hwui)|egl|gralloc|vulkan" /vendor/build.prop /vendor/default.prop /system_root/system/build.prop /product/build.prop > $O/props.txt 2>/dev/null
ls -l /cache /cache/recovery /data/system/dropbox /data/tombstones 2>&1 > $O/cache_data_listing.txt
cp /cache/recovery/last_log /cache/recovery/last_kmsg $O/ 2>/dev/null
cd /tmp && tar -czf boot_diag.tar.gz boot_diag && echo "wrote /tmp/boot_diag.tar.gz"
