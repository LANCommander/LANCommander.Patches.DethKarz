/* Frame interpolation (docs/framerate.md): frames drawn between the game's fixed ticks.
 *
 * The game simulates at a fixed tick (60 Hz with the patch, tickrate.c; network games too, which run in lockstep) and
 * draws at most one frame per tick. In a race this module keeps the tick and draws as many frames as [Graphics] MaxFps
 * allows (0 = the desktop refresh rate, -1 = no limit, 60 or less = none in between: the original's one per tick;
 * Options - Graphics "max framerate", gfxmenu.c), each showing the world between the last two ticks: alpha = how far
 * the clock is into the current tick. The picture runs one tick behind the simulation, as in every engine that
 * interpolates.
 *
 * - The game's own frame (GAME_TickEnd's GFX_RenderFrame call, 0x48f16a) is drawn exactly as the original draws it,
 *   at the tick's state with all its side effects, but its buffer swap is skipped (the device DLL's grBufferSwap import
 *   is hooked, dk_interp_device_loaded): only interpolated frames are shown. After it the B3D entity table (0x89af54,
 *   count 0x89af60) is snapshotted: position +0x54, quaternion +0x60, and for cameras (vtable 0x4f2038, whose matrix
 *   rows +0x70/+0x80/+0x90 the camera code sets directly) the rows; also the sprites and particles (points_*). Taken
 *   after the render, the snapshot includes what the render sets itself (it poses the wheels of the cars in view and
 *   adjusts the car bodies a little, every frame).
 * - An interpolated frame puts every entity that moved at lerp(position) / nlerp(quaternion) and flags it 0x40/0x80
 *   (+0x10), so the render's dirty pass (FUN_004a4dc0) rebuilds its matrix (+0x70); cameras get lerped, re-
 *   orthonormalised rows and their view rebuilt (CAM_BuildView). Afterwards everything it changed is put back,
 *   whatever the render wrote over it. The first one is drawn right after the hidden game frame, the rest in
 *   GAME_TickEnd's FRAME_WaitForTick call (0x48f16f) while the tick deadline allows, through GFX_RenderFrame past its
 *   FRAME_ShouldRender gate (0x47639d). Their side effects are undone (KEEP): the random generator (0x808004, which
 *   the render path advances), the race frame counter (0x806c54, alternate-frame flicker), HUD timers, and the tick
 *   length (0x521cf0 = 0 while drawing, so render-time "timer -= dt" code stands still). So the simulation sees the
 *   original's render, once per tick, and lockstep network play keeps its exact state (checked with DKI_HASH:
 *   entity state bit-identical with and without, standing, driving and in a network race).
 * Entities that jump (respawn), appear, or are posed by the render itself (the wheels of the cars in view) are drawn
 * where they are. Menus and the pause menu (the tick count stands still) are drawn as before.
 *
 * Test switches (environment): DKI_HASH=1..4 logs state hashes every 150 ticks (2: every entity, 3: with matrices,
 * 4: hierarchies only, which match across the machines of a network race), DKI_DIFF logs what an in-between frame
 * writes, DKI_TRACE=n logs n tick waits (frames, frame time) and -n n shown frames (camera, nearest car), DKI_PROF=s
 * samples the game thread for s seconds, DKI_SKIPEVERY=n makes FRAME_ShouldRender decline every n-th tick. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dkpatch.h"

#define ENT_TABLE       (*(uintptr_t **)0x89af54)
#define ENT_COUNT       (*(volatile int *)0x89af60)
#define RACE_VIEWPORT   (*(uintptr_t *)0x510b74)
#define RACE_SIM        (*(volatile char *)0x807258 == 0)   /* GAME_SimulateTick's race branch */
#define TICK_COUNT      (*(volatile uint32_t *)0x806c50)
#define TICK_DT         (*(volatile float *)0x521cf0)
#define NEXT_TICK_US    (*(volatile uint32_t *)0x806bb8)
#define TICK_US         (*(volatile uint32_t *)0x806b44)
#define FRAME_MODE      (*(volatile int *)0x507a34)           /* 0 15fps, 1 30, 2 60, 3 no wait (catch-up), 4 */
#define RENDER_ON       (*(volatile char *)0x4fc348)          /* the device renders (cleared while catching up) */
#define RACE_LAYER      (*(volatile char *)0x4fc349)          /* render layer 1: the race */

#define VT_CAMERA       0x4f2038u                             /* B3D camera entities */

#define F_ROT 0x40      /* entity flags: rebuild the matrix from the quaternion */
#define F_POS 0x80      /*               copy the position into it */

static void (*const GAME_SimulateTick)(void) = (void (*)(void))0x48f1b0;
static int (*const GFX_RenderFrame)(void) = (int (*)(void))0x476390;
static int (*const GFX_RenderNow)(void) = (int (*)(void))0x47639d;   /* GFX_RenderFrame past FRAME_ShouldRender */
static void (*const FRAME_WaitForTick)(void) = (void (*)(void))0x48cdd0;
static void (__attribute__((fastcall)) *const CAM_BuildView)(uintptr_t cam) = (void (__attribute__((fastcall)) *)(uintptr_t))0x497090;
static void (*const WIN_PumpMessages)(void) = (void (*)(void))0x471f20;
static uint32_t (__attribute__((thiscall)) *const timer_us)(void *) = (uint32_t (__attribute__((thiscall)) *)(void *))0x483410;

static int g_on;                     /* the hooks are installed */
static int g_enabled;                /* frames in between: the cap is above the tick rate */
static int g_setting;                /* [Graphics] MaxFps as configured */
static int g_diff, g_hash, g_trace;  /* test switches, see above */
static int g_max_fps;                /* ghost frame cap (-1 = none) */
static uint32_t g_frame_us;          /* 1e6 / g_max_fps */
static unsigned g_stat_ticks, g_stat_frames, g_stat_real;
static uint32_t g_last_frame;        /* when the last frame (the game's or an in-between one) started */

static void points_snapshot(void);
static void points_apply(float a);
static int g_nptsaved;
static int g_last_moved;             /* entities the last frame interpolated (DKI_TRACE) */

/* ---- snapshots ---- */

typedef struct {
    uintptr_t e;
    float s[7];                      /* +0x54 position, +0x60 quaternion (w x y z) */
    float m[12];                     /* cameras: +0x70 rows x, y, z (4 floats each, w unused) */
    int cam;
} snap_t;

static snap_t *g_snap[2];
static int g_n[2], g_cap;
static int g_cur;                    /* g_snap[g_cur] = last tick, g_snap[!g_cur] = the one before */
static int g_valid;                  /* the two snapshots are consecutive race ticks */
static uint32_t g_tick_seen;
static uint32_t g_base_us;           /* wall time (game timer, us) where the last tick's interval starts */

