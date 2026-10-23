/* Frontend (menus) with a gamepad only (docs/gamepad.md section 4).
 *
 * The game's menus are driven by "widgets" (array 0x526588, 300 slots, focus index 0x526530). The
 * frontend controller reads joystick 0 (D-pad/stick = move focus or change a value, X/Y/A = activate,
 * RB/LB = next/previous). What a pad could not do, and what this file adds:
 *   - B = back: activates the screen's BACK/CANCEL widget (the enabled button class widget at the lowest
 *     y on the left), per-screen overrides in back_override()
 *   - holding a direction repeats (menus step once per press; sliders ran 1 unit per press), sliders
 *     accelerate the longer the direction is held
 *   - text fields (names, IP address, chat, file names): the game's line editor (a coroutine,
 *     FUN_00437970 -> FUN_0047dab0, which pops 16-bit key codes from the queue at 0x540260) starts as soon as
 *     a text widget has the focus. A on the field puts the pad into edit mode: up/down cycle the character,
 *     see text_frame(); A confirms, B cancels
 *   - the control binding prompt (FE_BindControlKey 0x437a80, also started by focusing the row): stage 1
 *     waits for Enter/button 0 held (the "click"), which a pad's A is not; A posts the Enter. Stage 2 waits for
 *     the button; B (Esc) cancels and is not bindable */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"
#include "pad.h"

#define FE_W            ((void *volatile *)0x526588)     /* widget pointers */
#define FE_NW           300
#define FE_FOCUS        (*(volatile int *)0x526530)
#define FE_MODAL        (*(volatile char *)0x526580)     /* a dialog is up: only widgets flagged in FE_MODAL_OK count */
#define FE_MODAL_OK     ((volatile char *)0x526388)
#define FE_ACTIVE       (*(volatile char *)0x526a40)
#define FE_ACCEPT       (*(volatile char *)0x525af4)     /* controller actions accepted (cleared in the bind prompt) */
#define FE_SCREEN       (*(volatile uintptr_t *)0x525aec)
#define TEXT_TASK       (*(volatile uintptr_t *)0x527018)   /* line editor coroutine, non-zero while it exists */
#define BIND_TASK       (*(volatile uintptr_t *)0x5270d0)   /* binding prompt coroutine */
#define KQ_ON           (*(volatile int *)0x540300)
#define KQ_COUNT        (*(volatile int *)0x5402f4)
#define KQ              ((volatile uint16_t *)0x540260)
#define EDIT_FILTER     (*(int (**)(int, unsigned, char *))0x507278)   /* accepts a typed character */

#define VT_BUTTON  0x4f1880
#define VT_CHOICE  0x4f1940
#define VT_TEXT    0x4f18e0
#define VT_SLIDER  0x4f17f0
#define VT_BIND    0x4f19a0

typedef void (__attribute__((fastcall)) *w_call0)(void *self, int edx);
typedef void (__attribute__((fastcall)) *w_call1)(void *self, int edx, int arg);
typedef float *(__attribute__((fastcall)) *w_rect)(void *self, int edx, float *out);

static void *focus_widget(void)
{
    int f = FE_FOCUS;
    return f > 0 && f < FE_NW ? FE_W[f] : NULL;
}

static int g_on;
static int g_back_btn_prev, g_a_prev, g_y_prev;
static volatile int g_in_edit;           /* inside FUN_0047dab0 (the game's line editor loop) */
static void *g_edit_widget;              /* the text widget being edited with the pad */

static uintptr_t vt_of(void *w) { return w ? *(uintptr_t *)w : 0; }
static int vt_slot(void *w, int off) { return (int)((uintptr_t *)vt_of(w))[off / 4]; }

static const char *screen_name(void)
{
    uintptr_t d = FE_SCREEN;
    return d ? *(const char **)(d + 0x14) : "";
}

static void widget_rect(void *w, float r[4])
{
    float tmp[4];
    float *p = ((w_rect)vt_slot(w, 0x24))(w, 0, tmp);
    memcpy(r, p, 4 * sizeof(float));
}

