// libEGL_soft.so -- Android EGL driver shim over Mesa softpipe (surfaceless).
//
// Android's EGL loader (libEGL.so) loads this as the vendor EGL driver (ro.hardware.egl=soft).
// It forwards almost everything to the real Mesa libEGL (libEGL_mesa_real.so, surfaceless platform)
// and adds what SurfaceFlinger / HWUI need but Mesa-on-surfaceless lacks:
//   * window-capable EGLConfigs (+ EGL_ANDROID_recordable, native visual ids)
//   * EGLSurface for ANativeWindow: render to an internal pbuffer, copy to the window buffer on swap
//   * EGL_ANDROID_image_native_buffer: CPU-copy import/export of gralloc buffers as GL textures
//   * EGL_ANDROID_native_fence_sync: fence that is always already signalled (fd = -1)
// STATUS: experimental, untested on device. See docs/ARCHITECTURE.md.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "softegl", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "softegl", __VA_ARGS__)

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif
#ifndef EGL_FRAMEBUFFER_TARGET_ANDROID
#define EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147
#endif
#define EGL_PLATFORM_SURFACELESS_MESA_ 0x31DD

// ---- Minimal Android 11 native window/buffer layouts (frameworks/native nativebase.h, system/window.h) ----
struct android_native_base_t {
    int magic; int version; void* reserved[4];
    void (*incRef)(android_native_base_t*); void (*decRef)(android_native_base_t*);
};
struct ANativeWindowBuffer {
    android_native_base_t common;
    int width, height, stride, format, usage_deprecated;
    uintptr_t layerCount;
    void* reserved[1];
    const void* handle;
    uint64_t usage;
    void* reserved_proc[8 - (sizeof(uint64_t) / sizeof(void*))];
};
struct ANativeWindow {
    android_native_base_t common;
    const uint32_t flags; const int minSwapInterval; const int maxSwapInterval;
    const float xdpi; const float ydpi; intptr_t oem[4];
    int (*setSwapInterval)(ANativeWindow*, int);
    int (*dequeueBuffer_DEPRECATED)(ANativeWindow*, ANativeWindowBuffer**);
    int (*lockBuffer_DEPRECATED)(ANativeWindow*, ANativeWindowBuffer*);
    int (*queueBuffer_DEPRECATED)(ANativeWindow*, ANativeWindowBuffer*);
    int (*query)(const ANativeWindow*, int, int*);
    int (*perform)(ANativeWindow*, int, ...);
    int (*cancelBuffer_DEPRECATED)(ANativeWindow*, ANativeWindowBuffer*);
    int (*dequeueBuffer)(ANativeWindow*, ANativeWindowBuffer**, int*);
    int (*queueBuffer)(ANativeWindow*, ANativeWindowBuffer*, int);
    int (*cancelBuffer)(ANativeWindow*, ANativeWindowBuffer*, int);
};
enum { NW_WIDTH = 0, NW_HEIGHT = 1, NW_SET_USAGE64 = 30 };
enum { HAL_RGBA_8888 = 1, HAL_RGBX_8888 = 2, HAL_RGB_565 = 4, HAL_BGRA_8888 = 5 };

