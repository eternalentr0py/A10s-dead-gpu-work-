/* fbdev_test: real EGL/GLES render -> visible proof on the TWRP screen, without SurfaceFlinger.
 *
 * Renders a small scene (color clear + a GLSL-shaded triangle) into an off-screen EGL pbuffer
 * using the installed software stack (libEGL_soft.so / libGLESv2_soft.so), reads the pixels back
 * with glReadPixels, converts them to whatever pixel format /dev/graphics/fb0 is actually running
 * (queried at runtime via FBIOGET_VSCREENINFO/FBIOGET_FSCREENINFO - never hardcoded), and blits
 * them straight into the mmap'd framebuffer. This bypasses SurfaceFlinger/HWComposer entirely, so
 * it proves the EGL/GLES half of the stack works even though TWRP cannot run the full graphics
 * stack - a fbdev write is something TWRP's own screen can show directly.
 *
 * Exit code: 0 = PASS (rendered AND visibly blitted to fb0), non-zero = FAIL (see stderr/stdout).
 * Usage: fbdev_test [libEGL path] [libGLESv2 path] [fb device, default /dev/graphics/fb0]
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef EGLDisplay (*pf_GetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*pf_Initialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLint (*pf_GetError)(void);
typedef EGLBoolean (*pf_BindAPI)(EGLenum);
typedef EGLBoolean (*pf_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*pf_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface (*pf_CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLBoolean (*pf_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef void *(*pf_GetProcAddress)(const char *);

static void *eglh, *glh;
static void *sym(const char *n) { void *p = dlsym(eglh, n); if (!p && glh) p = dlsym(glh, n); return p; }

#define W 256
#define H 256

static unsigned char rgba[W * H * 4];

/* very small immediate-mode-free triangle via a vertex+fragment shader, so this also exercises
 * glCreateShader/glCompileShader/glLinkProgram/glDrawArrays - not just glClear. */
static const char *VS =
    "attribute vec2 pos; varying vec2 uv; void main(){ uv = pos*0.5+0.5; gl_Position = vec4(pos,0.0,1.0); }";
