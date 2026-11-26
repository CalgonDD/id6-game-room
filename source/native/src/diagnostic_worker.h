#ifndef DIAGNOSTIC_WORKER_H
#define DIAGNOSTIC_WORKER_H

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

// An empty saved address means discovery is required; never guess an endpoint.
static inline bool installed_gateway(const char* worker, char (&host)[16]) {
    char path[192];
    const char* last = strrchr(worker, '/');
    if (!last) return false;
    int length = snprintf(path, sizeof path, "%.*s/gateway-ip.txt",
                          static_cast<int>(last-worker), worker);
    if (length < 0 || static_cast<size_t>(length) >= sizeof path) return false;
    host[0]=0;
    FILE* file = fopen(path, "r");
    if (!file) return true;
    char line[32];
    bool valid = fgets(line, sizeof line, file) != NULL && fgetc(file) == EOF;
    fclose(file);
    if (!valid) return true;
    line[strcspn(line, "\r\n")] = 0;
    struct in_addr address;
    if (inet_pton(AF_INET, line, &address) != 1 ||
        (ntohl(address.s_addr) >> 16) != 0xa9feu ||
        ((ntohl(address.s_addr) >> 8) & 255u)==0u ||
        ((ntohl(address.s_addr) >> 8) & 255u)==255u) return true;
    strcpy(host, line);
    return true;
}

static inline pid_t launch(const char *const *args,int output,bool own_session=false) {
    pid_t child=-1;
    // /opt/sys/lib holds car libraries our programs and LayerManagerControl load at run time.
    char library_path[] = "LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib";
    char *environment[] = {library_path, 0};
    int error;
#ifdef __QNX__
    // Explicitly pass only stdin, stdout and stderr to the diagnostic process.
    int null = open("/dev/null", O_RDWR);
    error = null < 0 ? errno : 0;
    if (!error) {
        int descriptors[] = {null, output < 0 ? null : output, null};
        struct inheritance flags;
        memset(&flags, 0, sizeof flags);
        flags.flags = SPAWN_SETSIGDEF | SPAWN_SETSIGMASK | SPAWN_NOZOMBIE;
        if (own_session) flags.flags |= SPAWN_SETSID;
        sigfillset(&flags.sigdefault);
        sigemptyset(&flags.sigmask);
        child = spawn(args[0], 3, descriptors, &flags,
                           const_cast<char *const *>(args), environment);
        if (child < 0) error = errno;
        close(null);
    }
#else
    (void)own_session; // host lifecycle tests control the sink explicitly
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (!error) {
            if (output < 0) error=posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
            else error=posix_spawn_file_actions_adddup2(&actions,output,1);
        }
        if (!error) error = posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
        if (!error)
            error = posix_spawn(&child, args[0], &actions, 0,
                                const_cast<char *const *>(args), environment);
        posix_spawn_file_actions_destroy(&actions);
    }
#endif
    return error ? -1 : child;
}


// QNX workers have no exit status to collect (SPAWN_NOZOMBIE). Their pid stays
// owned until the process actually disappears. Host tests use ordinary children.
static inline void reap_worker(pid_t& child) {
    if (child<=0) return;
#ifdef __QNX__
    if (kill(child,0)<0 && errno==ESRCH) child=-1;
#else
    pid_t ended=waitpid(child,0,WNOHANG);
    if (ended==child || (ended<0 && errno==ECHILD)) child=-1;
#endif
}

#endif