static void snapshot(void)
{
    int n = ENT_COUNT, i, prev = g_cur;
    uintptr_t *tab = ENT_TABLE;
    snap_t *s;
    if (n > g_cap) {
        int cap = n + 256;
        snap_t *a = realloc(g_snap[0], cap * sizeof(snap_t)), *b;
        if (a) {
            g_snap[0] = a;
        }
        b = a ? realloc(g_snap[1], cap * sizeof(snap_t)) : NULL;
        if (!a || !b) {
            g_valid = 0;
            return;
        }
        g_snap[1] = b;
        g_cap = cap;
    }
    g_cur = !g_cur;
    s = g_snap[g_cur];
    for (i = 0; i < n; i++) {
        s[i].e = tab ? tab[i] : 0;
        if (s[i].e) {
            memcpy(s[i].s, (void *)(s[i].e + 0x54), sizeof s[i].s);
            s[i].cam = *(uint32_t *)s[i].e == VT_CAMERA;
            if (s[i].cam) {
                memcpy(s[i].m, (void *)(s[i].e + 0x70), sizeof s[i].m);
            }
        }
    }
    g_n[g_cur] = n;
    points_snapshot();
    g_valid = g_tick_seen && TICK_COUNT == g_tick_seen + 1 && g_n[prev] > 0;
    g_tick_seen = TICK_COUNT;
    g_base_us = NEXT_TICK_US - TICK_US;
}

static void log_hash(void)
{
    int n = ENT_COUNT, i;
    uintptr_t *tab = ENT_TABLE;
    uint32_t h = 2166136261u;
    for (i = 0; tab && i < n; i++) {
        const uint8_t *p = tab[i] ? (const uint8_t *)(tab[i] + 0x54) : NULL;
        int k;
        if (g_hash == 4 && p && *(uint32_t *)tab[i] != 0x4f2570u) {
            continue;   /* hierarchies only (cars): the same on every machine of a network race */
        }
        for (k = 0; p && k < (g_hash > 2 ? 0x5c : 28); k++) {
            h = (h ^ p[k]) * 16777619u;
        }
    }
    {
        uint32_t hp = 2166136261u;
        const uint8_t *q = (const uint8_t *)0x8b1018;
        int e, j, k;
        for (k = 0; k < 256; k++) {
            for (j = 0; (q[k * 0x58 + 0x54] & 3) == 3 && j < 12; j++) {
                hp = (hp ^ q[k * 0x58 + 4 + j]) * 16777619u;
            }
        }
        for (e = 0; e <= *(volatile int *)0x8c28ac && e < 256; e++) {
            const uint8_t *em = (const uint8_t *)(0x8b6888u + e * 0xc0);
            const uint8_t *parts = *(const uint8_t **)(em + 0x1c);
            int cnt = *(const int *)(em + 0x14);
            for (j = 0; (em[0xbc] & 1) && parts && j < cnt * 0x24; j++) {
                if (j % 0x24 >= 4 && parts[j - j % 0x24]) {
                    hp = (hp ^ parts[j]) * 16777619u;
                }
            }
        }
        dk_log("interp: tick %u hash %08x points %08x (%d entities, rng %04x)", TICK_COUNT, h, hp, n,
               *(volatile uint32_t *)0x808004);
    }
    if (g_hash > 1) {
        for (i = 0; tab && i < n; i++) {
            const float *f = tab[i] ? (const float *)(tab[i] + 0x54) : NULL;
            if (f) {
                dk_log("ent %d %08x vt %08x fl %08x %.9g %.9g %.9g %.9g %.9g %.9g %.9g", i, (unsigned)tab[i],
                       *(unsigned *)tab[i], *(unsigned *)(tab[i] + 0x10), f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            }
        }
    }
}


/* ---- interpolated transforms ---- */

typedef struct {
    uintptr_t e;
    uint32_t flags;
    uint8_t block[0x5c];             /* +0x54..+0xb0: position, quaternion, matrix */
} saved_t;

static saved_t *g_saved;
static int g_nsaved, g_savecap;

#define MAX_CAMS 8
static struct {
    uintptr_t cam;
    uint8_t block[0x1b0];            /* the camera up to its matrices (view +0xe4, projection +0x124, last +0x168) */
} g_camsaved[MAX_CAMS];
static int g_ncamsaved;

static float clamp01(float a) { return a < 0 ? 0 : a > 1 ? 1 : a; }

static void normalize3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* a camera: the game sets its matrix rows itself (the quaternion stays 1 0 0 0). Rows lerped, then made orthonormal
   again (z first, then y, then x), so the handedness stays; position copied into the matrix, view rebuilt */
static void interp_camera(const snap_t *p, const snap_t *c, float a)
{
    uintptr_t cam = c->e;
    float r[3][3], pos[3], d[3];
    int i, k;
    if (g_ncamsaved >= MAX_CAMS || memcmp((void *)(cam + 0x70), c->m, sizeof c->m)) {
        return;     /* the render moved it itself */
    }
    for (k = 0; k < 3; k++) {
        d[k] = c->s[k] - p->s[k];
        pos[k] = p->s[k] + d[k] * a;
    }
    /* a cut: more than 12 units or 35 degrees in one 60 Hz tick (cars move ~1..4 units per tick). It was 50 units /
       60 degrees at 30 Hz, and a respawn's ~27-unit camera cut got smeared over the tick while the car (> 25 units:
       not interpolated) was already there (harness trace, 2026-10-21). */
    if (dot3(d, d) > 12.0f * 12.0f || dot3(p->m + 8, c->m + 8) < 0.82f) {
        return;
    }
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 3; k++) {
            r[i][k] = p->m[i * 4 + k] + (c->m[i * 4 + k] - p->m[i * 4 + k]) * a;
        }
    }
    normalize3(r[2]);
    {
        float d2 = dot3(r[1], r[2]);
        for (k = 0; k < 3; k++) {
            r[1][k] -= r[2][k] * d2;
        }
    }
    normalize3(r[1]);
    {
        float d0 = dot3(r[0], r[2]), d1 = dot3(r[0], r[1]);
        for (k = 0; k < 3; k++) {
            r[0][k] -= r[2][k] * d0 + r[1][k] * d1;
        }
    }
    normalize3(r[0]);
    g_camsaved[g_ncamsaved].cam = cam;
    memcpy(g_camsaved[g_ncamsaved].block, (void *)cam, sizeof g_camsaved[0].block);
    g_ncamsaved++;
    for (i = 0; i < 3; i++) {
        memcpy((void *)(cam + 0x70 + i * 16), r[i], 12);
    }
    memcpy((void *)(cam + 0x54), pos, 12);
    memcpy((void *)(cam + 0xa0), pos, 12);
    *(volatile uint32_t *)(cam + 0x10) |= F_POS;
    CAM_BuildView(cam);
}

/* every entity that moved between the two ticks, at alpha a */
static uint32_t now_us(void);

