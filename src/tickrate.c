/* The game's tick: 60 Hz everywhere, and optionally the display refresh rate (docs/framerate.md section 0, plan in
 * docs/tickrate.md).
 *
 * The game simulates at a fixed tick and draws at most one frame per tick: 30 Hz, 60 Hz with the 60fps option,
 * 30 Hz in network games (lockstep). Most of its time-based code already scales with the tick length
 * (GAME_g_TickDt 0x521cf0 = 1 / FRAME_g_TicksPerSec 0x507a40), since it was written for both 30 and 60.
 *
 * 60 Hz (always, tick60): the frame rate option (Options - Graphics; the row is the "max framerate" there, gfxmenu.c)
 * reads as 60fps (CFG_GetFrameRate 0x489840 returns 2), and network games, which FRAME_SetMode forced to its 30 Hz
 * mode 1, get mode 2 as well. Every peer of a network game must tick at the same rate, so the join version
 * (DN_g_VersionBlock 0x4fe228, compared whole) gets "60" in its last two bytes: 30 Hz games are refused with "Wrong
 * game version". DragNet's auto tuning (DN_AutoTuneConfig, host; clients adopt its config) keeps the input buffer as
 * long in ms as at 30 Hz: Ratio (frames per input block) from the ping as at 30 Hz, Lead 3 instead of 1. The host's
 * latency measurement counts iterations (at most 60 = 2 s at 30), raised to 120.
 *
 * With [Graphics] ExperimentalTickRate ("TickRate" below; ini only, off by default: it changes how the game plays), single-player races tick at
 * N Hz instead (-1 = the desktop refresh rate): every frame is a real simulation step, nothing is interpolated. The
 * reference is the original at 60 Hz: what it does per second, the patch does per second at N.
 *
 * - The rate: a session that is a race (0x806c44, set by the frontend when it ends its session for one; checked
 *   when GAME_StartSession calls FRAME_InitTiming, 0x47488f) ticks at N while the race branch of GAME_SimulateTick
 *   runs (0x807258 == 0), with the 60fps option and no network game (0x806dd8). Checked per tick (GAME_Tick's first
 *   call, 0x48f180). The frontend keeps 30/60 Hz (its key repeat and animations count ticks). Switching sets what
 *   FRAME_InitTiming derives (TicksPerSec 0x507a40/0x806bbc/0x806bf0, TickUs 0x806b44, its half 0x806b40, ms
 *   0x806b48) and the next tick's deadline.
 * - Code that hard-codes the 30 or 60 Hz tick, set from the rate in use (rate_sites):
 *   - the camera springs (CAM_SpringStep 0x40b7b0 via 0x40ba70 and 0x40d100): dt 1/30 even at 60 Hz, where the
 *     original's camera springs ran twice as fast; 1/70 with 0x511144. Here 1/N (3/7N);
 *   - a HUD value (FUN_0041ff60: 1/30, 1/60 with the 60fps option);
 *   - per-tick factors without dt (FACTORS: the car body's angular momentum damping on the ground) -> k^(60/N);
 *   - per-tick steps without dt (STEPS: powerup timers, wheel spin, a sprite's grow-in) -> step * 60/N;
 *   - the AI's path follower (FUN_00430cf0, the point the AI cars chase): its speed steps by an acceleration per tick
 *     (CALLS) -> * 60/N. This was most of the difference: at 144 Hz the AI cars launched twice as fast;
 *   - FUN_004122a0 (a pull towards another body) applies its force as an impulse with dt 1.0 -> 60/N;
 *   - FUN_00438840: energy recharge on odd ticks (30 Hz at 60) -> on a 30 Hz sequence of ticks; the off-track
 *     counter that puts a car back after 128 ticks -> counts at 60 Hz;
 *   - the best-lap ghost (CTL_GetCarInput 0x48512b records, FUN_00417ca0 0x417dc1 plays back; rings
 *     0x574090/0x54f1f0) samples at 30 Hz: at 60 Hz the original records every other tick and plays the ticks in
 *     between halfway. Here any rate: a sample when floor(TickCount * 30 / N) advances, in between the position at
 *     that fraction. Its files are named after the rate (replays\<track>.<controller>.<rate>); the 60 Hz ones are
 *     used.
 *
 * Test switch (environment, also without TickRate): DKT_LOG=1 logs, once per second of race time (GAME_g_Time), the
 * tick count, the race phase and the position of every other hierarchy entity (the cars), for comparing runs at
 * different rates. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dkpatch.h"
#include <dwmapi.h>

#define TICKS_PER_SEC   (*(volatile int *)0x507a40)    /* FRAME_g_TicksPerSec */
#define TICKS_PER_SEC2  (*(volatile int *)0x806bbc)    /* copy of it (FRAME_InitTiming) */
#define TARGET_FPS      (*(volatile int *)0x806bf0)    /* FRAME_g_TargetFps (FRAME_SetMode) */
#define TICK_US         (*(volatile uint32_t *)0x806b44)
#define TICK_US_HALF    (*(volatile uint32_t *)0x806b40)
#define TICK_MS         (*(volatile uint32_t *)0x806b48)
#define NEXT_TICK_US    (*(volatile uint32_t *)0x806bb8)
#define TICK_COUNT      (*(volatile uint32_t *)0x806c50)  /* GAME_g_TickCount */
#define RACE_SIM        (*(volatile char *)0x807258 == 0) /* GAME_SimulateTick's race branch */
#define NETWORK         (*(volatile char *)0x806dd8)
#define RACE_SESSION    (*(volatile char *)0x806c44)      /* set by the frontend when it ends its session for a race */
#define RACE_PHASE      (*(volatile int *)0x521d28)       /* FUN_00438d50: 0 load, 1 pre-roll, 2 countdown, 3 race, .. 6 */
#define FRAME_MODE      (*(volatile int *)0x507a34)       /* FRAME_SetMode: 2 = the 60fps option's (render only on time) */
#define CFG             ((void *)0x7045c8)

static int (__attribute__((thiscall)) *const CFG_GetFrameRate)(void *) = (int (__attribute__((thiscall)) *)(void *))0x489840;
static uint32_t (__attribute__((thiscall)) *const timer_us)(void *) = (uint32_t (__attribute__((thiscall)) *)(void *))0x483410;
static int (*const FRAME_InitTiming)(int) = (int (*)(int))0x48cf30;
static void (*const JOY_UpdateDevices)(void) = (void (*)(void))0x47bf00;
static void (*const FRAME_WaitForTick)(void) = (void (*)(void))0x48cdd0;
static void (*const WIN_PumpMessages)(void) = (void (*)(void))0x471f20;
static void (*const VEC_Lerp)(float *out, float t, const float *a, const float *b) =
    (void (*)(float *, float, const float *, const float *))0x4aed10;

#define GAME_TIME       (*(volatile double *)0x521ce0)  /* GAME_g_Time */
#define ENT_TABLE       (*(uintptr_t **)0x89af54)
#define ENT_COUNT       (*(volatile int *)0x89af60)
#define VT_HIERARCHY    0x4f2570u

