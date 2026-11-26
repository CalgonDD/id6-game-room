/* sdl_shim.c: the SDL2 calls OpenTyrian makes, on top of the iDrive pieces (see SDL.h). Single-threaded like
 * the game, except the audio callback, which the car audio thread calls through tyr_audio_fill() under the
 * same lock SDL_LockAudioDevice takes. */
#define _POSIX_C_SOURCE 200809L
#include "SDL.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- init, errors, hints ---- */
int SDL_Init(Uint32 flags) { (void)flags; return 0; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
void SDL_Quit(void) {}
const char *SDL_GetError(void) { return "not available on the iDrive"; }
int SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return 1; }

/* ---- time ---- */
static Uint64 shim_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Uint64)t.tv_sec * 1000u + (Uint64)t.tv_nsec / 1000000u;
}
static Uint64 shim_start;
Uint32 SDL_GetTicks(void) {
    if (!shim_start) shim_start = shim_now_ms();
    return (Uint32)(shim_now_ms() - shim_start);
}
void SDL_Delay(Uint32 ms) {
    struct timespec t;
    t.tv_sec = ms / 1000u;
    t.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&t, &t) != 0) {}
}

/* ---- memory, strings, paths ---- */
void SDL_free(void *p) { free(p); }
size_t SDL_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
char *SDL_GetBasePath(void) { return NULL; }   /* data and user folders come from --data and XDG_CONFIG_HOME */

/* ---- logging: the game's messages go to stderr (and from there to nowhere on the car) ---- */
static void shim_log_output(void *userdata, int category, SDL_LogPriority priority, const char *message) {
    (void)userdata;
    (void)category;
    if (priority >= SDL_LOG_PRIORITY_WARN) fprintf(stderr, "tyrian: %s", message);
}
static void shim_vlog(SDL_LogPriority p, const char *fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    strncat(line, "\n", sizeof line - strlen(line) - 1);
    shim_log_output(0, 0, p, line);
}
void SDL_Log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); shim_vlog(SDL_LOG_PRIORITY_INFO, fmt, ap); va_end(ap); }
void SDL_LogDebug(int c, const char *fmt, ...) { va_list ap; (void)c; va_start(ap, fmt); shim_vlog(SDL_LOG_PRIORITY_DEBUG, fmt, ap); va_end(ap); }
void SDL_LogWarn(int c, const char *fmt, ...) { va_list ap; (void)c; va_start(ap, fmt); shim_vlog(SDL_LOG_PRIORITY_WARN, fmt, ap); va_end(ap); }
void SDL_LogError(int c, const char *fmt, ...) { va_list ap; (void)c; va_start(ap, fmt); shim_vlog(SDL_LOG_PRIORITY_ERROR, fmt, ap); va_end(ap); }
void SDL_LogSetPriority(int category, SDL_LogPriority priority) { (void)category; (void)priority; }
void SDL_LogGetOutputFunction(SDL_LogOutputFunction *callback, void **userdata) {
    if (callback) *callback = shim_log_output;
    if (userdata) *userdata = 0;
}
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags;
    (void)window;
    fprintf(stderr, "tyrian: %s: %s", title, message);
    return 0;
}