static void apply(float a)
{
    const snap_t *p = g_snap[!g_cur], *c = g_snap[g_cur];
    uintptr_t *tab = ENT_TABLE;
    int n = g_n[g_cur] < g_n[!g_cur] ? g_n[g_cur] : g_n[!g_cur], i, k;
    g_nsaved = 0;
    g_ncamsaved = 0;
    if (n > ENT_COUNT) {
        n = ENT_COUNT;
    }
    if (n > g_savecap) {
        saved_t *s = realloc(g_saved, (n + 256) * sizeof(saved_t));
        if (!s) {
            return;
        }
        g_saved = s;
        g_savecap = n + 256;
    }
    for (i = 0; i < n; i++) {
        uintptr_t e = c[i].e;
        float v[7], d[3];
        int rot;
        if (!e || p[i].e != e || tab[i] != e || memcmp((void *)(e + 0x54), c[i].s, sizeof c[i].s)) {
            continue;
        }
        if (c[i].cam) {
            if (p[i].cam && (memcmp(p[i].s, c[i].s, 12) || memcmp(p[i].m, c[i].m, sizeof c[i].m))) {
                interp_camera(&p[i], &c[i], a);
            }
            continue;
        }
        if (!memcmp(p[i].s, c[i].s, sizeof c[i].s)) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            d[k] = c[i].s[k] - p[i].s[k];
        }
        if (dot3(d, d) > 12.0f * 12.0f) {
            continue;   /* teleported (respawn, reset); the camera's cut limit (interp_camera) is the same */
        }
        for (k = 0; k < 3; k++) {
            v[k] = p[i].s[k] + d[k] * a;
        }
        rot = memcmp(p[i].s + 3, c[i].s + 3, 16) != 0;
        if (rot) {
            /* nlerp along the shorter way */
            float dq = p[i].s[3] * c[i].s[3] + p[i].s[4] * c[i].s[4] + p[i].s[5] * c[i].s[5] + p[i].s[6] * c[i].s[6];
            float l = 0, s;
            for (k = 3; k < 7; k++) {
                float q0 = dq < 0 ? -p[i].s[k] : p[i].s[k];
                v[k] = q0 + (c[i].s[k] - q0) * a;
                l += v[k] * v[k];
            }
            s = l > 1e-12f ? 1.0f / sqrtf(l) : 1.0f;
            for (k = 3; k < 7; k++) {
                v[k] *= s;
            }
        } else {
            memcpy(v + 3, c[i].s + 3, 16);
        }
        g_saved[g_nsaved].e = e;
        g_saved[g_nsaved].flags = *(volatile uint32_t *)(e + 0x10);
        memcpy(g_saved[g_nsaved].block, (void *)(e + 0x54), sizeof g_saved[0].block);
        g_nsaved++;
        memcpy((void *)(e + 0x54), v, sizeof v);
        *(volatile uint32_t *)(e + 0x10) |= rot ? F_ROT | F_POS : F_POS;
    }
    points_apply(a);
    g_last_moved = g_nsaved;
    if (g_trace < 0 && g_ncamsaved) {
        /* DKI_TRACE negative: one line per frame, the camera and the nearest interpolated hierarchy (the player's car) */
        const float *cp = (const float *)(g_camsaved[0].cam + 0x54), *best = NULL;
        uintptr_t be = 0;
        float bd = 1e30f;
        for (i = 0; i < g_nsaved; i++) {
            const float *ep = (const float *)(g_saved[i].e + 0x54);
            float d[3] = { ep[0] - cp[0], ep[1] - cp[1], ep[2] - cp[2] };
            if (*(uint32_t *)g_saved[i].e == 0x4f2570u && dot3(d, d) < bd) {
                bd = dot3(d, d);
                best = ep;
                be = g_saved[i].e;
            }
        }
        g_trace++;
        dk_log("frame t=%u tick=%u a=%.3f cam %.3f %.3f %.3f car %.3f %.3f %.3f e %08x", now_us(), TICK_COUNT, a, cp[0],
               cp[1], cp[2], best ? best[0] : 0, best ? best[1] : 0, best ? best[2] : 0, (unsigned)be);
    }
}

/* ---- points: sprites and particles, positioned by the tick like the entities ----
   Sprites (lights, flares, glows; FUN_004b1d40 sets the position): 256 at 0x8b1018, 0x58 bytes, position +4, drawn
   when (+0x54 & 3) == 3 (FUN_004b21c0). Particle emitters: 0x8b6888, 0xc0 bytes, the last index at 0x8c28ac, active
   when +0xbc & 1; particles at +0x1c (count +0x14, 0x24 bytes: +0 alive, +4 position, +0x10 velocity, +0x1c age),
   optional frames at +0x20 (0x30 bytes, position +0x18). FUN_004b0a50 moves them by the tick length. A particle is
   the same one in both ticks when its age grew; a new one is moved back along its velocity instead. */
#define SPRITES      0x8b1018u
#define SPRITE_N     256
#define EMITTERS     0x8b6888u
#define EMITTER_LAST (*(volatile int *)0x8c28ac)
#define EMITTER_MAX  256

typedef struct {
    float *at, *at2;                 /* live position (NULL: not drawn), frame position */
    float pos[3], pos2[3], age;
} pt_t;

typedef struct {
    uintptr_t parts;
    int count, start;
} em_t;

typedef struct {
    float *at;
    float orig[3];
} ptsave_t;

static pt_t *g_pt[2];
static int g_npt[2], g_ptcap;
static em_t g_em[2][EMITTER_MAX];
static int g_nem[2];
static float g_dt;                   /* the last tick's length */
static ptsave_t *g_ptsaved;
static int g_nptsaved, g_ptsavecap;

static pt_t *pt_add(int k)
{
    if (g_npt[k] >= g_ptcap) {
        int cap = g_ptcap ? g_ptcap * 2 : 4096;
        pt_t *a = realloc(g_pt[0], cap * sizeof(pt_t)), *b;
        if (a) {
            g_pt[0] = a;
        }
        b = a ? realloc(g_pt[1], cap * sizeof(pt_t)) : NULL;
        if (b) {
            g_pt[1] = b;
        }
        if (!a || !b) {
            return NULL;
        }
        g_ptcap = cap;
    }
    return &g_pt[k][g_npt[k]++];
}

static void points_snapshot(void)
{
    int k = g_cur, i, j, last = EMITTER_LAST;
    pt_t *t;
    g_npt[k] = 0;
    g_nem[k] = 0;
    g_dt = TICK_DT;
    for (i = 0; i < SPRITE_N; i++) {
        uint8_t *s = (uint8_t *)(uintptr_t)(SPRITES + i * 0x58);
        if (!(t = pt_add(k))) {
            return;
        }
        t->at = (s[0x54] & 3) == 3 ? (float *)(s + 4) : NULL;
        t->at2 = NULL;
        if (t->at) {
            memcpy(t->pos, t->at, 12);
        }
    }
    for (i = 0; i <= last && i < EMITTER_MAX; i++) {
        uint8_t *em = (uint8_t *)(uintptr_t)(EMITTERS + i * 0xc0);
        em_t *e = &g_em[k][i];
        uintptr_t frames;
        e->parts = (em[0xbc] & 1) ? *(uintptr_t *)(em + 0x1c) : 0;
        e->count = e->parts ? *(int *)(em + 0x14) : 0;
        e->start = g_npt[k];
        frames = *(uintptr_t *)(em + 0x20);
        for (j = 0; j < e->count; j++) {
            uint8_t *p = (uint8_t *)(e->parts + j * 0x24);
            if (!(t = pt_add(k))) {
                e->count = j;
                break;
            }
            t->at = p[0] ? (float *)(p + 4) : NULL;
            t->at2 = p[0] && frames ? (float *)(frames + j * 0x30 + 0x18) : NULL;
            if (t->at) {
                memcpy(t->pos, t->at, 12);
                t->age = *(float *)(p + 0x1c);
            }
            if (t->at2) {
                memcpy(t->pos2, t->at2, 12);
            }
        }
        g_nem[k] = i + 1;
    }
}

