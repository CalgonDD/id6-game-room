// Freeze rescue (Diagnostics/Crash_analysis_work/FREEZE_MITIGATION_PROPOSAL_2026-11-18.md, 3C).
//
// The 03:38 freeze: one of our updateModel() calls blocked its bmw::Looper in a condvar wait for a reply,
// the three other Loopers (priority 11) spun on both CPUs, and tHMI_App (priority 10) - presumably the
// thread that would send that reply - never ran again until the watchdog killed the HMI. A condvar wait
// lends no priority, so it never recovers on its own.
//
// This watcher thread does nothing unless one of OUR writes has been in flight for kRescueStuckMs. Then it
// raises tHMI_App to kRescueBoostPriority (above the Loopers) until that write returns or kRescueBoostMs
// passes, and puts tHMI_App's own policy and priority back - always. One rescue per stuck write. Every
// step goes to the HMI trace ("rescue_*" events), including refusals (EPERM etc.), so a pull shows whether
// the boost let the stuck write return - which is also the test of the starvation theory.
//
// Every plugin has its own copy (built with -DHMI_RESCUE; debug_trace.h wraps every updateModel). State is
// per DSO, so the four watchers cooperate only through tHMI_App's priority itself:
//  - tHMI_App already raised (another app is rescuing): wait, do not give up on our stuck write.
//  - our write still stuck but tHMI_App back down (another app restored it): raise it again.
//  - restore only ever writes back a priority read below the boost level, so it can never stick high.
// Host tests define HMI_RESCUE_TEST and supply rescue_find_app/rescue_get/rescue_set/trace_event.
#ifndef HMI_RESCUE_H
#define HMI_RESCUE_H
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef EOK
#define EOK 0
#endif
#ifndef HMI_RESCUE_APP
#define HMI_RESCUE_APP "GameRoom"   // trace tag and thread name prefix
#endif

namespace {
// Car test 2026-11-18 15:29 (FREEZE_AB_TEST_RESULTS): 324 writes took 0-32 ms, 95 hung until boosted and
// none fell in between, so anything past 50 ms is stuck. 300 ms here made the page visibly lag.
const unsigned kRescueStuckMs=50u;
const unsigned kRescueBoostMs=1000u;    // longest tHMI_App stays raised
const unsigned kRescuePollMs=10u;       // watcher's idle period (a mutex check, nothing else)
const unsigned kRescueWaitMs=5u;        // watcher's period while boosting
const int kRescueWatcherPriority=13;    // must be above the priority-11 Loopers, or it never runs mid-freeze
const int kRescueBoostPriority=12;      // tHMI_App while raised: above the Loopers (= tHMI_Timer, LMGR receive)

struct HmiRescue {
    pthread_mutex_t lock;       // guards everything below; only ever held for a few instructions
    unsigned in_flight;         // our updateModel calls running now (0 or 1 in practice)
    unsigned write_seq;         // +1 per write started: tells one stuck write from the next
    uint32_t started_ms;        // when the current write started (low 32 bits of CLOCK_MONOTONIC ms)
    unsigned rescued_seq;       // write already rescued (one rescue per stuck write)
    unsigned waited_seq;        // write already logged as waiting for another app's boost
    int stop;
    int running;
    pthread_t watcher;
    int app_tid;                // tHMI_App's tid once found (QNX pthread_t is the tid)
};
HmiRescue hmi_rescue={PTHREAD_MUTEX_INITIALIZER,0u,0u,0u,0u,0u,0,0,pthread_t(),0};

static uint32_t rescue_now_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return static_cast<uint32_t>(static_cast<uint64_t>(t.tv_sec)*1000u+t.tv_nsec/1000000u);
}
static void rescue_sleep_ms(unsigned ms) {
    struct timespec nap;
    nap.tv_sec=ms/1000u;
    nap.tv_nsec=static_cast<long>(ms%1000u)*1000000L;
    nanosleep(&nap,0);
}

