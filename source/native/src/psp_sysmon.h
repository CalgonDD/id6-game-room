/* ID6 PSP v30 (2026-11-24): system CPU monitor and in-run priority A/B for the PSP supervisor.
 *
 * Every 10 s while the game is attached:
 *   QNX_SYS   wall_ms idle_ms busy_ms and the top 8 processes by CPU time used in the window (name(pid)=ms).
 *             idle = procnto's priority-0 (idle) threads; busy = everything else. Both cores: 20000 ms per 10 s.
 *   QNX_THR   the game process's threads that used >= 20 ms: tid:priority:policy:cpu_ms.
 *   QNX_PHASE the current priority phase.
 * v31: the priority A/B below is compiled out (SYSMON_PRIORITY_AB=0; v30 showed no effect).
 * Priority A/B: the game process's threads that run at the base priority (8) are switched to 10 (equal to the
 * HMI's tHMI_App, switched to round-robin so they time-slice with it, never above it) for 60 s, then back to 8 for 60 s, alternating. Phase A
 * (base) first. Every switch is logged with the number of threads changed. Threads at any other priority (the
 * car audio device thread) are never touched. Read-only otherwise: /proc is only queried.
 */
#ifndef PSP_SYSMON_H
#define PSP_SYSMON_H
#ifdef __QNX__
#include <dirent.h>
#include <devctl.h>
#include <sys/procfs.h>
#include <sys/neutrino.h>

#define SYSMON_WINDOW_MS 10000u
#define SYSMON_PHASE_MS 60000u
#define SYSMON_BASE_PRIO 8
#define SYSMON_BOOST_PRIO 10
#define SYSMON_MAX_PROC 320
#define SYSMON_MAX_BOOST 64
#ifndef SYSMON_PRIORITY_AB
#define SYSMON_PRIORITY_AB 0
#endif

typedef struct { int pid, seen; uint64_t ns; char name[32]; } sysmon_proc;
static sysmon_proc sysmon_prev[SYSMON_MAX_PROC];
static int sysmon_count;
static uint64_t sysmon_last_ms, sysmon_phase_ms, sysmon_idle_prev, sysmon_thr_prev_ns[SYSMON_MAX_BOOST];
static int sysmon_thr_prev_tid[SYSMON_MAX_BOOST], sysmon_thr_n;
static int sysmon_phase = -1, sysmon_phase_n, sysmon_boosted[SYSMON_MAX_BOOST], sysmon_boosted_policy[SYSMON_MAX_BOOST], sysmon_boosted_n;

static int sysmon_open(int pid) {
    char path[48];
    snprintf(path, sizeof path, "/proc/%d/as", pid);
    return open(path, O_RDONLY);
}

static void sysmon_name(int fd, char *name, size_t len) {
    struct { procfs_debuginfo info; char buff[256]; } map;
    const char *base;
    memset(&map, 0, sizeof map);
    name[0] = 0;
    if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof map, 0) != EOK) return;
    map.buff[sizeof map.buff - 1] = 0;
    base = strrchr(map.info.path, '/');
    snprintf(name, len, "%s", base ? base + 1 : map.info.path);
}

/* Sum of the CPU time of pid 1's priority-0 threads (the per-core idle threads). */
static uint64_t sysmon_idle_ns(void) {
    procfs_status st;
    uint64_t sum = 0;
    int fd = sysmon_open(1), tid = 1;
    if (fd < 0) return 0;
    for (;;) {
        memset(&st, 0, sizeof st);
        st.tid = tid;
        if (devctl(fd, DCMD_PROC_TIDSTATUS, &st, sizeof st, 0) != EOK) break;
        if (st.priority == 0) sum += st.sutime;
        tid = st.tid + 1;
    }
    close(fd);
    return sum;
}

static sysmon_proc *sysmon_find(int pid) {
    int i;
    for (i = 0; i < sysmon_count; ++i) if (sysmon_prev[i].pid == pid) return &sysmon_prev[i];
    if (sysmon_count >= SYSMON_MAX_PROC) return 0;
    memset(&sysmon_prev[sysmon_count], 0, sizeof sysmon_prev[0]);
    sysmon_prev[sysmon_count].pid = pid;
    sysmon_prev[sysmon_count].ns = ~(uint64_t)0;  /* first sight: no delta yet */
    return &sysmon_prev[sysmon_count++];
}

