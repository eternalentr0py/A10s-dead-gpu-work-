# Stage 2: SwiftShader as an alternative GLES/EGL backend

This is a **parallel track to `stage1/` (Mesa softpipe)**, not a replacement or a "fix" by itself.
It swaps in [Google SwiftShader](https://github.com/google/swiftshader) (Apache-2.0, the same
software renderer used by the Android emulator and Chrome's software GL fallback) as the
CPU-only EGL/GLESv1/GLESv2 implementation, built for `armeabi-v7a` (this device is 32-bit
userland only — see `docs/CONTEXT.md`).

## Why this exists alongside stage1
`docs/ARCHITECTURE.md` already establishes that **no CPU-only EGL/GLES library, on its own,
gets SurfaceFlinger running on this device.** The allocator HAL (`gralloc.rogue.so`) also
depends on the dead PowerVR driver, and SurfaceFlinger's EGLImage-native-buffer path
(`EGL_ANDROID_image_native_buffer`) needs a gralloc-backed shim regardless of which GLES
library sits underneath it. That analysis applies equally to SwiftShader. This stage does not
re-litigate or bypass that — it produces a second candidate for component #1 in the
architecture table (`GLES 1/2/3 + EGL`), so there are two independently-buildable options by
the time components 2-6 (gralloc replacement, HWC fallback, wiring) are ready to test against
real hardware.

SwiftShader is worth having as a second option because:
- It's a far more actively maintained, widely-deployed software GL/EGL stack than Mesa softpipe
  on Android (it's what `ANGLE`/Chrome and the Android emulator fall back to), so if Mesa
  softpipe hits an Android-specific gap stage1 can't close, SwiftShader is a credible fallback.
- Its `EGL_ANDROID_image_native_buffer` support is more complete upstream than stock Mesa's,
  which may reduce (not eliminate) the shim work described in `ARCHITECTURE.md` component #2.

## Status
**CI build only, nothing tested on a device.** Same caveat as stage1. This produces:
1. `libEGL_swiftshader.so`, `libGLESv1_CM_swiftshader.so`, `libGLESv2_swiftshader.so`
   cross-compiled for armeabi-v7a, under `out/vendor_root/vendor/lib/egl/`.
2. A standalone smoke-test bundle (reusing `tools/egltest.c` / `tools/run_egltest.sh` from the
   shared `tools/` dir, same as stage1) to confirm an EGL context can actually be created with
   this build, independent of the device's real graphics HAL chain.

It does **not** yet include: gralloc replacement, HWC changes, vintf/init wiring, or a
TWRP-flashable installer zip. Those are shared, not duplicated per-backend — once one GLES
backend (stage1 or stage2) is confirmed working via the smoke test, the remaining components
from `ARCHITECTURE.md` get built once and paired with whichever backend tests out better.

## Files
| Path | What |
|---|---|
| `build_swiftshader.sh` | Cross-compiles SwiftShader EGL/GLESv1_CM/GLESv2 for armeabi-v7a via the NDK's CMake toolchain, Subzero (non-LLVM) JIT backend |
| `../.github/workflows/build-swiftshader.yml` | `workflow_dispatch` build; publishes a **GitHub Release** (not an Actions artifact) so the result isn't subject to the artifact storage cap |

## Running it
From the GitHub Actions tab on this branch: **Run workflow** on `build-swiftshader.yml`. Inputs:
- `swiftshader_ref` — git tag/branch/commit in google/swiftshader (default: `main`)
- `ndk` — NDK version (default: matches stage1's `r25c` for consistency)

When it finishes, a new Release is published on this repo (tag `swiftshader-<run id>`) with the
built `.so` files and the smoke-test bundle attached as downloadable Release assets.
