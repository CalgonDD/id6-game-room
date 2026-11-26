/* SDL.h for OpenTyrian on the iDrive (2026-11-20): just the parts of the SDL2 API OpenTyrian uses, so its own
 * code builds unchanged. Implemented in sdl_shim.c on top of the pieces the car already proved with Freedoom:
 * our own screen layer (freedoom_screen.h), the wired Xbox pad (seen by the game as an SDL joystick), and the
 * car's audio (car_audio.h). Values match real SDL2 where the game stores them (scancodes go into its config). */
#ifndef ID6_SDL_H
#define ID6_SDL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "SDL_types.h"
#include "SDL_endian.h"

#define SDL_VERSION_ATLEAST(x, y, z) 1
#define SDL_PRINTF_FORMAT_STRING
#define SDL_PRINTF_VARARG_FUNC(n)

/* ---- init ---- */
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_JOYSTICK 0x200u
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
int SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE "SDL_MOUSE_RELATIVE_SYSTEM_SCALE"

/* ---- time ---- */
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

/* ---- memory, strings, paths ---- */
void SDL_free(void *p);
size_t SDL_strlcpy(char *dst, const char *src, size_t size);
char *SDL_GetBasePath(void);

/* ---- logging ---- */
enum { SDL_LOG_CATEGORY_APPLICATION = 0 };
typedef enum {
    SDL_LOG_PRIORITY_VERBOSE = 1, SDL_LOG_PRIORITY_DEBUG, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_WARN,
    SDL_LOG_PRIORITY_ERROR, SDL_LOG_PRIORITY_CRITICAL
} SDL_LogPriority;
typedef void (*SDL_LogOutputFunction)(void *userdata, int category, SDL_LogPriority priority, const char *message);
void SDL_Log(const char *fmt, ...);
void SDL_LogDebug(int category, const char *fmt, ...);
void SDL_LogWarn(int category, const char *fmt, ...);
void SDL_LogError(int category, const char *fmt, ...);
void SDL_LogSetPriority(int category, SDL_LogPriority priority);
void SDL_LogGetOutputFunction(SDL_LogOutputFunction *callback, void **userdata);
#define SDL_MESSAGEBOX_ERROR 0x10u
typedef struct SDL_Window SDL_Window;
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

/* ---- surfaces and pixels ---- */
typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_Palette { int ncolors; SDL_Color *colors; } SDL_Palette;
typedef struct SDL_PixelFormat {
    Uint32 format;
    SDL_Palette *palette;
    Uint8 BitsPerPixel, BytesPerPixel;
} SDL_PixelFormat;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h, pitch;
    void *pixels;
    SDL_Rect clip_rect;
} SDL_Surface;
#define SDL_MUSTLOCK(s) 0
#define SDL_PIXELFORMAT_RGB888 0x16161804u
#define SDL_PIXELFORMAT_RGB565 0x15151002u
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 r, Uint32 g, Uint32 b, Uint32 a);
void SDL_FreeSurface(SDL_Surface *s);
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
SDL_PixelFormat *SDL_AllocFormat(Uint32 format);
void SDL_FreeFormat(SDL_PixelFormat *f);
const char *SDL_GetPixelFormatName(Uint32 format);
Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b);

/* ---- window, renderer, texture: one fixed 640x400 picture on the iDrive screen ---- */
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x1001u
#define SDL_WINDOW_HIDDEN 0x8u
#define SDL_WINDOW_RESIZABLE 0x20u
#define SDL_TEXTUREACCESS_STREAMING 1
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *w);
int SDL_SetWindowFullscreen(SDL_Window *w, Uint32 flags);
void SDL_SetWindowSize(SDL_Window *w, int width, int height);
void SDL_GetWindowSize(SDL_Window *w, int *width, int *height);
void SDL_SetWindowPosition(SDL_Window *w, int x, int y);
void SDL_ShowWindow(SDL_Window *w);
int SDL_GetWindowDisplayIndex(SDL_Window *w);
int SDL_GetNumVideoDisplays(void);
int SDL_GetDisplayBounds(int display, SDL_Rect *rect);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *r);
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha);
int SDL_RenderClear(SDL_Renderer *r);
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *r);
int SDL_RenderFlush(SDL_Renderer *r);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture *t);
int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *rect, void **pixels, int *pitch);
void SDL_UnlockTexture(SDL_Texture *t);
int SDL_QueryTexture(SDL_Texture *t, Uint32 *format, int *access, int *w, int *h);
int SDL_ShowCursor(int toggle);
int SDL_SetRelativeMouseMode(SDL_bool enabled);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);
#define SDL_DISABLE 0
#define SDL_ENABLE 1
#define SDL_IGNORE 0