/* ---- surfaces ---- */
static SDL_PixelFormat shim_format8 = {0, 0, 8, 1}, shim_format32 = {SDL_PIXELFORMAT_RGB888, 0, 32, 4};
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof *s);
    (void)flags; (void)r; (void)g; (void)b; (void)a;
    if (!s) return 0;
    s->format = depth == 8 ? &shim_format8 : &shim_format32;
    s->w = w;
    s->h = h;
    s->pitch = w * s->format->BytesPerPixel;
    s->pixels = calloc((size_t)s->pitch * (size_t)h, 1);
    s->clip_rect.w = w;
    s->clip_rect.h = h;
    if (!s->pixels) { free(s); return 0; }
    return s;
}
void SDL_FreeSurface(SDL_Surface *s) {
    if (!s) return;
    free(s->pixels);
    free(s);
}
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color) {
    int x0 = 0, y0 = 0, x1 = s->w, y1 = s->h, x, y;
    if (rect) {
        x0 = rect->x < 0 ? 0 : rect->x;
        y0 = rect->y < 0 ? 0 : rect->y;
        x1 = rect->x + rect->w > s->w ? s->w : rect->x + rect->w;
        y1 = rect->y + rect->h > s->h ? s->h : rect->y + rect->h;
    }
    for (y = y0; y < y1; ++y) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * (size_t)s->pitch;
        if (s->format->BytesPerPixel == 1) memset(row + x0, (int)(color & 0xFF), (size_t)(x1 > x0 ? x1 - x0 : 0));
        else for (x = x0; x < x1; ++x) ((Uint32 *)row)[x] = color;
    }
    return 0;
}
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    /* the game only blits whole same-size, same-depth surfaces (game_menu.c) */
    int y, h = src->h < dst->h ? src->h : dst->h;
    size_t row = (size_t)(src->pitch < dst->pitch ? src->pitch : dst->pitch);
    (void)srcrect; (void)dstrect;
    for (y = 0; y < h; ++y)
        memcpy((Uint8 *)dst->pixels + (size_t)y * (size_t)dst->pitch, (Uint8 *)src->pixels + (size_t)y * (size_t)src->pitch, row);
    return 0;
}
SDL_PixelFormat *SDL_AllocFormat(Uint32 format) {
    SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof *f);
    if (!f) return 0;
    f->format = format;
    f->BitsPerPixel = format == SDL_PIXELFORMAT_RGB565 ? 16 : 32;
    f->BytesPerPixel = format == SDL_PIXELFORMAT_RGB565 ? 2 : 4;
    return f;
}
void SDL_FreeFormat(SDL_PixelFormat *f) { free(f); }
const char *SDL_GetPixelFormatName(Uint32 format) { return format == SDL_PIXELFORMAT_RGB565 ? "RGB565" : "RGB888"; }
Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) {
    if (f && f->BitsPerPixel == 16) return (Uint32)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return ((Uint32)r << 16) | ((Uint32)g << 8) | b;
}

/* ---- window, renderer, texture ---- */
struct SDL_Window { int w, h; };
struct SDL_Renderer { int unused; };
struct SDL_Texture { int w, h; Uint32 *px; };
static SDL_Window shim_window = {640, 400};
static SDL_Renderer shim_renderer;
static SDL_Texture *shim_last;
/* Our window always has the focus. OpenTyrian only learns that from a focus event (windowHasFocus starts
 * false), and release builds pause the game every frame without it (car test 2026-11-20: "pauses and
 * unpauses constantly"), so the window says so as it opens. */
