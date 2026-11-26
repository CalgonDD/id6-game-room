/* freedoom_sound.c: Freedoom's sound effects on the car (doomgeneric DG_sound_module; built with the engine's
 * flags and -DFEATURE_SOUND), and the audio thread for the music too: Doom's own FM-synth music (src/opl/:
 * Chocolate Doom's OPL music player and MIDI reader, driving the Nuked OPL3 chip emulator through fd_opl.c,
 * using the GENMIDI instrument bank in the WAD). The music is rendered into each block after the effects.
 *
 * Output, as NBT-AAD's "browser" audio mode does it (car-verified there; their code is not copied):
 *   - `audioconn -b 0xb8` builds the head unit's browser audio source. It is an interrupting source like the
 *     Owner's Manual audio: the radio/Bluetooth pauses while it is up. `audioconn -r 0xb8` gives it back.
 *   - The car's multi-client libasound front end, /opt/mm/lib/libasound/libasound.so (dlopen), feeds A2I:
 *     playback opened with snd_pcm_open_preferred, 16-bit stereo, 44100 Hz (the device reports 22050..44100),
 *     4096-byte fragments (A2I refuses any other size), block mode.
 * One audio thread (priority FDS_PRIORITY) mixes 1024 frames (about 23 ms) at a time with freedoom_mixer.h and
 * writes them; the engine's calls only touch the voice table under a mutex. If the source or the device can't be
 * had, the game simply stays silent. Everything is logged as "GameRoom show sound ...". */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "freedoom_mixer.h"
#include "opl/fd_opl.h"
#include "gr_log.h"

#ifdef __QNX__
#include <sys/asoundlib.h>   /* types and constants only: every function comes from the dlopened front end */
#endif

#ifndef FDS_PRIORITY
#define FDS_PRIORITY 10
#endif
#define FDS_CONN "0xb8"
#define FDS_FRAMES 1024          /* one 4096-byte fragment of 16-bit stereo */
#define FDS_MUSIC_GAIN 512       /* of 256: the music peaks around 6k as generated; doubled after the car test */
#define FDS_MUSIC_CPU_LIMIT 300  /* per mille of the playing time; above it the music is switched off */
#define FDS_MUSIC_CHECK 430      /* blocks (about 10 s) before the music's processor time is judged */

/* Used by i_sound.c / m_config.c when FEATURE_SOUND is on. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
void I_InitTimidityConfig(void) {}

static fdm_mixer mixer;
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static volatile int audio_running = 0, audio_started = 0, audio_done = 0;
static int use_prefix = 1;
static unsigned starts = 0, loud_blocks = 0, music_blocks = 0, peak = 0;

/* ---- the source (audioconn) ---- */
static const char *fds_audioconn(void) {
    static const char *const paths[] = {"/fs/sda0/opt/mm/bin/audioconn", "/opt/mm/bin/audioconn"};
    unsigned i;
    for (i = 0; i < sizeof paths / sizeof paths[0]; ++i)
        if (access(paths[i], X_OK) == 0) return paths[i];
    return 0;
}

static int fds_route(char op) {
    const char *tool = fds_audioconn();
    char cmd[256], line[160];
    FILE *p;
    int rc;
    if (!tool) {
        mps_log("sound route_%c skipped: audioconn not found", op);
        return -1;
    }
    /* its own library path: --restore runs without the game's re-exec */
    snprintf(cmd, sizeof cmd, "LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib:/fs/sda0/opt/mm/lib %s -%c "
             FDS_CONN " 2>&1", tool, op);
    p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof line, p)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) mps_log("sound audioconn -%c: %.120s", op, line);
    }
    rc = pclose(p);
    mps_log("sound route_%s conn=" FDS_CONN " rc=%d", op == 'b' ? "build" : "release", rc);
    return rc;
}

#ifdef __QNX__
/* ---- the device (libasound front end) ---- */
typedef int (*FnOpenPreferred)(snd_pcm_t **, int *, int *, int);
typedef int (*FnClose)(snd_pcm_t *);
typedef int (*FnInfo)(snd_pcm_t *, snd_pcm_channel_info_t *);
typedef int (*FnParams)(snd_pcm_t *, snd_pcm_channel_params_t *);
typedef int (*FnSetup)(snd_pcm_t *, snd_pcm_channel_setup_t *);
typedef int (*FnStatus)(snd_pcm_t *, snd_pcm_channel_status_t *);
typedef int (*FnPrepare)(snd_pcm_t *, int);
typedef ssize_t (*FnWrite)(snd_pcm_t *, const void *, size_t);
typedef int (*FnFlush)(snd_pcm_t *, int);
typedef const char *(*FnStrerror)(int);

