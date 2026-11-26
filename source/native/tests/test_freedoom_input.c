/* freedoom_input.h: pad states become the right Doom key presses and releases, nothing repeats or sticks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/freedoom_input.h"

static int next(fd_input *q, int *p, unsigned char *k) { return fd_pop(q, p, k); }

int main(void) {
    fd_input q;
    xpad_state s;
    int p;
    unsigned char k;
    memset(&q, 0, sizeof q);
    memset(&s, 0, sizeof s);

    s.buttons = XPAD_A;                              /* A: use + Enter (menus) */
    fd_update(&q, &s);
    assert(next(&q, &p, &k) && p == 1 && k == FDK_USE);
    assert(next(&q, &p, &k) && p == 1 && k == FDK_ENTER);
    assert(!next(&q, &p, &k));
    fd_update(&q, &s);                               /* same state again: nothing new */
    assert(!next(&q, &p, &k));

    memset(&s, 0, sizeof s);
    s.ly = 30000; s.rx = -20000; s.rt = 200; s.lx = 5000;   /* forward, turn left, fire; lx inside dead zone */
    fd_update(&q, &s);
    /* A released first (use, enter), then forward, turn left, fire pressed, in virtual-button order */
    assert(next(&q, &p, &k) && p == 1 && k == FDK_UP);
    assert(next(&q, &p, &k) && p == 1 && k == FDK_LEFT);
    assert(next(&q, &p, &k) && p == 1 && k == FDK_FIRE);
    assert(next(&q, &p, &k) && p == 0 && k == FDK_USE);
    assert(next(&q, &p, &k) && p == 0 && k == FDK_ENTER);
    assert(!next(&q, &p, &k));

    s.buttons = XPAD_RB | XPAD_START | XPAD_Y | XPAD_LS;     /* next weapon, menu, map, run */
    fd_update(&q, &s);
    {
        int seen_next = 0, seen_esc = 0, seen_tab = 0, seen_run = 0;
        while (next(&q, &p, &k)) {
            assert(p == 1);
            seen_next |= k == FD_KEY_NEXTWEAPON; seen_esc |= k == FDK_ESCAPE;
            seen_tab |= k == FDK_TAB; seen_run |= k == FDK_RSHIFT;
        }
        assert(seen_next && seen_esc && seen_tab && seen_run);
    }
    fd_release_all(&q);                              /* everything held comes up */
    {
        unsigned ups = 0;
        while (next(&q, &p, &k)) { assert(p == 0); ++ups; }
        assert(ups == 7);                            /* up, left, fire, next, escape, tab, run */
    }
    /* yes/no questions: A (Enter) and Y (Tab) say yes, B (Backspace) says no, the rest pass through */
    assert(fd_prompt_key(FDK_ENTER) == 'y' && fd_prompt_key(FDK_TAB) == 'y' && fd_prompt_key(FDK_BACKSPACE) == 'n');
    assert(fd_prompt_key(FDK_USE) == FDK_USE && fd_prompt_key(FDK_ESCAPE) == FDK_ESCAPE);
    {
        char name[24];
        fd_save_name(name, sizeof name, 0, 1, 3, 1);
        assert(strcmp(name, "E1M3 SAVE 2") == 0);
        fd_save_name(name, sizeof name, 1, 1, 7, 0);
        assert(strcmp(name, "MAP07 SAVE 1") == 0);
    }
    printf("freedoom input: A = use+enter, sticks/triggers/d-pad/bumpers map once per change, release-all clears.\n");
    printf("freedoom input: A/Y answer yes and B no to the game's questions; empty save slots get a name.\n");
    return 0;
}
