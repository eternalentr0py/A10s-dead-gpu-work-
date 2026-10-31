/* egltest: load a specific EGL/GLES implementation by path and report what SurfaceFlinger would need.
 * Build: NDK clang, armv7a, -ldl only (everything else is dlopen'd).
 * Usage: egltest [libEGL path] [libGLESv2 path]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

typedef EGLDisplay (*pf_GetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*pf_Initialize)(EGLDisplay, EGLint *, EGLint *);
typedef const char *(*pf_QueryString)(EGLDisplay, EGLint);
typedef EGLint (*pf_GetError)(void);
typedef EGLBoolean (*pf_BindAPI)(EGLenum);
typedef EGLBoolean (*pf_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*pf_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface (*pf_CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLBoolean (*pf_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef void *(*pf_GetProcAddress)(const char *);
typedef EGLBoolean (*pf_Terminate)(EGLDisplay);

static void *eglh, *glh;
static void *sym(const char *n) {
    void *p = dlsym(eglh, n);
    if (!p && glh) p = dlsym(glh, n);
    return p;
}

static int has_ext(const char *list, const char *name) {
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    return 0;
}

#define CHECK(label, cond) printf("  [%s] %s\n", (cond) ? "ok " : "MISSING", label)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *eglpath = argc > 1 ? argv[1] : "libEGL_soft.so";
    const char *glpath = argc > 2 ? argv[2] : "libGLESv2_soft.so";
    int score = 0;

    printf("== egltest ==\nEGL lib: %s\nGLES lib: %s\n", eglpath, glpath);
    glh = dlopen(glpath, RTLD_NOW | RTLD_GLOBAL);
    if (!glh) printf("WARN dlopen(%s): %s\n", glpath, dlerror());
    eglh = dlopen(eglpath, RTLD_NOW | RTLD_GLOBAL);
    if (!eglh) { printf("FAIL dlopen(%s): %s\n", eglpath, dlerror()); printf("RESULT: load-failed\n"); return 2; }
    printf("PASS libraries loaded\n");

    pf_GetDisplay getDisplay = sym("eglGetDisplay");
    pf_Initialize initialize = sym("eglInitialize");
    pf_QueryString queryString = sym("eglQueryString");
    pf_GetError getError = sym("eglGetError");
    pf_BindAPI bindAPI = sym("eglBindAPI");
    pf_ChooseConfig chooseConfig = sym("eglChooseConfig");
    pf_CreateContext createContext = sym("eglCreateContext");
    pf_CreatePbuffer createPbuffer = sym("eglCreatePbufferSurface");
    pf_MakeCurrent makeCurrent = sym("eglMakeCurrent");
    pf_GetProcAddress getProc = sym("eglGetProcAddress");
    pf_Terminate terminate = sym("eglTerminate");
    if (!getDisplay || !initialize || !queryString || !getError || !chooseConfig || !createContext || !makeCurrent || !getProc) {
        printf("FAIL missing core EGL symbols\nRESULT: symbols-missing\n"); return 3;
    }

    const char *clientExt = queryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("Client extensions: %s\n", clientExt ? clientExt : "(none)");

    EGLDisplay dpy = getDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay -> %p (err 0x%x)\n", dpy, getError());
    if (dpy == EGL_NO_DISPLAY) { printf("RESULT: no-display\n"); return 4; }

    EGLint maj = 0, min = 0;
    if (!initialize(dpy, &maj, &min)) {
        printf("FAIL eglInitialize err=0x%x\nRESULT: initialize-failed\n", getError());
        return 5;
    }
    printf("PASS eglInitialize %d.%d\n", maj, min); score++;
    printf("Vendor: %s\nVersion: %s\nAPIs: %s\n", queryString(dpy, EGL_VENDOR), queryString(dpy, EGL_VERSION), queryString(dpy, EGL_CLIENT_APIS));
    const char *ext = queryString(dpy, EGL_EXTENSIONS);
    printf("Display extensions: %s\n", ext ? ext : "(none)");

    printf("SurfaceFlinger (Android 11 GLES RenderEngine) needs:\n");
    CHECK("EGL_KHR_image_base", has_ext(ext, "EGL_KHR_image_base"));
    CHECK("EGL_ANDROID_image_native_buffer  (HARD REQUIREMENT: import gralloc buffers)", has_ext(ext, "EGL_ANDROID_image_native_buffer"));
    CHECK("EGL_ANDROID_native_fence_sync", has_ext(ext, "EGL_ANDROID_native_fence_sync"));
    CHECK("EGL_KHR_fence_sync", has_ext(ext, "EGL_KHR_fence_sync"));
    CHECK("EGL_KHR_wait_sync", has_ext(ext, "EGL_KHR_wait_sync"));
    CHECK("EGL_KHR_surfaceless_context", has_ext(ext, "EGL_KHR_surfaceless_context"));
    CHECK("EGL_KHR_no_config_context", has_ext(ext, "EGL_KHR_no_config_context"));
    CHECK("EGL_IMG_context_priority (optional)", has_ext(ext, "EGL_IMG_context_priority"));
    CHECK("EGL_ANDROID_recordable (optional)", has_ext(ext, "EGL_ANDROID_recordable"));
    CHECK("eglCreateImageKHR proc", getProc("eglCreateImageKHR") != NULL);
    CHECK("eglCreateSyncKHR proc", getProc("eglCreateSyncKHR") != NULL);
    CHECK("eglDupNativeFenceFDANDROID proc", getProc("eglDupNativeFenceFDANDROID") != NULL);
    CHECK("glEGLImageTargetTexture2DOES proc", getProc("glEGLImageTargetTexture2DOES") != NULL);

    if (bindAPI) bindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg = NULL; EGLint n = 0;
    const EGLint pbAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLBoolean ok = chooseConfig(dpy, pbAttr, &cfg, 1, &n);
    printf("eglChooseConfig(pbuffer, RGBA8888, ES2) ok=%d n=%d err=0x%x\n", ok, n, getError());
    const EGLint winAttr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig wcfg; EGLint wn = 0;
    ok = chooseConfig(dpy, winAttr, &wcfg, 1, &wn);
    printf("eglChooseConfig(window, ES2) ok=%d n=%d (window configs are needed for the real display path)\n", ok, wn);
    if (n < 1) { printf("RESULT: init-ok-no-pbuffer-config\n"); return 6; }

    EGLContext ctx = EGL_NO_CONTEXT;
    int ver = 0;
    const EGLint c3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint c2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, c3);
    if (ctx != EGL_NO_CONTEXT) ver = 3;
    else { printf("ES3 context failed err=0x%x, trying ES2\n", getError()); ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, c2); if (ctx != EGL_NO_CONTEXT) ver = 2; }
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext err=0x%x\nRESULT: no-context\n", getError()); return 7; }
    printf("PASS context created (ES %d)\n", ver); score++;

    const EGLint pbs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = createPbuffer(dpy, cfg, pbs);
    if (surf == EGL_NO_SURFACE) { printf("FAIL pbuffer err=0x%x\nRESULT: no-pbuffer\n", getError()); return 8; }
    if (!makeCurrent(dpy, surf, surf, ctx)) { printf("FAIL eglMakeCurrent err=0x%x\nRESULT: makecurrent-failed\n", getError()); return 9; }
    printf("PASS eglMakeCurrent\n"); score++;

    typedef const GLubyte *(*pf_glGetString)(GLenum);
    typedef void (*pf_v)(void);
    pf_glGetString glGetStr = (pf_glGetString)sym("glGetString");
    if (!glGetStr) glGetStr = (pf_glGetString)getProc("glGetString");
    if (glGetStr) {
        printf("GL_VENDOR:   %s\nGL_RENDERER: %s\nGL_VERSION:  %s\nGLSL:        %s\n",
               glGetStr(GL_VENDOR), glGetStr(GL_RENDERER), glGetStr(GL_VERSION), glGetStr(GL_SHADING_LANGUAGE_VERSION));
    }

    void (*glClearColor_)(GLfloat, GLfloat, GLfloat, GLfloat) = getProc("glClearColor");
    void (*glClear_)(GLbitfield) = getProc("glClear");
    void (*glReadPixels_)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = getProc("glReadPixels");
    GLuint (*glCreateShader_)(GLenum) = getProc("glCreateShader");
    void (*glShaderSource_)(GLuint, GLsizei, const GLchar *const *, const GLint *) = getProc("glShaderSource");
    void (*glCompileShader_)(GLuint) = getProc("glCompileShader");
    void (*glGetShaderiv_)(GLuint, GLenum, GLint *) = getProc("glGetShaderiv");
    if (glClearColor_ && glClear_ && glReadPixels_) {
        unsigned char px[4] = {0, 0, 0, 0};
        glClearColor_(1.f, 0.f, 0.f, 1.f); glClear_(GL_COLOR_BUFFER_BIT);
        glReadPixels_(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int good = (px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
        printf("%s clear+readback pixel = %u,%u,%u,%u\n", good ? "PASS" : "FAIL", px[0], px[1], px[2], px[3]);
        if (good) score++;
    }
    if (glCreateShader_ && glShaderSource_ && glCompileShader_ && glGetShaderiv_) {
        const char *fs = "precision mediump float; uniform vec4 c; varying vec2 uv; void main(){ gl_FragColor = c * vec4(uv, 0.5, 1.0); }";
        GLuint sh = glCreateShader_(GL_FRAGMENT_SHADER);
        glShaderSource_(sh, 1, &fs, NULL); glCompileShader_(sh);
        GLint st = 0; glGetShaderiv_(sh, GL_COMPILE_STATUS, &st);
        printf("%s GLSL fragment shader compile\n", st ? "PASS" : "FAIL");
        if (st) score++;
    }
    makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (terminate) terminate(dpy);
    printf("RESULT: score=%d/5 (init, context, makecurrent, render, glsl)\n", score);
    return score == 5 ? 0 : 1;
}
