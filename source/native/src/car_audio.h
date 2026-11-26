/* car_audio.h: a game's sound on the car, the route Freedoom proved (2026-11-19/20): the head unit's browser
 * audio source (`audioconn -b 0xb8`, an interrupting source: the radio pauses, and comes back on release) and
 * the car's libasound front end (dlopen), 44100 Hz 16-bit stereo, 4096-byte fragments x5, block mode.
 * One thread at CA_PRIORITY asks the game for 1024 frames at a time (ca_fill) and writes them. If the source
 * or the device can't be had, the game is simply silent. Logs "GameRoom show sound ...".
 * Needs gr_log.h (mps_log, mps_sleep_ms) included first. Header-only: include it in one file. */
#ifndef CAR_AUDIO_H
#define CAR_AUDIO_H

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __QNX__
#include <sys/asoundlib.h>   /* types and constants only: every function comes from the dlopened front end */
#endif

#ifndef CA_PRIORITY
#define CA_PRIORITY 10
#endif
#define CA_CONN "0xb8"
#define CA_FRAMES 1024          /* one 4096-byte fragment of 16-bit stereo */

typedef void (*ca_fill_fn)(int16_t *stereo, unsigned frames);

static ca_fill_fn ca_fill;
static pthread_t ca_thread_id;
static volatile int ca_running = 0, ca_started = 0, ca_done = 0;
static unsigned ca_loud_blocks = 0;

static const char *ca_audioconn(void) {
    static const char *const paths[] = {"/fs/sda0/opt/mm/bin/audioconn", "/opt/mm/bin/audioconn"};
    unsigned i;
    for (i = 0; i < sizeof paths / sizeof paths[0]; ++i)
        if (access(paths[i], X_OK) == 0) return paths[i];
    return 0;
}

static int ca_route(char op) {
    const char *tool = ca_audioconn();
    char cmd[256], line[160];
    FILE *p;
    int rc;
    if (!tool) {
        mps_log("sound route_%c skipped: audioconn not found", op);
        return -1;
    }
    snprintf(cmd, sizeof cmd, "LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib:/fs/sda0/opt/mm/lib %s -%c "
             CA_CONN " 2>&1", tool, op);
    p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof line, p)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) mps_log("sound audioconn -%c: %.120s", op, line);
    }
    rc = pclose(p);
    mps_log("sound route_%s conn=" CA_CONN " rc=%d", op == 'b' ? "build" : "release", rc);
    return rc;
}

#ifdef __QNX__
typedef int (*CaOpenPreferred)(snd_pcm_t **, int *, int *, int);
typedef int (*CaClose)(snd_pcm_t *);
typedef int (*CaInfo)(snd_pcm_t *, snd_pcm_channel_info_t *);
typedef int (*CaParams)(snd_pcm_t *, snd_pcm_channel_params_t *);
typedef int (*CaSetup)(snd_pcm_t *, snd_pcm_channel_setup_t *);
typedef int (*CaStatus)(snd_pcm_t *, snd_pcm_channel_status_t *);
typedef int (*CaPrepare)(snd_pcm_t *, int);
typedef ssize_t (*CaWrite)(snd_pcm_t *, const void *, size_t);
typedef int (*CaFlush)(snd_pcm_t *, int);
typedef const char *(*CaStrerror)(int);

static struct {
    CaOpenPreferred open_preferred;
    CaClose close;
    CaInfo info;
    CaParams params;
    CaSetup setup;
    CaStatus status;
    CaPrepare prepare;
    CaWrite write;
    CaFlush flush;
    CaStrerror strerror;
} ca_as;

