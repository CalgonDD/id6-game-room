/* gr_show.h: Game Room's screen helpers: putting our own picture over the iDrive screen and taking it off
 * again. The games use them (freedoom_screen.h); the same file also holds a short test pattern (`--show`).
 *
 * How it gets pixels on screen (the recipe the NBT-AAD Android Auto project proved on a car; their code is not
 * copied, only the public QNX Screen calls and LayerManagerControl commands it showed to work):
 *   1. A QNX Screen window whose id string is an unused number (kSurface = 1300). LayerManagerService makes
 *      a LayerManager surface with that number on the window's first post.
 *   2. LayerManagerControl gives that surface the panel as source and destination and makes it visible.
 *   3. The iDrive picture is surface 256 in layer 0x80000000; the first id in a layer's render order is drawn
 *      on top. We put ours first and keep every surface that was there, so the iDrive page stays underneath.
 *      The window is panel-sized and fully transparent outside a 640x400 box (a smaller buffer showed a grey
 *      wash in the reference project), so the iDrive page stays visible around the box.
 *   4. Taking it off removes ONLY our id from whatever the render order is at that moment, so an Android Auto
 *      or CarPlay picture that came up meanwhile is not disturbed.
 *
 * Safety:
 *   - The render order is never changed unless it could be read first. No read, nothing touched.
 *   - A hard deadline (at most 30 s; the plugin asks for 5, or 20 for the Xbox pad test) and SIGALRM/SIGTERM/SIGINT all end in the same restore.
 *   - Only one show runs at a time (it owns UDP port GR_SHOW_PORT, which is also how the plugin sends
 *     controller turns and "stop").
 *   - If the worker dies anyway, its Screen window goes with it; the plugin runs --restore when the page hides.
 *   - Nothing here talks to a vehicle module.
 * Every step is logged to the HMI trace as "GameRoom show ...". */
#ifndef GR_SHOW_H
#define GR_SHOW_H

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_protocol.h"
#include "gr_log.h"
#include "xpad.h"

#include "gr_show_port.h"
#ifndef GR_LMC
#define GR_LMC "/opt/sys/bin/LayerManagerControl"
#endif

enum {
    kSurface = 1300,               /* 0x514: 256 is iDrive's, 1280-1282 CarPlay / Android Auto */
    kPanelW = 1280, kPanelH = 480, /* the installer's display setting for this car */
    kBoxX = 600, kBoxY = 40, kBoxW = 640, kBoxH = 400, /* Doom's 320x200 doubled */
    kMaxOrder = 16,
    kMaxSeconds = 30
};
#define GR_HMI_LAYER "0x80000000"

/* ---- QNX Screen, bound at run time (the SDK has no Screen header; numbers as on this unit's libscreen) ---- */
#define MPS_APPLICATION_CONTEXT 0
#define MPS_APPLICATION_WINDOW 0
#define MPS_PROPERTY_FORMAT 14
#define MPS_PROPERTY_ID_STRING 20
#define MPS_PROPERTY_POINTER 34
#define MPS_PROPERTY_POSITION 35
#define MPS_PROPERTY_RENDER_BUFFERS 37
#define MPS_PROPERTY_SOURCE_SIZE 40
#define MPS_PROPERTY_USAGE 48
#define MPS_FORMAT_RGBA8888 8
#define MPS_USAGE_NATIVE 1
#define MPS_USAGE_WRITE 4

typedef struct {
    int (*create_context)(void **ctx, int flags);
    int (*destroy_context)(void *ctx);
    int (*create_window_type)(void **win, void *ctx, int type);
    int (*destroy_window)(void *win);
    int (*create_window_buffers)(void *win, int count);
    int (*set_window_iv)(void *win, int prop, const int *val);
    int (*set_window_cv)(void *win, int prop, int len, const char *val);
    int (*get_window_pv)(void *win, int prop, void **val);
    int (*get_buffer_pv)(void *buf, int prop, void **val);
    int (*post_window)(void *win, void *buf, int count, const int *rects, int flags);
} mps_api;

/* Runs one shell command line and returns its output (NUL-terminated, cut to size). Replaceable for tests. */
typedef int (*mps_runner)(const char *cmd, char *out, size_t size);

__attribute__((unused)) static void mps_on_signal(int sig) { (void)sig; mps_stop_flag = 1; }

/* ---- render order parsing (host-tested) ---- */
/* Ids from "surface render order: 256(0x100), 1281(0x501)," in `LayerManagerControl get layer` output.
 * Returns how many, or -1 when the line is missing (then nothing may be changed). */