#if !defined(HMI_RESCUE_TEST) && !defined(__QNX__)
// Host builds of the plugin (tests/test_plugin_state.cpp): no thread of this process is called tHMI_App
// and pthread_t is not a tid, so the lookup always fails and nothing is ever re-prioritised.
static int rescue_find_app(int*) { return ENOSYS; }
static int rescue_get(int,int*,struct sched_param*) { return ENOSYS; }
static int rescue_set(int,int,const struct sched_param*) { return ENOSYS; }
#elif !defined(HMI_RESCUE_TEST)
// QNX: pthread_t is the thread ID, tids are small and dense (HmiMain3D has 30). pidin's thread names are
// the ones pthread_getname_np() returns. Re-checks a cached tid's name before trusting it.
static int rescue_find_app(int* tid) {
    char name[64];
    if (hmi_rescue.app_tid>0 && pthread_getname_np(hmi_rescue.app_tid,name,sizeof name)==EOK &&
        strcmp(name,"tHMI_App")==0) { *tid=hmi_rescue.app_tid; return EOK; }
    for (int t=1;t<256;++t) {
        if (pthread_getname_np(t,name,sizeof name)==EOK && strcmp(name,"tHMI_App")==0) {
            hmi_rescue.app_tid=t; *tid=t; return EOK;
        }
    }
    return ESRCH;
}
static int rescue_get(int tid,int* policy,struct sched_param* param) { return pthread_getschedparam(tid,policy,param); }
static int rescue_set(int tid,int policy,const struct sched_param* param) { return pthread_setschedparam(tid,policy,param); }
#endif

static void rescue_log(const char* text) { trace_event(HMI_RESCUE_APP,text); }   // non-blocking UDP send

// Around every updateModel() of ours. Cheap: an uncontended mutex. Not nested.
static inline void rescue_write_begin() {
    pthread_mutex_lock(&hmi_rescue.lock);
    ++hmi_rescue.in_flight;
    ++hmi_rescue.write_seq;
    hmi_rescue.started_ms=rescue_now_ms();
    pthread_mutex_unlock(&hmi_rescue.lock);
}
static inline void rescue_write_end() {
    pthread_mutex_lock(&hmi_rescue.lock);
    if (hmi_rescue.in_flight) --hmi_rescue.in_flight;
    pthread_mutex_unlock(&hmi_rescue.lock);
}

// Is write `seq` still running?
static bool rescue_still_stuck(unsigned seq) {
    pthread_mutex_lock(&hmi_rescue.lock);
    const bool stuck=hmi_rescue.in_flight && hmi_rescue.write_seq==seq;
    pthread_mutex_unlock(&hmi_rescue.lock);
    return stuck;
}

// Returns false only when tHMI_App is already raised by another app: the caller tries again next poll.
static bool rescue_once(unsigned seq,uint32_t stuck_ms) {
    char line[160];
    int tid=0;
    int err=rescue_find_app(&tid);
    if (err!=EOK) {
        snprintf(line,sizeof line,"rescue_refused stage=find_tHMI_App err=%d stuck_ms=%u",err,stuck_ms);
        rescue_log(line);
        return true;
    }
    int policy=0;
    struct sched_param original;
    memset(&original,0,sizeof original);
    err=rescue_get(tid,&policy,&original);
    if (err!=EOK) {
        snprintf(line,sizeof line,"rescue_refused stage=getschedparam tid=%d err=%d stuck_ms=%u",tid,err,stuck_ms);
        rescue_log(line);
        return true;
    }
    if (original.sched_priority>=kRescueBoostPriority) {   // another app is boosting: keep watching
        pthread_mutex_lock(&hmi_rescue.lock);
        const bool first=hmi_rescue.waited_seq!=seq;
        hmi_rescue.waited_seq=seq;
        pthread_mutex_unlock(&hmi_rescue.lock);
        if (first) {
            snprintf(line,sizeof line,"rescue_waiting tid=%d prio=%d already_high stuck_ms=%u",
                     tid,original.sched_priority,stuck_ms);
            rescue_log(line);
        }
        return false;
    }
    struct sched_param raised=original;   // keeps every other field (QNX: sched_curpriority, ss_*)
    raised.sched_priority=kRescueBoostPriority;
    err=rescue_set(tid,policy,&raised);
    if (err!=EOK) {   // EPERM: the HMI may not change scheduling; the pull will say so
        snprintf(line,sizeof line,"rescue_refused stage=setschedparam tid=%d prio=%d->%d err=%d%s stuck_ms=%u",
                 tid,original.sched_priority,kRescueBoostPriority,err,err==EPERM ? " (EPERM)" : "",stuck_ms);
        rescue_log(line);
        return true;
    }
    snprintf(line,sizeof line,"rescue_boost tid=%d prio=%d->%d policy=%d stuck_ms=%u",
             tid,original.sched_priority,kRescueBoostPriority,policy,stuck_ms);
    rescue_log(line);

    const uint32_t boosted_at=rescue_now_ms();
    bool returned=false;
    unsigned reraised=0;
    while (rescue_now_ms()-boosted_at<kRescueBoostMs) {
        if (!rescue_still_stuck(seq)) { returned=true; break; }
        // Another app's rescue may have put tHMI_App back while our write is still stuck.
        int now_policy=0;
        struct sched_param now;
        memset(&now,0,sizeof now);
        if (rescue_get(tid,&now_policy,&now)==EOK && now.sched_priority<kRescueBoostPriority &&
            rescue_set(tid,policy,&raised)==EOK) ++reraised;
        rescue_sleep_ms(kRescueWaitMs);
    }
    const uint32_t boosted_ms=rescue_now_ms()-boosted_at;

    // Strictly back to what it was, whatever happened above.
    const int restore_err=rescue_set(tid,policy,&original);
    int check_policy=0;
    struct sched_param check;
    memset(&check,0,sizeof check);
    const int check_err=rescue_get(tid,&check_policy,&check);
    // At the boost level right after our restore = another app re-raised it for its own stuck write.
    const bool restored=restore_err==EOK && check_err==EOK && check_policy==policy &&
                        (check.sched_priority==original.sched_priority ||
                         check.sched_priority==kRescueBoostPriority);
    snprintf(line,sizeof line,"rescue_end %s boosted_ms=%u restore=%s restore_err=%d prio_now=%d reraised=%u",
             returned ? "write_returned" : "still_stuck",boosted_ms,restored ? "ok" : "FAILED",
             restore_err,check_err==EOK ? check.sched_priority : -1,reraised);
    rescue_log(line);
    return true;
}

