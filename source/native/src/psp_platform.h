/* Frozen Gate2 v2 platform adapter; source/native/probes remains unchanged. */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include "psp_layer_policy.h"

#ifndef LMC
#define LMC "/opt/sys/bin/LayerManagerControl"
#endif
#ifndef PROBE_VISIBLE_MS
#define PROBE_VISIBLE_MS 5000u
#endif
#ifndef PROBE_WATCHDOG_MS
#define PROBE_WATCHDOG_MS 14000u
#endif
#ifndef PROBE_COMMAND_MS
#define PROBE_COMMAND_MS 1500u
#endif
#define LAYER "0x80000000"
#define WIDTH 1280
#define HEIGHT 480
/* Screen ABI IDs already used on this unit. 40 is window SIZE, not SOURCE_SIZE.
 * OPENGL_ES2=32 is public Screen usage; actual acceptance is part of this probe. */
enum { P_BUFFER_SIZE=5,P_FORMAT=14,P_ID=20,P_POSITION=35,P_SIZE=40,P_USAGE=48 };
static volatile sig_atomic_t stopped;
static void on_signal(int sig) {(void)sig;stopped=1;}
static uint64_t now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000u+t.tv_nsec/1000000u;}
static void sleep_ms(unsigned n) {struct timespec t;t.tv_sec=n/1000u;t.tv_nsec=(long)(n%1000u)*1000000L;while(nanosleep(&t,&t)<0 && errno==EINTR && !stopped) {}}
static void handlers(void) {struct sigaction a;memset(&a,0,sizeof a);a.sa_handler=on_signal;sigemptyset(&a.sa_mask);sigaction(SIGTERM,&a,0);sigaction(SIGINT,&a,0);sigaction(SIGHUP,&a,0);signal(SIGPIPE,SIG_IGN);}
static int bind_port(unsigned port) {
 int fd=socket(AF_INET,SOCK_DGRAM,0);struct sockaddr_in a;if(fd<0)return -1;
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((unsigned short)port);
 if(bind(fd,(struct sockaddr*)&a,sizeof a)!=0){close(fd);return -1;}return fd;
}
/* Uses only existing configuration. No driver/config/system environment writes. */
static unsigned configured_displays[8],configured_count;
static int inspect_graphics_root(const char *root) {
 char path[768],line[1024];FILE *f;unsigned long id;size_t total=0;
 if(!root || root[0]!='/' || strlen(root)>600 || strchr(root,'\n') || strchr(root,'\r'))return 0;
 if(snprintf(path,sizeof path,"%s/graphics.conf",root)>=(int)sizeof path)return 0;
 f=fopen(path,"r");if(!f)return 0;configured_count=0;
 while(fgets(line,sizeof line,f) && (total+=strlen(line))<=65536u) {
  if(sscanf(line," begin egl display %lu",&id)==1 && id>0 && id<=0x7ffffffful && configured_count<8)configured_displays[configured_count++]=(unsigned)id;
 }
 fclose(f);printf("psp graphics_config=%s candidate_displays=%u\n",path,configured_count);return 1;
}
static int terminate_owned(pid_t pid) {
 unsigned i;int status;kill(-pid,SIGTERM);
 for(i=0;i<15;++i){if(waitpid(pid,&status,WNOHANG)==pid)return WIFEXITED(status) && WEXITSTATUS(status)==0;sleep_ms(20);}
 kill(-pid,SIGKILL);
 for(i=0;i<25;++i){if(waitpid(pid,&status,WNOHANG)==pid)return WIFEXITED(status) && WEXITSTATUS(status)==0;sleep_ms(20);}
 printf("psp warning=child_not_reaped pid=%ld\n",(long)pid);return 0;
}
/* Every LayerManager invocation has its own bounded process group. No popen
 * that can hold restoration forever if a shell/service does not respond. */