// ---- real Mesa EGL ----
static void* g_real = nullptr;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void load_real() {
    std::string dir;
    Dl_info di;
    if (dladdr((void*)&load_real, &di) && di.dli_fname) {
        std::string p = di.dli_fname; size_t s = p.rfind('/');
        if (s != std::string::npos) dir = p.substr(0, s + 1);
    }
    g_real = dlopen((dir + "libEGL_mesa_real.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_real) g_real = dlopen("libEGL_mesa_real.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_real) LOGE("cannot load libEGL_mesa_real.so: %s", dlerror());
    else LOGI("loaded real Mesa EGL from %s", dir.c_str());
}
static void* R(const char* n) { pthread_once(&g_once, load_real); return g_real ? dlsym(g_real, n) : nullptr; }
#define REAL(NAME) static auto real_fn = reinterpret_cast<decltype(&NAME)>(R(#NAME))

// ---- GL access through Mesa's eglGetProcAddress (works for all core GL functions) ----
typedef void* (*PFGETPROC)(const char*);
static void* GLP(const char* n) { static PFGETPROC gp = (PFGETPROC)R("eglGetProcAddress"); return gp ? gp(n) : nullptr; }
struct GLFns {
    void (*GenTextures)(GLsizei, GLuint*); void (*DeleteTextures)(GLsizei, const GLuint*);
    void (*BindTexture)(GLenum, GLuint); void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void (*GetIntegerv)(GLenum, GLint*); void (*PixelStorei)(GLenum, GLint);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    void (*GenFramebuffers)(GLsizei, GLuint*); void (*DeleteFramebuffers)(GLsizei, const GLuint*);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*Finish)(void);
    void (*ImageTargetTexture2D)(GLenum, GLeglImageOES);
    bool ok = false;
};
static GLFns g_gl;
static bool gl_init() {
    if (g_gl.ok) return true;
#define L(f) g_gl.f = (decltype(g_gl.f))GLP("gl" #f)
    L(GenTextures); L(DeleteTextures); L(BindTexture); L(TexParameteri); L(TexImage2D); L(TexSubImage2D);
    L(GetIntegerv); L(PixelStorei); L(ReadPixels); L(GenFramebuffers); L(DeleteFramebuffers);
    L(BindFramebuffer); L(FramebufferTexture2D); L(Finish);
    g_gl.ImageTargetTexture2D = (decltype(g_gl.ImageTargetTexture2D))GLP("glEGLImageTargetTexture2DOES");
#undef L
    g_gl.ok = g_gl.GenTextures && g_gl.BindTexture && g_gl.TexImage2D && g_gl.ReadPixels && g_gl.GenFramebuffers &&
              g_gl.FramebufferTexture2D && g_gl.ImageTargetTexture2D && g_gl.GetIntegerv;
    if (!g_gl.ok) LOGE("GL entry points missing");
    return g_gl.ok;
}

// ---- pixel helpers ----
static uint64_t hash_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL; const uint32_t* w = (const uint32_t*)p; size_t nw = n / 4;
    for (size_t i = 0; i < nw; i++) { h ^= w[i]; h *= 1099511628211ULL; }
    return h;
}
static int bpp_of(int fmt) { return fmt == HAL_RGB_565 ? 2 : 4; }
static void buf_to_rgba(const uint8_t* src, int stride, int fmt, int w, int h, std::vector<uint8_t>& out) {
    out.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * stride * bpp_of(fmt); uint8_t* d = &out[(size_t)y * w * 4];
        if (fmt == HAL_RGB_565) {
            for (int x = 0; x < w; x++) { uint16_t v = ((const uint16_t*)s)[x];
                d[x*4] = ((v >> 11) & 31) * 255 / 31; d[x*4+1] = ((v >> 5) & 63) * 255 / 63; d[x*4+2] = (v & 31) * 255 / 31; d[x*4+3] = 255; }
        } else if (fmt == HAL_BGRA_8888) {
            for (int x = 0; x < w; x++) { d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3]; }
        } else {
            memcpy(d, s, (size_t)w * 4);
            if (fmt == HAL_RGBX_8888) for (int x = 0; x < w; x++) d[x*4+3] = 255;
        }
    }
}
static void rgba_to_buf(const uint8_t* rgba, int w, int h, uint8_t* dst, int stride, int fmt, bool flip) {
    for (int y = 0; y < h; y++) {
        const uint8_t* s = rgba + (size_t)(flip ? (h - 1 - y) : y) * w * 4; uint8_t* d = dst + (size_t)y * stride * bpp_of(fmt);
        if (fmt == HAL_RGB_565) {
            for (int x = 0; x < w; x++) ((uint16_t*)d)[x] = ((s[x*4] >> 3) << 11) | ((s[x*4+1] >> 2) << 5) | (s[x*4+2] >> 3);
        } else if (fmt == HAL_BGRA_8888) {
            for (int x = 0; x < w; x++) { d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3]; }
        } else memcpy(d, s, (size_t)w * 4);
    }
}
static bool fmt_supported(int f) { return f == HAL_RGBA_8888 || f == HAL_RGBX_8888 || f == HAL_RGB_565 || f == HAL_BGRA_8888; }

