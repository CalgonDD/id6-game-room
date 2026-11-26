/* PSP-only libretro frontend. QNX C99; modern C++ stays private in the DSO.
 * Supervisor alone owns layer order and input, so a blocked/crashed renderer
 * cannot prevent bounded restoration. No car testing is performed by builds. */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE
#define _LARGEFILE64_SOURCE 1
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include "gr_log.h"
#include "car_audio.h"
#include "nes_input.h"
#include "pad_remap.h"   /* your own buttons per game, set in the Game Room list (Y) */
#include "ps1_runtime.h" /* Reuse the tested queue only; no PS1 code/binary changes. */
#include "psp_libretro.h"
#include "psp_memory.h"
#include "psp_jit_preflight.h"
#ifdef __QNX__
#include "gr_pad.c"
#endif
static int psp_print(const char *fmt, ...) {
    char line[640]; int n; va_list ap;
    va_start(ap,fmt); n=vsnprintf(line,sizeof line,fmt,ap); va_end(ap);
    line[strcspn(line,"\r\n")]=0; mps_log("psp %s",line);
    return n;
}
#define printf psp_print
#include "psp_platform.h"
#include "psp_sysmon.h"
#undef printf

#ifndef PSP_CONTROL_PORT
#define PSP_CONTROL_PORT 42893
#endif
#ifndef PSP_SYSTEM
#define PSP_SYSTEM "/mnt/data/ID6Games/PSP"
#endif
#ifndef PSP_SAVE
#define PSP_SAVE "/var/opt/hmi/data/ID5/PSP-data"
#endif
#ifndef PSP_ROOT
#define PSP_ROOT "/mnt/data/ID6Games/PSP/yours/"
#endif
#ifndef PSP_BOOT_MS
#define PSP_BOOT_MS 120000u  /* v33: room for precompiling the shader cache before the first frame */
#endif
#ifndef PSP_FRAME_TIMEOUT_MS
#define PSP_FRAME_TIMEOUT_MS 10000u
#endif
#ifndef PSP_RENDER_STOP_MS
#define PSP_RENDER_STOP_MS 5000u
#endif
#ifndef PSP_PAD_STOP_MS
#define PSP_PAD_STOP_MS 2000u
#endif

static graphics gfx;
static EGLDisplay display=EGL_NO_DISPLAY;
static EGLSurface surface=EGL_NO_SURFACE;
static struct retro_hw_render_callback hw;
static ps1_audio_queue sound=PS1_AUDIO_INIT;
static xpad_state input;
static int input_fd=-1, heartbeat_fd=-1, requested_hw, shutdown_requested;
static unsigned video_frames, duplicate_frames;
static unsigned swap_max_ms;
static double core_fps=60.0;
static char self_path[800];
static uint64_t free_memory(void) {
    uint64_t bytes;int error;
    if(!psp_memory_available(&bytes,&error)) {
        mps_log("psp memory_query=unavailable source=proc_stat64 errno=%d",error);
        return 0;
    }
    return bytes;
}
static int flash_headroom(void) {
    struct statvfs space;uint64_t unit;
    if(statvfs(PSP_SAVE,&space)!=0)return 0;
    unit=space.f_frsize?space.f_frsize:space.f_bsize;
    return unit && (uint64_t)space.f_bavail*unit>=32u*1024u*1024u;
}
static int own_priority(int priority) {
#ifdef __QNX__
    struct sched_param sp;int policy,rc;
    if(pthread_getschedparam(pthread_self(),&policy,&sp))return 0;
    sp.sched_priority=priority;rc=pthread_setschedparam(pthread_self(),policy,&sp);
    mps_log("psp priority=%d rc=%d",priority,rc);return rc==0;
#else
    (void)priority;return 1;
#endif
}

typedef struct {
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*init)(void); void (*deinit)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void); void (*run)(void);
} psp_core;