static void* rescue_main(void*) {
#if defined(__QNX__) && !defined(HMI_RESCUE_TEST)
    pthread_setname_np(pthread_self(),HMI_RESCUE_APP "_rescue");   // shows in pidin and watchdog dumps
#endif
    for (;;) {
        rescue_sleep_ms(kRescuePollMs);
        pthread_mutex_lock(&hmi_rescue.lock);
        const int stop=hmi_rescue.stop;
        const bool in_flight=hmi_rescue.in_flight!=0;
        const unsigned seq=hmi_rescue.write_seq;
        const uint32_t started=hmi_rescue.started_ms;
        const bool fresh=seq!=hmi_rescue.rescued_seq;
        pthread_mutex_unlock(&hmi_rescue.lock);
        if (stop) break;
        if (!in_flight || !fresh) continue;
        const uint32_t stuck_ms=rescue_now_ms()-started;
        if (stuck_ms<kRescueStuckMs) continue;
        if (!rescue_once(seq,stuck_ms)) continue;   // another app is boosting: look again next poll
        pthread_mutex_lock(&hmi_rescue.lock);
        hmi_rescue.rescued_seq=seq;
        pthread_mutex_unlock(&hmi_rescue.lock);
    }
    return 0;
}

// onInit(): start the watcher at kRescueWatcherPriority; if that is refused, still run it at the
// inherited priority (logged - it may then not get the CPU during a freeze).
static void rescue_start() {
    pthread_mutex_lock(&hmi_rescue.lock);
    const int running=hmi_rescue.running;
    hmi_rescue.stop=0;
    pthread_mutex_unlock(&hmi_rescue.lock);
    if (running) return;
    char line[96];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,16384);
    pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr,SCHED_RR);
    struct sched_param p;
    memset(&p,0,sizeof p);
    p.sched_priority=kRescueWatcherPriority;
    pthread_attr_setschedparam(&attr,&p);
    int err=pthread_create(&hmi_rescue.watcher,&attr,rescue_main,0);
    pthread_attr_destroy(&attr);
    if (err!=EOK) {
        snprintf(line,sizeof line,"rescue_watcher_priority_refused prio=%d err=%d",kRescueWatcherPriority,err);
        rescue_log(line);
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr,16384);
        err=pthread_create(&hmi_rescue.watcher,&attr,rescue_main,0);
        pthread_attr_destroy(&attr);
    }
    if (err!=EOK) {
        snprintf(line,sizeof line,"rescue_watcher_failed err=%d",err);
        rescue_log(line);
        return;
    }
    pthread_mutex_lock(&hmi_rescue.lock);
    hmi_rescue.running=1;
    pthread_mutex_unlock(&hmi_rescue.lock);
    rescue_log("rescue_watcher_started");
}

// onFinalize()/destructor, BEFORE anything else: the HMI may unmap this .so right after, and a thread
// still running its code would crash the HMI. Returns within kRescuePollMs (+kRescueBoostMs mid-rescue),
// and a rescue in progress always restores tHMI_App before the thread exits.
static void rescue_stop() {
    pthread_mutex_lock(&hmi_rescue.lock);
    const int running=hmi_rescue.running;
    hmi_rescue.stop=1;
    pthread_mutex_unlock(&hmi_rescue.lock);
    if (!running) return;
    pthread_join(hmi_rescue.watcher,0);
    pthread_mutex_lock(&hmi_rescue.lock);
    hmi_rescue.running=0;
    pthread_mutex_unlock(&hmi_rescue.lock);
    rescue_log("rescue_watcher_stopped");
}
}
#endif
