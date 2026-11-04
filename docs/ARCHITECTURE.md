# Architecture and plan

## Why "just a software EGL" is not enough
SurfaceFlinger (Android 11) needs four graphics layers to agree. Two of them are GPU-bound on this device:

1. **EGL/GLES** (RenderEngine). PowerVR `libEGL_mtk` -> dead. Replace with a CPU implementation.
2. **gralloc allocator/mapper 4.0**. The vendor allocator links `libPVRMtkutils`/`libsrv_um` and allocates through
   the PVR driver -> also dead. Replace with a CPU-memory gralloc.
3. **hwcomposer**. MTK HWC (`hwcomposer.mt6765`) interprets MTK/PVR gralloc handles via `libgralloc_extra`. If gralloc
   is replaced, HWC must be replaced too. The composer service already links `libhwc2onfbadapter`, i.e. AOSP can drive
   the panel from `/dev/graphics/fb0` (MTK_FB is enabled in the kernel) with client composition only.
4. **Boot wiring**: `ro.hardware.egl`, vintf manifest, init rc, SELinux.

A software EGL must also support **`EGL_ANDROID_image_native_buffer`** (SurfaceFlinger imports every layer buffer and
the client-target buffer as EGLImages). Stock Mesa `swrast` on Android and ANGLE-on-SwiftShader both lack a CPU-only
path for that, so stage 2 needs a shim or a Mesa patch that backs EGLImages with gralloc-mapped memory.

## Components
| # | Component | Approach | Status |
|---|---|---|---|
| 1 | GLES 1/2/3 + EGL | Mesa **softpipe** (no LLVM; works on ARMv7) cross-built with the NDK, ARMv7, API 30 | implemented, in installer |
| 2 | EGL native-buffer shim | CPU copy-in/copy-out EGLImage backed by `gralloc` lock/unlock; native fence = already-signalled fd | implemented (`stage2/eglshim/`), in installer |
| 3 | gralloc | Replace allocator 4.0 + mapper 4.0, or fall back to 2.0/3.0 so `gralloc.default` (ashmem + fb) is used | **partial/unconfirmed**: installer sets `ro.hardware.gralloc=default` (device already ships `gralloc.default.so` alongside `gralloc.rogue.so`) if the allocator service respects it. `android.hardware.graphics.allocator@4.0-service-mediatek` is statically linked to `libPVRMtkutils.so` though (`context/readelf/`), so this is NOT confirmed to route allocation away from the PVR driver - may still need a real custom allocator/mapper replacement. Needs on-device data to resolve. |
| 4 | HWC | Remove `hwcomposer.mt6765.so` so composer falls back to `hwc2onfbadapter` (fb0 client composition) | implemented, in installer (rename to `.softgfx_disabled` / restore on uninstall) |
| 5 | Wiring | `ro.hardware.egl=soft`, disable PVR/GED services, vintf edit, SELinux permissive for bring-up | `ro.hardware.egl`/`ro.opengles.version`/`ro.hardware.gralloc` props done. From a real `tools/collect_diag.sh` bundle: `vendor.mediatek.hardware.gpu@1.0-service` (init service `gpu-1-0`) confirmed as a plain autostart `class hal` service with no listed dependents - installer now disables it. The graphics HALs actually required by the device's own vintf manifest are `graphics.allocator@4.0` (hwbinder, binary `android.hardware.graphics.allocator@4.0-service-mediatek`), `graphics.composer@2.1` (hwbinder, `android.hardware.graphics.composer@2.1-service`) and **`graphics.mapper@4.0` as `transport="passthrough"`** - mapper being passthrough (loaded via `hw_get_module()`, not a binder service) means `ro.hardware.gralloc=default` has a real chance of working for the mapper side; the allocator service's own respect for that property is still unconfirmed. vintf edit and SELinux permissive are still **not attempted** - no AVC denial evidence yet to justify blind policy changes. |
| 6 | TWRP zip | Mount `/vendor` `/system_root`, back up originals, install, `uninstall` zip that restores | implemented, CI-built |

## Risks / unknowns (be honest)
- Cannot be tested here. Every stage needs on-device logs. Expect several iterations.
- SurfaceFlinger on Android 11 may hard-fail without gralloc 4.0 if the vintf manifest/compat matrix requires it.
  Mitigation: keep declaring an allocator/mapper we implement ourselves.
- softpipe at the panel resolution is slow. Fine for a lock screen with TalkBack, not for general use.
- SELinux: Samsung kernel is `SELINUX_DEVELOP=y`; permissive via a vendor init rc is plausible but unverified.
- FBE unlock itself (keymaster/gatekeeper/vaultkeeper) is independent of the GPU, but `vaultkeeper` is present in the
  manifest; not yet checked that it starts without the GPU services.
- The `ro.hardware.egl=meow` anomaly may mean earlier experiments modified `/vendor`; restore stock first.

## Alternative that may be far simpler for the stated goal (data recovery)
Decrypting `/data` inside TWRP (see `a10s-twrp-crypto`) avoids all of the above. This repo exists because that route
is blocked; keep both options open.
