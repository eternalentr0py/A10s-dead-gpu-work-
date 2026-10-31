# A10s dead-GPU: software graphics stack

Goal: make a Samsung Galaxy A10s (SM-A107F, MT6762/mt6765, Android 11, 32-bit userland,
PowerVR GE8320 "rgx") **boot to the lock screen with a dead GPU**, by replacing the GPU-dependent
graphics userland with a pure-CPU one, delivered as a **TWRP-flashable zip**. Purpose: reach the
lock screen (TalkBack is enabled) so the user can unlock FBE-encrypted `/data` and recover it.

Status: **stage 1 builds** (Mesa softpipe EGL + GLESv1/v2 + swrast DRI driver, ARMv7, packaged under `vendor_root/vendor/lib`). Nothing has been tested on a device yet.
Read `docs/ARCHITECTURE.md` first, then `docs/CONTEXT.md`.

| Path | What |
|---|---|
| `docs/CONTEXT.md` | Facts established from the device dump (`gfx_collect`) and the original problem statement |
| `docs/ARCHITECTURE.md` | Why GLES alone is not enough, the component plan, risks, open questions |
| `context/` | Text-only extracts from the dump (props, kernel config, vintf, ELF dependency lists). No proprietary binaries |
| `stage1/` | Build script + Meson cross file for Mesa softpipe (EGL/GLESv1/GLESv2) for armeabi-v7a |
| `tools/collect_boot_diag.sh` | Run in TWRP to collect the data we still lack (real-boot failure evidence, init rc, sepolicy) |
| `.github/workflows/build-softgfx.yml` | `workflow_dispatch` build; uploads the `.so` files as an artifact |

Proprietary Samsung/MediaTek/Imagination blobs are deliberately **not** committed.