static void pt_write(float *at, const float *v)
{
    if (g_nptsaved >= g_ptsavecap) {
        int cap = g_ptsavecap ? g_ptsavecap * 2 : 4096;
        ptsave_t *s = realloc(g_ptsaved, cap * sizeof(ptsave_t));
        if (!s) {
            return;
        }
        g_ptsaved = s;
        g_ptsavecap = cap;
    }
    g_ptsaved[g_nptsaved].at = at;
    memcpy(g_ptsaved[g_nptsaved].orig, at, 12);
    g_nptsaved++;
    memcpy(at, v, 12);
}

static void lerp3(float *out, const float *p, const float *c, float a)
{
    out[0] = p[0] + (c[0] - p[0]) * a;
    out[1] = p[1] + (c[1] - p[1]) * a;
    out[2] = p[2] + (c[2] - p[2]) * a;
}

static void points_apply(float a)
{
    const pt_t *pp = g_pt[!g_cur], *cp = g_pt[g_cur];
    int i, j;
    float v[3], d[3];
    g_nptsaved = 0;
    if (g_npt[g_cur] < SPRITE_N || g_npt[!g_cur] < SPRITE_N) {
        return;
    }
    for (i = 0; i < SPRITE_N; i++) {
        const pt_t *c = &cp[i], *p = &pp[i];
        if (!c->at || p->at != c->at || memcmp(c->at, c->pos, 12) || !memcmp(p->pos, c->pos, 12)) {
            continue;
        }
        d[0] = c->pos[0] - p->pos[0];
        d[1] = c->pos[1] - p->pos[1];
        d[2] = c->pos[2] - p->pos[2];
        if (dot3(d, d) < 25.0f * 25.0f) {
            lerp3(v, p->pos, c->pos, a);
            pt_write(c->at, v);
        }
    }
    for (i = 0; i < g_nem[g_cur] && i < g_nem[!g_cur]; i++) {
        const em_t *ce = &g_em[g_cur][i], *pe = &g_em[!g_cur][i];
        if (!ce->parts || pe->parts != ce->parts || pe->count != ce->count) {
            continue;
        }
        for (j = 0; j < ce->count; j++) {
            const pt_t *c = &cp[ce->start + j], *p = &pp[pe->start + j];
            if (!c->at || memcmp(c->at, c->pos, 12)) {
                continue;
            }
            if (p->at == c->at && c->age > p->age) {
                lerp3(v, p->pos, c->pos, a);
                pt_write(c->at, v);
                if (c->at2 && p->at2 == c->at2) {
                    lerp3(v, p->pos2, c->pos2, a);
                    pt_write(c->at2, v);
                }
            } else {
                /* born in the last tick: back along its velocity, to where the picture's time is */
                float back = (1.0f - a) * g_dt, *vel = c->at + 3;
                if (back > c->age) {
                    back = c->age;
                }
                if (back > 0) {
                    v[0] = c->pos[0] - vel[0] * back;
                    v[1] = c->pos[1] - vel[1] * back;
                    v[2] = c->pos[2] - vel[2] * back;
                    pt_write(c->at, v);
                }
            }
        }
    }
}

static void points_unapply(void)
{
    int i;
    for (i = g_nptsaved - 1; i >= 0; i--) {
        memcpy(g_ptsaved[i].at, g_ptsaved[i].orig, 12);
    }
    g_nptsaved = 0;
}

/* puts back everything an in-between frame changed, whatever its render wrote over it */
static void unapply(void)
{
    int i;
    points_unapply();
    for (i = g_nsaved - 1; i >= 0; i--) {
        uintptr_t e = g_saved[i].e;
        memcpy((void *)(e + 0x54), g_saved[i].block, sizeof g_saved[0].block);
        *(volatile uint32_t *)(e + 0x10) = g_saved[i].flags;
    }
    g_nsaved = 0;
    for (i = g_ncamsaved - 1; i >= 0; i--) {
        memcpy((void *)g_camsaved[i].cam, g_camsaved[i].block, sizeof g_camsaved[0].block);
    }
    g_ncamsaved = 0;
}

static uint32_t now_us(void) { return timer_us((void *)0x53d310); }

static float alpha_now(void)
{
    uint32_t tick = TICK_US;
    return tick ? clamp01((float)(int32_t)(now_us() - g_base_us) / (float)tick) : 1.0f;
}

/* ---- the game's frame is drawn but not shown: the device DLL's grBufferSwap import is hooked ---- */

static void (__stdcall *real_swap)(int interval);
static int g_have_swap;              /* the loaded device is the Glide one, its swap hooked */
static int g_hide_swap, g_swap_hidden, g_hidden_interval;

static unsigned g_stat_shown, g_stat_hidden, g_stat_skipped;

static void __stdcall hk_swap(int interval)
{
    if (g_hide_swap) {
        g_swap_hidden = 1;
        g_hidden_interval = interval;
        g_stat_hidden++;
        return;
    }
    g_stat_shown++;
    real_swap(interval);
}

void dk_interp_device_loaded(HMODULE device)
{
    void *old;
    if (!g_on || !device) {
        return;
    }
    old = dk_hook_import_in(device, "glide2x.dll", "_grBufferSwap@4", (void *)hk_swap);
    if (old && old != (void *)hk_swap) {
        real_swap = (void (__stdcall *)(int))old;
    }
    g_have_swap = old != NULL && real_swap != NULL;
    if (!g_have_swap) {
        dk_log("interp: the renderer isn't Glide, no frames in between");
    }
}

/* without a cheap hidden frame (nGlide), a race whose ticks keep starting late gets no in-between frames: switching
   between interpolated frames (a tick behind) and the game's own (current) ones would shake the picture */
static int g_suspended;
static unsigned g_win_ticks, g_win_late;

static int eligible(void)
{
    return g_enabled && !g_suspended && g_have_swap && RACE_SIM && RENDER_ON && RACE_LAYER && FRAME_MODE != 3 && RACE_VIEWPORT && ENT_TABLE;
}

static int active(void)
{
    return eligible() && g_valid;
}

/* ---- hooks ---- */

static int g_ticked;                 /* a race tick ran since the last snapshot */