/* API v0 defaults are retained for every option not explicitly overridden. */
typedef struct {char key[96], value[96];} psp_option;
static psp_option defaults[256];
static unsigned default_count;
static const char *option_value(const char *key) {
    static const struct {const char *key,*value;} options[]={
        {"ppsspp_cpu_core","JIT"}, {"ppsspp_fast_memory","disabled"},
        {"ppsspp_ignore_bad_memory_access","disabled"},
        {"ppsspp_internal_resolution","480x272"}, {"ppsspp_cache_iso","disabled"},
        {"ppsspp_frameskip","1"}, {"ppsspp_frameskiptype","Number of frames"},  /* v32: auto frameskip (v31: 67% -> 94%) */
        {"ppsspp_auto_frameskip","enabled"}, {"ppsspp_frame_duplication","disabled"},
        {"ppsspp_detect_vsync_swap_interval","disabled"},
        {"ppsspp_inflight_frames","Up to 2"}, {"ppsspp_skip_gpu_readbacks","copy_to_texture"},
        {"ppsspp_software_skinning","disabled"},
        {"ppsspp_gpu_hardware_transform","enabled"},
        {"ppsspp_lower_resolution_for_effects","off"},
        {"ppsspp_spline_quality","Low"},
        {"ppsspp_lazy_texture_caching","disabled"},
        {"ppsspp_enable_wlan","disabled"}, {"ppsspp_enable_upnp","disabled"}
    };
    unsigned i;
    for(i=0;i<sizeof options/sizeof options[0];++i)
        if(!strcmp(key,options[i].key))return options[i].value;
    for(i=0;i<default_count;++i)if(!strcmp(key,defaults[i].key))return defaults[i].value;
    return 0;
}
static void core_log(enum retro_log_level level,const char *fmt,...) {
    char line[560];va_list ap;
    if(level<RETRO_LOG_WARN)return;
    va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    line[strcspn(line,"\r\n")]=0;mps_log("psp core level=%u %s",(unsigned)level,line);
}
static uintptr_t current_framebuffer(void) {return 0;}
static retro_proc_address_t get_proc(const char *name) {
    void *p;retro_proc_address_t result=0;
    static void *gles,*egl;
    typedef retro_proc_address_t (*get_fn)(const char *);
    get_fn get=0;
    if(!gles)gles=dlopen("libGLESv2.so.1",RTLD_NOW);
    p=gles?dlsym(gles,name):0;
    if(p){memcpy(&result,&p,sizeof result);return result;}
    if(!egl)egl=dlopen("libEGL.so.1",RTLD_NOW);
    p=egl?dlsym(egl,"eglGetProcAddress"):0;
    if(p){memcpy(&get,&p,sizeof get);return get(name);}
    return 0;
}
static int av_valid(const struct retro_system_av_info *av) {
    if(av->timing.sample_rate!=44100.0 || av->timing.fps<10.0 || av->timing.fps>120.0 ||
       av->geometry.base_width!=WIDTH || av->geometry.base_height!=HEIGHT ||
       av->geometry.max_width>WIDTH || av->geometry.max_height>HEIGHT)return 0;
    core_fps=av->timing.fps;return 1;
}
static bool environment(unsigned command,void *data) {
    switch(command) {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data=0;return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const struct retro_variable *v=(const struct retro_variable*)data;
        default_count=0;
        for(;v && v->key && default_count<256;++v){const char *p=strstr(v->value,"; ");size_t n;
            if(!p)continue;p+=2;n=strcspn(p,"|");
            if(strlen(v->key)>=sizeof defaults[0].key || n>=sizeof defaults[0].value)continue;
            strcpy(defaults[default_count].key,v->key);memcpy(defaults[default_count].value,p,n);
            defaults[default_count].value[n]=0;++default_count;
        }return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v=(struct retro_variable*)data;v->value=option_value(v->key);return v->value!=0;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data=false;return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data=PSP_SYSTEM;return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data=PSP_SAVE;return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log=core_log;return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *(enum retro_hw_context_type*)data=RETRO_HW_CONTEXT_OPENGLES2;return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback *r=(struct retro_hw_render_callback*)data;
        if(r->context_type!=RETRO_HW_CONTEXT_OPENGLES2 || !r->bottom_left_origin || r->version_major>2)return false;
        r->get_current_framebuffer=current_framebuffer;r->get_proc_address=get_proc;
        hw=*r;requested_hw=1;return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format*)data==RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if(!av_valid((const struct retro_system_av_info*)data)){shutdown_requested=1;return false;}return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g=(const struct retro_game_geometry*)data;
        return g->base_width==WIDTH && g->base_height==HEIGHT && g->max_width<=WIDTH && g->max_height<=HEIGHT;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data=true;return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: return true;
    case RETRO_ENVIRONMENT_SHUTDOWN: if(!shutdown_requested)shutdown_requested=2;return true;
    default: return false;
    }
}
static size_t audio_batch(const int16_t *samples,size_t frames) {
    size_t i;
    if(!samples || frames>44100u)return 0;
    pthread_mutex_lock(&sound.lock);
    if(sound.active)for(i=0;i<frames;++i)ps1_audio_put_locked(&sound,samples[i*2],samples[i*2+1]);
    pthread_mutex_unlock(&sound.lock);return frames;
}
static void audio_one(int16_t left,int16_t right) {int16_t s[2]={left,right};(void)audio_batch(s,1);}
static void audio_fill(int16_t *out,unsigned frames) {ps1_audio_take(&sound,out,frames);}
static void input_poll(void) {
    xpad_state latest;ssize_t n;
    while((n=read(input_fd,&latest,sizeof latest))==(ssize_t)sizeof latest)input=latest;
    if(n==0)stopped=1;
}
static rm_map psp_map;   /* this game's own button layout; active = 0: the built-in one below */
static uint16_t psp_buttons(const xpad_state *pad) {
    uint16_t buttons;
    if(psp_map.active)return (uint16_t)(rm_buttons(&rm_psp,&psp_map,pad,ns_retropad_ps1(pad))&0xfffu);
    buttons=(uint16_t)(ns_retropad_ps1(pad)&0xfffu);
    if(pad->lt>100)buttons|=1u<<RETRO_DEVICE_ID_JOYPAD_L;
    if(pad->rt>100)buttons|=1u<<RETRO_DEVICE_ID_JOYPAD_R;
    return buttons;
}
static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id) {
    if(port)return 0;
    if((device&RETRO_DEVICE_MASK)==RETRO_DEVICE_ANALOG && index==RETRO_DEVICE_INDEX_ANALOG_LEFT && id<=1)
        return ns_ps1_axis(&input,0,id);
    if((device&RETRO_DEVICE_MASK)!=RETRO_DEVICE_JOYPAD)return 0;
    if(id==RETRO_DEVICE_ID_JOYPAD_MASK)return (int16_t)psp_buttons(&input);
    if(id>15)return 0;
    return id<12?(int16_t)((psp_buttons(&input)>>id)&1u):0;
}
static void video(const void *data,unsigned width,unsigned height,size_t pitch) {
    uint64_t start;
    (void)pitch;
    if(!data){++duplicate_frames;return;}
    if(data!=RETRO_HW_FRAME_BUFFER_VALID || width!=WIDTH || height!=HEIGHT){shutdown_requested=1;return;}
    start=now_ms();
    if(!gfx.SwapBuffers(display,surface)){mps_log("psp swap_failed error=0x%x",gfx.GetError());shutdown_requested=1;}
    else ++video_frames;
    if(now_ms()-start>swap_max_ms)swap_max_ms=(unsigned)(now_ms()-start);
}
static int load_core(psp_core *c,void **handle) {
    char path[900],*slash;void *lib;
    snprintf(path,sizeof path,"%s",self_path);slash=strrchr(path,'/');if(!slash)return 0;
    strcpy(slash+1,"ppsspp_libretro.so");lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!lib){mps_log("psp core_load_failed %s",dlerror());return 0;}
