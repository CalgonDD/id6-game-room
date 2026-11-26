#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "QNXOpaquePresent.h"
int main() {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(eglInitialize(d, 0, 0));
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cfg; EGLint count;
    assert(eglChooseConfig(d,attrs,&cfg,1,&count) && count);
    EGLint pa[] = {EGL_WIDTH,8,EGL_HEIGHT,8,EGL_NONE};
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d,cfg,pa);
    EGLContext c = eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);
    assert(eglMakeCurrent(d,s,s,c));
    printf("GL_TEST_RENDERER %s\n",glGetString(GL_RENDERER));
    GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    for (unsigned n=0;n<32;n++) {
        glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glClearColor(0.75f,0.25f,0.5f,0.25f);glClear(GL_COLOR_BUFFER_BIT);
        unsigned char offBefore[16],offAfter[16];glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,offBefore);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        glClearColor(0.25f,0.5f,0.75f,0);glClear(GL_COLOR_BUFFER_BIT);
        unsigned char before[256],after[256];glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,before);
        glScissor(2,3,1,1);if(n&1)glEnable(GL_SCISSOR_TEST);
        GLboolean mask[4]={(GLboolean)((n>>1)&1),(GLboolean)((n>>2)&1),(GLboolean)((n>>3)&1),(GLboolean)((n>>4)&1)};
        glColorMask(mask[0],mask[1],mask[2],mask[3]);glClearColor(.125f,.25f,.5f,.75f);
        if(n&2)glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        QNXMakeBackbufferOpaque();
        GLint binding,box[4];GLboolean actualMask[4];GLfloat clear[4];
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&binding);assert(binding==((n&2)?(GLint)fbo:0));
        assert(glIsEnabled(GL_SCISSOR_TEST)==(n&1));
        glGetIntegerv(GL_SCISSOR_BOX,box);assert(box[0]==2 && box[1]==3 && box[2]==1 && box[3]==1);
        glGetBooleanv(GL_COLOR_WRITEMASK,actualMask);assert(!memcmp(mask,actualMask,4));
        glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);assert(clear[0]==.125f && clear[1]==.25f && clear[2]==.5f && clear[3]==.75f);
        glBindFramebuffer(GL_FRAMEBUFFER,0);glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,after);
        for(int i=0;i<64;i++){assert(!memcmp(before+4*i,after+4*i,3));assert(before[4*i+3]==0 && after[4*i+3]==255);}
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,offAfter);
        assert(!memcmp(offBefore,offAfter,16));assert(glGetError()==GL_NO_ERROR);
    }
    puts("OPAQUE_PRESENT_PASS cases=32 RGB=preserved alpha=255 offscreen=preserved state=restored");
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,c);eglDestroySurface(d,s);eglTerminate(d);
}