// ---- registries ----
static std::mutex g_mu;
struct ShimSurface { EGLSurface real; ANativeWindow* win; int w, h; EGLConfig cfg; std::vector<uint8_t> tmp; };
struct ShimImage {
    ANativeWindowBuffer* anb; AHardwareBuffer* ahb; int w, h, stride, fmt;
    GLuint tex = 0; EGLImageKHR realImage = nullptr; EGLContext ctx = nullptr; EGLDisplay dpy = nullptr;
    uint64_t knownHash = 0; bool uploaded = false, bound = false;
};
static std::set<ShimSurface*> g_surfs;
static std::set<ShimImage*> g_images;
static ShimSurface* find_surf(EGLSurface s) { std::lock_guard<std::mutex> l(g_mu); ShimSurface* p = (ShimSurface*)s; return g_surfs.count(p) ? p : nullptr; }
static ShimSurface* find_surf_by_real(EGLSurface r) { std::lock_guard<std::mutex> l(g_mu); for (auto* s : g_surfs) if (s->real == r) return s; return nullptr; }
static ShimImage* find_img(void* i) { std::lock_guard<std::mutex> l(g_mu); ShimImage* p = (ShimImage*)i; return g_images.count(p) ? p : nullptr; }
static EGLSurface to_real(EGLSurface s) { ShimSurface* p = find_surf(s); return p ? p->real : s; }

// ---- core forwards ----
EXPORT EGLDisplay eglGetDisplay(EGLNativeDisplayType) {
    typedef EGLDisplay (*PF)(EGLenum, void*, const EGLAttrib*);
    typedef EGLDisplay (*PFE)(EGLenum, void*, const EGLint*);
    static PF gpd = (PF)R("eglGetPlatformDisplay"); static PFE gpe = (PFE)R("eglGetPlatformDisplayEXT");
    if (gpd) return gpd(EGL_PLATFORM_SURFACELESS_MESA_, (void*)EGL_DEFAULT_DISPLAY, nullptr);
    if (gpe) return gpe(EGL_PLATFORM_SURFACELESS_MESA_, (void*)EGL_DEFAULT_DISPLAY, nullptr);
    LOGE("no eglGetPlatformDisplay in Mesa"); return EGL_NO_DISPLAY;
}
EXPORT EGLBoolean eglInitialize(EGLDisplay d, EGLint* a, EGLint* b) { REAL(eglInitialize); return real_fn ? real_fn(d, a, b) : EGL_FALSE; }
EXPORT EGLBoolean eglTerminate(EGLDisplay d) { REAL(eglTerminate); return real_fn(d); }
EXPORT EGLint eglGetError(void) { REAL(eglGetError); return real_fn ? real_fn() : EGL_NOT_INITIALIZED; }
EXPORT EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig* c, EGLint n, EGLint* num) { REAL(eglGetConfigs); return real_fn(d, c, n, num); }
EXPORT EGLBoolean eglBindAPI(EGLenum a) { REAL(eglBindAPI); return real_fn(a); }
EXPORT EGLenum eglQueryAPI(void) { REAL(eglQueryAPI); return real_fn(); }
EXPORT EGLBoolean eglWaitClient(void) { REAL(eglWaitClient); return real_fn(); }
EXPORT EGLBoolean eglWaitGL(void) { REAL(eglWaitGL); return real_fn(); }
EXPORT EGLBoolean eglWaitNative(EGLint e) { REAL(eglWaitNative); return real_fn(e); }
EXPORT EGLBoolean eglReleaseThread(void) { REAL(eglReleaseThread); return real_fn(); }
EXPORT EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint* a) { REAL(eglCreateContext); return real_fn(d, c, s, a); }
EXPORT EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { REAL(eglDestroyContext); return real_fn(d, c); }
EXPORT EGLContext eglGetCurrentContext(void) { REAL(eglGetCurrentContext); return real_fn(); }
EXPORT EGLDisplay eglGetCurrentDisplay(void) { REAL(eglGetCurrentDisplay); return real_fn(); }
EXPORT EGLBoolean eglQueryContext(EGLDisplay d, EGLContext c, EGLint a, EGLint* v) { REAL(eglQueryContext); return real_fn(d, c, a, v); }
EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* a) { REAL(eglCreatePbufferSurface); return real_fn(d, c, a); }
EXPORT EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint* a) { REAL(eglCreatePixmapSurface); return real_fn(d, c, p, a); }
EXPORT EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint* a) { REAL(eglCreatePbufferFromClientBuffer); return real_fn(d, t, b, c, a); }
EXPORT EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint b) { REAL(eglBindTexImage); return real_fn(d, to_real(s), b); }
EXPORT EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint b) { REAL(eglReleaseTexImage); return real_fn(d, to_real(s), b); }
EXPORT EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType p) { REAL(eglCopyBuffers); return real_fn(d, to_real(s), p); }
EXPORT EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint a, EGLint v) { REAL(eglSurfaceAttrib); return real_fn(d, to_real(s), a, v); }
EXPORT EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint* v) { REAL(eglQuerySurface); return real_fn(d, to_real(s), a, v); }
EXPORT EGLBoolean eglSwapInterval(EGLDisplay, EGLint) { return EGL_TRUE; }

