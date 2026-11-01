/* egltest: EGL/GLES capability probe for A10s dead-GPU project.
 * Compiled as static-pie (no PT_INTERP after patchelf strip) so it runs
 * directly from TWRP /tmp without needing /system/bin/linker.
 * Linked against NDK stub .a so the .so files are loaded at runtime via
 * the Bionic dynamic linker that IS present once /system is mounted —
 * but this binary itself needs no interpreter to start.
 *
 * Build (CI does this):
 *   clang -static-pie -o egltest egltest.c -lEGL -lGLESv2 -llog \
 *         --sysroot=$NDK/sysroot -I$NDK/sysroot/usr/include
 *   patchelf --set-interpreter "" egltest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

/* All EGL/GL calls go through function pointers loaded from the .so paths
 * the user passes, so we can test our specific Mesa libraries. */

typedef EGLDisplay (*pf_GetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*pf_Initialize)(EGLDisplay, EGLint *, EGLint *);
typedef const char *(*pf_QueryString)(EGLDisplay, EGLint);
typedef EGLint     (*pf_GetError)(void);
typedef EGLBoolean (*pf_BindAPI)(EGLenum);
typedef EGLBoolean (*pf_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*pf_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface (*pf_CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLBoolean (*pf_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef void *     (*pf_GetProcAddress)(const char *);
typedef EGLBoolean (*pf_Terminate)(EGLDisplay);

static void *eglh, *glh;
#define SYM(h,n) dlsym((h),(n))

static int has_ext(const char *list, const char *name) {
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    return 0;
}
#define CHECK(label, cond) printf("  [%s] %s\n", (cond) ? "ok " : "FAIL", label)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *eglpath = argc > 1 ? argv[1] : "/tmp/vendor_root/vendor/lib/egl/libEGL_soft.so";
    const char *glpath  = argc > 2 ? argv[2] : "/tmp/vendor_root/vendor/lib/egl/libGLESv2_soft.so";

    printf("== egltest ==\nEGL: %s\nGLES: %s\n", eglpath, glpath);

    glh  = dlopen(glpath,  RTLD_NOW | RTLD_GLOBAL);
    eglh = dlopen(eglpath, RTLD_NOW | RTLD_GLOBAL);
    if (!eglh) { printf("FAIL dlopen EGL: %s\nRESULT: load-failed\n", dlerror()); return 2; }
    if (!glh)  printf("WARN dlopen GLES: %s\n", dlerror());
    printf("PASS libraries loaded\n");

    pf_GetDisplay    eglGetDisplay_    = SYM(eglh,"eglGetDisplay");
    pf_Initialize    eglInitialize_    = SYM(eglh,"eglInitialize");
    pf_QueryString   eglQueryString_   = SYM(eglh,"eglQueryString");
    pf_GetError      eglGetError_      = SYM(eglh,"eglGetError");
    pf_BindAPI       eglBindAPI_       = SYM(eglh,"eglBindAPI");
    pf_ChooseConfig  eglChooseConfig_  = SYM(eglh,"eglChooseConfig");
    pf_CreateContext eglCreateContext_ = SYM(eglh,"eglCreateContext");
    pf_CreatePbuffer eglCreatePbuffer_ = SYM(eglh,"eglCreatePbufferSurface");
    pf_MakeCurrent   eglMakeCurrent_   = SYM(eglh,"eglMakeCurrent");
    pf_GetProcAddress eglGetProc_      = SYM(eglh,"eglGetProcAddress");
    pf_Terminate     eglTerminate_     = SYM(eglh,"eglTerminate");

    if (!eglGetDisplay_||!eglInitialize_||!eglQueryString_||!eglGetError_||
        !eglChooseConfig_||!eglCreateContext_||!eglMakeCurrent_||!eglGetProc_) {
        printf("FAIL missing core EGL symbols\nRESULT: symbols-missing\n"); return 3;
    }

    const char *cli = eglQueryString_(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("Client extensions: %s\n", cli ? cli : "(none)");

    EGLDisplay dpy = eglGetDisplay_(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay -> %p err=0x%x\n", (void*)dpy, eglGetError_());
    if (dpy == EGL_NO_DISPLAY) { printf("RESULT: no-display\n"); return 4; }

    EGLint maj=0,min=0;
    if (!eglInitialize_(dpy,&maj,&min)) {
        printf("FAIL eglInitialize err=0x%x\nRESULT: initialize-failed\n", eglGetError_()); return 5;
    }
    printf("PASS eglInitialize %d.%d\nVendor: %s\nVersion: %s\n",
           maj,min, eglQueryString_(dpy,EGL_VENDOR), eglQueryString_(dpy,EGL_VERSION));
    const char *ext = eglQueryString_(dpy, EGL_EXTENSIONS);
    printf("Display extensions:\n  %s\n", ext ? ext : "(none)");

    printf("SurfaceFlinger requirements:\n");
    CHECK("EGL_KHR_image_base",                  has_ext(ext,"EGL_KHR_image_base"));
    CHECK("EGL_ANDROID_image_native_buffer (key)",has_ext(ext,"EGL_ANDROID_image_native_buffer"));
    CHECK("EGL_ANDROID_native_fence_sync",        has_ext(ext,"EGL_ANDROID_native_fence_sync"));
    CHECK("EGL_KHR_fence_sync",                   has_ext(ext,"EGL_KHR_fence_sync"));
    CHECK("EGL_KHR_wait_sync",                    has_ext(ext,"EGL_KHR_wait_sync"));
    CHECK("EGL_KHR_surfaceless_context",          has_ext(ext,"EGL_KHR_surfaceless_context"));
    CHECK("EGL_KHR_no_config_context",            has_ext(ext,"EGL_KHR_no_config_context"));
    CHECK("eglCreateImageKHR proc",               eglGetProc_("eglCreateImageKHR")!=NULL);
    CHECK("eglDupNativeFenceFDANDROID proc",      eglGetProc_("eglDupNativeFenceFDANDROID")!=NULL);
    CHECK("glEGLImageTargetTexture2DOES proc",    eglGetProc_("glEGLImageTargetTexture2DOES")!=NULL);

    if (eglBindAPI_) eglBindAPI_(EGL_OPENGL_ES_API);
    const EGLint pbattr[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cfg=NULL; EGLint n=0;
    eglChooseConfig_(dpy,pbattr,&cfg,1,&n);
    printf("eglChooseConfig(pbuffer ES2 RGBA8) n=%d err=0x%x\n",n,eglGetError_());
    if (n<1) { printf("RESULT: no-pbuffer-config\n"); return 6; }

    const EGLint c3[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    const EGLint c2[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx = eglCreateContext_(dpy,cfg,EGL_NO_CONTEXT,c3);
    int ver=3;
    if (ctx==EGL_NO_CONTEXT){ver=2;eglGetError_();ctx=eglCreateContext_(dpy,cfg,EGL_NO_CONTEXT,c2);}
    if (ctx==EGL_NO_CONTEXT){printf("FAIL eglCreateContext err=0x%x\nRESULT: no-context\n",eglGetError_());return 7;}
    printf("PASS context ES%d\n",ver);

    const EGLint pbs[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
    EGLSurface surf = eglCreatePbuffer_(dpy,cfg,pbs);
    if (surf==EGL_NO_SURFACE){printf("FAIL pbuffer err=0x%x\nRESULT: no-pbuffer\n",eglGetError_());return 8;}
    if (!eglMakeCurrent_(dpy,surf,surf,ctx)){printf("FAIL MakeCurrent err=0x%x\nRESULT: makecurrent-failed\n",eglGetError_());return 9;}
    printf("PASS eglMakeCurrent\n");

    typedef const GLubyte*(*pf_GS)(GLenum);
    pf_GS glGetStr=(pf_GS)eglGetProc_("glGetString");
    if (!glGetStr && glh) glGetStr=(pf_GS)SYM(glh,"glGetString");
    if (glGetStr) printf("GL_RENDERER: %s\nGL_VERSION:  %s\n",glGetStr(GL_RENDERER),glGetStr(GL_VERSION));

    void(*glCC)(GLfloat,GLfloat,GLfloat,GLfloat)=eglGetProc_("glClearColor");
    void(*glCl)(GLbitfield)=eglGetProc_("glClear");
    void(*glRP)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)=eglGetProc_("glReadPixels");
    int score=0;
    if(glCC&&glCl&&glRP){
        unsigned char px[4]={0};
        glCC(1,0,0,1);glCl(GL_COLOR_BUFFER_BIT);
        glRP(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
        int ok=(px[0]==255&&px[1]==0&&px[2]==0&&px[3]==255);
        printf("%s render+readback px=%u,%u,%u,%u\n",ok?"PASS":"FAIL",px[0],px[1],px[2],px[3]);
        if(ok)score++;
    }
    GLuint(*glCS)(GLenum)=eglGetProc_("glCreateShader");
    void(*glSS)(GLuint,GLsizei,const GLchar*const*,const GLint*)=eglGetProc_("glShaderSource");
    void(*glCSh)(GLuint)=eglGetProc_("glCompileShader");
    void(*glGSiv)(GLuint,GLenum,GLint*)=eglGetProc_("glGetShaderiv");
    if(glCS&&glSS&&glCSh&&glGSiv){
        const char*fs="precision mediump float;void main(){gl_FragColor=vec4(1,0,0,1);}";
        GLuint sh=glCS(GL_FRAGMENT_SHADER);glSS(sh,1,&fs,NULL);glCSh(sh);
        GLint st=0;glGSiv(sh,GL_COMPILE_STATUS,&st);
        printf("%s GLSL compile\n",st?"PASS":"FAIL");if(st)score++;
    }
    eglMakeCurrent_(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    if(eglTerminate_)eglTerminate_(dpy);
    printf("RESULT: score=%d/2 (render, glsl)\n",score);
    return score==2?0:1;
}
