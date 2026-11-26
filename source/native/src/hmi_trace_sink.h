#ifndef HMI_TRACE_SINK_H
#define HMI_TRACE_SINK_H

#include "hmi_trace_protocol.h"
#include <sys/stat.h>
#include <time.h>

#ifndef ID6_DATA_ROOT
#define ID6_DATA_ROOT "/var/opt/hmi/data/ID5"
#endif

static volatile sig_atomic_t hmi_trace_running=1;
static void hmi_trace_stop(int sig) { (void)sig; hmi_trace_running=0; }

static int hmi_trace_installed(void) {
    static const char* const workers[]={
        ID6_DATA_ROOT "/Plugins/GameRoom/freedoom-qnx"};
    unsigned i;
    for (i=0;i<sizeof workers/sizeof workers[0];++i)
        if (access(workers[i],F_OK)==0) return 1;
    return 0;
}

/* Deliberately independent of HmiMain3D's lifetime: drain/retain evidence
 * after a watchdog kill. Exit once Game Room has been uninstalled.
 * Nothing on this path talks to the car's control units. */
static int hmi_trace_sink(void) {
    const char* path=ID6_DATA_ROOT "/HMI-apps-trace.txt";
    const char* previous=ID6_DATA_ROOT "/HMI-apps-trace.previous.txt";
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in address;
    time_t check_at=0;
    if (fd<0) return 1;
    memset(&address,0,sizeof address);
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(HMI_TRACE_PORT);
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0 ||
        bind(fd,(struct sockaddr*)&address,sizeof address)<0) { close(fd); return 1; }
    signal(SIGINT,hmi_trace_stop);
    signal(SIGTERM,hmi_trace_stop);
    signal(SIGPIPE,SIG_IGN);
    while (hmi_trace_running) {
        struct timespec now;
        struct pollfd pending;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if (now.tv_sec>=check_at) {
            if (!hmi_trace_installed()) break;
            check_at=now.tv_sec+5;
        }
        int ready_count;
        pending.fd=fd; pending.events=POLLIN; pending.revents=0;
        ready_count=poll(&pending,1,5000);
        if (ready_count<0) { if (errno==EINTR) continue; break; }
        if (ready_count>0 && (pending.revents&POLLIN)) {
            char line[HMI_TRACE_PACKET];
            struct stat info;
            ssize_t size=recv(fd,line,sizeof line,0);
            FILE* file;
            if (size<=0) continue;
            /* One exclusive writer rotates, never truncating in a callback.
             * If rename fails, retain the old log and drop this record. */
            if (stat(path,&info)==0 && info.st_size+size>HMI_TRACE_LIMIT)
                if (rename(path,previous)!=0) continue;
            file=fopen(path,"a");
            if (!file) continue;
            fwrite(line,1,(size_t)size,file);
            fclose(file);
        }
    }
    close(fd);
    return 0;
}
#endif