static void hk_simulate(void)
{
    GAME_SimulateTick();
    if (RACE_SIM && ENT_TABLE) {
        g_ticked = 1;
        if (g_hash && TICK_COUNT % 150 == 0) {
            log_hash();
        }
    } else {
        g_ticked = 0;
        g_valid = 0;
        g_tick_seen = 0;
        g_suspended = 0;   /* a new race tries again */
        g_win_ticks = g_win_late = 0;
    }
}

/* globals the render path changes, kept as one render per tick left them */
static const struct { uint32_t addr, size; } KEEP[] = {
    { 0x808004, 4 },    /* random generator */
    { 0x806c54, 4 },    /* race frame counter (alternate-frame flicker) */
    { 0x51525c, 4 },    /* HUD wrong-way check timer */
    { 0x515254, 4 },    /* HUD message frames */
    { 0x510e58, 0x20 }, /* RACE_UpdateCameraMatrices: camera position copy, flare */
    { 0x510f40, 1 },
    { 0x511960, 4 },    /* FUN_00411560 (weather): an angle stepped once per render */
};

#define KEEP_BYTES 0x40

static void keep_save(uint8_t *buf)
{
    unsigned i, o;
    for (i = o = 0; i < sizeof KEEP / sizeof KEEP[0]; o += KEEP[i].size, i++) {
        memcpy(buf + o, (void *)(uintptr_t)KEEP[i].addr, KEEP[i].size);
    }
}

static void keep_restore(const uint8_t *buf)
{
    unsigned i, o;
    for (i = o = 0; i < sizeof KEEP / sizeof KEEP[0]; o += KEEP[i].size, i++) {
        memcpy((void *)(uintptr_t)KEEP[i].addr, buf + o, KEEP[i].size);
    }
}

static void ghost(float a);

/* The game's frame, exactly as the original draws it (at the tick's state, with all its side effects), but not shown
   when in-between frames take over: the snapshot is taken after it, so what the render sets itself (the wheels of the
   cars in view, small adjustments of the car bodies) belongs to the tick's state, and the first interpolated frame
   is shown in its place. If none can be drawn after all, the hidden frame is shown.
   With our OpenGL Glide the hidden frame draws nothing on the GPU (dk_opengl_discard), so a tick costs one render as in
   the original. A tick that starts late (FRAME_ShouldRender declines: the original then draws nothing to catch up)
   shows nothing new either: with discard the hidden frame still runs for the snapshot (CPU only), without it (nGlide)
   the interpolation restarts two ticks later. Before 2026-10-21 both were drawn on the GPU every tick, late or not, so
   under GPU load (4K with supersampling) the game itself ran slow. */
static int hk_render(void)
{
    int r, discard = 0, late = 0;
    if (!g_enabled) {
        g_valid = 0;
        g_tick_seen = 0;
        return GFX_RenderFrame();
    }
    g_hide_swap = eligible() && (g_valid || (g_ticked && g_tick_seen && TICK_COUNT == g_tick_seen + 1));
    g_swap_hidden = 0;
    if (g_trace > 0) {
        g_trace--;
        dk_log("render t=%u tick=%u ticked=%d hide=%d valid=%d elig=%d seen=%u", now_us(), TICK_COUNT, g_ticked,
               g_hide_swap, g_valid, eligible(), g_tick_seen);
    }
    if (g_hide_swap) {
        discard = dk_opengl_discard(1);
    }
    {
        static int every = -1;
        char v[8];
        if (every < 0) {
            every = GetEnvironmentVariableA("DKI_SKIPEVERY", v, sizeof v) ? atoi(v) : 0;
        }
        r = every > 0 && TICK_COUNT % every == 0 && eligible() ? 0 : GFX_RenderFrame();
    }
    if (!r && g_hide_swap) {
        late = 1;
        if (discard) {
            /* FRAME_ShouldRender declined (late): drawn anyway, hidden and on the CPU only, so that the snapshot always
               holds what the render sets (car wheel and body poses); without it the cars jerked. The original drew
               nothing, so the counters and the random generator are left as they were. */
            uint8_t kept[KEEP_BYTES];
            keep_save(kept);
            r = GFX_RenderNow();
            keep_restore(kept);
        } else {
            /* no cheap hidden frame: as the original, nothing drawn, and no interpolation across this tick */
            g_ticked = 0;
            g_valid = 0;
            g_tick_seen = 0;
        }
    }
    if (discard) {
        dk_opengl_discard(0);
    } else if (g_hide_swap && ++g_win_ticks >= 120) {
        if (g_win_late * 10 >= g_win_ticks) {
            g_suspended = 1;
            dk_log("interp: %u of %u ticks started late and this Glide has no cheap hidden frame: no frames in between for "
                   "the rest of this race", g_win_late, g_win_ticks);
        }
        g_win_ticks = g_win_late = 0;
    }
    g_win_late += late && !discard;
    g_hide_swap = 0;
    g_stat_real += r != 0 && !late;
    g_stat_skipped += late || (!r && eligible());
    if (g_ticked && RACE_SIM && ENT_TABLE) {
        snapshot();
        g_ticked = 0;
    }
    if (g_swap_hidden) {
        if (late) {
            /* the original shows nothing new for a late tick */
        } else if (active()) {
            g_last_frame = now_us();
            ghost(alpha_now());
            g_stat_frames++;
        } else if (discard) {
            /* nothing to interpolate after all, and the hidden frame wasn't drawn: draw it again, shown */
            uint8_t kept[KEEP_BYTES];
            keep_save(kept);
            GFX_RenderNow();
            keep_restore(kept);
        } else {
            real_swap(g_hidden_interval);
        }
    }
    return r;
}

/* ---- the render's own pose writes during an in-between frame ----
   Every drawn frame the render copies the visual objects' state into their entities (FUN_00477510 -> FUN_00406d20:
   position from object +0x1c, rotation from +0x28), through ENT_SetPosition 0x4a4c20 (+0x54, flag 0x80) and
   ENT_SetRotation 0x4a4cb0 (+0x60 quaternion, flag 0x40). That state is the tick's, so in an in-between frame it put
   the car hierarchies back where the tick left them while the camera stayed interpolated: near cars shook back and
   forth at the frame rate (reported 2026-10-21; found with DKI_CARDUMP). Both setters are replaced: in an in-between
   frame an entity that was interpolated keeps its value (the flag is still set, so the matrix is rebuilt from it). */
static int g_in_ghost;

static int interpolated(uintptr_t e)
{
    int i;
    for (i = 0; i < g_nsaved; i++) {
        if (g_saved[i].e == e) {
            return 1;
        }
    }
    return 0;
}

static uint32_t __attribute__((thiscall)) ent_set_position(uintptr_t e, const float *p)
{
    if (!g_in_ghost || !interpolated(e)) {
        memcpy((void *)(e + 0x54), p, 12);
    }
    *(volatile uint32_t *)(e + 0x10) |= F_POS;
    return (*(volatile uint32_t *)(e + 0x10) & ~0xffu) | 1;   /* as the original: eax = flags, al = 1 */
}