static struct {
    FnOpenPreferred open_preferred;
    FnClose close;
    FnInfo info;
    FnParams params;
    FnSetup setup;
    FnStatus status;
    FnPrepare prepare;
    FnWrite write;
    FnFlush flush;
    FnStrerror strerror;
} as;

static int fds_load(void) {
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
#define FDS_BIND(field, name) do { void *f_ = dlsym(lib, name); if (!f_) { mps_log("sound fail step=symbol name=%s", name); ++missing; } memcpy(&as.field, &f_, sizeof f_); } while (0)
    FDS_BIND(open_preferred, "snd_pcm_open_preferred");
    FDS_BIND(close, "snd_pcm_close");
    FDS_BIND(info, "snd_pcm_plugin_info");
    FDS_BIND(params, "snd_pcm_plugin_params");
    FDS_BIND(setup, "snd_pcm_plugin_setup");
    FDS_BIND(status, "snd_pcm_plugin_status");
    FDS_BIND(prepare, "snd_pcm_plugin_prepare");
    FDS_BIND(write, "snd_pcm_plugin_write");
    FDS_BIND(flush, "snd_pcm_plugin_flush");
    FDS_BIND(strerror, "snd_strerror");
#undef FDS_BIND
    return missing == 0;
}

static snd_pcm_t *fds_open(unsigned *rate_out) {
    snd_pcm_t *h = 0;
    snd_pcm_channel_info_t info;
    snd_pcm_channel_params_t p;
    snd_pcm_channel_setup_t su;
    int card = -1, dev = -1, rc;
    unsigned rate = 44100;
    rc = as.open_preferred(&h, &card, &dev, SND_PCM_OPEN_PLAYBACK);
    if (rc < 0 || !h) {
        mps_log("sound fail step=open rc=%d %s", rc, as.strerror ? as.strerror(rc) : "");
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (as.info(h, &info) == 0) {
        mps_log("sound device card=%d dev=%d rates=%d..%d fragment=%d..%d", card, dev, info.min_rate, info.max_rate,
                info.min_fragment_size, info.max_fragment_size);
        if (info.max_rate > 0 && (int)rate > info.max_rate) rate = (unsigned)info.max_rate;
    }
    memset(&p, 0, sizeof p);
    p.channel = SND_PCM_CHANNEL_PLAYBACK;
    p.mode = SND_PCM_MODE_BLOCK;
    p.start_mode = SND_PCM_START_FULL;
    p.stop_mode = SND_PCM_STOP_STOP;
    p.buf.block.frag_size = FDS_FRAMES * 4;
    p.buf.block.frags_min = 1;
    p.buf.block.frags_max = 5;
    p.format.interleave = 1;
    p.format.format = SND_PCM_SFMT_S16_LE;
    p.format.rate = (int32_t)rate;
    p.format.voices = 2;
    rc = as.params(h, &p);
    if (rc < 0) {
        mps_log("sound fail step=params rc=%d %s rate=%u", rc, as.strerror ? as.strerror(rc) : "", rate);
        as.close(h);
        return 0;
    }
    memset(&su, 0, sizeof su);
    su.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (as.setup(h, &su) == 0) {
        if (su.format.rate > 0) rate = (unsigned)su.format.rate;
        mps_log("sound setup rate=%d voices=%d fragment=%d x%d", su.format.rate, su.format.voices,
                su.buf.block.frag_size, su.buf.block.frags);
    }
    rc = as.prepare(h, SND_PCM_CHANNEL_PLAYBACK);
    if (rc < 0) {
        mps_log("sound fail step=prepare rc=%d", rc);
        as.close(h);
        return 0;
    }
    *rate_out = rate;
    return h;
}

/* Writes one block; a short write (underrun) re-prepares and carries on. Returns 0 on a hard error. */
static int fds_write(snd_pcm_t *h, const int16_t *buf, size_t bytes, unsigned *prepares) {
    const uint8_t *p = (const uint8_t *)buf;
    int attempt;
    for (attempt = 0; bytes > 0 && attempt < 3; ++attempt) {
        ssize_t n = as.write(h, p, bytes);
        snd_pcm_channel_status_t st;
        if (n > 0) {
            p += n;
            bytes -= (size_t)n;
            if (!bytes) break;
        }
        memset(&st, 0, sizeof st);
        st.channel = SND_PCM_CHANNEL_PLAYBACK;
        if (as.status(h, &st) == 0 && (st.status == SND_PCM_STATUS_UNDERRUN || st.status == SND_PCM_STATUS_READY)) {
            as.prepare(h, SND_PCM_CHANNEL_PLAYBACK);
            ++*prepares;
        } else if (n < 0) {
            return 0;
        }
    }
    return bytes == 0;
}
#endif

static void *fds_thread(void *arg);
static void *fds_main(void *arg) {
    fds_thread(arg);
    audio_done = 1;
    return 0;
}

static void *fds_thread(void *arg) {
    (void)arg;
#ifdef __QNX__
    {
        struct sched_param sp;
        int policy;
        if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
            sp.sched_priority = FDS_PRIORITY;
            pthread_setschedparam(pthread_self(), policy, &sp);
        }
    }
    {
        static int16_t block[FDS_FRAMES * 2];
        snd_pcm_t *h = 0;
        unsigned rate = 44100, blocks = 0, prepares = 0, errors = 0;
        int routed = fds_route('b') == 0;
        if (fds_load()) h = fds_open(&rate);
        pthread_mutex_lock(&mix_lock);
        mixer.out_rate = rate;
        pthread_mutex_unlock(&mix_lock);
        mps_log("sound ready routed=%d device=%s rate=%u", routed, h ? "open" : "none", rate);
        /* The FM-chip emulation is the costliest thing here. Its processor time is measured; if it takes more
         * than FDS_MUSIC_CPU_LIMIT of the playing time, the music stops (the effects carry on), so the audio
         * thread can never crowd out the HMI, which runs at the same priority. */
        uint64_t music_ns = 0;
        unsigned music_on = 1, music_permille = 0;
        while (audio_running && h) {
            pthread_mutex_lock(&mix_lock);
            fdm_mix(&mixer, block, FDS_FRAMES);
            pthread_mutex_unlock(&mix_lock);
            if (music_on) {
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                music_blocks += (unsigned)fd_opl_render(block, FDS_FRAMES, FDS_MUSIC_GAIN);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                music_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
                if (blocks + 1 == FDS_MUSIC_CHECK && rate) {
                    uint64_t played_ns = (uint64_t)FDS_MUSIC_CHECK * FDS_FRAMES * 1000000000u / rate;
                    music_permille = (unsigned)(music_ns * 1000u / played_ns);
                    mps_log("sound music_cpu permille=%u limit=%u", music_permille, FDS_MUSIC_CPU_LIMIT);
                    if (music_permille > FDS_MUSIC_CPU_LIMIT) {
                        music_on = 0;
                        mps_log("sound music_off reason=cpu");
                    }
                }
            }
            if (!fds_write(h, block, sizeof block, &prepares)) {
                if (++errors >= 50) {
                    mps_log("sound stop reason=write_errors");
                    break;
                }
                mps_sleep_ms(20);
            }
            ++blocks;
        }
        if (h) {
            as.flush(h, SND_PCM_CHANNEL_PLAYBACK);
            as.close(h);
        }
        mps_log("sound end blocks=%u seconds=%u prepares=%u write_errors=%u sounds=%u music_blocks=%u music_on=%u "
                "music_cpu_permille=%u chip_clips=%u mix_clips=%u", blocks, rate ? (unsigned)((uint64_t)blocks * FDS_FRAMES / rate) : 0u,
                prepares, errors, starts, music_blocks, music_on, music_permille, fd_opl_chip_clips(),
                fd_opl_mix_clips());
        if (routed) fds_route('r');
    }
#else
    /* Host build: no device; mix in real time anyway and count the blocks that carry sound (tests). */
    {
        static int16_t block[FDS_FRAMES * 2];
        /* FD_RAW=path (host tests only): also save the mixed output as raw 16-bit stereo 44100 Hz. */
        FILE *raw = getenv("FD_RAW") ? fopen(getenv("FD_RAW"), "wb") : 0;
        mps_log("sound ready routed=0 device=none (host build)");
        while (audio_running) {
            unsigned k, loud = 0;
            pthread_mutex_lock(&mix_lock);
            fdm_mix(&mixer, block, FDS_FRAMES);
            pthread_mutex_unlock(&mix_lock);
            music_blocks += (unsigned)fd_opl_render(block, FDS_FRAMES, FDS_MUSIC_GAIN);
            for (k = 0; k < FDS_FRAMES * 2; ++k) {
                unsigned a = (unsigned)(block[k] < 0 ? -block[k] : block[k]);
                loud |= a != 0;
                if (a > peak) peak = a;
            }
            loud_blocks += loud;
            if (raw) fwrite(block, sizeof block, 1, raw);
            mps_sleep_ms(23);
        }
        if (raw) fclose(raw);
    }
#endif
    return 0;
}

