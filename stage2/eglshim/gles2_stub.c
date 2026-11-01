/* libGLESv2_soft.so -- tiny GLES2 "driver" for Android's GLES loader (plain C: no libc++ dependency).
 * The loader dlsym()s each gl* name from this library and falls back to eglGetProcAddress() (our EGL shim -> Mesa)
 * for everything it does not find. We only export the two entry points that must be intercepted. */
void soft_glEGLImageTargetTexture2DOES(unsigned target, void* image);
void soft_glEGLImageTargetRenderbufferStorageOES(unsigned target, void* image);
__attribute__((visibility("default"))) void glEGLImageTargetTexture2DOES(unsigned t, void* i) { soft_glEGLImageTargetTexture2DOES(t, i); }
__attribute__((visibility("default"))) void glEGLImageTargetRenderbufferStorageOES(unsigned t, void* i) { soft_glEGLImageTargetRenderbufferStorageOES(t, i); }