static uint32_t __attribute__((thiscall)) ent_set_rotation(uintptr_t e, const float *q)
{
    if (!g_in_ghost || !interpolated(e)) {
        memcpy((void *)(e + 0x60), q, 16);
    }
    *(volatile uint32_t *)(e + 0x10) |= F_ROT;
    return (*(volatile uint32_t *)(e + 0x10) & ~0xffu) | 1;   /* as the original: eax = flags, al = 1 */
}

/* DKI_CARDUMP: a hardware write breakpoint on the nearest car hierarchy's position during one in-between render,
   logging each writing instruction once with the return addresses into the exe */
static HANDLE g_bp_thread;
static uintptr_t g_bp_addr;
static void *g_bp_seen[16];
static int g_bp_nseen;

static LONG CALLBACK bp_veh(EXCEPTION_POINTERS *x)
{
    if (x->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (x->ContextRecord->Dr6 & 1)) {
        void *eip = (void *)x->ContextRecord->Eip;
        int i;
        for (i = 0; i < g_bp_nseen && g_bp_seen[i] != eip; i++) {
        }
        if (i == g_bp_nseen && g_bp_nseen < 16) {
            const uint32_t *sp = (const uint32_t *)x->ContextRecord->Esp;
            char calls[200];
            int j, o = 0, m = 0;
            g_bp_seen[g_bp_nseen++] = eip;
            calls[0] = 0;
            for (j = 0; j < 160 && m < 8; j++) {
                if (sp[j] > 0x401000 && sp[j] < 0x4e9000) {
                    o += snprintf(calls + o, sizeof calls - o, " %08x", (unsigned)sp[j]);
                    m++;
                }
            }
            dk_log("cardump: %08x written by the instruction before %p, stack%s", (unsigned)g_bp_addr, eip, calls);
        }
        x->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI bp_arm(LPVOID arg)
{
    CONTEXT c;
    SuspendThread(g_bp_thread);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(g_bp_thread, &c);
    c.Dr0 = g_bp_addr;
    c.Dr7 = arg ? 1 | (1u << 16) | (3u << 18) : 0;   /* L0, break on a 4-byte write */
    SetThreadContext(g_bp_thread, &c);
    ResumeThread(g_bp_thread);
    return 0;
}

static void bp_set(uintptr_t addr)
{
    static int veh;
    HANDLE t;
    if (!veh) {
        veh = 1;
        AddVectoredExceptionHandler(1, bp_veh);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_bp_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    }
    g_bp_addr = addr;
    t = CreateThread(NULL, 0, bp_arm, (LPVOID)(uintptr_t)(addr != 0), 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

/* DKI_CARDUMP=n (test switch): for n in-between frames after race tick 1500, every entity within 15 units of the camera:
   its position (+0x54) and matrix position (+0xa0) after the interpolation, after the render, and at the last tick */
static int g_cardump;

static void cardump(int phase)
{
    static struct { uintptr_t e; float p54[3], pa0[3]; } nb[48];
    static int n;
    const float *cp;
    int i, k;
    uintptr_t *tab = ENT_TABLE;
    if (!g_ncamsaved) {
        n = 0;
        return;
    }
    cp = (const float *)(g_camsaved[0].cam + 0x54);
    {
        /* the whole table: entities whose position or rotation the render changed */
        static float *all;
        static int nall;
        if (phase == 0) {
            nall = ENT_COUNT;
            all = realloc(all, (nall > 0 ? nall : 1) * 7 * sizeof(float));
            for (i = 0; all && i < nall; i++) {
                if (tab[i]) {
                    memcpy(all + i * 7, (const float *)(tab[i] + 0x54), 28);
                }
            }
        } else if (all) {
            int changed = 0, shown = 0;
            for (i = 0; i < nall && i < ENT_COUNT; i++) {
                if (tab[i] && memcmp(all + i * 7, (const float *)(tab[i] + 0x54), 28)) {
                    changed++;
                    if (shown < 6) {
                        const float *q = (const float *)(tab[i] + 0x54);
                        shown++;
                        dk_log("  render changed e %08x vt %08x %s pos %.3f %.3f %.3f -> %.3f %.3f %.3f", (unsigned)tab[i],
                               *(uint32_t *)tab[i], interpolated(tab[i]) ? "I" : "-", all[i * 7], all[i * 7 + 1],
                               all[i * 7 + 2], q[0], q[1], q[2]);
                    }
                }
            }
            dk_log("  render changed %d of %d entities", changed, nall);
        }
    }
    if (phase == 0) {
        n = 0;
        for (i = 0; i < ENT_COUNT && n < 48; i++) {
            const float *ep = tab[i] ? (const float *)(tab[i] + 0x54) : NULL;
            float d[3];
            if (!ep || *(uint32_t *)tab[i] == VT_CAMERA) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                d[k] = ep[k] - cp[k];
            }
            if (dot3(d, d) < 15.0f * 15.0f) {
                nb[n].e = tab[i];
                memcpy(nb[n].p54, ep, 12);
                memcpy(nb[n].pa0, (const float *)(tab[i] + 0xa0), 12);
                n++;
            }
        }
        return;
    }
    dk_log("cardump t=%u tick=%u cam %.3f %.3f %.3f, %d near", now_us(), TICK_COUNT, cp[0], cp[1], cp[2], n);
    for (i = 0; i < n; i++) {
        uintptr_t e = nb[i].e;
        const float *p54 = (const float *)(e + 0x54), *pa0 = (const float *)(e + 0xa0), *tick = NULL;
        int j, interp = 0;
        for (j = 0; j < g_nsaved; j++) {
            if (g_saved[j].e == e) {
                interp = 1;
                tick = (const float *)g_saved[j].block;
            }
        }
        dk_log("  e %08x vt %08x fl %08x %s set54 %.3f %.3f %.3f now54 %.3f %.3f %.3f seta0 %.3f %.3f %.3f nowa0 %.3f %.3f %.3f"
               " tick54 %.3f %.3f %.3f", (unsigned)e, *(uint32_t *)e, *(uint32_t *)(e + 0x10), interp ? "I" : "-",
               nb[i].p54[0], nb[i].p54[1], nb[i].p54[2], p54[0], p54[1], p54[2], nb[i].pa0[0], nb[i].pa0[1],
               nb[i].pa0[2], pa0[0], pa0[1], pa0[2], tick ? tick[0] : 0, tick ? tick[1] : 0, tick ? tick[2] : 0);
    }
}

static void ghost(float a)
{
    uint8_t keep[KEEP_BYTES];
    uint8_t vpm[0x40];
    float dt = TICK_DT;
    uintptr_t vp = RACE_VIEWPORT;
    int dump = g_cardump > 0 && TICK_COUNT > 1500;
    keep_save(keep);
    memcpy(vpm, (void *)(vp + 0x1ec), sizeof vpm);
    TICK_DT = 0;
    apply(a);
    g_in_ghost = 1;
    if (dump) {
        static int armed;
        cardump(0);
        if (!armed) {
            const float *cp = g_ncamsaved ? (const float *)(g_camsaved[0].cam + 0x54) : NULL;
            uintptr_t best = 0;
            float bd = 1e30f;
            int i;
            for (i = 0; cp && i < g_nsaved; i++) {
                const float *ep = (const float *)(g_saved[i].e + 0x54);
                float d[3] = { ep[0] - cp[0], ep[1] - cp[1], ep[2] - cp[2] };
                if (*(uint32_t *)g_saved[i].e == 0x4f2570u && dot3(d, d) < bd) {
                    bd = dot3(d, d);
                    best = g_saved[i].e;
                }
            }
            if (best) {
                armed = 1;
                bp_set(best + 0x54);
                GFX_RenderNow();
                bp_set(0);
                goto rendered;
            }
        }
    }
    GFX_RenderNow();
rendered:
    g_in_ghost = 0;
    if (dump) {
        cardump(1);
        g_cardump--;
    }
    unapply();
    TICK_DT = dt;
    memcpy((void *)(vp + 0x1ec), vpm, sizeof vpm);
    keep_restore(keep);
}

void interp_diff(void);

/* In-between frames until the next tick is due. A frame may run past the deadline by half its usual length (the tick
   deadlines are absolute, so a late tick only starts that much later; alpha follows the clock). The frame time is
   smoothed with outliers (texture uploads) clipped, and shrinks while nothing fits, so one slow frame can't stop
   the in-between frames for good. With a cap, frames start at most every g_frame_us. */
static void hk_wait(void)
{
    static int est = 2000;           /* us a ghost frame takes, smoothed */
    int frames = 0;
    uint32_t t0 = now_us();
    int32_t remain0 = (int32_t)(NEXT_TICK_US - t0);
    static unsigned waits;
    if (g_diff && active() && ++waits % 200 == 0) {
        interp_diff();
    }
    while (active() && frames < 32) {
        uint32_t t = now_us();
        int32_t remain = (int32_t)(NEXT_TICK_US - t), need = est / 2;
        if (remain < need) {
            if (!frames) {
                est -= est / 16;
            }
            break;
        }
        if (g_frame_us && (int32_t)(t - g_last_frame) < (int32_t)g_frame_us) {
            int32_t slot = (int32_t)(g_last_frame + g_frame_us - t);
            if (remain - slot < need) {
                break;
            }
            if (slot > 2000) {
                Sleep(1);
            } else {
                SwitchToThread();
            }
            continue;
        }
        g_last_frame = t;
        ghost(alpha_now());
        frames++;
        {
            int took = (int)(int32_t)(now_us() - t);
            if (took > 0) {
                if (took > est * 2 + 2000) {
                    took = est * 2 + 2000;
                }
                est = (est * 7 + took) / 8;
            }
        }
        WIN_PumpMessages();
    }
    if (g_trace > 0 && active()) {
        static uint32_t last_t0;
        dk_log("wait tick=%u remain=%d frames=%d est=%d spent=%d period=%d moved=%d points=%d", TICK_COUNT, remain0,
               frames, est, (int)(now_us() - t0), (int)(t0 - last_t0), g_last_moved, g_npt[g_cur]);
        last_t0 = t0;
    }
    if (active()) {
        g_stat_ticks++;
        g_stat_frames += frames;
        if (g_stat_ticks >= 300) {
            dk_log("interp: %u ticks, %u game frames (%u hidden) + %u in between (%.1f per tick, frame ~%d us), %u swaps shown, %u game frames skipped",
                   g_stat_ticks, g_stat_real, g_stat_hidden, g_stat_frames, (double)g_stat_frames / g_stat_ticks, est,
                   g_stat_shown, g_stat_skipped);
            g_stat_ticks = g_stat_frames = g_stat_real = g_stat_hidden = g_stat_shown = g_stat_skipped = 0;
        }
    }
    FRAME_WaitForTick();
}

/* ---- DKI_DIFF: draw one ghost frame and log what it wrote (private read-write memory and the exe's data, not
   this thread's stack): side effects that ghost frames would repeat ---- */

typedef struct { uint8_t *base; size_t size; uint8_t *copy; } region_t;

void interp_diff(void)
{
    static region_t r[4096];
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *p = NULL, *stack = (uint8_t *)&mbi;
    int nr = 0, i, runs = 0;
    size_t total = 0, k;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && (uintptr_t)p < 0x7fff0000u) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD)
            && !(stack >= (uint8_t *)mbi.BaseAddress && stack < (uint8_t *)mbi.BaseAddress + mbi.RegionSize)
            && (mbi.Type == MEM_PRIVATE || ((uintptr_t)mbi.BaseAddress >= 0x4fa000 && (uintptr_t)mbi.BaseAddress < 0x8e5000))
            && nr < 4096) {
            r[nr].base = mbi.BaseAddress;
            r[nr].size = mbi.RegionSize;
            nr++;
        }
        p = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    for (i = 0; i < nr; i++) {
        r[i].copy = VirtualAlloc(NULL, r[i].size, MEM_COMMIT, PAGE_READWRITE);
        if (r[i].copy) {
            memcpy(r[i].copy, r[i].base, r[i].size);
            total += r[i].size;
        }
    }
    ghost(alpha_now());
    for (i = 0; i < nr; i++) {
        const uint8_t *c = r[i].copy, *l = r[i].base;
        if (!c) {
            continue;
        }
        for (k = 0; k < r[i].size; k++) {
            size_t end, j, gap;
            char hex[80];
            if (c[k] == l[k]) {
                continue;
            }
            end = k + 1;
            for (gap = 0, j = end; j < r[i].size && gap < 8; j++) {
                if (c[j] != l[j]) {
                    end = j + 1;
                    gap = 0;
                } else {
                    gap++;
                }
            }
            if (runs++ < 400) {
                MEMORY_BASIC_INFORMATION m;
                VirtualQuery(l + k, &m, sizeof m);
                for (j = 0; j < end - k && j < 16; j++) {
                    sprintf(hex + j * 4, "%02x%02x", c[k + j], l[k + j]);
                }
                hex[j * 4] = 0;
                dk_log("diff: %08lx +%u %s  (alloc %08lx)", (unsigned long)(uintptr_t)(l + k), (unsigned)(end - k), hex,
                       (unsigned long)(uintptr_t)m.AllocationBase);
            }
            k = end;
        }
    }
    for (i = 0; i < nr; i++) {
        if (r[i].copy) {
            VirtualFree(r[i].copy, 0, MEM_RELEASE);
        }
    }
    dk_log("diff: tick %u, %d regions %u KB, %d ranges", TICK_COUNT, nr, (unsigned)(total >> 10), runs);
}

