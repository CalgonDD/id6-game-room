/* gr_log.h: time helpers and the "GameRoom show ..." trace lines, shared by every Game Room program. */
#ifndef GR_LOG_H
#define GR_LOG_H

#include <arpa/inet.h>
#include <errno.h>
#ifdef RETRO_CORE_PS1
#include <fcntl.h>
#endif
#include <signal.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_protocol.h"

static volatile sig_atomic_t mps_stop_flag;

__attribute__((unused)) static uint64_t mps_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

__attribute__((unused)) static void mps_sleep_ms(unsigned ms) {
    struct timespec t;
    t.tv_sec = ms / 1000u;
    t.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&t, &t) < 0 && errno == EINTR && !mps_stop_flag) {}
}

/* ---- trace: same line shape as the plugins' trace_event, sent to the shared writer ---- */
static int mps_trace_fd = -1;
static void mps_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mps_log(const char *fmt, ...) {
    char text[HMI_TRACE_PACKET - 96], line[HMI_TRACE_PACKET];
    struct timespec wall;
    struct sockaddr_in a;
    va_list ap;
    int n;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    clock_gettime(CLOCK_REALTIME, &wall);
    n = snprintf(line, sizeof line, "wall=%lld.%03ld mono=%llu pid=%ld GameRoom show %s\n",
                 (long long)wall.tv_sec, (long)(wall.tv_nsec / 1000000), (unsigned long long)mps_now_ms(),
                 (long)getpid(), text);
    if (n <= 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    if (mps_trace_fd < 0) {
        mps_trace_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (mps_trace_fd < 0) return;
#ifdef RETRO_CORE_PS1
        /* A diagnostic UDP receiver must not stall game/audio progress. */
        { int flags=fcntl(mps_trace_fd,F_GETFL,0);
          if(flags<0 || fcntl(mps_trace_fd,F_SETFL,flags|O_NONBLOCK)<0) {
              close(mps_trace_fd); mps_trace_fd=-1; return;
          }
        }
#endif
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(HMI_TRACE_PORT);
    sendto(mps_trace_fd, line, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}

/* Logs the interesting lines of LayerManagerControl output, one trace line each (at most 8). */
__attribute__((unused)) static void mps_log_output(const char *what, const char *out) {
    unsigned lines = 0;
    const char *p = out;
    while (*p && lines < 8) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (*p == ' ' || *p == '\t')) { ++p; --len; }
        if (len) {
            mps_log("%s: %.*s", what, (int)(len > 200 ? 200 : len), p);
            ++lines;
        }
        if (!e) break;
        p = e + 1;
    }
}

#endif