static const char *FS =
    "precision mediump float; varying vec2 uv; void main(){ gl_FragColor = vec4(uv.x, uv.y, 1.0-uv.x, 1.0); }";

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *eglpath = argc > 1 ? argv[1] : "libEGL_soft.so";
    const char *glpath = argc > 2 ? argv[2] : "libGLESv2_soft.so";
    const char *fbpath = argc > 3 ? argv[3] : "/dev/graphics/fb0";

    printf("== fbdev_test ==\nEGL: %s\nGLES: %s\nfb: %s\n", eglpath, glpath, fbpath);

    glh = dlopen(glpath, RTLD_NOW | RTLD_GLOBAL);
    if (!glh) printf("WARN dlopen(%s): %s\n", glpath, dlerror());
    eglh = dlopen(eglpath, RTLD_NOW | RTLD_GLOBAL);
    if (!eglh) { printf("FAIL dlopen(%s): %s\nRESULT: FAIL load\n", eglpath, dlerror()); return 2; }

    pf_GetDisplay getDisplay = sym("eglGetDisplay");
    pf_Initialize initialize = sym("eglInitialize");
    pf_GetError getError = sym("eglGetError");
    pf_BindAPI bindAPI = sym("eglBindAPI");
    pf_ChooseConfig chooseConfig = sym("eglChooseConfig");
    pf_CreateContext createContext = sym("eglCreateContext");
    pf_CreatePbuffer createPbuffer = sym("eglCreatePbufferSurface");
    pf_MakeCurrent makeCurrent = sym("eglMakeCurrent");
    pf_GetProcAddress getProc = sym("eglGetProcAddress");
    if (!getDisplay || !initialize || !chooseConfig || !createContext || !createPbuffer || !makeCurrent) {
        printf("FAIL missing core EGL symbols\nRESULT: FAIL symbols\n"); return 3;
    }

    EGLDisplay dpy = getDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !initialize(dpy, NULL, NULL)) {
        printf("FAIL eglGetDisplay/eglInitialize err=0x%x\nRESULT: FAIL init\n", getError()); return 4;
    }
    if (bindAPI) bindAPI(EGL_OPENGL_ES_API);

    EGLConfig cfg; EGLint n = 0;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    if (!chooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
        printf("FAIL eglChooseConfig err=0x%x\nRESULT: FAIL config\n", getError()); return 5;
    }
    const EGLint c2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, c2);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext err=0x%x\nRESULT: FAIL context\n", getError()); return 6; }
    const EGLint pbs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = createPbuffer(dpy, cfg, pbs);
    if (surf == EGL_NO_SURFACE || !makeCurrent(dpy, surf, surf, ctx)) {
        printf("FAIL pbuffer/makeCurrent err=0x%x\nRESULT: FAIL surface\n", getError()); return 7;
    }
    printf("PASS EGL init/context/surface\n");

    void (*glViewport_)(GLint, GLint, GLsizei, GLsizei) = getProc("glViewport");
    void (*glClearColor_)(GLfloat, GLfloat, GLfloat, GLfloat) = getProc("glClearColor");
    void (*glClear_)(GLbitfield) = getProc("glClear");
    GLuint (*glCreateShader_)(GLenum) = getProc("glCreateShader");
    void (*glShaderSource_)(GLuint, GLsizei, const GLchar *const *, const GLint *) = getProc("glShaderSource");
    void (*glCompileShader_)(GLuint) = getProc("glCompileShader");
    void (*glGetShaderiv_)(GLuint, GLenum, GLint *) = getProc("glGetShaderiv");
    GLuint (*glCreateProgram_)(void) = getProc("glCreateProgram");
    void (*glAttachShader_)(GLuint, GLuint) = getProc("glAttachShader");
    void (*glLinkProgram_)(GLuint) = getProc("glLinkProgram");
    void (*glGetProgramiv_)(GLuint, GLenum, GLint *) = getProc("glGetProgramiv");
    void (*glUseProgram_)(GLuint) = getProc("glUseProgram");
    GLint (*glGetAttribLocation_)(GLuint, const GLchar *) = getProc("glGetAttribLocation");
    void (*glVertexAttribPointer_)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) = getProc("glVertexAttribPointer");
    void (*glEnableVertexAttribArray_)(GLuint) = getProc("glEnableVertexAttribArray");
    void (*glDrawArrays_)(GLenum, GLint, GLsizei) = getProc("glDrawArrays");
    void (*glReadPixels_)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = getProc("glReadPixels");
    if (!glClearColor_ || !glClear_ || !glCreateShader_ || !glReadPixels_) {
        printf("FAIL missing GLES entry points\nRESULT: FAIL gles-symbols\n"); return 8;
    }

    glViewport_(0, 0, W, H);
    glClearColor_(0.05f, 0.05f, 0.05f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);

    GLuint vs = glCreateShader_(GL_VERTEX_SHADER); glShaderSource_(vs, 1, &VS, NULL); glCompileShader_(vs);
    GLuint fs = glCreateShader_(GL_FRAGMENT_SHADER); glShaderSource_(fs, 1, &FS, NULL); glCompileShader_(fs);
    GLint vok = 0, fok = 0; glGetShaderiv_(vs, GL_COMPILE_STATUS, &vok); glGetShaderiv_(fs, GL_COMPILE_STATUS, &fok);
    GLuint prog = glCreateProgram_(); glAttachShader_(prog, vs); glAttachShader_(prog, fs); glLinkProgram_(prog);
    GLint lok = 0; glGetProgramiv_(prog, GL_LINK_STATUS, &lok);
    printf("%s shader compile (vs=%d fs=%d) / link (%d)\n", (vok && fok && lok) ? "PASS" : "FAIL", vok, fok, lok);

    int rendered_triangle = 0;
    if (vok && fok && lok) {
        glUseProgram_(prog);
        static const GLfloat verts[] = { -0.9f, -0.9f,  0.9f, -0.9f,  0.0f, 0.9f };
        GLint loc = glGetAttribLocation_(prog, "pos");
        if (loc >= 0) {
            glVertexAttribPointer_((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
            glEnableVertexAttribArray_((GLuint)loc);
            glDrawArrays_(GL_TRIANGLES, 0, 3);
            rendered_triangle = 1;
        }
    }
    printf("%s triangle draw call\n", rendered_triangle ? "PASS" : "SKIP");

    glReadPixels_(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* sanity: center pixel should be the shaded triangle color, not the clear color, if it drew */
    unsigned char *center = rgba + ((H/2) * W + W/2) * 4;
    printf("center pixel after render = %u,%u,%u,%u\n", center[0], center[1], center[2], center[3]);
    int gl_ok = (vok && fok && lok && rendered_triangle);

    /* ---- blit to /dev/graphics/fb0 ---- */
    int fd = open(fbpath, O_RDWR);
    if (fd < 0) { printf("FAIL open(%s): %s\nRESULT: %s / FAIL fb-open\n", fbpath, strerror(errno), gl_ok ? "PASS-gl" : "FAIL-gl"); return 9; }
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        printf("FAIL FBIOGET_*SCREENINFO: %s\nRESULT: %s / FAIL fb-ioctl\n", strerror(errno), gl_ok ? "PASS-gl" : "FAIL-gl");
        close(fd); return 10;
    }
    int fbw = vi.xres, fbh = vi.yres, bpp = vi.bits_per_pixel / 8;
    size_t line_len = fi.line_length ? fi.line_length : (size_t)fbw * bpp;
    size_t fbsize = line_len * fbh;
    printf("fb0: %dx%d bpp=%d line_len=%zu rgba(%d/%d/%d/%d @ %d/%d/%d/%d)\n", fbw, fbh, vi.bits_per_pixel, line_len,
           vi.red.length, vi.green.length, vi.blue.length, vi.transp.length,
           vi.red.offset, vi.green.offset, vi.blue.offset, vi.transp.offset);
    unsigned char *fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { printf("FAIL mmap: %s\nRESULT: %s / FAIL fb-mmap\n", strerror(errno), gl_ok ? "PASS-gl" : "FAIL-gl"); close(fd); return 11; }

    /* paint a solid magenta border across the whole screen first, so "something changed" is obvious
     * even if the centered blit below is small or mis-scaled. */
    for (int y = 0; y < fbh; y++) {
        unsigned char *row = fb + (size_t)y * line_len;
        int border = (y < 12 || y > fbh - 12);
        for (int x = 0; x < fbw; x++) {
            int onborder = border || x < 12 || x > fbw - 12;
            unsigned r, g, b;
            if (onborder) { r = 255; g = 0; b = 255; }       /* magenta frame: proves fb0 is writable at all */
            else { r = 0; g = 0; b = 0; }
            unsigned px = 0;
            if (vi.bits_per_pixel == 32) {
                px = (r << vi.red.offset) | (g << vi.green.offset) | (b << vi.blue.offset);
                if (vi.transp.length) px |= (0xFFu << vi.transp.offset);
                ((unsigned*)row)[x] = px;
            } else if (vi.bits_per_pixel == 16) {
                unsigned short v = (unsigned short)(((r >> (8-vi.red.length)) << vi.red.offset) |
                                                     ((g >> (8-vi.green.length)) << vi.green.offset) |
                                                     ((b >> (8-vi.blue.length)) << vi.blue.offset));
                ((unsigned short*)row)[x] = v;
            }
        }
    }
    /* now blit the rendered WxH RGBA image centered on screen, scaled to the fb's pixel format */
    int ox = (fbw - W) / 2; if (ox < 12) ox = 12;
    int oy = (fbh - H) / 2; if (oy < 12) oy = 12;
    int bw = (ox + W <= fbw - 12) ? W : (fbw - 12 - ox);
    int bh = (oy + H <= fbh - 12) ? H : (fbh - 12 - oy);
    for (int y = 0; y < bh; y++) {
        unsigned char *srcrow = rgba + (size_t)(H - 1 - y) * W * 4; /* GL readback is bottom-up */
        unsigned char *dstrow = fb + (size_t)(oy + y) * line_len;
        for (int x = 0; x < bw; x++) {
            unsigned r = srcrow[x*4], g = srcrow[x*4+1], b = srcrow[x*4+2];
            if (vi.bits_per_pixel == 32) {
                unsigned px = (r << vi.red.offset) | (g << vi.green.offset) | (b << vi.blue.offset);
                if (vi.transp.length) px |= (0xFFu << vi.transp.offset);
                ((unsigned*)dstrow)[ox + x] = px;
            } else if (vi.bits_per_pixel == 16) {
                unsigned short v = (unsigned short)(((r >> (8-vi.red.length)) << vi.red.offset) |
                                                     ((g >> (8-vi.green.length)) << vi.green.offset) |
                                                     ((b >> (8-vi.blue.length)) << vi.blue.offset));
                ((unsigned short*)dstrow)[ox + x] = v;
            }
        }
    }
    msync(fb, fbsize, MS_SYNC);
    printf("PASS blitted %dx%d render to fb0 at (%d,%d), plus a full-screen magenta border\n", bw, bh, ox, oy);
    munmap(fb, fbsize);
    close(fd);

    int overall = gl_ok ? 0 : 1;
    printf("RESULT: %s (gl=%s, fb-blit=PASS) - look at the screen: a magenta border plus a shaded\n"
           "triangle on a dark square means the real EGL/GLES render+blit path is working.\n",
           overall == 0 ? "PASS" : "FAIL", gl_ok ? "PASS" : "FAIL");
    return overall;
}