SDL_Window *SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 f) {
    SDL_Event focus;
    (void)t; (void)x; (void)y; (void)w; (void)h; (void)f;
    memset(&focus, 0, sizeof focus);
    focus.window.type = SDL_WINDOWEVENT;
    focus.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    SDL_PushEvent(&focus);
    return &shim_window;
}
void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
int SDL_SetWindowFullscreen(SDL_Window *w, Uint32 f) { (void)w; (void)f; return 0; }
void SDL_SetWindowSize(SDL_Window *w, int width, int height) { (void)w; (void)width; (void)height; } /* always 640x400 */
void SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
    (void)w;
    if (width) *width = shim_window.w;
    if (height) *height = shim_window.h;
}
void SDL_SetWindowPosition(SDL_Window *w, int x, int y) { (void)w; (void)x; (void)y; }
void SDL_ShowWindow(SDL_Window *w) { (void)w; }
int SDL_GetWindowDisplayIndex(SDL_Window *w) { (void)w; return 0; }
int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetDisplayBounds(int d, SDL_Rect *r) { (void)d; r->x = r->y = 0; r->w = shim_window.w; r->h = shim_window.h; return 0; }
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int i, Uint32 f) { (void)w; (void)i; (void)f; return &shim_renderer; }
void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 a, Uint8 b, Uint8 c, Uint8 d) { (void)r; (void)a; (void)b; (void)c; (void)d; return 0; }
int SDL_RenderClear(SDL_Renderer *r) { (void)r; shim_last = 0; return 0; }
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s, const SDL_Rect *d) { (void)r; (void)s; (void)d; shim_last = t; return 0; }
void SDL_RenderPresent(SDL_Renderer *r) {
    (void)r;
    if (shim_last && shim_last->px) tyr_platform_present(shim_last->px, shim_last->w * 4, shim_last->w, shim_last->h);
}
int SDL_RenderFlush(SDL_Renderer *r) { (void)r; return 0; }
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h) {
    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof *t);
    (void)r; (void)format; (void)access;
    if (!t) return 0;
    t->w = w;
    t->h = h;
    t->px = (Uint32 *)calloc((size_t)w * (size_t)h, 4);
    if (!t->px) { free(t); return 0; }
    return t;
}
void SDL_DestroyTexture(SDL_Texture *t) {
    if (!t) return;
    if (shim_last == t) shim_last = 0;
    free(t->px);
    free(t);
}
int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *rect, void **pixels, int *pitch) {
    (void)rect;
    *pixels = t->px;
    *pitch = t->w * 4;
    return 0;
}
void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }
int SDL_QueryTexture(SDL_Texture *t, Uint32 *format, int *access, int *w, int *h) {
    if (format) *format = SDL_PIXELFORMAT_RGB888;
    if (access) *access = SDL_TEXTUREACCESS_STREAMING;
    if (w) *w = t->w;
    if (h) *h = t->h;
    return 0;
}
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}

/* ---- keyboard names (the game stores key settings by name) ---- */
static const char *shim_key_name(int s) {
    static char one[2];
    switch (s) {
    case SDL_SCANCODE_RETURN: return "Return";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_LCTRL: return "Left Ctrl";
    case SDL_SCANCODE_RCTRL: return "Right Ctrl";
    case SDL_SCANCODE_LSHIFT: return "Left Shift";
    case SDL_SCANCODE_RSHIFT: return "Right Shift";
    case SDL_SCANCODE_LALT: return "Left Alt";
    case SDL_SCANCODE_RALT: return "Right Alt";
    case SDL_SCANCODE_KP_ENTER: return "Keypad Enter";
    case SDL_SCANCODE_PAUSE: return "Pause";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    default: break;
    }
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) { one[0] = (char)('A' + s - SDL_SCANCODE_A); one[1] = 0; return one; }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) { one[0] = (char)('1' + s - SDL_SCANCODE_1); one[1] = 0; return one; }
    if (s == SDL_SCANCODE_0) return "0";
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) {
        static char f[4];
        snprintf(f, sizeof f, "F%d", 1 + s - SDL_SCANCODE_F1);
        return f;
    }
    return "";
}
const char *SDL_GetScancodeName(SDL_Scancode s) { return shim_key_name((int)s); }
SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    int s;
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    for (s = 1; s < SDL_NUM_SCANCODES; ++s) {
        const char *n = shim_key_name(s), *a = name;
        while (*n && *a && tolower((unsigned char)*n) == tolower((unsigned char)*a)) { ++n; ++a; }
        if (!*n && !*a && *shim_key_name(s)) return (SDL_Scancode)s;
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ---- events ---- */
#define SHIM_QUEUE 128
static SDL_Event shim_queue[SHIM_QUEUE];
static unsigned shim_head, shim_tail;
int SDL_PushEvent(SDL_Event *e) {
    unsigned next = (shim_tail + 1u) % SHIM_QUEUE;
    if (next == shim_head) return 0;
    shim_queue[shim_tail] = *e;
    shim_tail = next;
    return 1;
}
int SDL_PollEvent(SDL_Event *e) {
    tyr_platform_poll();
    if (shim_head == shim_tail) return 0;
    if (e) *e = shim_queue[shim_head];
    shim_head = (shim_head + 1u) % SHIM_QUEUE;
    return 1;
}

/* ---- joystick: one pad, read from the shared state tyrian_worker.c keeps up to date ---- */
volatile Sint16 tyr_axes[4];
volatile Uint32 tyr_buttons;
volatile Uint8 tyr_hat;
struct SDL_Joystick { int unused; };
static SDL_Joystick shim_pad;
#define SHIM_BUTTONS 8
int SDL_NumJoysticks(void) { return 1; }
SDL_Joystick *SDL_JoystickOpen(int index) { return index == 0 ? &shim_pad : 0; }
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
const char *SDL_JoystickName(SDL_Joystick *j) { (void)j; return "Xbox pad (iDrive)"; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 4; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return SHIM_BUTTONS; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 1; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis) { (void)j; return axis >= 0 && axis < 4 ? tyr_axes[axis] : 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b) { (void)j; return b >= 0 && b < SHIM_BUTTONS ? (Uint8)((tyr_buttons >> b) & 1u) : 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat) { (void)j; return hat == 0 ? tyr_hat : 0; }
void SDL_JoystickUpdate(void) { tyr_platform_poll(); }
int SDL_JoystickEventState(int state) { (void)state; return 0; }

/* ---- audio: the game's callback, called by the car audio thread ---- */
static pthread_mutex_t shim_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioSpec shim_spec;
static int shim_audio_open, shim_audio_paused = 1;
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    if (!desired || desired->format != AUDIO_S16SYS || desired->channels != 1) return 0;
    shim_spec = *desired;
    shim_spec.freq = 44100;          /* the car's output rate; the game adapts (it allows frequency changes) */
    if (obtained) *obtained = shim_spec;
    shim_audio_open = 1;
    return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    pthread_mutex_lock(&shim_audio_lock);
    shim_audio_open = 0;
    pthread_mutex_unlock(&shim_audio_lock);
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) { (void)dev; shim_audio_paused = pause_on; }
void SDL_LockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; pthread_mutex_lock(&shim_audio_lock); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; pthread_mutex_unlock(&shim_audio_lock); }