static int command(const char *body,char *out,size_t cap) {
 int pipes[2],status=0,done=0,truncated=0;pid_t pid;size_t used=0;uint64_t until;
 char cmd[4096];
 if(!cap)return -1;out[0]=0;
 snprintf(cmd,sizeof cmd,"LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib:/fs/sda0/lib:/fs/sda0/opt/sys/lib; export LD_LIBRARY_PATH; set -e; %s",body);
 if(pipe(pipes)!=0)return -1;
 pid=fork();if(pid<0){close(pipes[0]);close(pipes[1]);return -1;}
 if(pid==0){char *args[]={"sh","-c",cmd,0};setpgid(0,0);close(pipes[0]);dup2(pipes[1],1);dup2(pipes[1],2);close(pipes[1]);execv("/bin/sh",args);_exit(127);}
 setpgid(pid,pid);close(pipes[1]);fcntl(pipes[0],F_SETFL,O_NONBLOCK);until=now_ms()+PROBE_COMMAND_MS;
 while(now_ms()<until) {
  char temp[512];ssize_t got;
  while((got=read(pipes[0],temp,sizeof temp))>0) {
   size_t n=(size_t)got;if(n>cap-1-used){n=cap-1-used;truncated=1;}memcpy(out+used,temp,n);used+=n;
  }
  if(!done && waitpid(pid,&status,WNOHANG)==pid)done=1;
  if(done && got==0)break;
  sleep_ms(10);
 }
 out[used]=0;close(pipes[0]);
 if(!done){printf("psp lmc_timeout\n");terminate_owned(pid);return -1;}
 if(truncated){printf("psp lmc_output_truncated\n");return -1;}
 return WIFEXITED(status) && WEXITSTATUS(status)==0?0:-1;
}
static int graphics_environment(char **argv) {
 static const char *roots[]={"/usr/lib/graphics/omap5432","/usr/lib/graphics/omap5430","/usr/lib/graphics","/opt/sys/lib/graphics","/opt/sys/etc","/etc/system/config","/etc","/lib/graphics","/opt/mm/lib/graphics"};
 char selected[640],out[16384],library_path[4096];const char *root=getenv("GRAPHICS_ROOT"),*old;unsigned i;int found=0;
 if(inspect_graphics_root(root)){snprintf(selected,sizeof selected,"%s",root);found=1;}
 if(getenv("ID6_PSP_ENV_READY")){if(!found)puts("psp graphics_root_missing_after_reexec");return found;}
 if(!found){
  const char *commands[]={"pidin -p screen environment","pidin -p HmiMain3D environment"};
  for(i=0;i<sizeof commands/sizeof commands[0] && !found;++i){
   if(command(commands[i],out,sizeof out)==0){const char *p=strstr(out,"GRAPHICS_ROOT=");
    if(p){size_t n;p+=strlen("GRAPHICS_ROOT=");n=strcspn(p,"\r\n \t");if(n && n<sizeof selected){memcpy(selected,p,n);selected[n]=0;found=inspect_graphics_root(selected);if(found)printf("psp graphics_root_source=%s\n",commands[i]);}}
   }
  }
 }
 for(i=0;i<sizeof roots/sizeof roots[0] && !found;++i)if(inspect_graphics_root(roots[i])){snprintf(selected,sizeof selected,"%s",roots[i]);found=1;printf("psp graphics_root_source=existing_config\n");}
 if(!found){printf("psp graphics_root_unresolved inherited=%s\n",root?root:"unset");return 0;}
 old=getenv("LD_LIBRARY_PATH");
 if(snprintf(library_path,sizeof library_path,"%s:%s",selected,old?old:"/opt/sys/lib:/opt/mm/lib:/lib:/usr/lib")>=(int)sizeof library_path)return 0;
 if(setenv("GRAPHICS_ROOT",selected,1)!=0 || setenv("LD_LIBRARY_PATH",library_path,1)!=0 || setenv("ID6_PSP_ENV_READY","1",1)!=0)return 0;
 printf("psp graphics_env_reexec root=%s\n",selected);execv(argv[0],argv);printf("psp graphics_env_exec_failed errno=%d\n",errno);return 0;
}
static int read_order(unsigned *ids) {
 char out[8192];int rc=command(LMC " get layer " LAYER,out,sizeof out);
 if(rc){printf("psp layer_read_failed %s\n",out);return -1;}
 return probe_parse_order(out,ids,PROBE_MAX_ORDER);
}
static int restore_layer(void) {
 unsigned ids[PROBE_MAX_ORDER];char list[1024],cmd[2048],out[2048];int n=read_order(ids),m,ok=1;
 if(n<0){printf("psp restore_failed=read_current_order\n");ok=0;}
 else {
  m=probe_without_ours(ids,n);
  if(m!=n){if(!m){ids[0]=256u;m=1;}if(!probe_format_order(ids,m,list,sizeof list))return 0;
   snprintf(cmd,sizeof cmd,LMC " set layer " LAYER " render order %s",list);
   if(command(cmd,out,sizeof out)!=0)ok=0;else printf("psp restored_order=%s\n",list);
  }else printf("psp restored_order=unchanged\n");
 }
 snprintf(cmd,sizeof cmd,LMC " set surface %u visibility 0",PROBE_SURFACE);
 /* A gone window can make hide fail: current order restoration is authoritative. */
 (void)command(cmd,out,sizeof out);return ok;
}
static int attach_layer(void) {
 unsigned ids[PROBE_MAX_ORDER+1];char out[8192],cmd[2048],list[1024];const char *p;long long native=-1;int n,i;
 snprintf(cmd,sizeof cmd,LMC " set surface %u source region 0 0 %d %d; " LMC " set surface %u destination region 0 0 %d %d; " LMC " set surface %u opacity 1.0; " LMC " set surface %u visibility 1; " LMC " get surface %u",PROBE_SURFACE,WIDTH,HEIGHT,PROBE_SURFACE,WIDTH,HEIGHT,PROBE_SURFACE,PROBE_SURFACE,PROBE_SURFACE);
 if(command(cmd,out,sizeof out)!=0){printf("psp surface_config_failed %s\n",out);return 0;}
 p=strstr(out,"native surface:");if(p)native=strtoll(p+15,0,10);
 if(native<=0 || native==4294967295LL){printf("psp surface_has_no_native_image %s\n",out);return 0;}
 n=read_order(ids);if(n<=0 || n>=PROBE_MAX_ORDER || probe_order_contains(ids,n,PROBE_SURFACE))return 0;
 for(i=n;i>0;--i)ids[i]=ids[i-1];ids[0]=PROBE_SURFACE;
 if(!probe_format_order(ids,n+1,list,sizeof list))return 0;
 snprintf(cmd,sizeof cmd,LMC " set layer " LAYER " render order %s",list);
 if(command(cmd,out,sizeof out)!=0)return 0;
 printf("psp attached surface=%u order=%s\n",PROBE_SURFACE,list);return 1;
}