#define CORE(f) if(!bind_symbol(lib,"retro_" #f,&c->f,sizeof c->f)){dlclose(lib);return 0;}
    CORE(set_environment);CORE(set_video_refresh);CORE(set_audio_sample);CORE(set_audio_sample_batch);
    CORE(set_input_poll);CORE(set_input_state);CORE(api_version);CORE(get_system_info);CORE(get_system_av_info);
    CORE(init);CORE(deinit);CORE(load_game);CORE(unload_game);CORE(run);
#undef CORE
    *handle=lib;return c->api_version()==RETRO_API_VERSION;
}
/* ID6 v33: keep the core's disk shader cache between launches; delete it only when a different build is installed
 * (the marker holds the worker version, which changes with every build). */
#define PSP_CACHE_DIR "/var/opt/hmi/data/ID5/PSP-data/PSP/SYSTEM/CACHE"
#define PSP_BUILD_TAG "gate3-shcache-v33"
static void psp_shader_cache_check(void) {
    const char *mark=PSP_CACHE_DIR "/ID6-shadercache-build.txt";
    char old[64];int fd;ssize_t n=0;
    memset(old,0,sizeof old);
    fd=open(mark,O_RDONLY);
    if(fd>=0){n=read(fd,old,sizeof old-1);close(fd);if(n<0)old[0]=0;}
    if(!strcmp(old,PSP_BUILD_TAG)){mps_log("psp shadercache kept build=%s",PSP_BUILD_TAG);return;}
    (void)unlink(PSP_CACHE_DIR "/ULES00151.glshadercache");
    (void)unlink(PSP_CACHE_DIR "/ULUS10041.glshadercache");
    (void)mkdir("/var/opt/hmi/data/ID5/PSP-data/PSP/SYSTEM",0755);
    (void)mkdir(PSP_CACHE_DIR,0755);
    fd=open(mark,O_WRONLY|O_CREAT|O_TRUNC,0644);
    n=-1;
    if(fd>=0){n=write(fd,PSP_BUILD_TAG,strlen(PSP_BUILD_TAG));close(fd);}
    mps_log("psp shadercache reset build=%s old=%s marker=%s",PSP_BUILD_TAG,old[0]?old:"none",n>0?"written":"failed");
}