EXPORT const char* eglQueryString(EGLDisplay d, EGLint name) {
    REAL(eglQueryString);
    const char* s = real_fn ? real_fn(d, name) : nullptr;
    if (name == EGL_EXTENSIONS && d != EGL_NO_DISPLAY) {
        static std::string ext;
        static std::once_flag f;
        std::call_once(f, [&] { ext = std::string(s ? s : "") +
            " EGL_ANDROID_image_native_buffer EGL_ANDROID_native_fence_sync EGL_ANDROID_recordable"; });
        return ext.c_str();
    }
    return s;
}

// ---- configs: Mesa surfaceless only offers pbuffer configs; present them as window-capable too ----
EXPORT EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint* attrs, EGLConfig* cfgs, EGLint n, EGLint* num) {
    REAL(eglChooseConfig);
    std::vector<EGLint> f;
    for (const EGLint* p = attrs; p && *p != EGL_NONE; p += 2) {
        EGLint k = p[0], v = p[1];
        if (k == EGL_RECORDABLE_ANDROID || k == EGL_FRAMEBUFFER_TARGET_ANDROID || k == EGL_MATCH_NATIVE_PIXMAP) continue;
        if (k == EGL_NATIVE_VISUAL_ID && v != EGL_DONT_CARE) continue;
        if (k == EGL_SURFACE_TYPE) {
            EGLint nv = (v & EGL_PBUFFER_BIT) | ((v & (EGL_WINDOW_BIT | EGL_PIXMAP_BIT)) ? EGL_PBUFFER_BIT : 0);
            if (nv == 0) continue; v = nv;
        }
        f.push_back(k); f.push_back(v);
    }
    f.push_back(EGL_NONE);
    return real_fn(d, f.data(), cfgs, n, num);
}
EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint* v) {
    REAL(eglGetConfigAttrib);
    if (a == EGL_RECORDABLE_ANDROID || a == EGL_FRAMEBUFFER_TARGET_ANDROID) { if (v) *v = EGL_TRUE; return EGL_TRUE; }
    if (a == EGL_NATIVE_RENDERABLE) { if (v) *v = EGL_TRUE; return EGL_TRUE; }
    if (a == EGL_SURFACE_TYPE) { EGLBoolean r = real_fn(d, c, a, v); if (r && v) *v |= EGL_WINDOW_BIT; return r; }
    if (a == EGL_NATIVE_VISUAL_ID) {
        EGLint r8 = 0, g8 = 0, b8 = 0, a8 = 0;
        real_fn(d, c, EGL_RED_SIZE, &r8); real_fn(d, c, EGL_GREEN_SIZE, &g8); real_fn(d, c, EGL_BLUE_SIZE, &b8); real_fn(d, c, EGL_ALPHA_SIZE, &a8);
        int id = HAL_RGBA_8888;
        if (r8 == 5 && g8 == 6 && b8 == 5) id = HAL_RGB_565; else if (a8 == 0) id = HAL_RGBX_8888;
        if (v) *v = id; return EGL_TRUE;
    }
    return real_fn(d, c, a, v);
}