static int g_log;           /* DKT_LOG */
static int g_rate;          /* N, the race tick rate */
static int g_game_rate;     /* the rate FRAME_InitTiming chose */
static int g_cur;           /* the rate in use */
static int g_race;          /* this session is a race */
static uint32_t g_period_us; /* the display's refresh period in us less 1 (DWM), 0 = unknown or not used */
static int g_refresh;       /* TickRate -1: the desktop refresh rate; 0 = a fixed TickRate */
static int g_div = 1;       /*   N = refresh / g_div, raised when frames keep missing the refresh (hk_wait) */

/* the tick is one of a k Hz sequence at the rate in use: floor(t * k / n) advanced at it (every tick when n <= k);
   else *frac (if given) = how far it is past the last one, in its intervals */
static int tick_of(int k, uint32_t t, float *frac)
{
    int n = TICKS_PER_SEC;
    uint32_t r;
    if (n <= k) {
        if (frac) {
            *frac = 0;
        }
        return 1;
    }
    r = (uint32_t)(((uint64_t)t * k) % (uint32_t)n);
    if (frac) {
        *frac = (float)r / n;
    }
    return r < (uint32_t)k;
}

/* ---- sites that hard-code the tick ---- */

static void put_float(uint32_t addr, float f)
{
    if (*(float *)addr != f) {
        dk_write(addr, &f, 4);
    }
}

/* per-tick factors (x *= k once per tick, no dt), only read by per-tick code: k^(60/N) keeps their effect per second */
static const struct { uint32_t addr; float k; } FACTORS[] = {
    { 0x4efe7c, 0.95f },    /* FUN_0042a970 (car body on the ground): angular momentum, up axis */
    { 0x4efe80, 0.8f },     /*   the same, the other two axes */
};

/* per-tick steps (x += c once per tick, no dt) whose constant is shared with other code: the instruction's memory
   operand (disp32 at .disp) is pointed at a copy scaled by 60/N */
static struct { uint32_t disp, orig; float v; } STEPS[] = {
    { 0x438f12, 0x4fd1a8, 0 },  /* FUN_00438f00: wheel spin angle += speed * 1.31 */
    { 0x414fe5, 0x4ef828, 0 },  /* FUN_00414e90 (human car): powerup time (+0x224) -= 1 */
    { 0x42d14a, 0x4eff10, 0 },  /* FUN_0042d020 (AI car): the same */
    { 0x42e474, 0x4eff10, 0 },  /* FUN_0042e290 (AI car): the same */
    { 0x41ce43, 0x4efa2c, 0 },  /* FUN_0041c750: a sprite's scale (+0x21c) grows by 0.1 up to 1 */
    { 0x411587, 0x4ef720, 0 },  /* FUN_00411560, a render phase (when 0x511b78 is set): an angle (0x511960) += 6
                                   degrees per frame drawn (one per tick at most) */
};

static volatile uint32_t g_tick30;   /* 1 on the ticks of a 30 Hz sequence (read by the energy gate) */
float dkt_scale60 = 1;               /* 60 / N (read by the stubs below) */

/* FUN_00430cf0, the path follower the AI cars chase: its speed (+0x74) steps towards the target speed (+0x78) by +0x88
   per tick (an acceleration per tick). The two 6-byte instructions that read the step call these instead. */
void dkt_rabbit_up(void);     /* was fld dword [ebx+0x88] */
void dkt_rabbit_down(void);   /* was fsub dword [ebx+0x88] */
__asm__(".globl _dkt_rabbit_up\n_dkt_rabbit_up:\n"
        "    flds 0x88(%ebx)\n    fmuls _dkt_scale60\n    ret\n"
        ".globl _dkt_rabbit_down\n_dkt_rabbit_down:\n"
        "    flds 0x88(%ebx)\n    fmuls _dkt_scale60\n    .byte 0xde, 0xe9\n    ret\n");   /* fsubp st(1), st */

static const struct { uint32_t addr; uint8_t orig[6]; void (*stub)(void); } CALLS[] = {
    { 0x430d15, { 0xd9, 0x83, 0x88, 0x00, 0x00, 0x00 }, dkt_rabbit_up },
    { 0x430d42, { 0xd8, 0xa3, 0x88, 0x00, 0x00, 0x00 }, dkt_rabbit_down },
};

/* FUN_00438840, a car off the track: +0xbc counts ticks, every 8th a track search, after 128 the car is put back.
   Here the count advances at 60 Hz. */
static char __cdecl offtrack_count(uintptr_t car)
{
    if (tick_of(60, TICK_COUNT, NULL)) {
        (*(int *)(car + 0xbc))++;
    }
    return *(int *)(car + 0xbc) > 0x7f;
}

static const uint8_t OFFTRACK_ORIG[20] = {   /* 0x4389c1: mov ecx, [esi+0xbc]; inc ecx; mov eax, ecx; */
    0x8b, 0x8e, 0xbc, 0x00, 0x00, 0x00, 0x41, 0x8b, 0xc1, 0x89, 0x8e, 0xbc, 0x00, 0x00, 0x00,   /* mov [esi+0xbc], */
    0x83, 0xf8, 0x7f, 0x7e, 0x15                                                                  /* ecx; cmp; jle */
};

static const union { uint32_t u; float f; } PICKUP = { 0x3d8efa35 };   /* 0.0698 rad, 4 degrees */
#define PICKUP_STEP PICKUP.f