/* ---- keyboard ---- */
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F,
    SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M,
    SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
    SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE,
    SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
    SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_SEMICOLON = 51, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE,
    SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_CAPSLOCK,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_PRINTSCREEN = 70, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_PAUSE, SDL_SCANCODE_INSERT,
    SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
    SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, SDL_SCANCODE_NUMLOCKCLEAR,
    SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_PLUS,
    SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4,
    SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9,
    SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI, SDL_SCANCODE_RCTRL,
    SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI,
    SDL_NUM_SCANCODES = 512
} SDL_Scancode;
typedef Sint32 SDL_Keycode;
#define SDLK_RIGHTBRACKET ']'
#define SDLK_d 'd'
#define SDLK_g 'g'
#define SDLK_l 'l'
#define SDLK_o 'o'
#define SDLK_r 'r'
#define SDLK_s 's'
typedef enum {
    KMOD_NONE = 0, KMOD_LSHIFT = 0x1, KMOD_RSHIFT = 0x2, KMOD_LCTRL = 0x40, KMOD_RCTRL = 0x80, KMOD_LALT = 0x100,
    KMOD_RALT = 0x200, KMOD_LGUI = 0x400, KMOD_RGUI = 0x800
} SDL_Keymod;
#define KMOD_SHIFT (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_CTRL (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_ALT (KMOD_LALT | KMOD_RALT)
#define KMOD_GUI (KMOD_LGUI | KMOD_RGUI)
const char *SDL_GetScancodeName(SDL_Scancode s);
SDL_Scancode SDL_GetScancodeFromName(const char *name);

/* ---- events ---- */
enum {
    SDL_QUIT = 0x100, SDL_WINDOWEVENT = 0x200, SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTINPUT = 0x303,
    SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP
};
enum { SDL_WINDOWEVENT_RESIZED = 5, SDL_WINDOWEVENT_FOCUS_GAINED = 12, SDL_WINDOWEVENT_FOCUS_LOST = 13 };
#define SDL_RELEASED 0
#define SDL_PRESSED 1
#define SDL_BUTTON(x) (1u << ((x) - 1))
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK SDL_BUTTON(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK SDL_BUTTON(SDL_BUTTON_RIGHT)
typedef struct SDL_Keysym { SDL_Scancode scancode; SDL_Keycode sym; Uint16 mod; } SDL_Keysym;
typedef struct SDL_KeyboardEvent { Uint32 type; Uint8 state, repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct SDL_TextInputEvent { Uint32 type; char text[32]; } SDL_TextInputEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type; Uint8 button, state; Sint32 x, y; } SDL_MouseButtonEvent;
typedef struct SDL_WindowEvent { Uint32 type; Uint8 event; } SDL_WindowEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_WindowEvent window;
} SDL_Event;
int SDL_PollEvent(SDL_Event *e);
int SDL_PushEvent(SDL_Event *e);

/* ---- joystick: the wired Xbox pad ---- */
typedef struct SDL_Joystick SDL_Joystick;
#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP 0x01
#define SDL_HAT_RIGHT 0x02
#define SDL_HAT_DOWN 0x04
#define SDL_HAT_LEFT 0x08
int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *j);
const char *SDL_JoystickName(SDL_Joystick *j);
int SDL_JoystickNumAxes(SDL_Joystick *j);
int SDL_JoystickNumButtons(SDL_Joystick *j);
int SDL_JoystickNumHats(SDL_Joystick *j);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis);
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int button);
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat);
void SDL_JoystickUpdate(void);
int SDL_JoystickEventState(int state);

/* ---- audio ---- */
typedef Uint16 SDL_AudioFormat;
#define AUDIO_S8 0x8008
#define AUDIO_S16SYS 0x8010
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x1
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE 0x8
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels, silence;
    Uint16 samples;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;
typedef Uint32 SDL_AudioDeviceID;
typedef struct SDL_AudioCVT {
    int needed;
    int src_rate, dst_rate;
    Uint8 *buf;
    int len, len_cvt, len_mult;
} SDL_AudioCVT;
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate);
int SDL_ConvertAudio(SDL_AudioCVT *cvt);

/* ---- what the iDrive side gives the shim (tyrian_worker.c) ---- */
void tyr_platform_present(const Uint32 *pixels, int pitch, int w, int h); /* one finished 640x400 frame */
void tyr_platform_poll(void);                                            /* pad states, "stop", signals */
extern volatile Sint16 tyr_axes[4];   /* left x, left y (down +), right x, right y */
extern volatile Uint32 tyr_buttons;   /* bit n = SDL joystick button n */
extern volatile Uint8 tyr_hat;

#endif