// ---- window surfaces ----
EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint*) {
    REAL(eglCreatePbufferSurface);
    ANativeWindow* w = (ANativeWindow*)win;
    if (!w) return EGL_NO_SURFACE;
    int ww = 0, hh = 0; w->query(w, NW_WIDTH, &ww); w->query(w, NW_HEIGHT, &hh);
    if (ww <= 0) ww = 64; if (hh <= 0) hh = 64;
    w->common.incRef(&w->common);
    w->perform(w, NW_SET_USAGE64, (uint64_t)(0x200 /*HW_RENDER*/ | 0x30 /*SW_WRITE_OFTEN*/));
    EGLint pb[] = { EGL_WIDTH, ww, EGL_HEIGHT, hh, EGL_NONE };
    EGLSurface real = real_fn(d, c, pb);
    if (real == EGL_NO_SURFACE) { w->common.decRef(&w->common); return EGL_NO_SURFACE; }
    ShimSurface* s = new ShimSurface{ real, w, ww, hh, c, {} };
    std::lock_guard<std::mutex> l(g_mu); g_surfs.insert(s);
    LOGI("window surface %p %dx%d", (void*)s, ww, hh);
    return (EGLSurface)s;
}
static void sync_images();
EXPORT EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface surf) {
    REAL(eglDestroySurface);
    ShimSurface* s = find_surf(surf);
    if (!s) return real_fn(d, surf);
    { std::lock_guard<std::mutex> l(g_mu); g_surfs.erase(s); }
    EGLBoolean r = real_fn(d, s->real);
    s->win->common.decRef(&s->win->common);
    delete s; return r;
}
EXPORT EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    REAL(eglMakeCurrent);
    if (ctx == EGL_NO_CONTEXT) sync_images();
    return real_fn(d, to_real(draw), to_real(read), ctx);
}
EXPORT EGLSurface eglGetCurrentSurface(EGLint which) {
    REAL(eglGetCurrentSurface);
    EGLSurface r = real_fn(which); ShimSurface* s = find_surf_by_real(r); return s ? (EGLSurface)s : r;
}
static bool swap_to_window(ShimSurface* s) {
    if (!gl_init()) return false;
    ANativeWindowBuffer* buf = nullptr; int fence = -1;
    int err = s->win->dequeueBuffer(s->win, &buf, &fence);
    if (err != 0 || !buf) { LOGE("dequeueBuffer failed %d", err); return false; }
    AHardwareBuffer* ahb = (AHardwareBuffer*)buf;
    if (buf->width != s->w || buf->height != s->h) {
        // window was resized: rebuild the backing pbuffer at the new size, drop this frame
        REAL(eglCreatePbufferSurface); static auto dsurf = (EGLBoolean(*)(EGLDisplay, EGLSurface))R("eglDestroySurface");
        static auto mk = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))R("eglMakeCurrent");
        static auto gcs = (EGLSurface(*)(EGLint))R("eglGetCurrentSurface"); static auto gcd = (EGLDisplay(*)(void))R("eglGetCurrentDisplay");
        static auto gcc = (EGLContext(*)(void))R("eglGetCurrentContext");
        EGLDisplay dpy = gcd(); EGLContext ctx = gcc();
        EGLint pb[] = { EGL_WIDTH, buf->width, EGL_HEIGHT, buf->height, EGL_NONE };
        EGLSurface nr = real_fn(dpy, s->cfg, pb);
        if (nr != EGL_NO_SURFACE) {
            bool cur = (gcs(EGL_DRAW) == s->real);
            if (cur) mk(dpy, nr, nr, ctx);
            dsurf(dpy, s->real); s->real = nr; s->w = buf->width; s->h = buf->height;
        }
        s->win->cancelBuffer(s->win, buf, fence);
        return true;
    }
    // read back the default framebuffer (bottom-left origin) and write it top-down into the window buffer
    GLint prevRead = 0; g_gl.GetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &prevRead);
    g_gl.BindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, 0);
    g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    s->tmp.resize((size_t)s->w * s->h * 4);
    g_gl.ReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, s->tmp.data());
    g_gl.BindFramebuffer(0x8CA8, (GLuint)prevRead);
    void* addr = nullptr;
    int lr = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, fence, nullptr, &addr);
    if (lr != 0 || !addr) { LOGE("AHardwareBuffer_lock failed %d (gralloc?)", lr); s->win->cancelBuffer(s->win, buf, -1); return false; }
    if (fmt_supported(buf->format)) rgba_to_buf(s->tmp.data(), s->w, s->h, (uint8_t*)addr, buf->stride, buf->format, true);
    else LOGE("unsupported window format %d", buf->format);
    int outFence = -1; AHardwareBuffer_unlock(ahb, &outFence);
    s->win->queueBuffer(s->win, buf, outFence);
    return true;
}
EXPORT EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface surf) {
    REAL(eglSwapBuffers);
    ShimSurface* s = find_surf(surf);
    if (!s) return real_fn(d, surf);
    sync_images();
    return swap_to_window(s) ? EGL_TRUE : EGL_FALSE;
}