static void rate_sites(int rate)
{
    int on = rate != g_game_rate;
    unsigned i;
    for (i = 0; i < sizeof FACTORS / sizeof FACTORS[0]; i++) {
        put_float(FACTORS[i].addr, on ? powf(FACTORS[i].k, 60.0f / rate) : FACTORS[i].k);
    }
    for (i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        uint32_t a = on ? (uint32_t)(uintptr_t)&STEPS[i].v : STEPS[i].orig;
        STEPS[i].v = *(float *)STEPS[i].orig * 60.0f / rate;
        dk_write(STEPS[i].disp, &a, 4);
    }
    dkt_scale60 = on ? 60.0f / rate : 1;
    for (i = 0; i < sizeof CALLS / sizeof CALLS[0]; i++) {
        uint8_t code[6] = { 0xe8, 0, 0, 0, 0, 0x90 };
        int32_t rel = (int32_t)((uintptr_t)CALLS[i].stub - (CALLS[i].addr + 5));
        memcpy(code + 1, &rel, 4);
        dk_write(CALLS[i].addr, on ? code : CALLS[i].orig, 6);
    }
    put_float(0x40c09e, on ? 1.0f / rate : 1.0f / 30);                 /* camera spring dt */
    put_float(0x40c0a5, on ? 3.0f / (7.0f * rate) : 1.0f / 70);        /* the same, 0x511144 set */
    put_float(0x40d361, on ? 1.0f / rate : 1.0f / 30);                 /* camera, the other mode */
    put_float(0x4204c2, on ? 1.0f / rate : 1.0f / 60);                 /* HUD (60fps option) */
    {
        int32_t frames = on ? 5 * rate : 300;   /* FUN_0041f810: a HUD message stays 300 frames (0x515254) */
        if (*(int32_t *)0x41f82a != frames) {
            dk_write(0x41f82a, &frames, 4);
        }
    }
    put_float(0x41246a, on ? 60.0f / rate : 1.0f);
    {
        /* FUN_00426ee0, a task run every tick: the pickups share one mesh, whose vertices it turns by two rotations of
           0.07 rad (pushed at 0x4270d4 / 0x4270e6) per tick; the quaternions (0x5173e0, 0x517390) are built once
           (0x5173f0), so they are rebuilt here when the rate changes */
        float angle = on ? PICKUP_STEP * 60.0f / rate : PICKUP_STEP;
        put_float(0x4270d5, angle);
        put_float(0x4270e7, angle);
        if (*(volatile char *)0x5173f0) {
            ((void (*)(void *, float))0x4ae2c0)((void *)0x5173e0, angle);
            ((void (*)(void *, float))0x4ae340)((void *)0x517390, angle);
        }
    }
    {
        /* FUN_00483dd0 (the replays\<track>.<controller>.<rate> file, written at the end of a race: the race setup)
           reads the rate at 0x483e30 for the name */
        static const uint8_t orig[6] = { 0x8b, 0x0d, 0x40, 0x7a, 0x50, 0x00 };   /* mov ecx, [FRAME_g_TicksPerSec] */
        uint8_t fixed[6] = { 0xb9, 0, 0, 0, 0, 0x90 };                          /* mov ecx, game rate */
        memcpy(fixed + 1, &g_game_rate, 4);
        dk_write(0x483e30, on ? fixed : orig, 6);
    }
    {
        /* WIN_PumpMessages (0x471f20), called every tick: its two PeekMessage(PM_NOREMOVE) calls (wRemoveMsg pushed at
           0x471f89 / 0x471fd2) get PM_NOYIELD. Without it PeekMessage yields the thread's time slice whenever the queue
           is empty, and if another thread is ready on that CPU the game stalls for 5..100 ms (measured headless at
           240 Hz: ~30 stalls a second). */
        uint8_t flag = on ? PM_NOYIELD : 0;
        dk_write(0x471f89, &flag, 1);
        dk_write(0x471fd2, &flag, 1);
    }   /* FUN_004122a0: a pull towards a body, FUN_004b7370(body, dt 1.0) */
    {
        /* FUN_00438840: energy recharge (+0x214 += 2 in a recharge zone) on odd ticks */
        uint32_t a = on ? (uint32_t)(uintptr_t)&g_tick30 : 0x806c50;
        dk_write(0x438a41, &a, 4);
    }
    {
        uint8_t code[20] = { 0x56, 0xe8, 0, 0, 0, 0,          /* push esi; call offtrack_count */
                             0x83, 0xc4, 0x04, 0x84, 0xc0,    /* add esp, 4; test al, al */
                             0x74, 0x1c,                      /* je 0x4389ea */
                             0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        int32_t rel = (int32_t)((uintptr_t)offtrack_count - (0x4389c2 + 5));
        memcpy(code + 2, &rel, 4);
        dk_write(0x4389c1, on ? code : OFFTRACK_ORIG, 20);
    }
    {
        /* the best-lap ghost files: FUN_004862e0 save / FUN_00486350 load name them after the tick rate */
        static const uint8_t orig[5] = { 0xa1, 0x40, 0x7a, 0x50, 0x00 };   /* mov eax, [FRAME_g_TicksPerSec] */
        uint8_t fixed[5] = { 0xb8, 0, 0, 0, 0 };                          /* mov eax, game rate */
        memcpy(fixed + 1, &g_game_rate, 4);
        dk_write(0x4862e0, on ? fixed : orig, 5);
        dk_write(0x486350, on ? fixed : orig, 5);
    }
}

static void set_rate(int rate)
{
    if (rate == g_cur) {
        return;
    }
    TICKS_PER_SEC = rate;
    TICKS_PER_SEC2 = rate;
    TARGET_FPS = rate;
    TICK_MS = 1000u / rate;
    TICK_US = rate == g_rate && g_period_us ? g_div * (g_period_us + 1) - 1 : 1000000u / rate;
    TICK_US_HALF = TICK_US / 2;
    NEXT_TICK_US = timer_us((void *)0x53d310) + TICK_US;
    rate_sites(rate);
    /* the game thread: above the background load while a race runs at N (its waits sleep, see hk_wait) */
    SetThreadPriority(GetCurrentThread(), rate != g_game_rate ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
    dk_log("tickrate: %d Hz (tick %u)", rate, TICK_COUNT);
    g_cur = rate;
}

static int wanted(void)
{
    return g_race && RACE_SIM && !NETWORK && g_game_rate == 60 && CFG_GetFrameRate(CFG) == 2 ? g_rate : g_game_rate;
}

static int hk_init_timing(int arg)
{
    int r = FRAME_InitTiming(arg);
    g_race = RACE_SESSION != 0;
    g_game_rate = TICKS_PER_SEC;
    g_cur = g_game_rate;
    rate_sites(g_cur);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    set_rate(wanted());
    return r;
}

static const char *screen_name(void)
{
    uintptr_t d = *(volatile uintptr_t *)0x525aec;   /* the frontend screen descriptor, name at +0x14 */
    const char *n = d ? *(const char **)(d + 0x14) : NULL;
    return n ? n : "";
}

static void log_state(void)
{
    static int last = -1;
    char line[2048];
    int sec = (int)GAME_TIME, i, n = ENT_COUNT, o, k = 0;
    uintptr_t *tab = ENT_TABLE;
    if (!RACE_SIM || !tab) {
        last = -1;
        return;
    }
    if (sec == last) {
        return;
    }
    last = sec;
    o = snprintf(line, sizeof line, "tickrate: t=%d tick=%u hz=%d phase=%d wall=%lu [%s]", sec, TICK_COUNT,
                 TICKS_PER_SEC, RACE_PHASE, GetTickCount(), screen_name());
    {
        /* the human cars (0x511c78, relocated by players.c: the base is the disp32 at 0x415159), slot 0, its body's
           position (+0xe4 + 0x68) */
        const float *h = (const float *)(*(uintptr_t *)0x415159 + 0x14c);
        o += snprintf(line + o, sizeof line - o, " H %.1f,%.1f,%.1f", h[0], h[1], h[2]);
        o += snprintf(line + o, sizeof line - o, " Q %.5f,%.5f,%.5f,%.5f", ((float *)0x517128)[0],
                      ((float *)0x517128)[1], ((float *)0x517128)[2], ((float *)0x517128)[3]);   /* the pickups' turn */
    }
    for (i = 0; i < n && o < (int)sizeof line - 40; i++) {
        if (tab[i] && *(uint32_t *)tab[i] == VT_HIERARCHY && (k++ & 1) == 0) {
            const float *f = (const float *)(tab[i] + 0x54);
            o += snprintf(line + o, sizeof line - o, " %.1f,%.1f,%.1f", f[0], f[1], f[2]);
        }
    }
    dk_log("%s", line);
}

static int g_watch;   /* DKT_WATCH */
static int g_watchl;   /* DKT_WATCHL */
static void watch_list_tick(void);
static int g_watchs;   /* DKT_WATCHS */
static void watch_sprites_tick(void);
static int g_exec_mode, g_watchp;   /* DKT_WATCHP */
static volatile uintptr_t g_pickup_obj;
static int g_difftick;   /* DKT_DIFFTICK */
static uintptr_t g_watcha;   /* DKT_WATCHA */
static int g_qtrace;   /* DKT_QTRACE */
static int g_watch_all;   /* DKT_WATCHALL: log every write the watch sees (24) */
static uintptr_t g_watch_addr;
static HANDLE g_game_thread;
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *x);
static DWORD WINAPI watch_arm(LPVOID arg);
static void watch_tick(void);

static void hk_tick(void)
{
    if (g_game_rate) {
        set_rate(wanted());
    }
    g_tick30 = tick_of(30, TICK_COUNT + 1, NULL);   /* GAME_AdvanceClock counts this tick before the cars run */
    if (g_log) {
        log_state();
    }
    if (g_watch) {
        watch_tick();
    }
    if (g_watchl) {
        watch_list_tick();
    }
    if (g_watchs) {
        watch_sprites_tick();
    }
    if (g_qtrace > 0 && RACE_SIM && RACE_PHASE >= 3 && GAME_TIME >= 12) {
        /* DKT_QTRACE=n: the pickups' quaternion (0x517128) at n consecutive ticks */
        const float *q = (const float *)0x517128;
        dk_log("tickrate: qtrace tick %u q %.6f %.6f %.6f %.6f", TICK_COUNT, q[0], q[1], q[2], q[3]);
        g_qtrace--;
    }
    if (g_watcha && RACE_SIM && RACE_PHASE >= 3 && GAME_TIME >= 10) {
        /* DKT_WATCHA=hex address: a write breakpoint on it, 10 s into a race */
        g_watch_addr = g_watcha;
        g_watcha = 0;
        AddVectoredExceptionHandler(1, watch_veh);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
    }
    if (g_difftick && RACE_SIM && RACE_PHASE >= 3 && GAME_TIME >= g_difftick) {
        /* DKT_DIFFTICK=s: the exe's data (0x4fa000..0x8e5000) over one tick, the changed floats between -1000 and 1000 */
        static uint8_t *snap;
        enum { LO = 0x4fa000, HI = 0x8e5000 };
        if (!snap) {
            snap = malloc(HI - LO);
            memcpy(snap, (void *)LO, HI - LO);
        } else {
            uint32_t a2;
            int c = 0;
            for (a2 = LO; a2 < HI; a2 += 4) {
                uint32_t x0, x1;
                float f0, f1;
                memcpy(&x0, snap + a2 - LO, 4);
                x1 = *(uint32_t *)a2;
                memcpy(&f0, &x0, 4);
                memcpy(&f1, &x1, 4);
                if (x0 != x1 && fabsf(f0) < 1000 && fabsf(f1) < 1000 && fabsf(f0) > 1e-6f && c++ < 400) {
                    dk_log("tickrate: diff %08x %.6g > %.6g (%+.6g)", a2, f0, f1, f1 - f0);
                }
            }
            dk_log("tickrate: diff: %d floats changed", c);
            g_difftick = 0;
        }
    }
    if (g_pickup_obj) {
        /* compare the pickup's object and the one at its +0xa0 over a tick, then watch the first change */
        static uint8_t snap[3][0x200];
        static uintptr_t o[3];
        static int st;
        int j, k;
        if (st == 0) {
            o[0] = g_pickup_obj;
            o[1] = ((uintptr_t (*)(int))0x406f00)(*(int *)(o[0] + 0xa0));   /* its B3D object */
            if (!o[1]) {
                o[1] = o[0];
            }
            o[2] = ((uintptr_t *)0x50f36c)[*(int *)(o[0] + 0xa0) * 3];   /* the slot's object (the parent) */
            if (!o[2]) {
                o[2] = o[0];
            }
            dk_log("tickrate: pickup slot %d flags %08x object %08x vt %08x", *(int *)(o[0] + 0xa0),
                   ((unsigned *)0x50f370)[*(int *)(o[0] + 0xa0) * 3], (unsigned)o[2], *(unsigned *)o[2]);
            for (j = 0; j < 3; j++) {
                memcpy(snap[j], (void *)o[j], 0x200);
            }
            st = 1;
        } else if (st == 1) {
            st = 2;
            for (j = 0; j < 3; j++) {
                char line[600];
                int len = snprintf(line, sizeof line, "tickrate: pickup object %d %08x vt %08x changed:", j,
                                   (unsigned)o[j], *(unsigned *)o[j]);
                for (k = 1; k < 0x80; k++) {
                    float fa, fb;
                    uint32_t a0 = ((uint32_t *)snap[j])[k], b0 = ((uint32_t *)o[j])[k];
                    memcpy(&fa, &a0, 4);
                    memcpy(&fb, &b0, 4);
                    if (a0 != b0 && len < 520) {
                        len += snprintf(line + len, sizeof line - len, " +%x %.3g>%.3g", k * 4, fa, fb);
                        if (!g_watch_addr || g_watch_addr == 0x426b52) {
                            g_watch_addr = o[j] + k * 4;
                        }
                    }
                }
                dk_log("%s", line);
            }
            if (g_watch_addr != 0x426b52) {
                CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
            }
        }
    }
    if (g_watchp && RACE_SIM && RACE_PHASE >= 3 && GAME_TIME >= g_watchp) {
        g_watchp = 0;
        g_exec_mode = 1;
        g_watch_addr = 0x426b52;
        AddVectoredExceptionHandler(1, watch_veh);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
    }
    JOY_UpdateDevices();
}

/* GAME_TickEnd's FRAME_WaitForTick call (0x48f16f). The wait is for an absolute deadline (NextTickUs), and with the
   60fps option a tick that starts past it isn't drawn (FRAME_ShouldRender).
   - At N = the refresh rate with VSync the buffer swap blocks until the vertical blank, which drifts against the
     deadline: a frame would be skipped whenever the two cross. So the tick length is the display's measured refresh
     period less 1 us (display_period_us): the deadline drifts early against the vertical blank, by a microsecond per
     frame, and a deadline missed by less than a quarter tick is moved to now. The display then paces the game (game
     time runs at refresh / N, within 0.01 %).
   - A frame that took longer (missed a vertical blank: late by up to two ticks) is absorbed as well: the game would
     skip the next frame to catch up, a second hitch. The game time loses that bit.
   - With TickRate -1, two seconds of racing in a row with 10 % or more of the frames missing the refresh lower N to
     the next whole fraction of the refresh rate (240 -> 120 -> 80 ..., not below 60), where every picture stays up
     for the same number of refreshes; for the rest of the game run. (4K with 4x MSAA at 240 Hz is a lot of pixels.)
   - Later than two ticks is a real slowdown and left to the game, which catches up as before.
   - The wait itself replaces the original's at the 60fps option (case 2: message pump + Sleep(0) in a loop until the
     deadline): the message pump at most every 16 ms (as often as at 60 Hz) or when a message comes in,
     MsgWaitForMultipleObjects until 1 ms before the deadline, then a spin. Every call into the window manager can
     stall the thread (headless, the original's own 60 Hz wait returned > 3 ms late on most ticks on a loaded PC),
     and at 240 Hz there is no slack to hide it.
   Every 10 s of racing a line in the log: ticks, misses, absorbed deadlines, the latest one, the longest pump. */
static struct {
    unsigned ticks, misses, follows, worst, pump, sec_ticks, sec_misses, bad_secs;
} g_st;

static void hk_wait(void)
{
    if (g_cur == g_game_rate || FRAME_MODE == 3) {
        FRAME_WaitForTick();
        return;
    }
    {
        uint32_t now = timer_us((void *)0x53d310), late = now - NEXT_TICK_US;
        int racing = RACE_PHASE >= 3 && GAME_TIME >= 2.0;   /* not the start (loading spikes) */
        if ((int32_t)late >= 0 && late < 2 * TICK_US) {
            if (late >= TICK_US / 4 && racing) {
                g_st.misses++;
                g_st.sec_misses++;
            }
            g_st.follows += racing;
            NEXT_TICK_US = now;
        }
        if (racing && (int32_t)late > 0 && late > g_st.worst) {
            g_st.worst = late;
        }
        if (racing && ++g_st.sec_ticks >= (unsigned)g_cur) {
            g_st.bad_secs = g_st.sec_misses * 10 >= g_st.sec_ticks ? g_st.bad_secs + 1 : 0;
            if (g_st.bad_secs >= 2 && g_refresh && (g_refresh + (g_div + 1) / 2) / (g_div + 1) >= 60) {
                g_div++;
                g_rate = (g_refresh + g_div / 2) / g_div;
                dk_log("tickrate: %u of %u frames missed the refresh, now %d Hz (1/%d of %d)", g_st.sec_misses,
                       g_st.sec_ticks, g_rate, g_div, g_refresh);
                set_rate(g_rate);
                g_st.bad_secs = 0;
            }
            g_st.sec_ticks = g_st.sec_misses = 0;
        }
        if (racing && ++g_st.ticks >= 10u * g_cur) {
            dk_log("tickrate: 10 s at %d Hz: %u frames missed the refresh, %u deadlines moved, latest %u us, longest "
                   "message pump %u us (tick %u us)", g_cur, g_st.misses, g_st.follows, g_st.worst, g_st.pump, TICK_US);
            g_st.ticks = g_st.misses = g_st.follows = g_st.worst = g_st.pump = 0;
        }
    }
    if (FRAME_MODE != 2) {
        FRAME_WaitForTick();
        return;
    }
    {
        static uint32_t last_pump;
        uint32_t t = timer_us((void *)0x53d310);
        if (t - last_pump >= 16000) {
            uint32_t d;
            last_pump = t;
            WIN_PumpMessages();
            d = timer_us((void *)0x53d310) - t;
            g_st.pump = d > g_st.pump ? d : g_st.pump;
        }
        for (;;) {
            int32_t rem = (int32_t)(NEXT_TICK_US - timer_us((void *)0x53d310));
            if (rem <= 0) {
                break;
            }
            if (rem > 1500) {
                if (MsgWaitForMultipleObjects(0, NULL, FALSE, (rem - 1000) / 1000, QS_ALLINPUT) == WAIT_OBJECT_0) {
                    last_pump = timer_us((void *)0x53d310);
                    WIN_PumpMessages();
                }
            } else {
                YieldProcessor();
            }
        }
        *(volatile uint32_t *)0x806be8 += TICK_MS;   /* what the original's wait counts */
        (*(volatile uint32_t *)0x806bd0)++;
        NEXT_TICK_US += TICK_US;
    }
}

/* ---- DKT_WATCH=n: finds what turns things in place. n seconds into a race, the entities whose position stayed
   while their rotation (+0x60 quaternion or +0x70 matrix) changed over one tick are logged, and a hardware write
   breakpoint on the first one's changed field logs every instruction that writes it (once per address). ---- */

static void *g_seen[64];
static int g_nseen;

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *x)
{
    if (x->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (x->ContextRecord->Dr6 & 1) && g_exec_mode) {
        /* DKT_WATCHP: the pickup task (0x426b52, ebx = task, +0x30 its entity): log it, then watch its rotation */
        uintptr_t ent = *(uintptr_t *)(x->ContextRecord->Ebx + 0x30);
        const float *f = (const float *)(ent + 0x54);
        dk_log("tickrate: pickup task %08lx entity %08x vt %08x pos %.1f,%.1f,%.1f q %.3f %.3f %.3f %.3f flags %08x",
               x->ContextRecord->Ebx, (unsigned)ent, *(unsigned *)ent, f[0], f[1], f[2], f[3], f[4], f[5], f[6],
               *(unsigned *)(ent + 0x10));
        g_exec_mode = 0;
        g_pickup_obj = ent;
        x->ContextRecord->Dr7 = 0;
        x->ContextRecord->Dr6 = 0;
        x->ContextRecord->EFlags |= 0x10000;   /* RF: resume past the execute breakpoint */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (x->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (x->ContextRecord->Dr6 & 1)) {
        void *eip = (void *)x->ContextRecord->Eip;
        int i;
        for (i = 0; i < g_nseen && g_seen[i] != eip; i++) {
        }
        static int every;
        if (g_watch_all && every < 24) {
            every++;
            dk_log("tickrate: watch hit %p tick %u", eip, TICK_COUNT);
        }
        if (i == g_nseen && g_nseen < 64) {
            g_seen[g_nseen++] = eip;
            const uint32_t *sp = (const uint32_t *)x->ContextRecord->Esp;
            char calls[200];
            int j, o = 0, m = 0;
            calls[0] = 0;
            for (j = 0; j < 128 && m < 6; j++) {   /* return addresses into the exe */
                if (sp[j] > 0x401000 && sp[j] < 0x4e9000) {
                    o += snprintf(calls + o, sizeof calls - o, " %08x", (unsigned)sp[j]);
                    m++;
                }
            }
            dk_log("tickrate: watch %08x written by the instruction before %p (tick %u), stack%s", (unsigned)g_watch_addr,
                   eip, TICK_COUNT, calls);
        }
        x->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watch_arm(LPVOID arg)
{
    CONTEXT c;
    (void)arg;
    SuspendThread(g_game_thread);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(g_game_thread, &c);
    c.Dr0 = g_watch_addr;
    c.Dr7 = g_exec_mode ? 1 : 1 | (1u << 16) | (3u << 18);   /* L0, break on execute / write 4 bytes */
    SetThreadContext(g_game_thread, &c);
    ResumeThread(g_game_thread);
    return 0;
}

/* DKT_WATCHL: the same for the world objects FUN_00436210 draws (list at *0x520c30 + 8, linked through +0xc, each
   linked at its +4): 0x200 bytes of each object over one tick, the changed dwords logged per object, a write
   breakpoint on the first changed dword of the first object that changed */
static void watch_list_tick(void)
{
    enum { MAXO = 512, SZ = 0x200 };
    static uint8_t *snap;
    static uintptr_t objs[MAXO];
    static int n, state;
    uintptr_t head = *(volatile uintptr_t *)0x520c30, o;
    int i, found = 0;
    if (!RACE_SIM || !head || RACE_PHASE < 3 || GAME_TIME < g_watchl || state == 2) {
        return;
    }
    if (state == 0) {
        snap = malloc(MAXO * SZ);
        for (o = *(uintptr_t *)(head + 8), n = 0; snap && o && n < MAXO; o = *(uintptr_t *)(o - 4 + 0xc), n++) {
            objs[n] = o - 4;
            memcpy(snap + n * SZ, (void *)(o - 4), SZ);
        }
        state = 1;
        return;
    }
    state = 2;
    for (i = 0; i < n; i++) {
        const uint32_t *a = (const uint32_t *)(snap + i * SZ), *b = (const uint32_t *)objs[i];
        char line[400];
        int k, c = 0, len;
        len = snprintf(line, sizeof line, "tickrate: object %d %08x vt %08x changed:", i, (unsigned)objs[i],
                       (unsigned)b[0]);
        for (k = 1; k < SZ / 4; k++) {
            if (a[k] != b[k] && k != 0xb0 / 4) {   /* +0xb0: the distance to the camera (the cull) */
                if (c < 12) {
                    len += snprintf(line + len, sizeof line - len, " +%x %.3g>%.3g", k * 4, *(const float *)&a[k],
                                    *(const float *)&b[k]);
                }
                if (!c && !found) {
                    g_watch_addr = objs[i] + k * 4;
                }
                c++;
            }
        }
        if (c) {
            dk_log("%s (%d)", line, c);
            if (!found++) {
                AddVectoredExceptionHandler(1, watch_veh);
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
                CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
            }
        }
    }
    dk_log("tickrate: watch: %d of %d world objects changed", found, n);
}

/* DKT_WATCHS: the sprite table (256 x 0x58 at 0x8b1018) over one tick */
static void watch_sprites_tick(void)
{
    static uint8_t snap[256 * 0x58];
    static int state;
    int i, found = 0;
    if (!RACE_SIM || RACE_PHASE < 3 || GAME_TIME < g_watchs || state == 2) {
        return;
    }
    if (state == 0) {
        memcpy(snap, (void *)0x8b1018, sizeof snap);
        state = 1;
        return;
    }
    state = 2;
    for (i = 0; i < 256; i++) {
        const uint32_t *a = (const uint32_t *)(snap + i * 0x58), *b = (const uint32_t *)(0x8b1018 + i * 0x58);
        char line[500];
        int k, c = 0, len = snprintf(line, sizeof line, "tickrate: sprite %d flags %08x at %.1f,%.1f,%.1f changed:", i,
                                     (unsigned)b[0x15], *(const float *)&b[1], *(const float *)&b[2],
                                     *(const float *)&b[3]);
        for (k = 0; k < 0x58 / 4; k++) {
            if (a[k] != b[k]) {
                len += snprintf(line + len, sizeof line - len, " +%x %08x>%08x", k * 4, (unsigned)a[k], (unsigned)b[k]);
                if (!g_watch_addr && a[1] == b[1] && a[2] == b[2] && k > 3) {   /* a sprite that stays */
                    g_watch_addr = 0x8b1018 + i * 0x58 + k * 4;
                }
                c++;
            }
        }
        if (c && found++ < 40) {
            dk_log("%s", line);
        }
    }
    if (found) {
        AddVectoredExceptionHandler(1, watch_veh);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
    }
    dk_log("tickrate: watch: %d sprites changed", found);
}

static void watch_tick(void)
{
    enum { SZ = 0x200 };
    static uint8_t *snap;
    static uintptr_t *ents;
    static int n, state;
    uintptr_t *tab = ENT_TABLE;
    int i, found = 0;
    if (!RACE_SIM || !tab || RACE_PHASE < 3 || GAME_TIME < g_watch || state == 2) {
        return;
    }
    if (state == 0) {
        n = ENT_COUNT;
        snap = malloc(n * SZ);
        ents = malloc(n * sizeof *ents);
        for (i = 0; snap && ents && i < n; i++) {
            ents[i] = tab[i];
            if (tab[i]) {
                memcpy(snap + i * SZ, (void *)tab[i], SZ);
            }
        }
        state = 1;
        return;
    }
    state = 2;
    for (i = 0; snap && ents && i < n && i < ENT_COUNT; i++) {
        const uint32_t *a = (const uint32_t *)(snap + i * SZ), *b = (const uint32_t *)tab[i];
        uint32_t vt;
        char line[400];
        int k, c = 0, len;
        if (!tab[i] || tab[i] != ents[i]) {
            continue;
        }
        vt = b[0];
        if (vt == VT_HIERARCHY || vt == 0x4f2598u || vt == 0x4f2038u) {   /* cars, their parts, cameras */
            continue;
        }
        len = snprintf(line, sizeof line, "tickrate: entity %d %08x vt %08x at %.1f,%.1f,%.1f changed:", i,
                       (unsigned)tab[i], (unsigned)vt, *(const float *)&b[0x15], *(const float *)&b[0x16],
                       *(const float *)&b[0x17]);
        for (k = 1; k < SZ / 4; k++) {
            if (a[k] != b[k]) {
                if (c < 10) {
                    len += snprintf(line + len, sizeof line - len, " +%x %.3g>%.3g", k * 4, *(const float *)&a[k],
                                    *(const float *)&b[k]);
                }
                if (!c && !found) {
                    g_watch_addr = tab[i] + k * 4;
                }
                c++;
            }
        }
        if (c) {
            dk_log("%s (%d)", line, c);
            if (!found++) {
                AddVectoredExceptionHandler(1, watch_veh);
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
                CloseHandle(CreateThread(NULL, 0, watch_arm, NULL, 0, NULL));
            }
            if (found >= 60) {
                break;
            }
        }
    }
    dk_log("tickrate: watch: %d entities (not cars or cameras) changed", found);
}

/* ---- the ghost car: 30 Hz samples at any rate ---- */

#define REC_FULL    (*(volatile char *)0x7043f0)
#define REC_INDEX   (*(volatile int *)0x54c98c)
#define REC_LAST    (*(volatile int *)0x54f1dc)
#define REC_RING    ((float *)0x574090)
#define PLAY_INDEX  (*(volatile int *)0x54c988)
#define PLAY_LAST   (*(volatile int *)0x54f1d8)
#define PLAY_RING   ((float *)0x54f1f0)
#define PLAY_OUT    ((float *)0x598f40)

static char __cdecl ghost_record(int unused, uintptr_t car)
{
    uintptr_t e;
    float *s;
    (void)unused;
    if (REC_FULL || !tick_of(30, TICK_COUNT, NULL)) {
        return 0;
    }
    s = REC_RING + REC_INDEX * 7;
    e = *(uintptr_t *)(car + 0x30);
    memcpy(s, (void *)(e + 0x68), 12);       /* position */
    memcpy(s + 3, (void *)(e + 0x10), 16);   /* orientation */
    if (REC_LAST < ++REC_INDEX) {
        REC_FULL = 1;
        REC_INDEX = 0;
    }
    return 1;
}

static char __cdecl ghost_play(int unused, float **out)
{
    float frac;
    (void)unused;
    if (!tick_of(30, TICK_COUNT, &frac)) {
        int prev = PLAY_INDEX - 1 < 0 ? 0 : PLAY_INDEX - 1;
        memcpy(PLAY_OUT, PLAY_RING + PLAY_INDEX * 7, 28);
        VEC_Lerp(PLAY_OUT, frac, PLAY_RING + prev * 7, PLAY_RING + PLAY_INDEX * 7);
        *out = PLAY_OUT;
    } else {
        *out = PLAY_RING + PLAY_INDEX * 7;
        PLAY_INDEX++;
    }
    if (PLAY_LAST < PLAY_INDEX) {
        PLAY_INDEX = 0;
        return 0;
    }
    return 1;
}

/* the display's refresh period in us less 1, measured by DWM (6944 at "144" Hz that is really 143.98), 0 if unknown */
static uint32_t display_period_us(void)
{
    HRESULT (WINAPI *get)(HWND, DWM_TIMING_INFO *);
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    DWM_TIMING_INFO t;
    LARGE_INTEGER f;
    uint32_t us = 0;
    get = dwm ? (HRESULT (WINAPI *)(HWND, DWM_TIMING_INFO *))(void (*)(void))GetProcAddress(dwm, "DwmGetCompositionTimingInfo")
              : NULL;
    memset(&t, 0, sizeof t);
    t.cbSize = sizeof t;
    if (get && get(NULL, &t) == 0 && QueryPerformanceFrequency(&f) && t.qpcRefreshPeriod) {
        us = (uint32_t)(t.qpcRefreshPeriod * 1000000u / (uint64_t)f.QuadPart) - 1;
    }
    return us;
}

static int desktop_refresh(void)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        return (int)dm.dmDisplayFrequency;
    }
    return 60;
}


/* the rate for a TickRate setting; takes effect at the next tick (a race at the old rate switches at once) */
static void configure(int setting)
{
    int rate = setting, refresh = desktop_refresh();
    g_refresh = 0;
    g_period_us = display_period_us();
    if (rate == 0) {
        g_rate = 60;   /* = the game's rate at the 60fps option: wanted() changes nothing */
        g_div = 1;
        g_period_us = 0;
        return;
    }
    if (rate < 0) {
        rate = g_refresh = refresh;   /* adapts (hk_wait) */
    }
    if (rate < 60) {
        rate = 60;
    } else if (rate > 1000) {
        rate = 1000;   /* a sanity limit (the tick length is whole microseconds) */
    }
    /* a whole fraction of the refresh rate (or the rate itself) ticks at a multiple of the measured period */
    g_div = refresh % rate == 0 ? refresh / rate : 0;
    if (!g_div || (g_period_us + 1) * (uint64_t)g_div * rate < 990000u
        || (g_period_us + 1) * (uint64_t)g_div * rate > 1010000u) {
        g_period_us = 0;   /* not this rate's (clamped, not a fraction, or DWM reports another display) */
        g_div = 1;
    }
    g_rate = rate;
}

/* the refresh rates the display reports for its modes (EnumDisplaySettings), 60 Hz and above, highest first */
int dk_tickrate_rates(int *out, int max)
{
    DEVMODEA dm;
    DWORD i;
    int n = 0, j, k;
    for (i = 0;; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(NULL, i, &dm)) {
            break;
        }
        if (dm.dmDisplayFrequency < 60) {
            continue;
        }
        for (j = 0; j < n && out[j] != (int)dm.dmDisplayFrequency; j++) {
        }
        if (j == n && n < max) {
            out[n++] = (int)dm.dmDisplayFrequency;
        }
    }
    for (j = 1; j < n; j++) {   /* insertion sort, highest first */
        int v = out[j];
        for (k = j; k > 0 && out[k - 1] < v; k--) {
            out[k] = out[k - 1];
        }
        out[k] = v;
    }
    return n;
}

