/* fd_opl.c: the OPL "driver" for Freedoom on the iDrive.
 *
 * Replaces Chocolate Doom's opl.c + opl_sdl.c (GPL-2.0, which this follows closely). The music player
 * (i_oplmusic.c) programs an emulated Yamaha OPL chip (Nuked OPL3, opl3.c) and schedules its own timing
 * callbacks; our audio thread (freedoom_sound.c) calls fd_opl_render() for every block it writes, which
 * generates the chip's output and runs the callbacks that fall inside that block, exactly as the SDL mixer
 * callback did. One recursive lock covers the chip, the queue and the callbacks: the game thread holds it
 * (OPL_Lock, register writes) while it changes songs, and the audio thread holds it while it renders. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl.h"
#include "opl3.h"
#include "opl_queue.h"
#include "fd_opl.h"

#define OPL_REGS_LEVEL     0x40
#define OPL_REGS_ATTACK    0x60
#define OPL_REGS_WAVEFORM  0xE0
#define OPL_NUM_OPERATORS  21

static pthread_mutex_t fdo_lock;
static int fdo_ready;
static unsigned fdo_rate = 44100;
static opl3_chip fdo_chip;
static opl_callback_queue_t *fdo_queue;
static uint64_t fdo_now;            /* us of audio rendered since OPL_Init */
static uint64_t fdo_pause_offset;
static int fdo_paused;
static unsigned fdo_register;
static int16_t fdo_buf[2 * 512];
static unsigned fdo_chip_clips, fdo_mix_clips;   /* samples clipped inside the chip / when added to the effects */

unsigned fd_opl_chip_clips(void) { return fdo_chip_clips; }
unsigned fd_opl_mix_clips(void) { return fdo_mix_clips; }

static void fdo_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&fdo_lock, &a);
    pthread_mutexattr_destroy(&a);
}

void OPL_SetSampleRate(unsigned int rate) {   /* the output is always 44100 Hz; the config value is ignored */
    (void)rate;
}

opl_init_result_t OPL_Init(unsigned int port_base) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)port_base;
    pthread_once(&once, fdo_lock_init);
    pthread_mutex_lock(&fdo_lock);
    if (!fdo_queue) fdo_queue = OPL_Queue_Create();
    OPL_Queue_Clear(fdo_queue);
    OPL3_Reset(&fdo_chip, fdo_rate);
    fdo_now = 0;
    fdo_pause_offset = 0;
    fdo_paused = 0;
    fdo_ready = 1;
    pthread_mutex_unlock(&fdo_lock);
    return OPL_INIT_OPL3;
}

void OPL_Shutdown(void) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    fdo_ready = 0;
    OPL_Queue_Clear(fdo_queue);
    pthread_mutex_unlock(&fdo_lock);
}

static void fdo_write_reg(unsigned reg, unsigned value) {
    /* The timer registers only matter for detecting a real card; the emulator plays everything else. */
    if (reg == 0x02 || reg == 0x03 || reg == 0x04) return;
    OPL3_WriteRegBuffered(&fdo_chip, (uint16_t)reg, (uint8_t)value);
}

void OPL_WritePort(opl_port_t port, unsigned int value) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    if (port == OPL_REGISTER_PORT) fdo_register = value;
    else if (port == OPL_REGISTER_PORT_OPL3) fdo_register = value | 0x100;
    else if (port == OPL_DATA_PORT) fdo_write_reg(fdo_register, value);
    pthread_mutex_unlock(&fdo_lock);
}

unsigned int OPL_ReadPort(opl_port_t port) {
    return port == OPL_REGISTER_PORT_OPL3 ? 0xff : 0;
}

unsigned int OPL_ReadStatus(void) { return 0; }

void OPL_WriteRegister(int reg, int value) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    fdo_write_reg((unsigned)reg, (unsigned)value);
    pthread_mutex_unlock(&fdo_lock);
}

opl_init_result_t OPL_Detect(void) { return OPL_INIT_OPL3; }

/* As Doom (and Chocolate Doom's opl.c) initialises the chip, including the writes to registers that
 * don't exist. */