static int mps_parse_order(const char *dump, unsigned *ids, int max) {
    const char *k = strstr(dump, "surface render order:");
    int n = 0;
    if (!k) return -1;
    k += strlen("surface render order:");
    while (*k && *k != '\n' && n < max) {
        if (*k >= '0' && *k <= '9') {
            char *end;
            unsigned long v = strtoul(k, &end, 10);
            if (*end == 'x' || *end == 'X') {          /* a bare hex id: "0x100" */
                v = strtoul(k, &end, 16);
            }
            ids[n++] = (unsigned)v;
            k = end;
            if (*k == '(') { while (*k && *k != ')' && *k != '\n') ++k; }
        } else {
            ++k;
        }
    }
    return n;
}

/* "0x514,0x100" style list for `set layer ... render order`. Returns 0 when it does not fit. */
static int mps_format_order(const unsigned *ids, int n, char *out, size_t size) {
    size_t used = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < n; ++i) {
        int w = snprintf(out + used, size - used, "%s0x%x", i ? "," : "", ids[i]);
        if (w < 0 || (size_t)w >= size - used) return 0;
        used += (size_t)w;
    }
    return 1;
}

/* Drops kSurface from a list; returns the new count. */
static int mps_without_ours(unsigned *ids, int n) {
    int i, m = 0;
    for (i = 0; i < n; ++i)
        if (ids[i] != (unsigned)kSurface) ids[m++] = ids[i];
    return m;
}

/* ---- LayerManagerControl ---- */
__attribute__((unused)) static int mps_popen_runner(const char *cmd, char *out, size_t size) {
    FILE *p = popen(cmd, "r");
    size_t used = 0;
    if (!p) { out[0] = 0; return -1; }
    while (used + 1 < size) {
        size_t got = fread(out + used, 1, size - 1 - used, p);
        if (!got) break;
        used += got;
    }
    out[used] = 0;
    if (used + 1 >= size) { char sink[256]; while (fread(sink, 1, sizeof sink, p)) {} }
    return pclose(p);
}

/* Every command gets the library path LayerManagerControl needs (the installer's shell lacked
 * /opt/sys/lib and got "Could not load library libilmClient.cpp-ne.so.1"). */
#define MPS_ENV "LD_LIBRARY_PATH=/opt/sys/lib:/fs/sda0/opt/sys/lib:/lib:/fs/sda0/lib:/usr/lib "