/* 60 Hz in single player and in network games (see the top) */
static void tick60(void)
{
    static const uint8_t get_rate[7] = { 0x8b, 0x81, 0xb0, 0x00, 0x00, 0x00, 0xc3 };   /* mov eax, [ecx+0xb0]; ret */
    static const uint8_t get_60[7] = { 0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3, 0x90 };     /* mov eax, 2; ret */
    static const uint8_t net_mode[5] = { 0xb9, 0x01, 0x00, 0x00, 0x00 };               /* 0x48cb44: mov ecx, 1 */
    static const uint8_t net_60[5] = { 0xb9, 0x02, 0x00, 0x00, 0x00 };
    static const uint8_t ping_cap[7] = { 0x83, 0xbe, 0x98, 0x18, 0x00, 0x00, 0x3c };   /* cmp [esi+0x1898], 60 */
    static const uint8_t version_end[2] = { 0, 0 }, version_60[2] = { '6', '0' };
    static const uint8_t tune_rate[6] = { 0x8b, 0x15, 0x40, 0x7a, 0x50, 0x00 };   /* 0x440d89: mov edx, [0x507a40] */
    static const uint8_t tune_30[6] = { 0xba, 0x1e, 0x00, 0x00, 0x00, 0x90 };     /* mov edx, 30 */
    static const uint8_t tune_lead[7] = { 0xc7, 0x46, 0x04, 0x01, 0x00, 0x00, 0x00 };   /* 0x440dcf: Lead = 1 */
    static const uint8_t iterations = 120, lead = 3;
    if (!dk_expect(0x489840, get_rate, 7) || !dk_expect(0x48cb44, net_mode, 5) || !dk_expect(0x43f345, ping_cap, 7)
        || !dk_expect(0x4fe24e, version_end, 2) || !dk_expect(0x440d89, tune_rate, 6)
        || !dk_expect(0x440dcf, tune_lead, 7)) {
        dk_log("tickrate: code differs, the game keeps its 30/60 Hz option");
        return;
    }
    dk_write(0x489840, get_60, 7);
    dk_write(0x48cb44, net_60, 5);
    dk_write(0x43f34b, &iterations, 1);
    /* the lockstep's input buffer, (Lead + 1) blocks of Ratio frames, as long in ms as the original's at 30 Hz: Ratio
       from the ping as at 30 Hz, and 4 blocks instead of 2. With Lead 1 it was half as long, and two units on one PC
       had no input for 3.3 % of the 60 loop iterations per second (58.0 ticks/s; with Lead 3 59.95, the original's
       30 Hz 29.95). */
    dk_write(0x440d89, tune_30, 6);
    dk_write(0x440dd2, &lead, 1);
    dk_write(0x4fe24e, version_60, 2);
    dk_log("tickrate: the game ticks at 60 Hz, network games too");
}

