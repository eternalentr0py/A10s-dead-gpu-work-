# Testing stage 1 on the device

The CI artifact `softgfx-stage1-armv7` contains `softgfx-test-bundle.zip`:
`egltest` (static probe), `run_egltest.sh`, and `vendor_root/vendor/lib/{egl,dri,...}`.

## Run (nothing is installed or modified on the phone)
1. Boot TWRP. Mount `/system_root` and `/vendor` (read-only is fine) so the platform libs
   (`libcutils`, `libhardware`, `libnativewindow`, `libsync`, `liblog`) can be borrowed.
2. Unzip the bundle on the SD card (or `/tmp`), e.g. `/external_sd/softgfx/`.
3. From the TWRP terminal: `sh /external_sd/softgfx/run_egltest.sh /external_sd/softgfx`
4. Send back `egltest_result.txt` (written next to the script).

## Reading the result
`RESULT: score=N/5` counts: eglInitialize, context, makeCurrent, clear+readback, GLSL compile.
Failure points and what they mean:
| Output | Meaning / next step |
|---|---|
| `dlopen(...)` FAIL | missing platform lib or wrong namespace; the message names it |
| `no-display` / `initialize-failed` | Mesa's Android EGL needs a gralloc/DRM device; plan: EGL shim (ARCHITECTURE #2) |
| `MISSING EGL_ANDROID_image_native_buffer` | expected for stock Mesa swrast; this is the shim's job |
| score 5/5 but image_native_buffer missing | GL itself works; only the buffer-import shim is left |