static void sysmon_processes(unsigned wall_ms) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int top_i[8], i, j, k;
    uint64_t top_ms[8], busy = 0, idle_now, idle_ms = 0;
    char line[300];
    size_t used;
    for (i = 0; i < 8; ++i) { top_i[i] = -1; top_ms[i] = 0; }
    for (i = 0; i < sysmon_count; ++i) sysmon_prev[i].seen = 0;
    if (!d) { mps_log("psp QNX_SYS proc_unavailable errno=%d", errno); return; }
    while ((e = readdir(d)) != 0) {
        procfs_info info;
        sysmon_proc *p;
        int pid, fd;
        uint64_t ns, delta;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid = atoi(e->d_name);
        if (pid <= 0) continue;
        fd = sysmon_open(pid);
        if (fd < 0) continue;
        memset(&info, 0, sizeof info);
        if (devctl(fd, DCMD_PROC_INFO, &info, sizeof info, 0) != EOK) { close(fd); continue; }
        p = sysmon_find(pid);
        if (!p) { close(fd); continue; }
        if (!p->name[0]) sysmon_name(fd, p->name, sizeof p->name);
        close(fd);
        ns = info.utime + info.stime;
        p->seen = 1;
        if (p->ns == ~(uint64_t)0 || ns < p->ns) { p->ns = ns; continue; }
        delta = (ns - p->ns) / 1000000u;
        p->ns = ns;
        if (pid == 1) continue;  /* procnto: its time includes the idle threads, reported separately */
        busy += delta;
        for (j = 0; j < 8; ++j) if (top_i[j] < 0 || delta > top_ms[j]) break;
        if (j < 8) {
            for (k = 7; k > j; --k) { top_i[k] = top_i[k - 1]; top_ms[k] = top_ms[k - 1]; }
            top_i[j] = (int)(p - sysmon_prev); top_ms[j] = delta;
        }
    }
    closedir(d);
    /* forget processes that are gone, so the table never fills up */
    for (i = 0, j = 0; i < sysmon_count; ++i) {
        if (!sysmon_prev[i].seen) continue;
        if (i != j) {
            for (k = 0; k < 8; ++k) if (top_i[k] == i) top_i[k] = j;
            sysmon_prev[j] = sysmon_prev[i];
        }
        ++j;
    }
    sysmon_count = j;
    idle_now = sysmon_idle_ns();
    if (sysmon_idle_prev && idle_now >= sysmon_idle_prev) idle_ms = (idle_now - sysmon_idle_prev) / 1000000u;
    sysmon_idle_prev = idle_now;
    used = (size_t)snprintf(line, sizeof line, "psp QNX_SYS wall_ms=%u idle_ms=%llu busy_ms=%llu top=", wall_ms,
        (unsigned long long)idle_ms, (unsigned long long)busy);
    for (i = 0; i < 8 && top_i[i] >= 0 && used < sizeof line - 48; ++i)
        used += (size_t)snprintf(line + used, sizeof line - used, "%s%s(%d)=%llu", i ? "," : "",
            sysmon_prev[top_i[i]].name[0] ? sysmon_prev[top_i[i]].name : "?", sysmon_prev[top_i[i]].pid,
            (unsigned long long)top_ms[i]);
    mps_log("%s", line);
}

/* Visit the game's threads: log CPU use; mode 1 raises base-priority threads to the boost priority and records
 * them, mode 2 returns exactly the recorded threads (still at the boost priority) to the base priority. */