void dk_tickrate_install(void)
{
    static const uint8_t c_init[5] = { 0xe8, 0x9c, 0x86, 0x01, 0x00 };   /* call FRAME_InitTiming */
    static const uint8_t c_joy[5] = { 0xe8, 0x7b, 0xcd, 0xfe, 0xff };    /* call JOY_UpdateDevices */
    static const uint8_t c_wait[5] = { 0xe8, 0x5c, 0xdc, 0xff, 0xff };   /* call FRAME_WaitForTick */
    static const uint8_t rec[6] = { 0xa0, 0xf0, 0x43, 0x70, 0x00, 0x53 };
    static const uint8_t play[6] = { 0xa1, 0x40, 0x7a, 0x50, 0x00, 0x56 };
    static const uint8_t f30[4] = { 0x89, 0x88, 0x08, 0x3d }, f70[4] = { 0xa1, 0x0e, 0x6a, 0x3c };
    static const uint8_t f60[4] = { 0x89, 0x88, 0x88, 0x3c };
    static const uint8_t tps[5] = { 0xa1, 0x40, 0x7a, 0x50, 0x00 };
    static const uint8_t PUSH00[4] = { 0x6a, 0x00, 0x6a, 0x00 };   /* push 0; push 0 */
    static const uint8_t MOVECX_TPS[6] = { 0x8b, 0x0d, 0x40, 0x7a, 0x50, 0x00 };   /* mov ecx, [0x507a40] */
    int setting = dk_ini_int("Graphics", "ExperimentalTickRate", 0);
    unsigned i;
    char v[8];
    if (dk_ini_int("Graphics", "TickRate", 0)) {
        /* written by the HIGH REFRESH RATE row of earlier versions; the game now ticks at 60 Hz */
        dk_log("tickrate: TickRate=%d of an earlier version ignored (ExperimentalTickRate sets it now)",
               dk_ini_int("Graphics", "TickRate", 0));
    }
    g_log = GetEnvironmentVariableA("DKT_LOG", v, sizeof v) > 0 ? atoi(v) : 0;
    g_watch = GetEnvironmentVariableA("DKT_WATCH", v, sizeof v) > 0 ? atoi(v) : 0;
    g_watchl = GetEnvironmentVariableA("DKT_WATCHL", v, sizeof v) > 0 ? atoi(v) : 0;
    g_watchs = GetEnvironmentVariableA("DKT_WATCHS", v, sizeof v) > 0 ? atoi(v) : 0;
    g_watchp = GetEnvironmentVariableA("DKT_WATCHP", v, sizeof v) > 0 ? atoi(v) : 0;
    g_difftick = GetEnvironmentVariableA("DKT_DIFFTICK", v, sizeof v) > 0 ? atoi(v) : 0;
    g_qtrace = GetEnvironmentVariableA("DKT_QTRACE", v, sizeof v) > 0 ? atoi(v) : 0;
    g_watch_all = GetEnvironmentVariableA("DKT_WATCHALL", v, sizeof v) > 0;
    {
        char w[16];
        g_watcha = GetEnvironmentVariableA("DKT_WATCHA", w, sizeof w) > 0 ? strtoul(w, NULL, 16) : 0;
    }
    tick60();
    if (setting == 0) {
        /* the game's 60 Hz (the test switches only log) */
        if ((g_log || g_watch || g_watchl || g_watchs || g_watchp || g_difftick || g_watcha || g_qtrace)
            && dk_expect(0x48f180, c_joy, 5)) {
            dk_call(0x48f180, hk_tick);
        }
        return;
    }
    if (!dk_expect(0x47488f, c_init, 5) || !dk_expect(0x48f180, c_joy, 5) || !dk_expect(0x48f16f, c_wait, 5)
        || !dk_expect(0x485180, rec, 6) || !dk_expect(0x485230, play, 6) || !dk_expect(0x40c09e, f30, 4)
        || !dk_expect(0x40c0a5, f70, 4) || !dk_expect(0x40d361, f30, 4) || !dk_expect(0x4204c2, f60, 4)
        || *(float *)0x41246a != 1.0f || *(int32_t *)0x41f82a != 300
        || *(float *)0x4270d5 != PICKUP_STEP || *(float *)0x4270e7 != PICKUP_STEP
        || !dk_expect(0x483e30, MOVECX_TPS, 6) || !dk_expect(0x471f88, PUSH00, 4) || !dk_expect(0x471fd1, PUSH00, 4)
        || !dk_expect(0x4862e0, tps, 5) || !dk_expect(0x486350, tps, 5) || !dk_expect(0x4389c1, OFFTRACK_ORIG, 20)
        || *(uint32_t *)0x438a41 != 0x806c50) {
        dk_log("tickrate: code differs, not installed");
        return;
    }
    for (i = 0; i < sizeof FACTORS / sizeof FACTORS[0]; i++) {
        if (*(float *)FACTORS[i].addr != FACTORS[i].k) {
            dk_log("tickrate: constant %08x differs, not installed", FACTORS[i].addr);
            return;
        }
    }
    for (i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        if (*(uint32_t *)STEPS[i].disp != STEPS[i].orig) {
            dk_log("tickrate: operand %08x differs, not installed", STEPS[i].disp);
            return;
        }
    }
    for (i = 0; i < sizeof CALLS / sizeof CALLS[0]; i++) {
        if (!dk_expect(CALLS[i].addr, CALLS[i].orig, 6)) {
            dk_log("tickrate: code at %08x differs, not installed", CALLS[i].addr);
            return;
        }
    }
    configure(setting);
    dk_jmp(0x485180, ghost_record);
    dk_jmp(0x485230, ghost_play);
    dk_call(0x47488f, hk_init_timing);
    dk_call(0x48f180, hk_tick);
    dk_call(0x48f16f, hk_wait);
    timeBeginPeriod(1);   /* 1 ms resolution for the waits */
    dk_log("tickrate: single-player races tick at %d Hz (tick %u us)%s", g_rate,
           g_period_us ? g_div * (g_period_us + 1) - 1 : 1000000u / g_rate,
           g_refresh ? ", lower if frames can't keep up" : "");
}