static int ca_load(void) {
    static const char *const paths[] = {"/opt/mm/lib/libasound/libasound.so", "/fs/sda0/opt/mm/lib/libasound/libasound.so",
                                        "libasound.so.2", "libasound.so"};
    void *lib = 0;
    unsigned i;
    int missing = 0;
    for (i = 0; i < sizeof paths / sizeof paths[0] && !lib; ++i) {
        lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (lib) mps_log("sound library=%s", paths[i]);
    }
    if (!lib) {
        const char *why = dlerror();
        mps_log("sound fail step=library error=%.150s", why ? why : "?");
        return 0;
    }
#define CA_BIND(field, name) do { void *f_ = dlsym(lib, name); if (!f_) { mps_log("sound fail step=symbol name=%s", name); ++missing; } memcpy(&ca_as.field, &f_, sizeof f_); } while (0)
    CA_BIND(open_preferred, "snd_pcm_open_preferred");
    CA_BIND(close, "snd_pcm_close");
    CA_BIND(info, "snd_pcm_plugin_info");
    CA_BIND(params, "snd_pcm_plugin_params");
    CA_BIND(setup, "snd_pcm_plugin_setup");
    CA_BIND(status, "snd_pcm_plugin_status");
    CA_BIND(prepare, "snd_pcm_plugin_prepare");
    CA_BIND(write, "snd_pcm_plugin_write");
    CA_BIND(flush, "snd_pcm_plugin_flush");
    CA_BIND(strerror, "snd_strerror");
#undef CA_BIND
    return missing == 0;
}

