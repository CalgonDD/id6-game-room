#ifndef PS1_RUNTIME_H
#define PS1_RUNTIME_H
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define PS1_AUDIO_CAP 32768u
#define PS1_AUDIO_PRIME 2048u
#define PS1_AUDIO_LEAD 4096u
typedef struct {
    pthread_mutex_t lock;
    int16_t samples[PS1_AUDIO_CAP * 2u];
    unsigned read, write, active, priming, drops, gaps, high;
    int16_t last_l, last_r;
} ps1_audio_queue;
#define PS1_AUDIO_INIT { PTHREAD_MUTEX_INITIALIZER, {0}, 0, 0, 0, 1, 0, 0, 0, 0, 0 }

static void ps1_audio_active(ps1_audio_queue *q, int active) {
    pthread_mutex_lock(&q->lock);
    q->active = active != 0; q->priming = 1; q->read = q->write = 0;
    q->last_l = q->last_r = 0;
    pthread_mutex_unlock(&q->lock);
}
static unsigned ps1_audio_depth(ps1_audio_queue *q) {
    unsigned n;
    pthread_mutex_lock(&q->lock); n = q->write - q->read; pthread_mutex_unlock(&q->lock);
    return n;
}
static void ps1_audio_put_locked(ps1_audio_queue *q, int16_t left, int16_t right) {
    unsigned n = q->write - q->read;
    if (n >= PS1_AUDIO_CAP) { ++q->drops; return; }
    q->samples[(q->write % PS1_AUDIO_CAP) * 2u] = left;
    q->samples[(q->write % PS1_AUDIO_CAP) * 2u + 1u] = right;
    ++q->write;
    if (n + 1u > q->high) q->high = n + 1u;
}
static void ps1_audio_take(ps1_audio_queue *q, int16_t *out, unsigned frames) {
    unsigned i;
    pthread_mutex_lock(&q->lock);
    if (!q->active || (q->priming && q->write - q->read < PS1_AUDIO_PRIME)) {
        memset(out, 0, frames * 2u * sizeof(*out));
        pthread_mutex_unlock(&q->lock); return;
    }
    q->priming = 0;
    for (i = 0; i < frames; ++i) {
        if (q->write != q->read) {
            q->last_l = out[i*2u] = q->samples[(q->read % PS1_AUDIO_CAP)*2u];
            q->last_r = out[i*2u+1u] = q->samples[(q->read % PS1_AUDIO_CAP)*2u+1u];
            ++q->read;
        } else {
            /* Smooth exponential decay towards 0 prevents harsh square-wave DC clicks / crispy crackle on starvation */
            q->last_l = (int16_t)((q->last_l * 7) / 8);
            q->last_r = (int16_t)((q->last_r * 7) / 8);
            out[i*2u] = q->last_l;
            out[i*2u+1u] = q->last_r;
            ++q->gaps;
        }
    }
    pthread_mutex_unlock(&q->lock);
}

/* Absolute monotonic deadlines; no unbounded burst after a compositor stall. */
typedef struct { uint64_t due; unsigned period, late; } ps1_pacer;
static unsigned ps1_frame_period(double fps) {
    if (!(fps >= 10.0 && fps <= 240.0)) fps = 60.0;
    return (unsigned)(1000000000.0 / fps + 0.5);
}
static void ps1_pacer_reset(ps1_pacer *p, uint64_t now, unsigned period) {
    p->due = now; p->period = period; p->late = 0;
}
static void ps1_pacer_step(ps1_pacer *p, uint64_t finished) {
    p->due += p->period;
    if (finished > p->due + p->period) { p->due = finished; ++p->late; }
}
#endif