/* moves the cursor onto a widget (the menus are cursor based: the focus is whatever is under the cursor, what
   D-pad moves do too: vtable +0x1c) and then presses A for it: the fake joystick reports button 2 from 120 to
   400 ms later. The game's own press/release handling runs, which dialogs need */
static DWORD g_virt_a0, g_virt_a1;
static void widget_press(void *w)
{
    ((w_call0)vt_slot(w, 0x1c))(w, 0);
    g_virt_a0 = timeGetTime() + 120;
    g_virt_a1 = g_virt_a0 + 280;
}

/* ---- back ---- */

static int is_screen(const char *name) { return !strcmp(screen_name(), name); }

/* screens where B must not press the lowest button, or has a better target. NULL: use the lowest button */
static int back_disabled(void)
{
    static const char *const none[] = {
        "Main Menu Screen", "License Screen", "Loading Screen", "Loading Empty Screen", "FMV Screen",
        "Demosplash Screen", "Limbo", "Best Lap Time Screen",
    };
    unsigned i;
    for (i = 0; i < sizeof none / sizeof none[0]; i++) {
        if (is_screen(none[i])) {
            return 1;
        }
    }
    return 0;
}

static void *find_back(void)
{
    void *best = NULL;
    float besty = 1e9f;
    int i;
    for (i = 1; i < FE_NW; i++) {
        void *w = FE_W[i];
        float r[4];
        if (!w || !*(volatile char *)((char *)w + 0x10) || vt_of(w) != VT_BUTTON) {
            continue;
        }
        if (FE_MODAL && !FE_MODAL_OK[i]) {
            continue;
        }
        widget_rect(w, r);
        if (r[2] <= r[0]) {
            continue;
        }
        if (!FE_MODAL && (r[0] > 60 || r[1] >= 60)) {      /* a dialog: its lowest button (NO / CANCEL / OK) */
            continue;
        }
        if (r[1] <= besty) {           /* ties: the later widget */
            besty = r[1];
            best = w;
        }
    }
    return best;
}

/* screens whose back is a fixed widget (global pointer address) */
static void *back_override(void)
{
    if (!FE_MODAL && is_screen("Pause Screen")) {
        return *(void **)0x538250;         /* RESUME (the top button; the lowest is QUIT) */
    }
    return NULL;
}

static void do_back(void)
{
    void *w;
    if (!FE_ACTIVE || !FE_ACCEPT || back_disabled()) {
        return;
    }
    w = back_override();
    if (!w) {
        w = find_back();
    }
    if (w) {
        {
            float r[4];
            widget_rect(w, r);
            dk_log("gamepad: B = back on %s (widget %p index %d rect %.0f %.0f %.0f %.0f modal %d)", screen_name(), w,
                   *(int *)((char *)w + 0xc), r[0], r[1], r[2], r[3], FE_MODAL);
        }
        widget_press(w);
    }
}

/* ---- direction repeat ---- */

typedef struct { int mask; DWORD t0, tlast; int out; } rep_t;
static rep_t g_rep[PAD_MAX];

/* extra slider steps for a held direction: 1 per pulse at first, doubling every 700 ms after 800 ms */
static void slider_boost(int mask, DWORD held)
{
    void *w = FE_FOCUS > 0 && FE_FOCUS < FE_NW ? FE_W[FE_FOCUS] : NULL;
    int range, extra, i, arg = (mask & 4) ? -2 : -1;
    if (!w || vt_of(w) != VT_SLIDER || !(mask & 12) || held < 800) {
        return;
    }
    range = *(int *)((char *)w + 0xd0) - *(int *)((char *)w + 0xcc);
    extra = 1 << (int)((held - 800) / 700 > 7 ? 7 : (held - 800) / 700);
    if (extra > range / 40) {
        extra = range / 40;
    }
    for (i = 1; i < extra; i++) {        /* the pulse itself is one step */
        ((w_call1)vt_slot(w, 0x3c))(w, 0, arg);
    }
}