static snd_pcm_t *ca_open(unsigned *rate_out) {
    snd_pcm_t *h = 0;
    snd_pcm_channel_info_t info;
    snd_pcm_channel_params_t p;
    snd_pcm_channel_setup_t su;
    int card = -1, dev = -1, rc;
    unsigned rate = 44100;
    rc = ca_as.open_preferred(&h, &card, &dev, SND_PCM_OPEN_PLAYBACK);
    if (rc < 0 || !h) {
        mps_log("sound fail step=open rc=%d %s", rc, ca_as.strerror ? ca_as.strerror(rc) : "");
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (ca_as.info(h, &info) == 0) {
        mps_log("sound device card=%d dev=%d rates=%d..%d fragment=%d..%d", card, dev, info.min_rate, info.max_rate,
                info.min_fragment_size, info.max_fragment_size);
        if (info.max_rate > 0 && (int)rate > info.max_rate) rate = (unsigned)info.max_rate;
    }
    memset(&p, 0, sizeof p);
    p.channel = SND_PCM_CHANNEL_PLAYBACK;
    p.mode = SND_PCM_MODE_BLOCK;
    p.start_mode = SND_PCM_START_FULL;
    p.stop_mode = SND_PCM_STOP_STOP;
    p.buf.block.frag_size = CA_FRAMES * 4;
    p.buf.block.frags_min = 1;
    p.buf.block.frags_max = 5;
    p.format.interleave = 1;
    p.format.format = SND_PCM_SFMT_S16_LE;
    p.format.rate = (int32_t)rate;
    p.format.voices = 2;
    rc = ca_as.params(h, &p);
    if (rc < 0) {
        mps_log("sound fail step=params rc=%d %s rate=%u", rc, ca_as.strerror ? ca_as.strerror(rc) : "", rate);
        ca_as.close(h);
        return 0;
    }
    memset(&su, 0, sizeof su);
    su.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (ca_as.setup(h, &su) == 0) {
        if (su.format.rate > 0) rate = (unsigned)su.format.rate;
        mps_log("sound setup rate=%d voices=%d fragment=%d x%d", su.format.rate, su.format.voices,
                su.buf.block.frag_size, su.buf.block.frags);
    }
    rc = ca_as.prepare(h, SND_PCM_CHANNEL_PLAYBACK);
    if (rc < 0) {
        mps_log("sound fail step=prepare rc=%d", rc);
        ca_as.close(h);
        return 0;
    }
    *rate_out = rate;
    return h;
}

static int ca_write(snd_pcm_t *h, const int16_t *buf, size_t bytes, unsigned *prepares) {
    const uint8_t *p = (const uint8_t *)buf;
    int attempt;
    for (attempt = 0; bytes > 0 && attempt < 3; ++attempt) {
        ssize_t n = ca_as.write(h, p, bytes);
        snd_pcm_channel_status_t st;
        if (n > 0) {
            p += n;
            bytes -= (size_t)n;
            if (!bytes) break;
        }
        memset(&st, 0, sizeof st);
        st.channel = SND_PCM_CHANNEL_PLAYBACK;
        if (ca_as.status(h, &st) == 0 && (st.status == SND_PCM_STATUS_UNDERRUN || st.status == SND_PCM_STATUS_READY)) {
            ca_as.prepare(h, SND_PCM_CHANNEL_PLAYBACK);
            ++*prepares;
        } else if (n < 0) {
            return 0;
        }
    }
    return bytes == 0;
}
#endif

static void *ca_thread(void *arg) {
    static int16_t block[CA_FRAMES * 2];
    (void)arg;
#ifdef __QNX__
    {
        struct sched_param sp;
        int policy;
        if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
            sp.sched_priority = CA_PRIORITY;
            pthread_setschedparam(pthread_self(), policy, &sp);
        }
    }
    {
        snd_pcm_t *h = 0;
        unsigned rate = 44100, blocks = 0, prepares = 0, errors = 0;
        int routed = ca_route('b') == 0;
        if (ca_load()) h = ca_open(&rate);
        mps_log("sound ready routed=%d device=%s rate=%u", routed, h ? "open" : "none", rate);
        while (ca_running && h) {
            unsigned k, loud = 0;
            ca_fill(block, CA_FRAMES);
            for (k = 0; k < CA_FRAMES * 2u && !loud; ++k) loud = block[k] != 0;
            ca_loud_blocks += loud;
            if (!ca_write(h, block, sizeof block, &prepares)) {
                if (++errors >= 50) {
                    mps_log("sound stop reason=write_errors");
                    break;
                }
                mps_sleep_ms(20);
            }
            ++blocks;
        }
        if (h) {
            ca_as.flush(h, SND_PCM_CHANNEL_PLAYBACK);
            ca_as.close(h);
        }
        mps_log("sound end blocks=%u seconds=%u prepares=%u write_errors=%u loud_blocks=%u", blocks,
                rate ? (unsigned)((uint64_t)blocks * CA_FRAMES / rate) : 0u, prepares, errors, ca_loud_blocks);
        if (routed) ca_route('r');
    }
#else
    /* Host build: no device; ask the game in real time anyway and count the blocks that carry sound (tests). */
    mps_log("sound ready routed=0 device=none (host build)");
    while (ca_running) {
        unsigned k, loud = 0;
        ca_fill(block, CA_FRAMES);
        for (k = 0; k < CA_FRAMES * 2u && !loud; ++k) loud = block[k] != 0;
        ca_loud_blocks += loud;
        mps_sleep_ms(23);
    }
#endif
    ca_done = 1;
    return 0;
}

static void ca_start(ca_fill_fn fill) {
    ca_fill = fill;
    ca_running = 1;
    ca_done = 0;
    ca_started = pthread_create(&ca_thread_id, 0, ca_thread, 0) == 0;
    if (!ca_started) mps_log("sound fail step=thread");
}

/* Stops the thread (which closes the device and gives the source back). At most 3 s; if the thread is still
 * inside `audioconn -b`, the source is released directly so the radio never stays muted. */
static void ca_stop(void) {
    unsigned waited = 0;
    if (!ca_started) return;
    ca_running = 0;
    while (!ca_done && waited < 3000u) {
        mps_sleep_ms(20);
        waited += 20u;
    }
    if (ca_done) pthread_join(ca_thread_id, 0);
    else {
        mps_log("sound stop_timeout: releasing the source directly");
        ca_route('r');
    }
    ca_started = 0;
}

static void ca_release_source(void) { ca_route('r'); }

#endif
