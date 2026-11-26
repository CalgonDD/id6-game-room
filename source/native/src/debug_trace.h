#ifndef DEBUG_TRACE_H
#define DEBUG_TRACE_H

#include "diagnostic_worker.h"
#include "hmi_trace_protocol.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// HMI callbacks only send best-effort, nonblocking loopback datagrams. A
// separate --hmi-trace worker owns all filesystem I/O and rotation. Socket
// queue pressure drops records rather than waiting; seq/lost expose gaps.
namespace {
struct HmiTraceSender {
    int fd;
    unsigned sequence, lost;
};
static HmiTraceSender hmi_trace_sender={-1,0,0};
}

static inline uint64_t trace_mono_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return static_cast<uint64_t>(t.tv_sec)*1000u+t.tv_nsec/1000000u;
}

// Startup only: launch() itself is synchronous process-manager IPC, so this
// is deliberately never done lazily inside a button/focus/model callback.
static inline void trace_start(const char* worker) {
    if (hmi_trace_sender.fd>=0) return;
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd<0) return;
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
        close(fd); return;
    }
    hmi_trace_sender.fd=fd;
    const char* args[]={worker,"--hmi-trace",0};
    // Exclusive bind makes concurrent starts harmless: only one sink stays.
    launch(args,-1,true); // own QNX session: independent of HMI process-group cleanup
}

static inline void trace_stop() {
    if (hmi_trace_sender.fd>=0) close(hmi_trace_sender.fd);
    hmi_trace_sender.fd=-1;
}

static inline void trace_event(const char* app, const char* event) {
    HmiTraceSender& sender=hmi_trace_sender;
    if (sender.fd<0) return;
    const int saved_errno=errno;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME,&wall);
    char line[HMI_TRACE_PACKET];
    int n=snprintf(line,sizeof line,
        "wall=%lld.%03ld mono=%llu pid=%ld tid=%lu seq=%u lost=%u %s %s\n",
        static_cast<long long>(wall.tv_sec),static_cast<long>(wall.tv_nsec/1000000),
        static_cast<unsigned long long>(trace_mono_ms()),static_cast<long>(getpid()),
        static_cast<unsigned long>(pthread_self()),++sender.sequence,sender.lost,app,event);
    if (n>0) {
        struct sockaddr_in address;
        memset(&address,0,sizeof address);
        address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        address.sin_port=htons(HMI_TRACE_PORT);
        size_t length=static_cast<size_t>(n)<sizeof line ? static_cast<size_t>(n) : sizeof line-1;
        if (sendto(sender.fd,line,length,0,reinterpret_cast<struct sockaddr*>(&address),sizeof address)==static_cast<ssize_t>(length)) sender.lost=0;
        else ++sender.lost;
    }
    errno=saved_errno;
}

// Keep scopes local to each plugin DSO: weak inline symbol interposition must
// never redirect another plugin through this module's sender state.
namespace {
// Scope IDs and object address pair entry/return even with concurrent apps.
class HmiTraceCall {
    const char* app_;
    const char* kind_;
    const void* instance_;
    unsigned id_, call_;
    uint64_t start_;
public:
    HmiTraceCall(const char* app,const char* kind,const void* instance,unsigned id)
        : app_(app),kind_(kind),instance_(instance),id_(id),
          call_(hmi_trace_sender.sequence+1),start_(trace_mono_ms()) {
        char event[128];
        snprintf(event,sizeof event,"%s_begin instance=%p id=%u call=%u",kind_,instance_,id_,call_);
        trace_event(app_,event);
    }
    ~HmiTraceCall() {
        char event[144];
        snprintf(event,sizeof event,"%s_end instance=%p id=%u call=%u elapsed_ms=%llu",
            kind_,instance_,id_,call_,static_cast<unsigned long long>(trace_mono_ms()-start_));
        trace_event(app_,event);
    }
private:
    HmiTraceCall(const HmiTraceCall&);
    HmiTraceCall& operator=(const HmiTraceCall&);
};
}

#ifdef HMI_RESCUE
#include "hmi_rescue.h"
#endif

// Every updateModel() of every app goes through here, so the freeze rescue (HMI_RESCUE builds) sees how
// long each one takes. See hmi_rescue.h and Diagnostics/Crash_analysis_work/FREEZE_AB_TEST_RESULTS_2026-11-18.md.
template <class Value>
static inline void trace_update(bmw::PluginApp* instance,const char* app,unsigned model,const Value& value) {
    HmiTraceCall call(app,"model",instance,model);
#ifdef HMI_RESCUE
    rescue_write_begin();
#endif
    instance->updateModel(model,value);
#ifdef HMI_RESCUE
    rescue_write_end();
#endif
}

static inline pid_t trace_launch(const char* app,const void* instance,const char* const* args,int output) {
    HmiTraceCall call(app,"spawn",instance,0);
    return launch(args,output);
}

#endif