static int repeat(rep_t *r, int mask)
{
    DWORD now = timeGetTime(), held, interval;
    if (mask != r->mask) {
        r->mask = mask;
        r->t0 = r->tlast = now;
        r->out = mask;
        return mask;
    }
    if (!mask) {
        r->out = 0;
        return 0;
    }
    held = now - r->t0;
    if (held < 400) {
        r->out = mask;
        return mask;
    }
    if (r->out) {                        /* release first, so the game sees a new press */
        r->out = 0;
        r->tlast = now;
        return 0;
    }
    interval = held < 1200 ? 110 : held < 2500 ? 70 : 40;
    if (now - r->tlast >= interval) {
        r->out = mask;
        slider_boost(mask, held);
        return mask;
    }
    return 0;
}

/* ---- text entry ---- */

static const char TE_SET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .-_:/'";
#define TE_N ((int)sizeof TE_SET - 1)
static const int TE_GROUPS[] = { 0, 26, 52, 62 };    /* A, a, 0, space */

/* the call to the line editor at 0x437a50 (cdecl): lets us know it is running. It returns on Enter/Esc */
static int (*real_edit)(char *, unsigned, void *, int, unsigned) = (int (*)(char *, unsigned, void *, int, unsigned))0x47dab0;

static int edit_wrapper(char *buf, unsigned maxlen, void *redraw, int input, unsigned cursor)
{
    int r;
    g_in_edit = 1;
    r = real_edit(buf, maxlen, redraw, input, cursor);
    g_in_edit = 0;
    return r;
}

static int te_active, te_release_wait, te_pending, te_idx, te_prev, te_prev2;
static int te_clear, te_done;
static DWORD te_t[16], te_tl[16];          /* repeat timers per control */
enum { C_UP, C_DOWN, C_LEFT, C_RIGHT, C_A, C_B, C_X, C_Y, C_LB, C_RB, C_START, C_LS, C_N };
static int te_held[C_N];

static void kq_push(unsigned code)
{
    if (dk_unit) {
        dk_log("gamepad: key code %04x", code);
    }
    if (KQ_ON && KQ_COUNT < 0x1e) {
        KQ[KQ_COUNT++] = (uint16_t)code;
    }
}

static int te_accepts(int ch)
{
    int (*f)(int, unsigned, char *) = EDIT_FILTER;
    return f ? f(ch, 0, NULL) != 0 : 1;
}

/* index of the next accepted character from idx in direction dir, or -1 */
static int te_next(int idx, int dir)
{
    int i, j = idx;
    for (i = 0; i < TE_N; i++) {
        j = (j + dir + TE_N) % TE_N;
        if (te_accepts(TE_SET[j])) {
            return j;
        }
    }
    return -1;
}

static void te_put(int idx)
{
    if (te_pending) {
        kq_push(0x0e08);                 /* backspace: the pending character is replaced */
    }
    kq_push((unsigned char)TE_SET[idx]);
    te_idx = idx;
    te_pending = 1;
}

static void te_cycle(int dir)
{
    int idx;
    if (!te_pending) {
        int start = 0;                   /* first character: A; after a letter: a; after a digit: 0 */
        if (te_prev >= 'A' && te_prev <= 'Z') {
            start = 26;
        } else if (te_prev >= 'a' && te_prev <= 'z') {
            start = 26;
        } else if (te_prev >= '0' && te_prev <= '9') {
            start = 52;
        } else if ((te_prev == '.' || te_prev == ':' || te_prev == '/') && te_prev2 >= '0' && te_prev2 <= '9') {
            start = 52;                  /* 127.0.0.1, 12:30 */
        }
        idx = te_accepts(TE_SET[start]) ? start : te_next(start, 1);
        if (idx < 0) {
            return;
        }
        te_put(idx);
        return;
    }
    idx = te_next(te_idx, dir);
    if (idx >= 0) {
        te_put(idx);
    }
}

static void te_group(int dir)
{
    int g = 0, i, idx;
    for (i = 0; i < 4; i++) {
        if (te_idx >= TE_GROUPS[i]) {
            g = i;
        }
    }
    g = (g + dir + 4) % 4;
    for (i = 0; i < 4; i++, g = (g + dir + 4) % 4) {
        idx = TE_GROUPS[g];
        if (te_accepts(TE_SET[idx])) {
            te_put(idx);
            return;
        }
    }
}