/* ---- DKI_PROF=seconds: samples the game thread's EIP every millisecond for that long and logs the hottest 256-byte
   blocks (exe addresses; DLLs by module) ---- */
static HANDLE g_prof_thread;
static int g_prof_seconds;

static DWORD WINAPI prof_main(LPVOID arg)
{
    static uint32_t hits[0x10000];   /* (eip - 0x400000) >> 8 for the exe */
    static struct { uintptr_t base; uint32_t n; } mods[64];
    static uint32_t ghits[0x8000];   /* dkpatch\glide2x.dll, 16-byte blocks */
    uintptr_t gbase = (uintptr_t)GetModuleHandleA("glide2x.dll");
    int nmods = 0, i, samples = 0, dll = 0;
    DWORD end;
    (void)arg;
    Sleep(25000);                    /* into the race */
    end = GetTickCount() + g_prof_seconds * 1000;
    timeBeginPeriod(1);
    while ((int)(GetTickCount() - end) < 0) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_prof_thread) != (DWORD)-1) {
            if (GetThreadContext(g_prof_thread, &ctx)) {
                uintptr_t eip = ctx.Eip;
                samples++;
                if (eip >= 0x401000 && eip < 0x4ea000) {
                    hits[(eip - 0x400000) >> 8]++;
                } else {
                    MEMORY_BASIC_INFORMATION m;
                    dll++;
                    if (gbase && eip >= gbase && eip - gbase < 0x80000) {
                        ghits[(eip - gbase) >> 4]++;
                    }
                    if (VirtualQuery((void *)eip, &m, sizeof m)) {
                        for (i = 0; i < nmods && mods[i].base != (uintptr_t)m.AllocationBase; i++) {
                        }
                        if (i < 64) {
                            if (i == nmods) {
                                mods[nmods++].base = (uintptr_t)m.AllocationBase;
                            }
                            mods[i].n++;
                        }
                    }
                }
            }
            ResumeThread(g_prof_thread);
        }
        Sleep(1);
    }
    dk_log("prof: %d samples, %d outside the exe", samples, dll);
    for (i = 0; i < nmods; i++) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA((HMODULE)mods[i].base, name, sizeof name);
        dk_log("prof: module %08lx %5u %s", (unsigned long)mods[i].base, mods[i].n, name);
    }
    for (;;) {
        uint32_t best = 0, bi = 0;
        for (i = 0; i < 0x10000; i++) {
            if (hits[i] > best) {
                best = hits[i];
                bi = i;
            }
        }
        if (best * 200 < (uint32_t)samples) {
            break;
        }
        dk_log("prof: %08x %5u", 0x400000 + (bi << 8), best);
        hits[bi] = 0;
    }
    for (;;) {
        uint32_t best = 0, bi = 0;
        for (i = 0; i < 0x8000; i++) {
            if (ghits[i] > best) {
                best = ghits[i];
                bi = i;
            }
        }
        if (best * 100 < (uint32_t)samples) {
            break;
        }
        dk_log("prof: glide2x+%05x %5u", bi << 4, best);
        ghits[bi] = 0;
    }
    return 0;
}