/* Fills `frames` stereo frames (interleaved) from the game's mono callback; silence while paused or closed. */
void tyr_audio_fill(Sint16 *stereo, unsigned frames) {
    static Sint16 mono[4096];
    unsigned i, n = frames > 4096u ? 4096u : frames;
    pthread_mutex_lock(&shim_audio_lock);
    if (shim_audio_open && !shim_audio_paused && shim_spec.callback)
        shim_spec.callback(shim_spec.userdata, (Uint8 *)mono, (int)(n * sizeof mono[0]));
    else
        memset(mono, 0, n * sizeof mono[0]);
    pthread_mutex_unlock(&shim_audio_lock);
    for (i = 0; i < n; ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
    for (; i < frames; ++i) stereo[2 * i] = stereo[2 * i + 1] = 0;
}

/* Sound effects: 8-bit signed 11025 Hz mono -> 16-bit at the output rate (a whole-number multiple). */
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate) {
    if (src_format != AUDIO_S8 || dst_format != AUDIO_S16SYS || src_channels != 1 || dst_channels != 1 ||
        src_rate <= 0 || dst_rate % src_rate != 0)
        return -1;
    memset(cvt, 0, sizeof *cvt);
    cvt->needed = 1;
    cvt->src_rate = src_rate;
    cvt->dst_rate = dst_rate;
    cvt->len_mult = 2 * (dst_rate / src_rate);
    return 1;
}
int SDL_ConvertAudio(SDL_AudioCVT *cvt) {
    int step = cvt->dst_rate / cvt->src_rate, i, k;
    Sint16 *out = (Sint16 *)cvt->buf;
    const Sint8 *in = (const Sint8 *)cvt->buf;
    /* in place, from the end, so the input is read before it is overwritten */
    for (i = cvt->len - 1; i >= 0; --i) {
        Sint16 v = (Sint16)(in[i] * 256);
        for (k = step - 1; k >= 0; --k) out[i * step + k] = v;
    }
    cvt->len_cvt = cvt->len * 2 * step;
    return 0;
}