static void te_commit(void)
{
    if (te_pending) {
        te_prev2 = te_prev;
        te_prev = (unsigned char)TE_SET[te_idx];
        te_pending = 0;
    }
}

static void te_ctl(int c, int down, DWORD now)
{
    int fire = 0;
    if (down && !te_held[c]) {
        fire = 1;
        te_t[c] = now;
        te_tl[c] = now;
    } else if (down && (c == C_UP || c == C_DOWN || c == C_LEFT) && now - te_t[c] > 400
               && now - te_tl[c] > 90) {
        fire = 1;
        te_tl[c] = now;
    }
    te_held[c] = down;
    if (!fire) {
        return;
    }
    switch (c) {
    case C_UP: te_cycle(1); break;
    case C_DOWN: te_cycle(-1); break;
    case C_RIGHT: te_commit(); break;
    case C_LEFT:
        kq_push(0x0e08);
        if (te_pending) {
            te_pending = 0;
        }
        te_prev = te_prev2 = 0;
        break;
    case C_X:                            /* a space */
        te_commit();
        if (te_accepts(' ')) {
            kq_push(' ');
            te_prev2 = te_prev;
            te_prev = ' ';
        }
        break;
    case C_Y:                            /* a dot (IP addresses, file names) */
        te_commit();
        if (te_accepts('.')) {
            kq_push('.');
            te_prev2 = te_prev;
            te_prev = '.';
        }
        break;
    case C_LS:                           /* clear the field: End, then backspaces over the next frames */
        te_pending = 0;
        te_prev = te_prev2 = 0;
        kq_push(0x4fe0);
        te_clear = 24;
        break;
    case C_LB: te_group(-1); break;
    case C_RB: te_group(1); break;
    /* the line editor ignores Enter and Esc here (its key translator 0x437700 blanks them): the text is
       live in the field and stays when the focus leaves it. A/Start = done, on to the next widget */
    case C_A:
    case C_START:
        te_done = 2;
        break;
    case C_B:
        te_done = 1;
        break;
    }
}

static void text_start(void)
{
    int c;
    te_pending = 0;
    te_prev = te_prev2 = 0;
    te_clear = 0;
    for (c = 0; c < C_N; c++) {
        te_held[c] = 1;                  /* the A that started it is still down */
    }
    te_active = 1;
    te_done = 0;
    g_edit_widget = focus_widget();
    dk_log("gamepad: text entry on");
}

static void text_frame(void)
{
    DWORD now = timeGetTime();
    int any = 0, i, c;
    int in[C_N];
    memset(in, 0, sizeof in);
    for (i = 0; i < PAD_MAX; i++) {
        SDL_GameController *gc = pad_controller(i);
        int ly;
        if (!gc) {
            continue;
        }
        ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        in[C_UP] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -16000;
        in[C_DOWN] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 16000;
        ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        in[C_LEFT] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ly < -16000;
        in[C_RIGHT] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ly > 16000;
        in[C_A] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
        in[C_B] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B);
        in[C_X] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X);
        in[C_Y] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y);
        in[C_LB] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        in[C_RB] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        in[C_START] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START);
        in[C_LS] |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    }
    for (c = 0; c < C_N; c++) {
        any |= in[c];
    }
    if (te_active && (!g_in_edit || focus_widget() != g_edit_widget || te_done)) {
        if (te_done == 2 && focus_widget() == g_edit_widget) {
            ((void (*)(void))0x449f70)();        /* focus the next widget (what D-pad down does) */
        }
        te_active = 0;
        te_done = 0;
        te_release_wait = 1;             /* keep the pad off the menu until every button is up */
        dk_log("gamepad: text entry off");
    }
    if (te_release_wait && !any) {
        te_release_wait = 0;
    }
    if (!te_active) {
        return;
    }
    if (te_clear > 0 && KQ_COUNT == 0) {
        kq_push(0x0e08);
        te_clear--;
    }
    for (c = 0; c < C_N; c++) {
        te_ctl(c, in[c], now);
    }
}