static int render_game(const char *rom,int ready,int inputs,int control) {
    void *sctx=0,*win=0,*lib=0;EGLContext context=EGL_NO_CONTEXT;EGLConfig conf;
    EGLint count=0,major,minor,width=0,height=0;int bsize[]={WIDTH,HEIGHT},size[]={WIDTH,HEIGHT},pos[]={0,0},usage=32,format=8;
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    psp_core core;struct retro_game_info game;struct retro_system_info info;struct retro_system_av_info av;
    int rc=1,initialized=0,loaded=0,reset=0,ready_sent=0,detach_ack=0;char id[32];uint64_t due,last_report,run_total_ms=0;unsigned calls=0,run_max_ms=0;
    memset(&core,0,sizeof core);memset(&gfx,0,sizeof gfx);handlers();
#ifdef __QNX__
    {struct rlimit limit;limit.rlim_cur=limit.rlim_max=512u*1024u*1024u;
        if(setrlimit(RLIMIT_AS,&limit)!=0){mps_log("psp refused=address_space_limit errno=%d",errno);goto end;}}
#endif
    heartbeat_fd=ready;input_fd=inputs;fcntl(inputs,F_SETFL,O_NONBLOCK);
    if(!psp_jit_preflight())goto end;
    if(!load_graphics(&gfx))goto end;
    if(gfx.s_create(&sctx,0)!=0 || gfx.s_window(&win,sctx,0)!=0)goto end;
    snprintf(id,sizeof id,"%u",PROBE_SURFACE);
    if(gfx.s_setcv(win,P_ID,(int)strlen(id),id) || gfx.s_setiv(win,P_USAGE,&usage) ||
       gfx.s_setiv(win,P_FORMAT,&format) || gfx.s_setiv(win,P_SIZE,size) || gfx.s_setiv(win,P_POSITION,pos) ||
       gfx.s_setiv(win,P_BUFFER_SIZE,bsize) || gfx.s_buffers(win,2))goto end;
    display=gfx.GetDisplay(EGL_DEFAULT_DISPLAY);
    if(display==EGL_NO_DISPLAY || !gfx.Initialize(display,&major,&minor) || !gfx.BindAPI(EGL_OPENGL_ES_API))goto end;
    if(!gfx.ChooseConfig(display,attrs,&conf,1,&count) || count<1)goto end;
    context=gfx.CreateContext(display,conf,EGL_NO_CONTEXT,ca);if(context==EGL_NO_CONTEXT)goto end;
    surface=gfx.CreateWindowSurface(display,conf,(EGLNativeWindowType)win,0);if(surface==EGL_NO_SURFACE)goto end;
    if(!gfx.MakeCurrent(display,surface,surface,context))goto end;
    if(!gfx.QuerySurface(display,surface,EGL_WIDTH,&width) || !gfx.QuerySurface(display,surface,EGL_HEIGHT,&height) ||
       width!=WIDTH || height!=HEIGHT)goto end;
    mps_log("psp graphics GL=%s renderer=%s",gfx.glGetString(GL_VERSION),gfx.glGetString(GL_RENDERER));
    gfx.glClearColor(0,0,0,1);gfx.glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    if(!gfx.SwapBuffers(display,surface))goto end;
    (void)gfx.SwapInterval(display,1);
    if(write(ready,"R",1)!=1)goto end;
    ready_sent=1;
    {struct pollfd p;char ack=0;p.fd=control;p.events=POLLIN;p.revents=0;
        if(poll(&p,1,5000)<=0 || read(control,&ack,1)!=1)goto end;
        if(ack=='D')detach_ack=1;
        if(ack!='G')goto end;}
    if(!load_core(&core,&lib))goto end;
    core.get_system_info(&info);mps_log("psp core=%s version=%s",info.library_name,info.library_version);
    core.set_environment(environment);core.set_video_refresh(video);core.set_audio_sample(audio_one);
    core.set_audio_sample_batch(audio_batch);core.set_input_poll(input_poll);core.set_input_state(input_state);
    core.init();initialized=1;memset(&game,0,sizeof game);game.path=rom;
    psp_shader_cache_check();
    if(!core.load_game(&game))goto end;loaded=1;
    if(!requested_hw || !hw.context_reset)goto end;
    core.get_system_av_info(&av);if(!av_valid(&av))goto end;
    hw.context_reset();reset=1;ps1_audio_active(&sound,1);ca_start(audio_fill);
    due=now_ms();last_report=due;
    while(!stopped && !shutdown_requested && getppid()>1) {
        uint64_t start=now_ms(),period=(uint64_t)(1000000.0/core_fps+0.5);
        input_poll();core.run();++calls;
        {unsigned elapsed=(unsigned)(now_ms()-start);run_total_ms+=elapsed;if(elapsed>run_max_ms)run_max_ms=elapsed;}
        if(write(ready,"F",1)!=1)break;
        /* Absolute time in microseconds, capped catch-up. Never add a fresh
         * full frame sleep after a slow swap or an audio-device delay. */
        if(calls==1)due=start*1000u;
        due+=period;
        while(!stopped && !shutdown_requested && now_ms()*1000u<due) sleep_ms(1);
        if(now_ms()*1000u>due+2*period)due=now_ms()*1000u;
        if(ps1_audio_depth(&sound)>8192u) { /* Bound sound lead if swap does not throttle. */
            uint64_t until=now_ms()+100u;
            while(ps1_audio_depth(&sound)>4096u && now_ms()<until && !stopped)sleep_ms(1);
        }
        if(now_ms()-last_report>=10000u){unsigned gaps,drops,depth;
            pthread_mutex_lock(&sound.lock);gaps=sound.gaps;drops=sound.drops;depth=sound.write-sound.read;pthread_mutex_unlock(&sound.lock);
            mps_log("psp metrics calls=%u frames=%u dupes=%u run_ms=%llu run_max_ms=%u swap_max_ms=%u audio_depth=%u gaps=%u drops=%u",calls,video_frames,duplicate_frames,(unsigned long long)run_total_ms,run_max_ms,swap_max_ms,depth,gaps,drops);
            if(!flash_headroom()){mps_log("psp stop=save_reserve");shutdown_requested=1;}
            last_report=now_ms();}
    }
    rc=shutdown_requested==1?1:0;
end:
    /* Normal/error exits request detachment before destroying the image. A
     * dedicated control pipe keeps this ack independent of queued pad input. */
    if(ready_sent && !detach_ack) {
        struct pollfd p;char ack=0;p.fd=control;p.events=POLLIN;p.revents=0;
        (void)write(ready,"D",1);
        if(poll(&p,1,5000)>0 && read(control,&ack,1)==1 && ack=='D')detach_ack=1;
        if(!detach_ack)mps_log("psp detach_ack_missing");
    }
    ps1_audio_active(&sound,0);ca_stop();
    if(reset && hw.context_destroy)hw.context_destroy();
    if(loaded)core.unload_game();if(initialized)core.deinit();
    /* One core per renderer process. Keep its static C++ runtime mapped until
     * _exit after explicit game/audio/graphics cleanup. QNX dlclose/static
     * destructor teardown is not a supported reload path for this worker. */
    if(display!=EGL_NO_DISPLAY && gfx.MakeCurrent)gfx.MakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    if(surface!=EGL_NO_SURFACE)gfx.DestroySurface(display,surface);
    if(context!=EGL_NO_CONTEXT)gfx.DestroyContext(display,context);
    if(display!=EGL_NO_DISPLAY)gfx.Terminate(display);
    if(win)gfx.s_destroy_window(win);if(sctx)gfx.s_destroy(sctx);
    mps_log("psp renderer_end rc=%d calls=%u frames=%u",rc,calls,video_frames);return rc;
}