// ---- EGLImage from ANativeWindowBuffer (CPU copy emulation) ----
static PFNEGLCREATEIMAGEKHRPROC real_createImage() { static auto f = (PFNEGLCREATEIMAGEKHRPROC)R("eglCreateImageKHR"); return f; }
static PFNEGLDESTROYIMAGEKHRPROC real_destroyImage() { static auto f = (PFNEGLDESTROYIMAGEKHRPROC)R("eglDestroyImageKHR"); return f; }

EXPORT EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint* attrs) {
    if (target != EGL_NATIVE_BUFFER_ANDROID) return real_createImage()(d, ctx, target, buffer, attrs);
    ANativeWindowBuffer* anb = (ANativeWindowBuffer*)buffer;
    if (!anb) return EGL_NO_IMAGE_KHR;
    if (!fmt_supported(anb->format)) { LOGE("image: unsupported format %d", anb->format); return EGL_NO_IMAGE_KHR; }
    anb->common.incRef(&anb->common);
    ShimImage* im = new ShimImage();
    im->anb = anb; im->ahb = (AHardwareBuffer*)anb; im->w = anb->width; im->h = anb->height; im->stride = anb->stride; im->fmt = anb->format; im->dpy = d;
    std::lock_guard<std::mutex> l(g_mu); g_images.insert(im);
    return (EGLImageKHR)im;
}
EXPORT EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR image) {
    ShimImage* im = find_img(image);
    if (!im) return real_destroyImage()(d, image);
    sync_images();
    { std::lock_guard<std::mutex> l(g_mu); g_images.erase(im); }
    if (im->realImage) real_destroyImage()(d, im->realImage);
    if (im->tex && g_gl.DeleteTextures) g_gl.DeleteTextures(1, &im->tex);
    im->anb->common.decRef(&im->anb->common);
    delete im; return EGL_TRUE;
}
static bool lock_rgba(ShimImage* im, std::vector<uint8_t>& out) {
    void* a = nullptr;
    if (AHardwareBuffer_lock(im->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &a) != 0 || !a) return false;
    buf_to_rgba((const uint8_t*)a, im->stride, im->fmt, im->w, im->h, out);
    int f = -1; AHardwareBuffer_unlock(im->ahb, &f); return true;
}
EXPORT void soft_glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image) {
    ShimImage* im = find_img(image);
    if (!gl_init()) return;
    if (!im) { g_gl.ImageTargetTexture2D(target, image); return; }
    static auto gcc = (EGLContext(*)(void))R("eglGetCurrentContext"); static auto gcd = (EGLDisplay(*)(void))R("eglGetCurrentDisplay");
    EGLContext ctx = gcc(); EGLDisplay dpy = gcd();
    GLint prev = 0; g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    if (!im->tex || im->ctx != ctx) {
        g_gl.GenTextures(1, &im->tex); g_gl.BindTexture(GL_TEXTURE_2D, im->tex);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->w, im->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        EGLint at[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE };
        im->realImage = real_createImage()(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)im->tex, at);
        if (!im->realImage) LOGE("real image from texture failed");
        im->ctx = ctx; im->uploaded = false;
    }
    std::vector<uint8_t> px;
    if (lock_rgba(im, px)) {
        uint64_t h = hash_bytes(px.data(), px.size());
        if (!im->uploaded || h != im->knownHash) {
            g_gl.BindTexture(GL_TEXTURE_2D, im->tex); g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
            g_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, im->w, im->h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            im->knownHash = h; im->uploaded = true;
        }
    } else LOGE("image lock for read failed (gralloc?)");
    g_gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev);
    if (im->realImage) g_gl.ImageTargetTexture2D(target, (GLeglImageOES)im->realImage);
    im->bound = true;
}
EXPORT void soft_glEGLImageTargetRenderbufferStorageOES(GLenum, GLeglImageOES) { LOGE("glEGLImageTargetRenderbufferStorageOES not supported"); }