unsigned fd_sound_starts(void) { return starts; }
unsigned fd_sound_loud_blocks(void) { return loud_blocks; }
unsigned fd_sound_music_blocks(void) { return music_blocks; }
unsigned fd_sound_peak(void) { return peak; }   /* host build only */

/* Called from the game's cleanup (atexit): stops the thread, which closes the device and releases the source. */
void fd_sound_stop(void) {
    unsigned waited = 0;
    if (!audio_started) return;
    audio_running = 0;
    /* At most 3 s: the thread may still be inside `audioconn -b`. If it is, give the source back ourselves so
     * the radio never stays muted after the game. */
    while (!audio_done && waited < 3000u) {
        mps_sleep_ms(20);
        waited += 20u;
    }
    if (audio_done) pthread_join(audio_thread, 0);
    else {
        mps_log("sound stop_timeout: releasing the source directly");
        fds_route('r');
    }
    audio_started = 0;
}

/* --restore: give the source back after a game that died (harmless if it is not built). */
void fd_sound_release_source(void) { fds_route('r'); }

/* ---- the engine's sound module ---- */
static boolean FDS_Init(boolean use_sfx_prefix) {
    use_prefix = use_sfx_prefix;
    memset(&mixer, 0, sizeof mixer);
    mixer.out_rate = 44100;
    audio_running = 1;
    audio_started = pthread_create(&audio_thread, 0, fds_main, 0) == 0;
    if (!audio_started) mps_log("sound fail step=thread");
    return true;                                     /* silent if the thread or device fails */
}

