/* Force feedback -> gamepad rumble (docs/gamepad.md section 5).
 *
 * The game's force feedback (an Immersion IFORCE2 project, Joystick.ifr / Wheel.ifr: effect 0 "Spring"
 * = steering centring, 1 "Out_of_control" = damper, 2 "Explosion" = periodic, 3 "Collision" = vector force)
 * is driven through a handful of thin wrappers in the 0x478830 area, all taking the JOY joystick index
 * first. The fake joystick has no DIDC_FORCEFEEDBACK, so none of it runs for an SDL pad. These are the
 * call sites in the car code; they are redirected to the functions below, which turn the events into
 * SDL_GameControllerRumble for an SDL pad and call the original for any other joystick:
 *   FF_PlayEffect(joy, fx, flags)       0x478830   start an effect
 *   FF_SetGain(joy, fx, gain float)     0x4788c0   magnitude 0..1 (collision strength)
 *   FF_SetDirection(joy, fx, centideg)  0x478910   (collision direction, unused for rumble)
 *   FF_IsPlaying(joy, fx)               0x478960
 *   FF_StopEffect(joy, fx)              0x4789b0
 *   FF_StopAll(joy)                     0x4789e0
 *   FF_IsEnabled(joy)                   0x478b20   (the options' Force Feedback switch)
 *   FF_IsWheel(joy)                     0x478b00   (joystick type 3)
 * Effects used by the car code: 0 at race start/resume and after recovering, 1 while out of control,
 * 3 on every collision with the gain set from the impact (CAR_ApplyPlayerInput 0x415a6b..0x415a95). */
#include <math.h>
#include <string.h>
#include "dkpatch.h"
#include "pad.h"

int pad_slot_for_joystick(int joy);          /* gamepad.c */

typedef int (*fn_play)(int, int, int);
typedef int (*fn_gain)(int, int, float);
typedef int (*fn_dir)(int, int, int);
typedef int (*fn_2)(int, int);
typedef int (*fn_1)(int);

#define O_PLAY    ((fn_play)0x478830)
#define O_GAIN    ((fn_gain)0x4788c0)
#define O_DIR     ((fn_dir)0x478910)
#define O_PLAYING ((fn_2)0x478960)
#define O_STOP    ((fn_2)0x4789b0)
#define O_STOPALL ((fn_1)0x4789e0)
#define O_ENABLED ((fn_1)0x478b20)
#define O_WHEEL   ((fn_1)0x478b00)

static int g_on;
static int g_strength = 100;            /* percent */

typedef struct {
    float gain[4];                       /* last gain per effect */
    DWORD hit_t;                         /* collision start (0 = none) */
    float hit;                           /* collision peak 0..1 */
    int ooc;                             /* out of control damper is on */
    DWORD boom_t;                        /* explosion start */
    int last_low, last_high;
    DWORD last_t;
    int logged;                          /* first rumble of this pad logged (1 ok, -1 failed) */
} rumble_t;

static rumble_t g_r[PAD_MAX];

#define HIT_MS   260
#define BOOM_MS  800

static int slot_of(int joy)
{
    return g_on ? pad_slot_for_joystick(joy) : -1;
}

static int w_play(int joy, int fx, int flags)
{
    int s = slot_of(joy);
    rumble_t *r;
    if (s < 0) {
        return O_PLAY(joy, fx, flags);
    }
    r = &g_r[s];
    switch (fx) {
    case 1: r->ooc = 1; break;
    case 2: r->boom_t = timeGetTime() | 1; break;
    case 3:
        r->hit_t = timeGetTime() | 1;
        r->hit = r->gain[3] > 0.05f ? (r->gain[3] > 1 ? 1 : r->gain[3]) : 0.05f;
        break;
    case 0: r->ooc = 0; break;
    }
    return 1;
}

static int w_gain(int joy, int fx, float gain)
{
    int s = slot_of(joy);
    if (s < 0) {
        return O_GAIN(joy, fx, gain);
    }
    if (fx >= 0 && fx < 4) {
        g_r[s].gain[fx] = gain;
    }
    return 1;
}

static int w_dir(int joy, int fx, int centideg)
{
    return slot_of(joy) < 0 ? O_DIR(joy, fx, centideg) : 1;
}

static int w_playing(int joy, int fx)
{
    int s = slot_of(joy);
    if (s < 0) {
        return O_PLAYING(joy, fx);
    }
    return fx == 3 && g_r[s].hit_t && timeGetTime() - g_r[s].hit_t < HIT_MS;
}

static int w_stop(int joy, int fx)
{
    int s = slot_of(joy);
    if (s < 0) {
        return O_STOP(joy, fx);
    }
    if (fx == 1) {
        g_r[s].ooc = 0;
    } else if (fx == 3) {
        g_r[s].hit_t = 0;
    }
    return 1;
}

static int w_stopall(int joy)
{
    int s = slot_of(joy);
    if (s < 0) {
        return O_STOPALL(joy);
    }
    g_r[s].ooc = 0;
    g_r[s].hit_t = 0;
    g_r[s].boom_t = 0;
    return 1;
}

static int w_enabled(int joy)
{
    return slot_of(joy) >= 0 ? 1 : O_ENABLED(joy);
}

static int w_wheel(int joy)
{
    return slot_of(joy) >= 0 ? 0 : O_WHEEL(joy);
}