static int mps_lm(mps_runner run, char *out, size_t size, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int mps_lm(mps_runner run, char *out, size_t size, const char *fmt, ...) {
    char args[1024], cmd[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);
    snprintf(cmd, sizeof cmd, MPS_ENV GR_LMC " %s 2>&1", args);
    return run(cmd, out, size);
}

static int mps_read_order(mps_runner run, unsigned *ids, int max) {
    char out[2048];
    int n;
    mps_lm(run, out, sizeof out, "get layer " GR_HMI_LAYER);
    n = mps_parse_order(out, ids, max);
    if (n < 0) mps_log_output("get_layer", out);
    return n;
}

/* Takes our surface out of the HMI layer's CURRENT render order and hides it. Safe to run any number of times. */
static int mps_restore(mps_runner run) {
    unsigned ids[kMaxOrder];
    char list[160], out[1024];
    int n = mps_read_order(run, ids, kMaxOrder), had, m;
    if (n < 0) {
        mps_log("restore_skipped reason=no_render_order");
        mps_lm(run, out, sizeof out, "set surface %d visibility 0", kSurface);
        return 0;
    }
    had = n;
    m = mps_without_ours(ids, n);
    if (m != had) {
        if (!m) { ids[0] = 0x100u; m = 1; }   /* only ours was left: the iDrive picture */
        mps_format_order(ids, m, list, sizeof list);
        mps_lm(run, out, sizeof out, "set layer " GR_HMI_LAYER " render order %s", list);
        mps_log("restore render_order=%s", list);
    } else {
        mps_log("restore render_order_unchanged count=%d", n);
    }
    mps_lm(run, out, sizeof out, "set surface %d visibility 0", kSurface);
    return 1;
}

/* ---- the test pattern ---- */
/* Bars left to right. Car result 2026-11-18: bar 1 showed BLUE, so a pixel is 0xAARRGGBB (memory B,G,R,A).
 * The bars are kept as they were, so a photo of any run compares with the first one. */
#define MPS_YELLOW 0xFFFFFF00u
#define MPS_GREEN 0xFF00FF00u
#define MPS_RED 0xFFFF0000u
#define MPS_BLUE 0xFF0000FFu
static const uint32_t mps_bars[6] = {
    0xFF0000FFu,  /* bar 1: red if the low byte is R, blue if it is B */
    0xFF00FF00u,  /* bar 2: green either way */
    0xFFFF0000u,  /* bar 3: blue if the low byte is R, red if it is B */
    0xFFFFFFFFu,  /* bar 4: white */
    0xFF808080u,  /* bar 5: grey */
    0x80FFFFFFu   /* bar 6: half-transparent white: the iDrive page should show through */
};

static void mps_fill(uint32_t *px, int x0, int y0, int w, int h, uint32_t v) {
    int x, y;
    for (y = y0; y < y0 + h; ++y) {
        uint32_t *row = px + (size_t)y * kPanelW;
        for (x = x0; x < x0 + w; ++x) row[x] = v;
    }
}

/* The square's colour follows the pad's face buttons (A green, B red, X blue, Y yellow). */
static uint32_t mps_square_colour(const xpad_state *pad) {
    if (pad->buttons & XPAD_A) return MPS_GREEN;
    if (pad->buttons & XPAD_B) return MPS_RED;
    if (pad->buttons & XPAD_X) return MPS_BLUE;
    return MPS_YELLOW;
}

/* One frame of the box. Outside the box is never touched (it stays transparent from the first clear).
 * With a pad: a row of 15 button lamps (lit white while pressed) and two trigger bars under the strip. */
static void mps_paint_pad(uint32_t *px, const xpad_state *pad) {
    int b;
    for (b = 0; b < XPAD_BUTTON_COUNT; ++b)
        mps_fill(px, kBoxX + 12 + b * 40, kBoxY + kBoxH - 40, 32, 28,
                 (pad->buttons & (1u << b)) ? 0xFFFFFFFFu : 0xFF505050u);
    mps_fill(px, kBoxX + 12, kBoxY + kBoxH - 56, 600, 10, 0xFF303030u);
    mps_fill(px, kBoxX + 12, kBoxY + kBoxH - 56, pad->lt * 290 / 255, 10, MPS_RED);
    mps_fill(px, kBoxX + 322, kBoxY + kBoxH - 56, pad->rt * 290 / 255, 10, MPS_GREEN);
}

static void mps_paint(uint32_t *px, int square_x, int square_y, unsigned frame) {
    const int bar_w = (kBoxW - 16) / 6;
    int i;
    mps_fill(px, kBoxX, kBoxY, kBoxW, kBoxH, 0xFFFFFFFFu);                       /* white frame */
    mps_fill(px, kBoxX + 4, kBoxY + 4, kBoxW - 8, kBoxH - 8, 0xFF202020u);      /* dark inside */
    for (i = 0; i < 6; ++i)
        mps_fill(px, kBoxX + 8 + i * bar_w, kBoxY + 8, bar_w - 4, 120, mps_bars[i]);
    /* a strip that moves one step per frame: smooth = fast enough for a game */
    mps_fill(px, kBoxX + 8 + (int)(frame * 8u % (unsigned)(kBoxW - 64)), kBoxY + 140, 48, 16, MPS_YELLOW);
    /* the square the controller moves */
    mps_fill(px, kBoxX + square_x, kBoxY + square_y, 48, 48, MPS_YELLOW);
}

/* ---- the show ---- */
typedef struct {
    unsigned frames, posts_failed, max_post_ms, turns, pad_msgs;
    uint64_t total_post_ms;
    int shown, restored, ended_by;   /* ended_by: 0 deadline, 1 stop message, 2 signal */
} mps_result;

static int mps_bind_control(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(GR_SHOW_PORT);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int mps_show(const mps_api *s, mps_runner run, unsigned seconds, mps_result *r) {
    unsigned saved[kMaxOrder], with_ours[kMaxOrder + 1];
    char list[200], out[2048], idstr[12];
    void *ctx = 0, *win = 0, *bufs[2] = {0, 0};
    uint32_t *px[2] = {0, 0};
    int n, i, ctl, last_value = -1, square_x = (kBoxW - 48) / 2, square_y = 180, have_pad = 0;
    uint64_t start, deadline;
    xpad_state pad;
    memset(&pad, 0, sizeof pad);
    memset(r, 0, sizeof *r);
    if (seconds < 1) seconds = 1;
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;

    ctl = mps_bind_control();
    if (ctl < 0) { mps_log("refused reason=already_running"); return 3; }

    n = mps_read_order(run, saved, kMaxOrder);
    if (n <= 0) {
        mps_log("refused reason=%s", n < 0 ? "no_render_order" : "empty_render_order");
        close(ctl);
        return 5;
    }
    n = mps_without_ours(saved, n);            /* a leftover from a run that died */
    mps_format_order(saved, n, list, sizeof list);
    mps_log("begin seconds=%u layer=" GR_HMI_LAYER " render_order=%s", seconds, list);

    if (s->create_context(&ctx, MPS_APPLICATION_CONTEXT) != 0 || !ctx) {
        mps_log("fail step=create_context errno=%d", errno);
        close(ctl);
        return 6;
    }
    if (s->create_window_type(&win, ctx, MPS_APPLICATION_WINDOW) != 0 || !win) {
        mps_log("fail step=create_window errno=%d", errno);
        s->destroy_context(ctx);
        close(ctl);
        return 6;
    }
    {
        int usage = MPS_USAGE_NATIVE | MPS_USAGE_WRITE, format = MPS_FORMAT_RGBA8888;
        int size[2] = {kPanelW, kPanelH}, pos[2] = {0, 0};
        snprintf(idstr, sizeof idstr, "%d", kSurface);
        s->set_window_cv(win, MPS_PROPERTY_ID_STRING, (int)strlen(idstr), idstr);
        s->set_window_iv(win, MPS_PROPERTY_USAGE, &usage);
        s->set_window_iv(win, MPS_PROPERTY_FORMAT, &format);
        s->set_window_iv(win, MPS_PROPERTY_SOURCE_SIZE, size);
        s->set_window_iv(win, MPS_PROPERTY_POSITION, pos);
    }
    if (s->create_window_buffers(win, 2) != 0) {
        mps_log("fail step=create_buffers errno=%d", errno);
        goto teardown;
    }
    s->get_window_pv(win, MPS_PROPERTY_RENDER_BUFFERS, bufs);
    for (i = 0; i < 2; ++i) {
        void *p = 0;
        if (bufs[i]) s->get_buffer_pv(bufs[i], MPS_PROPERTY_POINTER, &p);
        px[i] = (uint32_t *)p;
    }
    if (!px[0] || !px[1]) {
        mps_log("fail step=buffer_pointers");
        goto teardown;
    }
    /* Both buffers fully transparent, then the first post: that creates LayerManager surface 1300. */
    memset(px[0], 0, (size_t)kPanelW * kPanelH * 4u);
    memset(px[1], 0, (size_t)kPanelW * kPanelH * 4u);
    {
        int rect[4] = {0, 0, kPanelW, kPanelH};
        int rc = s->post_window(win, bufs[0], 1, rect, 0);
        mps_log("first_post rc=%d errno=%d window=%s", rc, rc ? errno : 0, idstr);
    }

    /* Wait for the surface (up to 3 s), giving it the panel as source and destination and making it visible. */
    for (i = 0; i < 10 && !mps_stop_flag; ++i) {
        const char *k;
        long long native = -1;
        mps_lm(run, out, sizeof out,
               "set surface %d source region 0 0 %d %d; " MPS_ENV GR_LMC
               " set surface %d destination region 0 0 %d %d; " MPS_ENV GR_LMC
               " set surface %d opacity 1.0; " MPS_ENV GR_LMC " set surface %d visibility 1; " MPS_ENV GR_LMC
               " get surface %d",
               kSurface, kPanelW, kPanelH, kSurface, kPanelW, kPanelH, kSurface, kSurface, kSurface);
        k = strstr(out, "native surface:");
        if (k) native = strtoll(k + 15, 0, 10);
        if (native > 0 && native != 4294967295LL) {
            mps_log("surface_ready id=%d native=%lld tries=%d", kSurface, native, i + 1);
            r->shown = 1;
            break;
        }
        if (i == 9) mps_log_output("get_surface", out);
        mps_sleep_ms(300);
    }
    if (!r->shown) {
        mps_log("fail step=surface_not_created");
        goto teardown;
    }

    /* Ours first (on top), then everything that was there. */
    with_ours[0] = (unsigned)kSurface;
    for (i = 0; i < n; ++i) with_ours[i + 1] = saved[i];
    mps_format_order(with_ours, n + 1, list, sizeof list);
    mps_lm(run, out, sizeof out, "set layer " GR_HMI_LAYER " render order %s", list);
    {
        unsigned now[kMaxOrder];
        int m = mps_read_order(run, now, kMaxOrder);
        mps_log("render_order_set wanted=%s took=%s", list, (m > 0 && now[0] == (unsigned)kSurface) ? "yes" : "no");
    }

    start = mps_now_ms();
    deadline = start + seconds * 1000u;
    while (!mps_stop_flag) {
        uint64_t t0, t1;
        char msg[96];
        int rect[4] = {0, 0, kPanelW, kPanelH}, rc;
        ssize_t got;
        if (mps_now_ms() >= deadline) { r->ended_by = 0; break; }
        while ((got = recv(ctl, msg, sizeof msg - 1, 0)) > 0) {
            int v;
            msg[got] = 0;
            if (!strncmp(msg, "stop", 4)) { r->ended_by = 1; goto done; }
            if (sscanf(msg, "v %d", &v) == 1) {
                if (last_value >= 0) square_x += (v - last_value) * 16;
                last_value = v;
                ++r->turns;
                if (square_x < 8) square_x = 8;
                if (square_x > kBoxW - 56) square_x = kBoxW - 56;
            } else if (xpad_parse(msg, &pad)) {
                if (!have_pad) mps_log("pad_input first=%s", msg);
                have_pad = 1;
                ++r->pad_msgs;
            }
        }
        if (have_pad) {
            /* left stick moves the square (dead zone 6000), the d-pad nudges it */
            int dx = (pad.lx > 6000 || pad.lx < -6000) ? pad.lx / 2048 : 0;
            int dy = (pad.ly > 6000 || pad.ly < -6000) ? -pad.ly / 2048 : 0;
            if (pad.buttons & XPAD_LEFT) dx -= 6;
            if (pad.buttons & XPAD_RIGHT) dx += 6;
            if (pad.buttons & XPAD_UP) dy -= 6;
            if (pad.buttons & XPAD_DOWN) dy += 6;
            square_x += dx;
            square_y += dy;
            if (square_y < 160) square_y = 160;
            if (square_y > kBoxH - 112) square_y = kBoxH - 112;
        } else {
            square_y = 180 + (int)((r->frames * 6u) % 300u > 150u ? 300u - (r->frames * 6u) % 300u : (r->frames * 6u) % 300u);
            if (square_y > kBoxH - 56) square_y = kBoxH - 56;
        }
        if (square_x < 8) square_x = 8;
        if (square_x > kBoxW - 56) square_x = kBoxW - 56;
        mps_paint(px[r->frames % 2u], square_x, square_y, r->frames);
        if (have_pad) {
            uint32_t *f = px[r->frames % 2u];
            mps_fill(f, kBoxX + square_x, kBoxY + square_y, 48, 48, mps_square_colour(&pad));
            mps_paint_pad(f, &pad);
        }
        t0 = mps_now_ms();
        rc = s->post_window(win, bufs[r->frames % 2u], 1, rect, 0);
        t1 = mps_now_ms();
        if (rc) ++r->posts_failed;
        if (t1 - t0 > r->max_post_ms) r->max_post_ms = (unsigned)(t1 - t0);
        r->total_post_ms += t1 - t0;
        ++r->frames;
        if (r->frames == 1) mps_log("first_frame rc=%d post_ms=%u", rc, (unsigned)(t1 - t0));
        mps_sleep_ms(33);
    }
    if (mps_stop_flag) r->ended_by = 2;
done:
    mps_log("frames=%u fps_x10=%u post_avg_ms=%u post_max_ms=%u posts_failed=%u turns=%u pad_msgs=%u ended_by=%s",
            r->frames,
            (unsigned)(r->frames * 10000u / (unsigned)((mps_now_ms() - start) ? (mps_now_ms() - start) : 1u)),
            r->frames ? (unsigned)(r->total_post_ms / r->frames) : 0u, r->max_post_ms, r->posts_failed, r->turns, r->pad_msgs,
            r->ended_by == 0 ? "deadline" : r->ended_by == 1 ? "stop" : "signal");

teardown:
    r->restored = mps_restore(run);
    if (px[0] && px[1]) {                      /* a clear frame, in case the surface is still drawn somewhere */
        int rect[4] = {0, 0, kPanelW, kPanelH};
        memset(px[r->frames % 2u], 0, (size_t)kPanelW * kPanelH * 4u);
        s->post_window(win, bufs[r->frames % 2u], 1, rect, 0);
    }
    s->destroy_window(win);
    s->destroy_context(ctx);
    close(ctl);
    mps_log("end shown=%d restored=%d", r->shown, r->restored);
    return r->shown ? 0 : 7;
}

#endif