static void FDS_Shutdown(void) { fd_sound_stop(); }

static int FDS_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[16];
    if (sfx->link) sfx = sfx->link;
    if (use_prefix) snprintf(name, sizeof name, "ds%s", sfx->name);
    else snprintf(name, sizeof name, "%s", sfx->name);
    return W_CheckNumForName(name);
}

static void FDS_Update(void) {}

static void FDS_UpdateSoundParams(int channel, int vol, int sep) {
    pthread_mutex_lock(&mix_lock);
    fdm_params(&mixer, channel, vol, sep);
    pthread_mutex_unlock(&mix_lock);
}

static int FDS_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    const uint8_t *data;
    uint32_t len, rate;
    int lump = sfx->lumpnum;
    uint8_t *raw;
    if (lump < 0) lump = FDS_GetSfxLumpNum(sfx);
    if (lump < 0 || channel < 0 || channel >= FDM_VOICES) return -1;
    raw = (uint8_t *)W_CacheLumpNum(lump, PU_STATIC);   /* stays cached: the mixer reads it from its thread */
    if (!fdm_parse_lump(raw, (uint32_t)W_LumpLength(lump), &data, &len, &rate)) return -1;
    pthread_mutex_lock(&mix_lock);
    fdm_start(&mixer, channel, data, len, rate, vol, sep);
    pthread_mutex_unlock(&mix_lock);
    ++starts;
    return channel;
}

static void FDS_StopSound(int channel) {
    pthread_mutex_lock(&mix_lock);
    fdm_stop(&mixer, channel);
    pthread_mutex_unlock(&mix_lock);
}

static boolean FDS_SoundIsPlaying(int channel) {
    int playing;
    pthread_mutex_lock(&mix_lock);
    playing = fdm_playing(&mixer, channel);
    pthread_mutex_unlock(&mix_lock);
    return playing ? true : false;
}

static void FDS_CacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

static snddevice_t fds_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
    fds_devices, (int)(sizeof fds_devices / sizeof fds_devices[0]),
    FDS_Init, FDS_Shutdown, FDS_GetSfxLumpNum, FDS_Update, FDS_UpdateSoundParams, FDS_StartSound, FDS_StopSound,
    FDS_SoundIsPlaying, FDS_CacheSounds,
};

/* ---- music: DG_music_module is in src/opl/i_oplmusic.c ---- */