static void return_to_menu(void) {
    char path[900],*slash;char *args[]={path,"--play",0};int fd;
    snprintf(path,sizeof path,"%s",self_path);slash=strrchr(path,'/');if(!slash)return;
    strcpy(slash+1,"freedoom-qnx");
    for(fd=3;fd<256;++fd)(void)fcntl(fd,F_SETFD,FD_CLOEXEC);
    execv(path,args);mps_log("psp menu_exec_failed errno=%d",errno);
}
static int stop_child(pid_t pid,unsigned grace_ms) {
    uint64_t until=now_ms()+grace_ms;int status;
    kill(-pid,SIGTERM);
    do {if(waitpid(pid,&status,WNOHANG)==pid)return 1;sleep_ms(20);}while(now_ms()<until);
    kill(-pid,SIGKILL);until=now_ms()+1000u;
    do {if(waitpid(pid,&status,WNOHANG)==pid)return 1;sleep_ms(20);}while(now_ms()<until);
    mps_log("psp child_not_reaped pid=%ld",(long)pid);return 0;
}
static int release_audio_bounded(void) {
    const char *tool=ca_audioconn();char body[768],out[512];
    if(!tool)return 1;
    snprintf(body,sizeof body,"LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib:/fs/sda0/opt/mm/lib; export LD_LIBRARY_PATH; %s -r 0xb8",tool);
    if(command(body,out,sizeof out)!=0){mps_log("psp audio_release_failed %s",out);return 0;}
    return 1;
}
/* ID6 v25: GPU driver app hints. The car's libsrv_um reads /etc/powervr.ini, then app/native/powervr.ini relative
 * to the working directory (the later file wins). Our copy is PSP_SYSTEM/app/native/powervr.ini
 * (v25: DisableHWTextureUpload=1, no effect. v26: ParamBufferSize/MaxParamBufferSize 32 MB and PDSFragBufferSize
 * 300 KB, BMW's [apnnavk] values, against mid-frame "additional renders": fixed the squares. v27: also
 * FBODepthDiscard=0 (keep render-target depth across FBO switches) and ExternalZBufferMode=2, BMW's nav value). */