/* All graphics entry points are runtime-bound to the car's libraries. This
 * psp ships no proprietary driver or newer OS runtime. */
typedef struct {
 int (*s_create)(void **,int);int (*s_window)(void **,void *,int);int (*s_setiv)(void *,int,const int*);
 int (*s_setcv)(void *,int,int,const char*);int (*s_buffers)(void *,int);int (*s_destroy_window)(void*);int (*s_destroy)(void*);
 EGLDisplay (*GetDisplay)(EGLNativeDisplayType);EGLBoolean (*Initialize)(EGLDisplay,EGLint*,EGLint*);
 EGLBoolean (*ChooseConfig)(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*);
 EGLBoolean (*GetConfigAttrib)(EGLDisplay,EGLConfig,EGLint,EGLint*);
 EGLBoolean (*BindAPI)(EGLenum);EGLContext (*CreateContext)(EGLDisplay,EGLConfig,EGLContext,const EGLint*);
 EGLSurface (*CreateWindowSurface)(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*);
 EGLBoolean (*MakeCurrent)(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
 EGLBoolean (*QuerySurface)(EGLDisplay,EGLSurface,EGLint,EGLint*);
 EGLBoolean (*SwapBuffers)(EGLDisplay,EGLSurface);EGLBoolean (*SwapInterval)(EGLDisplay,EGLint);
 EGLBoolean (*DestroySurface)(EGLDisplay,EGLSurface);EGLBoolean (*DestroyContext)(EGLDisplay,EGLContext);
 EGLBoolean (*Terminate)(EGLDisplay);EGLint (*GetError)(void);const char *(*QueryString)(EGLDisplay,EGLint);
 const GLubyte *(*glGetString)(GLenum);void (*glGetIntegerv)(GLenum,GLint*);GLenum (*glGetError)(void);
 void (*glViewport)(GLint,GLint,GLsizei,GLsizei);void (*glClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);void (*glClear)(GLbitfield);
 void (*glEnable)(GLenum);void (*glDisable)(GLenum);void (*glScissor)(GLint,GLint,GLsizei,GLsizei);
 GLuint (*glCreateShader)(GLenum);void (*glShaderSource)(GLuint,GLsizei,const char*const*,const GLint*);
 void (*glCompileShader)(GLuint);void (*glGetShaderiv)(GLuint,GLenum,GLint*);void (*glGetShaderInfoLog)(GLuint,GLsizei,GLsizei*,char*);
 GLuint (*glCreateProgram)(void);void (*glAttachShader)(GLuint,GLuint);void (*glBindAttribLocation)(GLuint,GLuint,const char*);
 void (*glLinkProgram)(GLuint);void (*glGetProgramiv)(GLuint,GLenum,GLint*);void (*glGetProgramInfoLog)(GLuint,GLsizei,GLsizei*,char*);
 void (*glUseProgram)(GLuint);void (*glVertexAttribPointer)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
 void (*glEnableVertexAttribArray)(GLuint);void (*glDrawArrays)(GLenum,GLint,GLsizei);
 void (*glDeleteShader)(GLuint);void (*glDeleteProgram)(GLuint);
} graphics;
static int bind_symbol(void *lib,const char *name,void *slot,size_t size) {
 void *p=dlsym(lib,name);if(!p || size!=sizeof p){printf("psp missing_symbol=%s\n",name);return 0;}memcpy(slot,&p,size);return 1;
}
static int load_graphics(graphics *g) {
 void *s=dlopen("libscreen.so.1",RTLD_NOW),*e=dlopen("libEGL.so.1",RTLD_NOW),*gl=dlopen("libGLESv2.so.1",RTLD_NOW);
 if(!s || !e || !gl){const char *why=dlerror();printf("psp graphics_load_failed screen=%u egl=%u gles=%u error=%s\n",s!=0,e!=0,gl!=0,why?why:"unknown");return 0;}
#define B(lib,field,name) do {if(!bind_symbol(lib,name,&g->field,sizeof g->field))return 0;}while(0)
 B(s,s_create,"screen_create_context");B(s,s_window,"screen_create_window_type");B(s,s_setiv,"screen_set_window_property_iv");B(s,s_setcv,"screen_set_window_property_cv");B(s,s_buffers,"screen_create_window_buffers");B(s,s_destroy_window,"screen_destroy_window");B(s,s_destroy,"screen_destroy_context");
#define E(field) B(e,field,"egl" #field)
 E(GetDisplay);E(Initialize);E(ChooseConfig);E(GetConfigAttrib);E(BindAPI);E(CreateContext);E(CreateWindowSurface);E(MakeCurrent);E(QuerySurface);E(SwapBuffers);E(SwapInterval);E(DestroySurface);E(DestroyContext);E(Terminate);E(GetError);E(QueryString);
#define G(field) B(gl,field,#field)
 G(glGetString);G(glGetIntegerv);G(glGetError);G(glViewport);G(glClearColor);G(glClear);G(glEnable);G(glDisable);G(glScissor);G(glCreateShader);G(glShaderSource);G(glCompileShader);G(glGetShaderiv);G(glGetShaderInfoLog);G(glCreateProgram);G(glAttachShader);G(glBindAttribLocation);G(glLinkProgram);G(glGetProgramiv);G(glGetProgramInfoLog);G(glUseProgram);G(glVertexAttribPointer);G(glEnableVertexAttribArray);G(glDrawArrays);G(glDeleteShader);G(glDeleteProgram);
#undef B
#undef E
#undef G
 return 1;
}
static GLuint shader(graphics *g,GLenum type,const char *source) {
 GLuint s=g->glCreateShader(type);GLint ok=0;char text[512];if(!s)return 0;
 g->glShaderSource(s,1,&source,0);g->glCompileShader(s);g->glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){g->glGetShaderInfoLog(s,sizeof text,0,text);printf("psp shader_failed=%s\n",text);g->glDeleteShader(s);return 0;}return s;
}
