
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fault(const char*s){const char*f=getenv("PROBE_FAULT");return f&&!strcmp(s,f);}
static void event(const char*s){FILE*f=fopen("events.txt","a");if(f){fprintf(f,"%s\n",s);fclose(f);}}
int screen_create_context(void**p,int x){(void)x;*p=(void*)1;event("screen_context");return 0;}
int screen_create_window_type(void**p,void*c,int x){(void)c;(void)x;*p=(void*)2;event("screen_window");return 0;}
int screen_set_window_property_iv(void*w,int p,const int*v){(void)w;(void)p;(void)v;return 0;}
int screen_set_window_property_cv(void*w,int p,int n,const char*v){(void)w;(void)p;(void)n;(void)v;return 0;}
int screen_create_window_buffers(void*w,int n){(void)w;(void)n;return 0;}
int screen_destroy_window(void*w){(void)w;event("destroy_window");return 0;}
int screen_destroy_context(void*w){(void)w;event("destroy_screen");return 0;}
EGLDisplay eglGetDisplay(EGLNativeDisplayType n){if(fault("display_all") || (fault("display_default") && !n))return EGL_NO_DISPLAY;return (EGLDisplay)1;}
EGLBoolean eglInitialize(EGLDisplay d,EGLint*a,EGLint*b){(void)d;*a=1;*b=4;event("egl_initialize");return !fault("initialize");}
EGLBoolean eglChooseConfig(EGLDisplay d,const EGLint*a,EGLConfig*c,EGLint n,EGLint*k){(void)d;(void)a;(void)n;*c=(EGLConfig)1;*k=1;return 1;}
EGLBoolean eglGetConfigAttrib(EGLDisplay d,EGLConfig c,EGLint k,EGLint*v){(void)d;(void)c;(void)k;*v=8;return 1;}
EGLBoolean eglBindAPI(EGLenum e){(void)e;return 1;}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig c,EGLContext s,const EGLint*a){(void)d;(void)c;(void)s;(void)a;event("egl_context");return fault("context")?EGL_NO_CONTEXT:(EGLContext)1;}
EGLSurface eglCreateWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType w,const EGLint*a){(void)d;(void)c;(void)w;(void)a;event("egl_surface");return (EGLSurface)1;}
EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface a,EGLSurface b,EGLContext c){(void)d;(void)a;(void)b;event(c?"current":"unbind");return 1;}
EGLBoolean eglQuerySurface(EGLDisplay d,EGLSurface s,EGLint k,EGLint*v){(void)d;(void)s;*v=k==EGL_WIDTH?1280:480;return 1;}
EGLBoolean eglSwapBuffers(EGLDisplay d,EGLSurface s){static int n;(void)d;(void)s;if(fault("hang")||(fault("latehang")&&++n>1))for(;;)pause();event("swap");return !fault("swap");}
EGLBoolean eglSwapInterval(EGLDisplay d,EGLint n){(void)d;(void)n;return 1;}
EGLBoolean eglDestroySurface(EGLDisplay d,EGLSurface s){(void)d;(void)s;event("destroy_surface");return 1;}
EGLBoolean eglDestroyContext(EGLDisplay d,EGLContext c){(void)d;(void)c;event("destroy_context");return 1;}
EGLBoolean eglTerminate(EGLDisplay d){(void)d;event("terminate");return 1;}
EGLint eglGetError(void){return EGL_BAD_ALLOC;}
const char*eglQueryString(EGLDisplay d,EGLint k){(void)d;(void)k;return "fake-only";}
const GLubyte*glGetString(GLenum k){(void)k;return (const GLubyte*)"fake-only";}
void glGetIntegerv(GLenum k,GLint*v){(void)k;*v=2048;}
GLenum glGetError(void){return GL_NO_ERROR;}
void glViewport(GLint a,GLint b,GLsizei c,GLsizei d){(void)a;(void)b;(void)c;(void)d;}
void glClearColor(GLfloat a,GLfloat b,GLfloat c,GLfloat d){(void)a;(void)b;(void)c;(void)d;}
void glClear(GLbitfield b){(void)b;}
void glEnable(GLenum e){(void)e;} void glDisable(GLenum e){(void)e;}
void glScissor(GLint a,GLint b,GLsizei c,GLsizei d){(void)a;(void)b;(void)c;(void)d;}
GLuint glCreateShader(GLenum e){return e==GL_VERTEX_SHADER?1:2;}
void glShaderSource(GLuint s,GLsizei n,const char**c,const GLint*l){(void)s;(void)n;(void)c;(void)l;}
void glCompileShader(GLuint s){(void)s;}
void glGetShaderiv(GLuint s,GLenum k,GLint*v){(void)s;(void)k;*v=!fault("shader");}
void glGetShaderInfoLog(GLuint s,GLsizei n,GLsizei*l,char*p){(void)s;(void)n;(void)l;strcpy(p,"injected");}
GLuint glCreateProgram(void){return 3;} void glAttachShader(GLuint p,GLuint s){(void)p;(void)s;}
void glBindAttribLocation(GLuint p,GLuint i,const char*n){(void)p;(void)i;(void)n;}
void glLinkProgram(GLuint p){(void)p;}
void glGetProgramiv(GLuint p,GLenum k,GLint*v){(void)p;(void)k;*v=1;}
void glGetProgramInfoLog(GLuint p,GLsizei n,GLsizei*l,char*s){(void)p;(void)n;(void)l;*s=0;}
void glUseProgram(GLuint p){(void)p;}
void glVertexAttribPointer(GLuint i,GLint s,GLenum t,GLboolean n,GLsizei z,const void*p){(void)i;(void)s;(void)t;(void)n;(void)z;(void)p;}
void glEnableVertexAttribArray(GLuint i){(void)i;}
void glDrawArrays(GLenum m,GLint f,GLsizei n){(void)m;(void)f;(void)n;}
void glDeleteShader(GLuint s){(void)s;event("delete_shader");} void glDeleteProgram(GLuint p){(void)p;event("delete_program");}