/* ---- control binding prompt ---- */

static int bind_escape_down, g_enter_down, esc_sent, g_bind_mask;

static int bind_stage2(void) { return BIND_TASK && !FE_ACCEPT; }

static int any_pad_input(void)
{
    int i;
    for (i = 0; i < PAD_MAX; i++) {
        SDL_GameController *gc = pad_controller(i);
        int b;
        if (!gc) {
            continue;
        }
        for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) {
                return 1;
            }
        }
        if (abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)) > 8000
            || abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)) > 8000) {
            return 1;
        }
    }
    return 0;
}

static void bind_frame(void)
{
    if (g_bind_mask && (!BIND_TASK || (bind_stage2() && !any_pad_input()))) {
        g_bind_mask = 0;
    }
    if (bind_stage2()) {                 /* waiting for the button; B cancels (Esc) */
        int b = pad_any_button(SDL_CONTROLLER_BUTTON_B);
        if (b && !bind_escape_down) {
            pad_post_key(VK_ESCAPE, 0);
            esc_sent = 1;
        } else if (!b && esc_sent) {
            pad_post_key(VK_ESCAPE, 1);
            esc_sent = 0;
        }
        bind_escape_down = b;
    } else {
        bind_escape_down = 1;            /* the B that cancelled is still down: not a new press */
    }
}

/* A on the focused widget */
static void on_a_pressed(void)
{
    void *w = focus_widget();
    if (dk_unit) {
        dk_log("gamepad: A on widget %p class %x bind task %x accept %d", w, (unsigned)vt_of(w), (unsigned)BIND_TASK, FE_ACCEPT);
    }
    if (!w || te_active || te_release_wait) {
        return;
    }
    if (vt_of(w) == VT_TEXT) {
        if (!g_in_edit) {
            ((w_call0)vt_slot(w, 0x48))(w, 0);       /* restart the line editor on this field */
        }
        if (g_in_edit) {
            text_start();
        }
    } else if (vt_of(w) == VT_BIND && BIND_TASK && FE_ACCEPT) {
        pad_post_key(VK_RETURN, 0);      /* stage 1: the "click"; the key goes up when A does */
        g_enter_down = 1;
        g_bind_mask = 1;                 /* hide the A (and the stick) from the prompt until they are up */
    }
}

/* ---- the pad as the player's controller ---- */

#define CTL_CHOICE (*(volatile int *)0x8002d8)      /* the options' controller: 1 keyboard, 3.. = joystick 0.. */
static int g_auto_pad = 1, g_auto_done;

/* the first time a pad is used in a menu while the controller is still the keyboard (the default), the
   pad becomes the player's controller, so a pad-only player doesn't have to find Options - Controls */
static void auto_select(void)
{
    int i;
    if (!g_auto_pad || g_auto_done || !pad_frontend() || CTL_CHOICE != 1) {
        return;
    }
    if (!any_pad_input()) {
        return;
    }
    g_auto_done = 1;
    for (i = 0; i < PAD_MAX; i++) {
        if (pad_controller(i)) {
            int joy = pad_joystick_for_slot(i);
            if (joy >= 0 && joy < 4) {
                CTL_CHOICE = 3 + joy;
                dk_log("gamepad: pad used in the menus: controller set to joystick %d", joy + 1);
            }
            return;
        }
    }
}

/* ---- per frame ---- */

/* Network lobby: the "ready" tick next to your own name only reacts to the mouse. Y toggles it: what the tick's
   click does (0x4591d8: FUN_004406d0(slot, !MP_IsSlotReady(slot)), thiscall 0x4898b0 on 0x7045c8) */
static void lobby_ready_toggle(void)
{
    int slot = *(volatile int *)0x524c34;                  /* DN_g_LocalSlot */
    int ready = ((char (__attribute__((fastcall)) *)(void *, int, int))0x4898b0)((void *)0x7045c8, 0, slot);
    dk_log("gamepad: lobby ready %d -> %d (slot %d)", ready != 0, ready == 0, slot);
    ((void (*)(int, int))0x4406d0)(slot, ready == 0);
}