// Write GPU-rendered image contents back into their gralloc buffers (only if they changed).
static void sync_images() {
    if (!g_gl.ok) return;
    static auto gcc = (EGLContext(*)(void))R("eglGetCurrentContext");
    EGLContext cur = gcc(); if (!cur) return;
    std::vector<ShimImage*> list;
    { std::lock_guard<std::mutex> l(g_mu); for (auto* im : g_images) if (im->bound && im->tex && im->ctx == cur) list.push_back(im); }
    if (list.empty()) return;
    GLint pd = 0, pr = 0; g_gl.GetIntegerv(0x8CA6, &pd); g_gl.GetIntegerv(0x8CAA, &pr);
    GLuint fbo = 0; g_gl.GenFramebuffers(1, &fbo); g_gl.BindFramebuffer(0x8D40, fbo);
    g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    std::vector<uint8_t> px;
    for (ShimImage* im : list) {
        im->bound = false;
        g_gl.FramebufferTexture2D(0x8D40, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, im->tex, 0);
        px.resize((size_t)im->w * im->h * 4);
        g_gl.ReadPixels(0, 0, im->w, im->h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        uint64_t h = hash_bytes(px.data(), px.size());
        if (h == im->knownHash) continue;
        void* a = nullptr;
        if (AHardwareBuffer_lock(im->ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &a) == 0 && a) {
            rgba_to_buf(px.data(), im->w, im->h, (uint8_t*)a, im->stride, im->fmt, false);
            int f = -1; AHardwareBuffer_unlock(im->ahb, &f); im->knownHash = h;
        } else LOGE("image lock for write failed (gralloc?)");
    }
    g_gl.FramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, 0, 0);
    g_gl.BindFramebuffer(0x8CA9, (GLuint)pd); g_gl.BindFramebuffer(0x8CA8, (GLuint)pr);
    g_gl.DeleteFramebuffers(1, &fbo);
}

// ---- fences: GL work is synchronous on softpipe, so native fences are "already signalled" ----
EXPORT EGLSyncKHR eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint* attrs) {
    static auto real = (PFNEGLCREATESYNCKHRPROC)R("eglCreateSyncKHR");
    sync_images();
    if (type == EGL_SYNC_NATIVE_FENCE_ANDROID) { if (gl_init() && g_gl.Finish) g_gl.Finish(); type = EGL_SYNC_FENCE_KHR; attrs = nullptr; }
    return real(d, type, attrs);
}
EXPORT EGLint eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR) { return -1; /* EGL_NO_NATIVE_FENCE_FD_ANDROID */ }

// ---- eglGetProcAddress: route intercepted entry points to us, everything else to Mesa ----
EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    struct E { const char* n; void* f; };
    static const E t[] = {
        { "eglCreateImageKHR", (void*)eglCreateImageKHR }, { "eglDestroyImageKHR", (void*)eglDestroyImageKHR },
        { "eglCreateSyncKHR", (void*)eglCreateSyncKHR }, { "eglDupNativeFenceFDANDROID", (void*)eglDupNativeFenceFDANDROID },
        { "glEGLImageTargetTexture2DOES", (void*)soft_glEGLImageTargetTexture2DOES },
        { "glEGLImageTargetRenderbufferStorageOES", (void*)soft_glEGLImageTargetRenderbufferStorageOES },
        { "eglGetDisplay", (void*)eglGetDisplay }, { "eglChooseConfig", (void*)eglChooseConfig },
        { "eglGetConfigAttrib", (void*)eglGetConfigAttrib }, { "eglCreateWindowSurface", (void*)eglCreateWindowSurface },
        { "eglDestroySurface", (void*)eglDestroySurface }, { "eglSwapBuffers", (void*)eglSwapBuffers },
        { "eglMakeCurrent", (void*)eglMakeCurrent }, { "eglQueryString", (void*)eglQueryString },
        { "eglGetCurrentSurface", (void*)eglGetCurrentSurface }, { "eglSwapInterval", (void*)eglSwapInterval },
    };
    for (auto& e : t) if (name && !strcmp(name, e.n)) return (__eglMustCastToProperFunctionPointerType)e.f;
    typedef __eglMustCastToProperFunctionPointerType (*PF)(const char*);
    static PF real = (PF)R("eglGetProcAddress");
    return real ? real(name) : nullptr;
}