static int sysmon_threads(pid_t game, int log, int mode) {
    procfs_status st;
    char line[300];
    size_t used;
    int fd = sysmon_open((int)game), tid = 1, changed = 0, shown = 0, n = 0, i;
    if (fd < 0) return 0;
    used = (size_t)snprintf(line, sizeof line, "psp QNX_THR pid=%d", (int)game);
    for (;;) {
        memset(&st, 0, sizeof st);
        st.tid = tid;
        if (devctl(fd, DCMD_PROC_TIDSTATUS, &st, sizeof st, 0) != EOK) break;
        tid = st.tid + 1;
        if (log) {
            uint64_t prev = 0, ms;
            for (i = 0; i < sysmon_thr_n; ++i) if (sysmon_thr_prev_tid[i] == (int)st.tid) { prev = sysmon_thr_prev_ns[i]; break; }
            ms = st.sutime >= prev ? (st.sutime - prev) / 1000000u : 0;
            if (n < SYSMON_MAX_BOOST) { sysmon_thr_prev_tid[n] = (int)st.tid; sysmon_thr_prev_ns[n] = st.sutime; ++n; }
            if (ms >= 20 && shown < 12 && used < sizeof line - 32) {
                used += (size_t)snprintf(line + used, sizeof line - used, " %d:%d:%d:%llu", (int)st.tid, (int)st.priority,
                    (int)st.policy, (unsigned long long)ms);
                ++shown;
            }
        }
        if (mode == 1 && st.real_priority == SYSMON_BASE_PRIO && sysmon_boosted_n < SYSMON_MAX_BOOST) {
            struct sched_param sp;
            memset(&sp, 0, sizeof sp);
            sp.sched_priority = SYSMON_BOOST_PRIO;
            /* Round-robin while boosted, so at the HMI's priority the game time-slices with it instead of
             * holding a core until it blocks (FIFO). The original policy is restored with the base priority. */
            if (SchedSet(game, (int)st.tid, SCHED_RR, &sp) == EOK) {
                sysmon_boosted_policy[sysmon_boosted_n] = st.policy;
                sysmon_boosted[sysmon_boosted_n++] = (int)st.tid;
                ++changed;
            }
        } else if (mode == 2 && st.real_priority == SYSMON_BOOST_PRIO) {
            for (i = 0; i < sysmon_boosted_n; ++i) if (sysmon_boosted[i] == (int)st.tid) break;
            if (i < sysmon_boosted_n) {
                struct sched_param sp;
                memset(&sp, 0, sizeof sp);
                sp.sched_priority = SYSMON_BASE_PRIO;
                if (SchedSet(game, (int)st.tid, sysmon_boosted_policy[i] ? sysmon_boosted_policy[i] : SCHED_NOCHANGE, &sp) == EOK) ++changed;
            }
        }
    }
    close(fd);
    if (log) { sysmon_thr_n = n; mps_log("%s", line); }
    return changed;
}

/* Call often from the supervisor loop once the game is attached. */
static void sysmon_tick(pid_t game) {
    uint64_t now = now_ms();
    if (sysmon_phase < 0) {
        sysmon_phase = 0; sysmon_phase_n = 1; sysmon_phase_ms = now; sysmon_last_ms = now;
        sysmon_processes(0);
        (void)sysmon_threads(game, 1, 0);
        return;
    }
    /* v31: the v30 priority A/B is off (no effect: A 61%, B 59%); the boost code stays for a later test. */
#if SYSMON_PRIORITY_AB
    if (now - sysmon_phase_ms >= SYSMON_PHASE_MS) {
        int changed;
        sysmon_phase = !sysmon_phase; ++sysmon_phase_n; sysmon_phase_ms = now;
        if (sysmon_phase) {
            sysmon_boosted_n = 0;
            changed = sysmon_threads(game, 0, 1);
        } else {
            changed = sysmon_threads(game, 0, 2);
            sysmon_boosted_n = 0;
        }
        mps_log("psp QNX_PHASE n=%d phase=%s prio=%d changed=%d", sysmon_phase_n, sysmon_phase ? "B" : "A",
            sysmon_phase ? SYSMON_BOOST_PRIO : SYSMON_BASE_PRIO, changed);
    }
#endif
    if (now - sysmon_last_ms >= SYSMON_WINDOW_MS) {
        unsigned wall = (unsigned)(now - sysmon_last_ms);
        sysmon_last_ms = now;
        sysmon_processes(wall);
        (void)sysmon_threads(game, 1, 0);
    }
}
#else
static void sysmon_tick(pid_t game) { (void)game; }
#endif
#endif
