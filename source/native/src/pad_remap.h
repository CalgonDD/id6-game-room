/* pad_remap.h: your own button layout for a console game (NES, Game Boy, PS1, PSP), saved per game on the car.
 * Host-tested in tests/test_pad_remap.c.
 *
 * Each console button (NES A, PS1 Cross, ...) gets one Xbox button or trigger. The d-pad and the sticks keep
 * their usual jobs and are not part of a layout. A game with no saved layout plays with the built-in mapping
 * (nes_input.h, ps1_controls.h), exactly as before; a saved layout replaces only the buttons.
 *
 * The layout is a small text file in the emulator's save folder, named after the game's file name:
 *   <save dir>/controls-<system>-<8 hex digits>.cfg
 *   game-room-controls 1
 *   <RetroPad id> <Xbox source>      one line per console button
 * Xbox sources are xpad.h button numbers (0..14) and RM_LT / RM_RT for the triggers. A layout equal to the
 * default is not kept (the file is removed), so "Reset to default" really is the built-in mapping again.
 * The on-screen editor is pad_remap_screen.h. */
#ifndef PAD_REMAP_H
#define PAD_REMAP_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "xpad.h"

enum { RM_LT = 15, RM_RT = 16, RM_SOURCES = 17, RM_NONE = 255 };
#define RM_MAX 12
#define RM_DIRECTIONS 0xF0u   /* RetroPad up, down, left, right (ids 4..7): never remapped */
#define RM_TRIGGER 100        /* a trigger pulled past this counts as pressed (as nes_input.h) */

typedef struct { unsigned char slot; const char *label; unsigned char def; } rm_button;
typedef struct { const char *key, *title; unsigned count; rm_button b[RM_MAX]; } rm_system;
typedef struct { int active; unsigned char src[16]; } rm_map;   /* src[RetroPad id] */

/* The consoles. RetroPad ids: B 0, Y 1, Select 2, Start 3, A 8, X 9, L 10, R 11, L2 12, R2 13, L3 14, R3 15.
 * Defaults match the built-in mapping's main button for each (nes_input.h). */
static const rm_system rm_nes = {"nes", "NES", 4, {{8, "A", 11}, {0, "B", 13}, {3, "Start", 4}, {2, "Select", 5}}};
static const rm_system rm_gb = {"gb", "Game Boy", 4, {{8, "A", 11}, {0, "B", 13}, {3, "Start", 4}, {2, "Select", 5}}};
static const rm_system rm_ps1 = {"ps1", "PS1", 12, {
    {0, "Cross", 11}, {8, "Circle", 12}, {1, "Square", 13}, {9, "Triangle", 14}, {10, "L1", 8}, {11, "R1", 9},
    {12, "L2", RM_LT}, {13, "R2", RM_RT}, {14, "L3", 6}, {15, "R3", 7}, {3, "Start", 4}, {2, "Select", 5}}};
static const rm_system rm_psp = {"psp", "PSP", 8, {
    {0, "Cross", 11}, {8, "Circle", 12}, {1, "Square", 13}, {9, "Triangle", 14}, {10, "L", 8}, {11, "R", 9},
    {3, "Start", 4}, {2, "Select", 5}}};

/* The Xbox buttons a layout may use, in the order the editor lists them. Not the d-pad or the Xbox button. */
static inline int rm_source_ok(unsigned s) {
    return s == RM_LT || s == RM_RT || s == 4 || s == 5 || s == 6 || s == 7 || s == 8 || s == 9 || (s >= 11 && s <= 14);
}
static inline const char *rm_source_name(unsigned s) {
    switch (s) {
    case 11: return "A";
    case 12: return "B";
    case 13: return "X";
    case 14: return "Y";
    case 8: return "LB";
    case 9: return "RB";
    case RM_LT: return "LT";
    case RM_RT: return "RT";
    case 6: return "L stick click";
    case 7: return "R stick click";
    case 4: return "Menu";
    case 5: return "View";
    }
    return "(none)";
}

static inline void rm_defaults(const rm_system *sys, rm_map *m) {
    unsigned i;
    memset(m, 0, sizeof *m);
    memset(m->src, RM_NONE, sizeof m->src);
    for (i = 0; i < sys->count; ++i) m->src[sys->b[i].slot] = sys->b[i].def;
}
static inline int rm_is_default(const rm_system *sys, const rm_map *m) {
    unsigned i;
    for (i = 0; i < sys->count; ++i)
        if (m->src[sys->b[i].slot] != sys->b[i].def) return 0;
    return 1;
}

