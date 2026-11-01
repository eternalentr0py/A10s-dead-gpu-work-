# Context

## Problem statement (from the user)
- GPU is dead. The PowerVR kernel driver ("PVRK") expects a GPU microcontroller (META/MIPS firmware
  processor) status value of `0x1` and always reads `0x0`; it marks the driver bad, SurfaceFlinger and
  everything attached crash, init restarts SurfaceFlinger, loop forever.
- Wanted: boot to the lock screen, enter the password (TalkBack already enabled) and recover data.
- Chosen approach: provide a different (software) GPU stack so SurfaceFlinger can start.
- NOTE: the original shared chat could not be fetched by the assistant (JS-rendered page). Anything
  from that chat not restated above is missing here; add it to this file.

## Device facts (from `gfx_collect`, taken in recovery)
- Model A107F, firmware `A107FXXS8CWD3`, Android 11 (SDK 30), security patch 2023-03-01
- `ro.board.platform=mt6765`, `ro.hardware.chipname=MT6762`
- **32-bit only**: `ro.product.cpu.abilist=armeabi-v7a,armeabi`, `ro.zygote=zygote32`. All native code must be ARMv7.
- `ro.vndk.version=30`, Treble enabled, `ro.opengles.version=196610` (GLES 3.2 advertised)
- `ro.hardware.egl=meow` in `/vendor/build.prop` — anomalous (stock EGL lib is `libEGL_mtk.so`, there is no
  `libEGL_meow.so`). Probably a previous experiment; confirm. With this value the EGL loader finds nothing.
- Partitions are classic (not dynamic): `/vendor`=mmcblk0p33, `/product`=mmcblk0p34, `/system_root`=mmcblk0p35
- Kernel (from `/proc/config.gz`): `CONFIG_MTK_FB=y` (fbdev), `CONFIG_ION`/`MTK_ION`, `ASHMEM`, `DMA_SHARED_BUFFER`,
  `CONFIG_MTK_GPU_VERSION="rgx doma ..."`, **`DRM_MEDIATEK` not set, `DRM_VGEM` not set** (so no DRM/KMS display node,
  no vgem for Mesa kms_swrast path), `SECURITY_SELINUX_DEVELOP=y` (permissive is possible), `SELINUX_BOOTPARAM` not set.
- Graphics HALs present in `/vendor`:
  - `android.hardware.graphics.allocator@4.0-service-mediatek` — links `libPVRMtkutils.so`; strings show
    `gralloc_device_alloc_img` → **allocator depends on the PowerVR userland, so it fails with the dead GPU too**
  - `android.hardware.graphics.composer@2.1-service` — links `libhwc2on1adapter` **and** `libhwc2onfbadapter`
  - `hwcomposer.mt6765.so` (MTK HWC; uses `libdpframework`, `libgralloc_extra`, ION)
  - `gralloc.rogue.so` (PowerVR gralloc), `gralloc.default.so` (stock AOSP-style fb/ashmem gralloc, has `/dev/graphics/fb%u`)
  - `vulkan.mt6765.so`, `libEGL_mtk.so`, `libGLESv1_CM_mtk.so`, `libGLESv2_mtk.so` (all PowerVR: `libsrv_um`, `libusc`, `libPVRMtkutils`)
  - mapper 4.0 declared passthrough 32+64 in the vintf manifest
- `vendor/firmware/rgx.fw` (112 KiB) and `rgx.sh` (302 KiB) = PowerVR firmware
- `surfaceflinger.rc` has `onrestart restart zygote` (why the whole UI loops)

## What the dump does NOT contain
- The dmesg is from **recovery**, so it has no PVR driver messages. Evidence of the real failure is missing.
  Run `tools/collect_boot_diag.sh` in TWRP after a failed normal boot to capture it (pstore / last_kmsg).

## Findings from diag_bundle (taken in TWRP)
- Real-boot failure (user-pasted kernel log): `PVRSRVPollForValueKM: Failed! Error(PVRSRV_ERROR_TIMEOUT)` ->
  `RGXPostPowerState: Polling for 'FW started' flag failed` -> `PVRSRVDeviceFinalise: Failed to set device power
  state to 'on'` -> `PVRSRVDeviceOpen: ... Driver already in bad state`. i.e. the GPU firmware never reports
  started; every later PVR client (gralloc allocator, EGL, ...) is refused.
- pstore/last_kmsg were empty (no logs from the failed boot survive).
- TWRP cmdline: `androidboot.vbmeta.device_state=unlocked`, `verifiedbootstate=green`, `veritymode=enforcing`,
  `dm="1 vroot ... verity ..."` (system), fstab uses `avb` for vendor/product, `androidboot.selinux=permissive`
  (may be TWRP-only), ramoops/pstore configured.
- Services: `vendor.gralloc-4-0` (allocator 4.0, PVR) and `vendor.hwcomposer-2-1` both have `onrestart restart surfaceflinger`;
  surfaceflinger has `onrestart restart zygote`. `gralloc.default.so` needs only liblog/libcutils/libc++ (GPU-free).
- `/vendor/build.prop` has `ro.hardware.egl=meow` (stock would be `mtk`): /vendor was edited before.
- vendor manifest declares allocator 4.0, mapper 4.0 (passthrough), composer 2.1.

## Revised plan (two routes)
- **Route B (preferred, smallest change):** patch the PowerVR kernel driver in boot.img so the FW-wait helper
  `PVRSRVPollForValueKM` returns success immediately -> driver stays "OK", stock gralloc 4.0 / HWC keep working
  (no GPU needed for plain buffer allocation) -> add the software EGL shim (`ro.hardware.egl=soft`). Needs: boot.img.
- **Route A (fallback):** replace gralloc (use legacy `gralloc.default` via gralloc 2.0/2.1 passthrough) and drop
  `hwcomposer.mt6765.so` so composer falls back to fb0. More components, needs HIDL headers (VNDK v30).

## Kernel analysis (boot.img dump, boot header v2, arm64 gzip Image, 27,011,072 bytes decompressed)
- PowerVR Rogue driver is built into the kernel. Image offsets (= vaddr - base):
  - `PVRSRVPollForValueKM(dev, addr, value, mask, flags)` @ 0x59e1f4: spins on `*addr & mask == value`, 5 s budget (0x4c4b40 us), returns 9 on timeout
  - `RGXPostPowerState` calls it @ 0x57bba4 with (value=1, mask=-1, flags=3) on the FW "started" flag (fw struct +0x84); on failure it logs and returns 9
  - 10 call sites in total (3 are generic PMR/memory pollers at 0x563c78/0x563cfc/0x563d20)
- Other firmware-dependent steps exist after "FW started" (e.g. RGXDevInitCompatCheck compares FW-reported BVNC/DDK; KCCB command
  completion). A patch of the FW-started wait alone may only move the failure; hence two patch variants (`tools/patch_pvr_boot.py`).
- Kernel recompressed with zopfli (11,258,19x bytes < original 11,648,764), zero padded to the original kernel_size; header id recomputed
  (formula verified against the original); AVB footer untouched. Original gzip was `gzip -9 -n`.
- ramoops/pstore is configured on the cmdline, so after a failed boot `/sys/fs/pstore/console-ramoops*` may hold the kernel log.