/* Start in a menu: resumes the pause menu (the game only pauses on Escape, it doesn't unpause). 1 = used */
int padfe_start(void)
{
    void *w;
    if (!g_on || !pad_frontend() || te_active || te_release_wait || bind_stage2() || !FE_ACTIVE || !FE_ACCEPT
        || FE_MODAL || !is_screen("Pause Screen")) {
        return 0;
    }
    w = back_override();
    if (w) {
        widget_press(w);
    }
    return 1;
}

void padfe_frame(void)
{
    int a, b, y;
    if (!g_on) {
        return;
    }
    if (g_in_edit && vt_of(focus_widget()) != VT_TEXT) {
        g_in_edit = 0;                   /* the focus left the field: the game ended the editor */
    }
    auto_select();
    text_frame();
    bind_frame();
    a = pad_any_button(SDL_CONTROLLER_BUTTON_A);
    if (a && !g_a_prev && pad_frontend()) {
        on_a_pressed();
    }
    if (g_enter_down && !a) {
        g_enter_down = 0;
        pad_post_key(VK_RETURN, 1);
    }
    g_a_prev = a;
    y = pad_any_button(SDL_CONTROLLER_BUTTON_Y);
    if (y && !g_y_prev && pad_frontend() && !te_active && !te_release_wait && FE_ACTIVE && FE_ACCEPT && !FE_MODAL
        && is_screen("Network Lobby Screen")) {
        lobby_ready_toggle();
    }
    g_y_prev = y;
    b = pad_any_button(SDL_CONTROLLER_BUTTON_B);
    if (b && !g_back_btn_prev && pad_frontend() && !te_active && !te_release_wait && !bind_stage2()) {
        do_back();
    }
    g_back_btn_prev = b;
}

void padfe_filter(int slot, double *x, double *y, unsigned char *btn)
{
    int mask, out;
    if (!g_on) {
        return;
    }
    if (te_active || te_release_wait || g_bind_mask) {      /* the pad edits text: the menu sees a neutral joystick */
        *x = *y = 0;
        memset(btn, 0, PAD_GAME_BUTTONS);
        return;
    }
    if (bind_stage2()) {
        btn[5] = 0;                          /* B = cancel, never a binding */
    }
    if (g_virt_a1) {
        DWORD now = timeGetTime();
        if ((int)(now - g_virt_a0) >= 0 && (int)(g_virt_a1 - now) > 0) {
            btn[2] = 0x80;
        } else if ((int)(g_virt_a1 - now) <= 0) {
            g_virt_a0 = g_virt_a1 = 0;
        }
    }
    if (!pad_frontend() || slot < 0 || slot >= PAD_MAX) {
        g_rep[slot < 0 || slot >= PAD_MAX ? 0 : slot].mask = 0;
        return;
    }
    mask = (*y < -0.5 ? 1 : 0) | (*y > 0.5 ? 2 : 0) | (*x < -0.5 ? 4 : 0) | (*x > 0.5 ? 8 : 0);
    out = repeat(&g_rep[slot], mask);
    if (out != mask) {                       /* a pulse gap: no direction */
        *x = *y = 0;
    }
}

void padfe_install(void)
{
    g_auto_pad = dk_ini_int("Gamepad", "AutoSelect", 1);
    static const unsigned char call_old[] = { 0xe8, 0x5b, 0x60, 0x04, 0x00 };    /* call 0x47dab0 at 0x437a50 */
    unsigned char call_new[5];
    uint32_t rel = (uint32_t)(uintptr_t)edit_wrapper - (0x437a50 + 5);
    call_new[0] = 0xe8;
    memcpy(call_new + 1, &rel, 4);
    if (!dk_patch(0x437a50, call_old, call_new, 5)) {
        dk_log("gamepad: line editor call not found, pad text entry off");
        return;
    }
    g_on = 1;
}
