// GameRoom (src/gameroom.cpp): Play starts the game once while shown+focused, focus loss or hide stops it,
// a game that already died is restored on hide, and the page never writes to the screen.
#include "bmw_list.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

uint64_t hmi_test_now=10000;
HmiTestWrite hmi_test_writes[4096];
unsigned hmi_test_write_count=0;
extern "C" int hmi_test_clock_gettime(clockid_t clock,struct timespec* out){
    uint64_t now=hmi_test_now+(clock==CLOCK_REALTIME ? 1700000000000ull : 0ull);
    out->tv_sec=now/1000;out->tv_nsec=(now%1000)*1000000;return 0;
}
extern "C" int hmi_test_open(const char*,int,...){errno=ENOENT;return -1;}
void* operator new(size_t size){void* p=malloc(size);if(!p)abort();return p;}
void operator delete(void* p) throw(){free(p);}
#define DIAGNOSTIC_WORKER_H
static inline bool installed_gateway(const char*,char (&host)[16]){host[0]=0;return false;}
static int launches=0;
static char launch_log[512];
static bool show_alive=false;
static inline pid_t launch(const char* const* args,int,bool=false){
    ++launches;
    strncat(launch_log,args[1],sizeof launch_log-strlen(launch_log)-1);
    strncat(launch_log," ",sizeof launch_log-strlen(launch_log)-1);
    if(args[2]){strncat(launch_log,args[2],sizeof launch_log-strlen(launch_log)-1);strncat(launch_log," ",sizeof launch_log-strlen(launch_log)-1);}
    if(!strcmp(args[1],"--play")){show_alive=true;return 4242;}
    errno=ENOENT;return -1;
}
static inline void reap_worker(pid_t& child){if(child==4242 && show_alive)return;child=-1;}
#define DEBUG_TRACE_H
static char trace_log[16384];
static inline void trace_event(const char* app,const char* text){
    strncat(trace_log,app,sizeof trace_log-strlen(trace_log)-1);
    strncat(trace_log," ",sizeof trace_log-strlen(trace_log)-1);
    strncat(trace_log,text,sizeof trace_log-strlen(trace_log)-1);
    strncat(trace_log,"\n",sizeof trace_log-strlen(trace_log)-1);
}
static inline void trace_start(const char* worker){const char* a[]={worker,"--hmi-trace",0};launch(a,-1,true);}
static inline void trace_stop(){}
class HmiTraceCall{public:HmiTraceCall(const char*,const char*,const void*,unsigned){}};
template<class V>static inline void trace_update(bmw::PluginApp* app,const char*,unsigned id,const V& value){app->updateModel(id,value);}
static inline pid_t trace_launch(const char*,const void*,const char* const* args,int output){return launch(args,output);}
#include "../src/gameroom.cpp"

int main(){
    static bmw::IDispatcher dispatcher;static bmw::Bundle bundle;
    bmw::PluginApp* app=pluginAppInfo.create(dispatcher,"host-test",bundle,bundle);
    assert(app->onInit());
    assert(launches==1 && strstr(trace_log,"GameRoom ready"));            // the shared trace writer only

    {bmw::Parcel p;assert(app->deliver(0,30,p)==1);}                      // not shown yet: ignored
    assert(launches==1 && strstr(trace_log,"play_ignored reason=not_shown_or_focused"));
    app->event(100,11,true);app->event(100,1,true);
    {bmw::Parcel p;assert(app->deliver(0,30,p)==1);}
    assert(launches==2 && strstr(launch_log,"--hmi-trace --play ") && strstr(trace_log,"GameRoom play pid=4242"));
    {bmw::Parcel p;assert(app->deliver(0,30,p)==1);}                      // second push while running
    assert(launches==2 && strstr(trace_log,"play_ignored reason=already_running"));
    app->event(100,1,false);                                               // focus lost: "stop", no new process
    assert(launches==2 && strstr(trace_log,"GameRoom stop reason=focus_lost"));
    show_alive=false;
    app->event(100,1,true);
    {bmw::Parcel p;assert(app->deliver(0,30,p)==1);}
    assert(launches==3);
    show_alive=false;                                                      // the game died on its own
    app->event(100,11,false);
    assert(launches==4 && strstr(launch_log,"--play --play --restore ") && strstr(trace_log,"restore reason=hidden"));
    app->event(100,11,true);app->event(100,1,true);                       // Continue: --play --continue
    {bmw::Parcel p;assert(app->deliver(0,31,p)==1);}
    assert(launches==5 && strstr(launch_log,"--restore --play --continue ") && strstr(trace_log,"GameRoom continue pid=4242"));
    {bmw::Parcel p;assert(app->deliver(0,31,p)==1);}                      // already running: ignored
    assert(launches==5);
    app->event(100,11,false);
    assert(strstr(trace_log,"stop reason=hidden"));
    app->advance(hmi_test_now+60000);
    assert(hmi_test_write_count==0);
    assert(app->onFinalize() && strstr(trace_log,"GameRoom finalize"));
    pluginAppInfo.destroy(app);
    printf("GameRoom page: Play starts one game while shown+focused, focus loss stops it, a dead game is restored"
           " on hide, no screen writes.\n");
    printf("GameRoom page: Continue last save starts the game with --continue.\n");
    return 0;
}