#define PSP_SGXDUMP "/var/dump/sgxdump.txt"
static struct stat sgxdump_before;
static int sgxdump_had;
static void psp_log_file_lines(const char *tag,const char *path,unsigned max_lines) {
    FILE *f=fopen(path,"r");char line[200];unsigned n=0;
    if(!f){mps_log("psp %s missing path=%s errno=%d",tag,path,errno);return;}
    while(n<max_lines && fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;++n;mps_log("psp %s %u: %s",tag,n,line);}
    if(!feof(f))mps_log("psp %s truncated_after=%u",tag,n);
    fclose(f);
}
static void psp_sgxdump_note(int after) {
    struct stat st;int have=stat(PSP_SGXDUMP,&st)==0;
    mps_log("psp sgxdump_%s present=%d size=%lld mtime=%lld",after?"after":"before",have,
        have?(long long)st.st_size:0LL,have?(long long)st.st_mtime:0LL);
    if(!after){sgxdump_had=have;if(have){sgxdump_before=st;psp_log_file_lines("sgxdump_head",PSP_SGXDUMP,12);}return;}
    if(have && (!sgxdump_had || st.st_mtime!=sgxdump_before.st_mtime || st.st_size!=sgxdump_before.st_size)){
        FILE *f=fopen(PSP_SGXDUMP,"r");char line[200];unsigned n=0;
        if(!f)return;
        if(st.st_size>4000 && fseek(f,(long)st.st_size-4000,SEEK_SET)==0 && !fgets(line,sizeof line,f)){fclose(f);return;}
        while(n<60 && fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;++n;mps_log("psp sgxdump_tail %u: %s",n,line);}
        fclose(f);
    }
}
static void psp_driver_hints(void) {
    char cwd[512];
    if(self_path[0]!='/' && getcwd(cwd,sizeof cwd)){char abs[sizeof self_path];
        snprintf(abs,sizeof abs,"%s/%s",cwd,self_path);snprintf(self_path,sizeof self_path,"%s",abs);}
    mps_log("psp cwd_before=%s self=%s",getcwd(cwd,sizeof cwd)?cwd:"?",self_path);
    if(chdir(PSP_SYSTEM)!=0)mps_log("psp chdir_failed errno=%d",errno);
    mps_log("psp cwd_after=%s",getcwd(cwd,sizeof cwd)?cwd:"?");
    psp_log_file_lines("hint_etc","/etc/powervr.ini",150);
    psp_log_file_lines("hint_own","app/native/powervr.ini",20);
    psp_sgxdump_note(0);
}
int main(int argc,char **argv) {
    int ctl=-1,ready[2],inputs[2],control[2],status=0,done=0,attached=0,menu=0,saw_frame=0,order_count,pending_input=0,detached=0;pid_t child=-1,pad_pid=-1;
    uint64_t last,last_memory_check=0,available;unsigned ids[PROBE_MAX_ORDER];
    handlers();mps_log("psp worker version=" PSP_BUILD_TAG " source=proc_stat64");
    if(mps_trace_fd>=0)fcntl(mps_trace_fd,F_SETFL,O_NONBLOCK);
    snprintf(self_path,sizeof self_path,"%s",argv[0]);
    if(argc==2 && !strcmp(argv[1],"--restore")){int ok=restore_layer();
        if(!release_audio_bounded())ok=0;return ok?0:1;}
    if(argc!=3 || strcmp(argv[1],"--play-rom") || strncmp(argv[2],PSP_ROOT,strlen(PSP_ROOT)) ||
       strstr(argv[2],"/../") || strchr(argv[2],'\n'))return 2;
    if(!graphics_environment(argv))return 3;
    rm_load(&rm_psp,PSP_SAVE,argv[2],&psp_map);mps_log("psp buttons layout=%s",psp_map.active?"own":"default");
    psp_driver_hints();
    available=free_memory();mps_log("psp memory_free_before=%llu",(unsigned long long)available);
    if(available<256u*1024u*1024u){mps_log("psp refused=memory_headroom");return 4;}
    if(mkdir(PSP_SAVE,0755)!=0 && errno!=EEXIST)return 4;
    if(!flash_headroom()){
        mps_log("psp refused=save_flash_reserve");return 4;}
    ctl=bind_port(PSP_CONTROL_PORT);if(ctl<0)return 5;fcntl(ctl,F_SETFL,O_NONBLOCK);
    {unsigned port;for(port=42890;port<=42892;++port){int fd=bind_port(port);if(fd<0){close(ctl);return 5;}close(fd);}}
    order_count=read_order(ids);
    if(order_count<=0 || probe_order_contains(ids,order_count,PROBE_SURFACE))goto finish;
    if(pipe(ready) || pipe(inputs) || pipe(control))goto finish;
    /* The watchdog must preempt a CPU-bound FIFO renderer. Both stay below
     * HMI priority 10; the audio device thread retains the existing priority. */
    if(!own_priority(9))goto finish;
    child=fork();if(child<0)goto finish;
    if(!child){int result;setpgid(0,0);close(ctl);close(ready[0]);close(inputs[1]);close(control[1]);
        if(!own_priority(8))_exit(1);
        result=render_game(argv[2],ready[1],inputs[0],control[0]);_exit(result);}
    setpgid(child,child);close(ready[1]);close(inputs[0]);close(control[0]);fcntl(ready[0],F_SETFL,O_NONBLOCK);fcntl(inputs[1],F_SETFL,O_NONBLOCK);
#ifdef __QNX__
    pad_pid=fork();
    if(!pad_pid){int result;setpgid(0,0);close(ctl);close(ready[0]);close(inputs[1]);close(control[1]);if(!own_priority(8))_exit(1);result=pad_run(0,PSP_CONTROL_PORT,1);_exit(result);}
    if(pad_pid>0)setpgid(pad_pid,pad_pid);
#endif
    last=now_ms();
    while(!stopped && getppid()>1){char messages[128],heart[128];ssize_t n;unsigned i;
        while((n=recv(ctl,messages,sizeof messages-1,0))>0){messages[n]=0;
            if(!strncmp(messages,"stop",4)){stopped=1;menu=0;break;}
            if(xpad_parse(messages,&input)){
                if(ns_nav(&input)&NS_MENU){stopped=1;menu=1;break;}
                pending_input=1;
            }
        }
        if(stopped)break;
        while((n=read(ready[0],heart,sizeof heart))>0)for(i=0;i<(unsigned)n;++i){
            if(heart[i]=='R' && !attached){if(!attach_layer()){stopped=1;break;}attached=1;
                if(write(control[1],"G",1)!=1){stopped=1;break;}}
            if(heart[i]=='D' && !detached){
                if(!restore_layer()){stopped=1;menu=0;break;}
                detached=1;(void)write(control[1],"D",1);
            }
            if(heart[i]=='F')saw_frame=1;
            if(heart[i]=='F' || heart[i]=='R')last=now_ms();
        }
        if(attached && pending_input && write(inputs[1],&input,sizeof input)==(ssize_t)sizeof input)pending_input=0;
        if(attached && !detached)sysmon_tick(child);  /* ID6 v30/v31: CPU monitor (priority A/B off in v31) */
        if(waitpid(child,&status,WNOHANG)==child){done=1;menu=WIFEXITED(status) && WEXITSTATUS(status)==0;break;}
        if(now_ms()-last_memory_check>=1000u){
            last_memory_check=now_ms();available=free_memory();
            if(available<64u*1024u*1024u){mps_log("psp stop=memory_headroom bytes=%llu",(unsigned long long)available);menu=0;break;}}
        if(now_ms()-last>(saw_frame?PSP_FRAME_TIMEOUT_MS:PSP_BOOT_MS)){mps_log("psp watchdog timeout_ms=%u",saw_frame?PSP_FRAME_TIMEOUT_MS:PSP_BOOT_MS);menu=0;break;}
        sleep_ms(2);
    }
    /* Detach first; child shutdown/driver calls have no authority over this. */
    if(!detached && !restore_layer())menu=0;
    if(!detached)(void)write(control[1],"D",1);
    if(!done && child>0 && !stop_child(child,PSP_RENDER_STOP_MS))menu=0;
    if(pad_pid>0 && !stop_child(pad_pid,PSP_PAD_STOP_MS))menu=0;
    close(ready[0]);close(inputs[1]);close(control[1]);
finish:
    (void)release_audio_bounded();
    if(ctl>=0){char msg[128];ssize_t n;
        while((n=recv(ctl,msg,sizeof msg-1,0))>0){msg[n]=0;if(!strncmp(msg,"stop",4))menu=0;}
        close(ctl);
    }
    psp_sgxdump_note(1);
    mps_log("psp supervisor_end attached=%d done=%d status=%d menu=%d",attached,done,status,menu);
    if(menu && getppid()>1)return_to_menu();return done && WIFEXITED(status)?WEXITSTATUS(status):0;
}