void OPL_InitRegisters(int opl3) {
    int r, bank;
    for (bank = 0; bank <= (opl3 ? 1 : 0); ++bank) {
        int hi = bank ? 0x100 : 0;
        if (bank) OPL_WriteRegister(0x105, 0x01);
        for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) OPL_WriteRegister(r | hi, 0x3f);
        for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) OPL_WriteRegister(r | hi, 0x00);
        for (r = 1; r < OPL_REGS_LEVEL; ++r) OPL_WriteRegister(r | hi, 0x00);
        if (!bank) OPL_WriteRegister(0x01, 0x20);     /* waveform select enable */
    }
    OPL_WriteRegister(0x08, 0x40);
    if (opl3) OPL_WriteRegister(0x105, 0x01);
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    OPL_Queue_Push(fdo_queue, callback, data, fdo_now - fdo_pause_offset + us);
    pthread_mutex_unlock(&fdo_lock);
}

void OPL_AdjustCallbacks(float factor) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    OPL_Queue_AdjustCallbacks(fdo_queue, fdo_now, factor);
    pthread_mutex_unlock(&fdo_lock);
}

void OPL_ClearCallbacks(void) {
    if (!fdo_ready) return;
    pthread_mutex_lock(&fdo_lock);
    OPL_Queue_Clear(fdo_queue);
    pthread_mutex_unlock(&fdo_lock);
}

void OPL_Lock(void) {
    if (fdo_ready) pthread_mutex_lock(&fdo_lock);
}

void OPL_Unlock(void) {
    if (fdo_ready) pthread_mutex_unlock(&fdo_lock);
}

void OPL_Delay(uint64_t us) { (void)us; }

void OPL_SetPaused(int paused) {
    fdo_paused = paused;
}

/* Adds `frames` stereo frames of music into out (interleaved L R, clipped), advancing the song.
 * Called only by the audio thread. Returns 1 if any music sample was non-zero. */
int fd_opl_render(int16_t *out, unsigned frames, int gain_q8) {
    unsigned done = 0;
    int heard = 0;
    if (!fdo_ready) return 0;
    pthread_mutex_lock(&fdo_lock);
    while (done < frames && fdo_ready) {
        uint64_t n = frames - done, us;
        unsigned i;
        if (n > sizeof fdo_buf / sizeof fdo_buf[0] / 2) n = sizeof fdo_buf / sizeof fdo_buf[0] / 2;
        if (!fdo_paused && !OPL_Queue_IsEmpty(fdo_queue)) {
            uint64_t next = OPL_Queue_Peek(fdo_queue) + fdo_pause_offset, until;
            until = next > fdo_now ? next - fdo_now : 0;
            until = (until * fdo_rate + OPL_SECOND - 1) / OPL_SECOND;
            if (until < n) n = until;
        }
        if (n) {
            OPL3_GenerateStream(&fdo_chip, fdo_buf, (uint32_t)n);
            for (i = 0; i < 2 * n; ++i) {
                int32_t s = out[2 * done + i] + ((fdo_buf[i] * gain_q8) >> 8);
                if (fdo_buf[i]) heard = 1;
                if (fdo_buf[i] == 32767 || fdo_buf[i] == -32768) ++fdo_chip_clips;
                if (s > 32767) { s = 32767; ++fdo_mix_clips; }
                if (s < -32768) { s = -32768; ++fdo_mix_clips; }
                out[2 * done + i] = (int16_t)s;
            }
            done += (unsigned)n;
        }
        us = (n * OPL_SECOND) / fdo_rate;
        fdo_now += us;
        if (fdo_paused) fdo_pause_offset += us;
        while (!OPL_Queue_IsEmpty(fdo_queue) && fdo_now >= OPL_Queue_Peek(fdo_queue) + fdo_pause_offset) {
            opl_callback_t cb;
            void *data;
            if (!OPL_Queue_Pop(fdo_queue, &cb, &data)) break;
            cb(data);                       /* may schedule the next one (the lock is recursive) */
        }
    }
    pthread_mutex_unlock(&fdo_lock);
    return heard;
}
