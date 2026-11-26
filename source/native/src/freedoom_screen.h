/* freedoom_screen.h: the game's picture on the iDrive screen, the route proven on the car
 * (2026-11-18): a QNX Screen window "1300" becomes LayerManager surface 1300, which goes FIRST in layer
 * 0x80000000's render order (on top of the iDrive picture, surface 256). Pixels are 0xAARRGGBB.
 *
 * The game covers the whole panel: black everywhere, Doom's 640x400 picture (320x200 doubled by the engine)
 * centred. Taking it off removes only our surface from the order at that moment (mps_restore), exactly as the
 * display test does. Needs gr_show.h (the shared helpers) included first. */
#ifndef FREEDOOM_SCREEN_H
#define FREEDOOM_SCREEN_H

/* The game area: 640x400 (Doom, Tyrian: 320x200 doubled) unless a game sets another size first (Quake: 640x480). */
#ifndef FDS_GAME_W
#define FDS_GAME_W 640
#define FDS_GAME_H 400
#endif
enum { kGameW = FDS_GAME_W, kGameH = FDS_GAME_H, kGameX = (kPanelW - kGameW) / 2, kGameY = (kPanelH - kGameH) / 2 };

typedef struct {
    const mps_api *s;
    mps_runner run;
    void *ctx, *win, *bufs[2];
    uint32_t *px[2];
    unsigned frame, posts_failed, max_post_ms;
    int shown;
} fd_screen;

/* Opens the window, waits for the surface and puts it on top. Returns 1 when the picture is showing. */
static int fds_open(fd_screen *f, const mps_api *s, mps_runner run) {
    unsigned saved[kMaxOrder], with_ours[kMaxOrder + 1];
    char list[200], out[2048], idstr[12];
    int n, i;
    memset(f, 0, sizeof *f);
    f->s = s;
    f->run = run;
    n = mps_read_order(run, saved, kMaxOrder);
    if (n <= 0) {
        mps_log("game refused reason=%s", n < 0 ? "no_render_order" : "empty_render_order");
        return 0;
    }
    n = mps_without_ours(saved, n);
    if (s->create_context(&f->ctx, MPS_APPLICATION_CONTEXT) != 0 || !f->ctx) {
        mps_log("game fail step=create_context errno=%d", errno);
        return 0;
    }
    if (s->create_window_type(&f->win, f->ctx, MPS_APPLICATION_WINDOW) != 0 || !f->win) {
        mps_log("game fail step=create_window errno=%d", errno);
        return 0;
    }
    {
        int usage = MPS_USAGE_NATIVE | MPS_USAGE_WRITE, format = MPS_FORMAT_RGBA8888;
        int size[2] = {kPanelW, kPanelH}, pos[2] = {0, 0};
        snprintf(idstr, sizeof idstr, "%d", kSurface);
        s->set_window_cv(f->win, MPS_PROPERTY_ID_STRING, (int)strlen(idstr), idstr);
        s->set_window_iv(f->win, MPS_PROPERTY_USAGE, &usage);
        s->set_window_iv(f->win, MPS_PROPERTY_FORMAT, &format);
        s->set_window_iv(f->win, MPS_PROPERTY_SOURCE_SIZE, size);
        s->set_window_iv(f->win, MPS_PROPERTY_POSITION, pos);
    }
    if (s->create_window_buffers(f->win, 2) != 0) {
        mps_log("game fail step=create_buffers errno=%d", errno);
        return 0;
    }
    s->get_window_pv(f->win, MPS_PROPERTY_RENDER_BUFFERS, f->bufs);
    for (i = 0; i < 2; ++i) {
        void *p = 0;
        if (f->bufs[i]) s->get_buffer_pv(f->bufs[i], MPS_PROPERTY_POINTER, &p);
        f->px[i] = (uint32_t *)p;
    }
    if (!f->px[0] || !f->px[1]) {
        mps_log("game fail step=buffer_pointers");
        return 0;
    }
    for (i = 0; i < 2; ++i) {   /* opaque black panel in both buffers; only the game area changes later */
        unsigned k;
        for (k = 0; k < (unsigned)(kPanelW * kPanelH); ++k) f->px[i][k] = 0xFF000000u;
    }
    {
        int rect[4] = {0, 0, kPanelW, kPanelH};
        int rc = s->post_window(f->win, f->bufs[0], 1, rect, 0);
        mps_log("game first_post rc=%d window=%s", rc, idstr);
    }
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
        if (native > 0 && native != 4294967295LL) { f->shown = 1; break; }
        mps_sleep_ms(300);
    }
    if (!f->shown) {
        mps_log("game fail step=surface_not_created");
        return 0;
    }
    with_ours[0] = (unsigned)kSurface;
    for (i = 0; i < n; ++i) with_ours[i + 1] = saved[i];
    mps_format_order(with_ours, n + 1, list, sizeof list);
    mps_lm(run, out, sizeof out, "set layer " GR_HMI_LAYER " render order %s", list);
    mps_log("game on_screen render_order=%s game=%dx%d at %d,%d", list, kGameW, kGameH, kGameX, kGameY);
    return 1;
}

/* Copies one 640x400 engine frame (0x00RRGGBB, the engine leaves alpha at 0) and posts it. */
static void fds_frame(fd_screen *f, const uint32_t *src) {
    uint32_t *dst;
    int y, x, rect[4] = {kGameX, kGameY, kGameW, kGameH}, rc;
    uint64_t t0, t1;
    if (!f->shown) return;
    dst = f->px[f->frame % 2u];
    for (y = 0; y < kGameH; ++y) {
        uint32_t *row = dst + (size_t)(kGameY + y) * kPanelW + kGameX;
        const uint32_t *in = src + (size_t)y * kGameW;
#ifdef RETRO_CORE_PS1
        /* PS1 callback and menu painters already produce opaque ARGB pixels. */
        (void)x; memcpy(row,in,(size_t)kGameW*sizeof(*row));
#else
        for (x = 0; x < kGameW; ++x) row[x] = in[x] | 0xFF000000u;
#endif
    }
    t0 = mps_now_ms();
    rc = f->s->post_window(f->win, f->bufs[f->frame % 2u], 1, rect, 0);
    t1 = mps_now_ms();
    if (rc) ++f->posts_failed;
    if (t1 - t0 > f->max_post_ms) f->max_post_ms = (unsigned)(t1 - t0);
    ++f->frame;
}

/* Takes the picture off (only our surface) and frees the window. Safe to call more than once. */
static void fds_close(fd_screen *f) {
    if (f->run) mps_restore(f->run);
    if (f->win) {
        if (f->px[0] && f->px[1]) {
            int rect[4] = {0, 0, kPanelW, kPanelH};
            memset(f->px[f->frame % 2u], 0, (size_t)kPanelW * kPanelH * 4u);
            f->s->post_window(f->win, f->bufs[f->frame % 2u], 1, rect, 0);
        }
        f->s->destroy_window(f->win);
    }
    if (f->ctx) f->s->destroy_context(f->ctx);
    f->win = f->ctx = 0;
    f->run = 0;
    f->shown = 0;
}

#endif