/* Whether an Xbox source is held. View and Menu held together open the emulator's menu, so then neither counts. */
static inline int rm_pressed(const xpad_state *s, unsigned src) {
    int both = (s->buttons & XPAD_START) && (s->buttons & XPAD_BACK);
    if (src == RM_LT) return s->lt > RM_TRIGGER;
    if (src == RM_RT) return s->rt > RM_TRIGGER;
    if (src > 14) return 0;
    if (both && (src == 4 || src == 5)) return 0;
    return (s->buttons >> src) & 1u;
}
/* The RetroPad buttons held under a layout. dirs: the built-in mapping's bits (only the directions are kept). */
static inline uint32_t rm_buttons(const rm_system *sys, const rm_map *m, const xpad_state *s, uint32_t dirs) {
    uint32_t b = dirs & RM_DIRECTIONS;
    unsigned i;
    for (i = 0; i < sys->count; ++i)
        if (m->src[sys->b[i].slot] != RM_NONE && rm_pressed(s, m->src[sys->b[i].slot])) b |= 1u << sys->b[i].slot;
    return b;
}

/* Gives console button i the source src; a button that had src takes i's old one (a swap), so every Xbox
 * button still does one thing. */
static inline void rm_assign(const rm_system *sys, rm_map *m, unsigned i, unsigned src) {
    unsigned j, old;
    if (i >= sys->count || !rm_source_ok(src)) return;
    old = m->src[sys->b[i].slot];
    for (j = 0; j < sys->count; ++j)
        if (j != i && m->src[sys->b[j].slot] == src) m->src[sys->b[j].slot] = (unsigned char)old;
    m->src[sys->b[i].slot] = (unsigned char)src;
}

/* <dir>/controls-<key>-<hash of the game's file name>.cfg. Only the file name counts, so the layout follows the
 * game whichever list or folder it was started from. */
static inline void rm_path(const rm_system *sys, const char *dir, const char *game, char *out, size_t size) {
    const char *name = strrchr(game, '/');
    const unsigned char *c = (const unsigned char *)(name ? name + 1 : game);
    uint32_t h = 2166136261u;
    while (*c) { h ^= *c++; h *= 16777619u; }
    snprintf(out, size, "%s/controls-%s-%08x.cfg", dir, sys->key, (unsigned)h);
}

/* The game's saved layout, or the default (active = 0) when there is none or the file is not one of ours. */
static inline int rm_load(const rm_system *sys, const char *dir, const char *game, rm_map *m) {
    char path[700], line[64];
    unsigned slot, src, i, seen = 0;
    FILE *f;
    rm_defaults(sys, m);
    rm_path(sys, dir, game, path, sizeof path);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "game-room-controls 1", 20)) { fclose(f); return 0; }
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%u %u", &slot, &src) == 2 && slot < 16 && rm_source_ok(src))
            for (i = 0; i < sys->count; ++i)
                if (sys->b[i].slot == slot) { m->src[slot] = (unsigned char)src; seen |= 1u << i; }
    fclose(f);
    if (seen != (1u << sys->count) - 1u) { rm_defaults(sys, m); return 0; }   /* incomplete: not trusted */
    for (i = 0; i < sys->count; ++i) {   /* two buttons on one source: not trusted either */
        unsigned j;
        for (j = i + 1; j < sys->count; ++j)
            if (m->src[sys->b[i].slot] == m->src[sys->b[j].slot]) { rm_defaults(sys, m); return 0; }
    }
    m->active = !rm_is_default(sys, m);
    return m->active;
}

/* Keeps the layout (written to a temporary file, then renamed), or removes the file when it is the default.
 * Returns 1 when that worked. */
static inline int rm_save(const rm_system *sys, const char *dir, const char *game, rm_map *m) {
    char path[700], tmp[720];
    unsigned i;
    FILE *f;
    rm_path(sys, dir, game, path, sizeof path);
    if (rm_is_default(sys, m)) {
        m->active = 0;
        return remove(path) == 0 || errno == ENOENT;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "game-room-controls 1\n");
    for (i = 0; i < sys->count; ++i) fprintf(f, "%u %u\n", (unsigned)sys->b[i].slot, (unsigned)m->src[sys->b[i].slot]);
    if (fclose(f) != 0 || rename(tmp, path) != 0) { remove(tmp); return 0; }
    m->active = 1;
    return 1;
}

#endif