int dk_interp_refresh(void)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        return (int)dm.dmDisplayFrequency;
    }
    return 60;
}

void dk_interp_install(void)
{
    static const uint8_t c_sim[5] = { 0xe8, 0x05, 0x00, 0x00, 0x00 };
    static const uint8_t c_render[5] = { 0xe8, 0x21, 0x72, 0xfe, 0xff };
    static const uint8_t c_wait[5] = { 0xe8, 0x5c, 0xdc, 0xff, 0xff };
    static const uint8_t render_body[2] = { 0x56, 0x33 };   /* 0x47639d: push esi ; xor esi, esi */
    char v[8];
    g_diff = GetEnvironmentVariableA("DKI_DIFF", v, sizeof v) > 0;
    g_hash = GetEnvironmentVariableA("DKI_HASH", v, sizeof v) > 0 ? atoi(v) : 0;
    g_trace = GetEnvironmentVariableA("DKI_TRACE", v, sizeof v) > 0 ? atoi(v) : 0;
    g_cardump = GetEnvironmentVariableA("DKI_CARDUMP", v, sizeof v) > 0 ? atoi(v) : 0;
    g_prof_seconds = GetEnvironmentVariableA("DKI_PROF", v, sizeof v) > 0 ? atoi(v) : 0;
    if (g_prof_seconds > 0) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, prof_main, NULL, 0, NULL);
    }
    if (!dk_expect(0x48f1a6, c_sim, 5) || !dk_expect(0x48f16a, c_render, 5) || !dk_expect(0x48f16f, c_wait, 5)
        || !dk_expect(0x47639d, render_body, 2)) {
        dk_log("interp: main loop differs, not installed");
        return;
    }
    if (dk_ini_int("Graphics", "ExperimentalTickRate", 0)) {
        dk_log("interp: off, ExperimentalTickRate is set (the two don't combine)");
        if (g_hash) {
            dk_call(0x48f1a6, hk_simulate);   /* hashes only, for comparisons */
        }
        return;
    }
    timeBeginPeriod(1);   /* Sleep(1) for the frame cap */
    dk_call(0x48f1a6, hk_simulate);
    dk_call(0x48f16a, hk_render);
    dk_call(0x48f16f, hk_wait);
    {
        static const uint8_t setter[5] = { 0x8b, 0x54, 0x24, 0x04, 0x56 };   /* mov edx, [esp+4]; push esi */
        if (GetEnvironmentVariableA("DKI_NOSETTERS", v, sizeof v)) {   /* test switch: the render's poses stay */
            dk_log("interp: DKI_NOSETTERS, entity setters left alone");
        } else if (dk_expect(0x4a4c20, setter, 5) && dk_expect(0x4a4cb0, setter, 5)) {
            dk_jmp(0x4a4c20, (void *)ent_set_position);
            dk_jmp(0x4a4cb0, (void *)ent_set_rotation);
        } else {
            dk_log("interp: entity setters differ, the render's own poses stay in in-between frames");
        }
    }
    g_on = 1;
    dk_interp_set_max_fps(dk_ini_int("Graphics", "MaxFps", 0));
}

/* [Graphics] MaxFps: 0 the desktop refresh rate, -1 no limit, else frames per second (60 or less: one per tick, as the
   original). Also from Options - Graphics (gfxmenu.c); takes effect at the next race tick. */
void dk_interp_set_max_fps(int setting)
{
    if (!g_on) {
        return;
    }
    g_setting = setting;
    g_max_fps = setting == 0 ? dk_interp_refresh() : setting;
    g_frame_us = g_max_fps > 0 ? 1000000u / (unsigned)g_max_fps : 0;
    g_enabled = g_max_fps < 0 || g_max_fps > 60;
    if (!g_enabled) {
        dk_log("interp: MaxFps=%d: one frame per tick (60/s)", setting);
    } else if (g_max_fps < 0) {
        dk_log("interp: MaxFps=%d: frames in between, no limit%s", setting, g_diff ? " (diff mode)" : "");
    } else {
        dk_log("interp: MaxFps=%d: frames in between, at most %d/s%s", setting, g_max_fps, g_diff ? " (diff mode)" : "");
    }
}

int dk_interp_available(void)
{
    return g_on;
}

int dk_interp_max_fps(void)
{
    return g_setting;
}