/* motors for a slot right now: low = big (heavy) motor, high = small (buzz) motor, 0..65535 */
static void motors(rumble_t *r, DWORD now, int *low, int *high)
{
    float l = 0, h = 0;
    if (r->hit_t && now - r->hit_t < HIT_MS) {
        float k = 1.0f - (float)(now - r->hit_t) / HIT_MS;
        l = r->hit * k;
        h = 0.55f * r->hit * k;
    }
    if (r->boom_t && now - r->boom_t < BOOM_MS) {
        float k = 1.0f - (float)(now - r->boom_t) / BOOM_MS;
        if (k > l) {
            l = k;
        }
        h = h > 0.8f * k ? h : 0.8f * k;
    }
    if (r->ooc) {                        /* a steady low growl while the car is out of control */
        if (l < 0.22f) {
            l = 0.22f;
        }
    }
    l *= g_strength / 100.0f;
    h *= g_strength / 100.0f;
    *low = (int)(l * 65535.0f);
    *high = (int)(h * 65535.0f);
    if (*low > 65535) {
        *low = 65535;
    }
    if (*high > 65535) {
        *high = 65535;
    }
}

static void rumble(SDL_GameController *gc, rumble_t *r, int low, int high)
{
    int ok = SDL_GameControllerRumble(gc, (Uint16)low, (Uint16)high, 100) == 0 ? 1 : -1;
    if ((low || high) && r->logged != ok) {
        if (ok > 0) {
            dk_log("gamepad: rumble works on %s", SDL_GameControllerName(gc));
        } else {
            dk_log("gamepad: rumble failed on %s: %s", SDL_GameControllerName(gc), SDL_GetError());
        }
        r->logged = ok;
    }
}

void padrumble_frame(void)
{
    DWORD now;
    int s;
    if (!g_on) {
        return;
    }
    now = timeGetTime();
    for (s = 0; s < PAD_MAX; s++) {
        SDL_GameController *gc = pad_controller(s);
        rumble_t *r = &g_r[s];
        int low, high;
        if (!gc) {
            continue;
        }
        motors(r, now, &low, &high);
        /* SDL keeps a rumble running for the duration given; refresh every 60 ms while it is non-zero */
        if ((low != r->last_low || high != r->last_high) && now - r->last_t >= 20) {
            r->last_low = low;
            r->last_high = high;
            r->last_t = now;
            rumble(gc, r, low, high);
        } else if ((low || high) && now - r->last_t >= 60) {
            r->last_t = now;
            rumble(gc, r, low, high);
        }
    }
}

/* test harness: "pad ffplay <fx>" / "pad ffgain <v>" drive the same entry points on joystick 0's slot */
void padrumble_test(const char *what, float value)
{
    int joy = pad_slot_for_joystick(0) >= 0 ? 0 : -1, k;
    if (joy < 0) {
        k = 0;
        while (k < 4 && pad_slot_for_joystick(k) < 0) {
            k++;
        }
        joy = k < 4 ? k : -1;
    }
    if (joy < 0) {
        return;
    }
    if (!strcmp(what, "ffgain")) {
        w_gain(joy, 3, value);
    } else if (!strcmp(what, "ffplay")) {
        w_play(joy, (int)value, 0);
    } else if (!strcmp(what, "ffstop")) {
        w_stop(joy, (int)value);
    }
}

static void hook(uint32_t site, uint32_t target, void *fn)
{
    unsigned char old[5], now[5];
    uint32_t rel = target - (site + 5), nrel = (uint32_t)(uintptr_t)fn - (site + 5);
    old[0] = now[0] = 0xe8;
    memcpy(old + 1, &rel, 4);
    memcpy(now + 1, &nrel, 4);
    if (!dk_patch(site, old, now, 5)) {
        dk_log("gamepad: rumble: call at %x does not match, left alone", site);
    }
}

void padrumble_install(void)
{
    g_strength = dk_ini_int("Gamepad", "Rumble", 1) ? dk_ini_int("Gamepad", "RumbleStrength", 100) : 0;
    if (g_strength <= 0) {
        dk_log("gamepad: rumble off");
        return;
    }
    g_strength = g_strength > 100 ? 100 : g_strength;
    hook(0x415a6b, 0x478830, w_play);
    hook(0x415a83, 0x478910, w_dir);
    hook(0x415a95, 0x4788c0, w_gain);
    hook(0x4158d7, 0x478960, w_playing);
    hook(0x4158fb, 0x478b20, w_enabled);
    hook(0x415966, 0x478b00, w_wheel);
    hook(0x4159a8, 0x478b00, w_wheel);
    hook(0x42277c, 0x4789b0, w_stop);
    hook(0x42278f, 0x478830, w_play);
    hook(0x422767, 0x478b20, w_enabled);
    hook(0x422cb3, 0x4789b0, w_stop);
    hook(0x422cc6, 0x478830, w_play);
    hook(0x422c9e, 0x478b20, w_enabled);
    hook(0x416c81, 0x478830, w_play);
    hook(0x41743d, 0x4789e0, w_stopall);
    hook(0x417450, 0x478830, w_play);
    hook(0x41742a, 0x478b20, w_enabled);
    g_on = 1;
    dk_log("gamepad: rumble on (strength %d%%)", g_strength);
}
