/* Gamepads through SDL2 (docs/gamepad.md).
 *
 * The game's input library (Beam Software "JOY", 0x47b590..0x47c470) takes up to 4 DirectInput
 * joysticks, reads X/Y and 10 buttons of a DIJOYSTATE, and steers analog only for devices whose
 * DirectInput subtype is a joystick or wheel ("gamepad" subtypes steer digitally). Modern pads
 * through DirectInput report the triggers as one shared Z axis, which the game ignores.
 *
 * The IDirectInputA the game creates is wrapped: its joystick enumeration hides the DirectInput
 * devices SDL handles as gamepads and offers the SDL gamepads instead, as joysticks of their own
 * (fake IDirectInputDevice2A objects filled from SDL_GameController):
 *   X = left stick (D-pad left/right = full), Y = triggers in a race (RT accelerates, LT brakes),
 *   left stick in menus (D-pad up/down = full in both), buttons 0..9 = X Y A RB LB B Back Start
 *   LS RS (in the frontend: A selects, RB/LB go to the next/previous item, B does nothing).
 * Menus: see pad_fe.c. Start also acts as Escape (pause) while a race is running, and while an intro movie plays any
 * button skips it (the movie's skip reads the keyboard, so a button reports the key as held).
 * SDL slots are kept for gamepads
 * plugged in later (at least one slot always exists). Other DirectInput joysticks (wheels, sticks)
 * stay as they were and come first, so their numbers don't change. The game enumerated without
 * limit before (its tables hold 4); at most 4 joysticks are offered now.
 *
 * Everything SDL runs on the game thread: initialised from the game's EnumDevices call, pumped from
 * the devices' Poll and from GetAsyncKeyState. With the test harness (DK_UNIT) only a virtual SDL
 * gamepad exists, driven by "pad" commands (dk_gamepad_command). */
#include "pad.h"
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

#define MAX_JOY      PAD_MAX                /* the JOY library's device slots */
#define GAME_BUTTONS 10                /* buttons the JOY library maps to bits (0x10..0x2000) */
#define GAME_HWND    (*(HWND *)0x53f694)
#define FE_VISIBLE   (*(volatile char *)0x4fc34a)   /* render layer 2: the frontend (menus, pause menu) */
#define RACE_DRAWN   (*(volatile char *)0x50ef60)   /* FE_Render's argument: a race is drawn underneath */

/* The intro/attract screens play an FMV and skip it on keyboard action 0x1000 (FUN_004850a0), which
   comes from JOY device 4 (the keyboard, JOY_ReadKeyboard 0x47d800 over this VK->bits table through the
   GetAsyncKeyState we hook). The joysticks don't feed it, so a gamepad couldn't skip the movie. While an
   FMV plays, a gamepad button reports that table's action-0x1000 key as held, through the same hook. */
#define FMV_PLAYING()  (*(volatile int *)0x53f53c != 0 && *(volatile int *)0x53f544 != 0)   /* FUN_0047a6f0 */

/* the intro/attract movies play on the "FMV Screen" and "Demosplash Screen". The movie flag is also set by the
   video walls inside a race: without this check a face button held down in the pause menu or a dialog reported
   the confirm key and cancelled the dialog (or accelerated the car, key W) */
static int fmv_screen(void)
{
    uintptr_t d = *(volatile uintptr_t *)0x525aec;
    const char *n = d ? *(const char **)(d + 0x14) : NULL;
    return n && (!strcmp(n, "FMV Screen") || !strcmp(n, "Demosplash Screen"));
}
#define KBD_TABLE      (*(const uint32_t **)0x5402f0)   /* JOY_ReadKeyboard's {VK, bits} pairs */
#define KBD_COUNT      (*(const uint32_t *)0x5402f8)
#define CONFIRM_BITS   0x1000           /* the keyboard action the FMV skip and menu confirm check */

/* {d4c3b2a1-6b1e-4a5f-9d2c-44 4b 50 41 44 00 00 0k}: "DKPAD", k = slot */
static const GUID PAD_GUID = { 0xd4c3b2a1, 0x6b1e, 0x4a5f, { 0x44, 0x4b, 0x50, 0x41, 0x44, 0, 0, 0 } };
static const GUID IID_DIDevice2A = { 0x5944e682, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DIDeviceA = { 0x5944e680, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_Unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID AXIS_X = { 0xa36d02e0, 0xc9f3, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID AXIS_Y = { 0xa36d02e1, 0xc9f3, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID BUTTON = { 0xa36d02f0, 0xc9f3, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };

/* game button i <- SDL button. The frontend's joystick bindings (CTL_InitBindings 0x484730 case 0x15,
   handled in FE_HandleControllerActions 0x449da0): button 0 held = mouse click at the cursor, 1 and 2 = activate the focused widget,
   3 = next widget, 4 = previous, 5 = nothing (the menus have no "back" key: they have BACK widgets) */
static const SDL_GameControllerButton BUTTON_MAP[GAME_BUTTONS] = {
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
};

static int g_deadzone = 10;          /* percent, left stick */
static int g_trigger_throttle = 1;

static int g_on;                     /* installed */
static int g_sdl;                    /* 1 = SDL is up, -1 = it failed */
static DWORD g_thread;               /* the game thread (SDL's) */

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
    SDL_JoystickGUID last;           /* the pad this slot had last, preferred when it comes back */
    int had;
    int start;                       /* Start held (Escape) */
} pad_t;

static pad_t g_pads[MAX_JOY];
static int g_nslots;                 /* SDL slots offered to the game */
static int g_nreal;                  /* other DirectInput joysticks offered before the SDL slots */

/* the JOY joystick index of an SDL slot (what the options' controller choice refers to: type 3 + index) */
int pad_joystick_for_slot(int slot) { return g_nreal + slot; }

/* JOY joystick index (as the game numbers them) -> SDL pad slot, or -1 for a real DirectInput device */
int pad_slot_for_joystick(int joy)
{
    int s = joy - g_nreal;
    return g_on && joy >= g_nreal && s >= 0 && s < g_nslots && g_pads[s].gc ? s : -1;
}

/* ---- SDL ---- */

static void pad_name(int slot, char *out, size_t size)
{
    const char *name = g_pads[slot].gc ? SDL_GameControllerName(g_pads[slot].gc) : NULL;
    if (name && *name) {
        snprintf(out, size, "%s", name);
    } else {
        snprintf(out, size, "Gamepad %d", slot + 1);
    }
}

static int find_id(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < MAX_JOY; i++) {
        if (g_pads[i].gc && g_pads[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* opens joystick device index if it is a gamepad, into a free slot (< limit); the slot or -1 */
static int open_pad(int index, int limit)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    int i, slot = -1;
    char name[128];
    if (!SDL_IsGameController(index) || id < 0 || find_id(id) >= 0) {
        return -1;
    }
    for (i = 0; i < limit && slot < 0; i++) {
        if (!g_pads[i].gc && g_pads[i].had && !memcmp(&g_pads[i].last, &guid, sizeof guid)) {
            slot = i;
        }
    }
    for (i = 0; i < limit && slot < 0; i++) {
        if (!g_pads[i].gc) {
            slot = i;
        }
    }
    if (slot < 0) {
        dk_log("gamepad: %s connected, but all %d gamepad slots are taken", SDL_GameControllerNameForIndex(index), limit);
        return -1;
    }
    g_pads[slot].gc = SDL_GameControllerOpen(index);
    if (!g_pads[slot].gc) {
        dk_log("gamepad: cannot open %s: %s", SDL_GameControllerNameForIndex(index), SDL_GetError());
        return -1;
    }
    g_pads[slot].id = id;
    g_pads[slot].last = guid;
    g_pads[slot].had = 1;
    pad_name(slot, name, sizeof name);
    dk_log("gamepad: %s connected (gamepad %d, VID %04x PID %04x)", name, slot + 1,
           SDL_GameControllerGetVendor(g_pads[slot].gc), SDL_GameControllerGetProduct(g_pads[slot].gc));
    return slot;
}

static void close_pad(int slot)
{
    char name[128];
    pad_name(slot, name, sizeof name);
    dk_log("gamepad: %s disconnected (gamepad %d)", name, slot + 1);
    SDL_GameControllerClose(g_pads[slot].gc);
    g_pads[slot].gc = NULL;
    g_pads[slot].start = 0;
}

/* ---- test harness: one virtual SDL gamepad ---- */

enum { CMD_BUTTON, CMD_AXIS, CMD_PLUG, CMD_FF };
void padrumble_test(const char *what, float value);

static int test_rumble(void *user, Uint16 low, Uint16 high)
{
    (void)user;
    dk_log("test: rumble low %u high %u", low, high);
    return 0;
}

static CRITICAL_SECTION g_cmd_lock;
static struct { int kind; int control; float value; char name[32]; } g_cmds[64];
static int g_ncmds;
static SDL_Joystick *g_virtual;

static void plug_virtual(int on)
{
    if (on && !g_virtual) {
        SDL_VirtualJoystickDesc desc;
        int index;
        memset(&desc, 0, sizeof desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = "Virtual Gamepad";
        desc.Rumble = test_rumble;
        index = SDL_JoystickAttachVirtualEx(&desc);
        g_virtual = index >= 0 ? SDL_JoystickOpen(index) : NULL;
        if (g_virtual) {
            SDL_JoystickSetVirtualAxis(g_virtual, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            SDL_JoystickSetVirtualAxis(g_virtual, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        }
        dk_log("test: virtual gamepad %s", g_virtual ? "plugged in" : SDL_GetError());
    } else if (!on && g_virtual) {
        SDL_JoystickID id = SDL_JoystickInstanceID(g_virtual);
        int i;
        SDL_JoystickClose(g_virtual);
        g_virtual = NULL;
        for (i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_JoystickGetDeviceInstanceID(i) == id) {
                SDL_JoystickDetachVirtual(i);
            }
        }
        dk_log("test: virtual gamepad unplugged");
    }
}

static void test_apply(void)
{
    int i;
    if (!dk_unit || !g_ncmds) {
        return;
    }
    EnterCriticalSection(&g_cmd_lock);
    for (i = 0; i < g_ncmds; i++) {
        if (g_cmds[i].kind == CMD_FF) {
            padrumble_test(g_cmds[i].name, g_cmds[i].value);
            continue;
        }
        plug_virtual(g_cmds[i].kind != CMD_PLUG || g_cmds[i].value != 0);
        if (!g_virtual) {
            continue;
        }
        if (g_cmds[i].kind == CMD_AXIS) {
            int trigger = g_cmds[i].control == SDL_CONTROLLER_AXIS_TRIGGERLEFT
                || g_cmds[i].control == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            float v = g_cmds[i].value;     /* -1..1, triggers 0..1 */
            int raw = trigger ? (int)(-32768 + v * 65535) : (int)(v * 32767);
            SDL_JoystickSetVirtualAxis(g_virtual, g_cmds[i].control, (Sint16)(raw < -32768 ? -32768 : raw > 32767 ? 32767 : raw));
        } else if (g_cmds[i].kind == CMD_BUTTON) {
            SDL_JoystickSetVirtualButton(g_virtual, g_cmds[i].control, g_cmds[i].value != 0);
        }
    }
    g_ncmds = 0;
    LeaveCriticalSection(&g_cmd_lock);
}

/* "pad <control> <value> [<control> <value> ...]" from the test harness's command file (any thread);
   controls are SDL names (a, b, start, dpup, leftx, righttrigger, ...), axes -1..1, triggers 0..1,
   or plug 0|1 (the pad is plugged in by its first command) */
void dk_gamepad_command(const char *args)
{
    char control[32];
    float value;
    int n;
    if (!g_on) {
        return;
    }
    EnterCriticalSection(&g_cmd_lock);
    while (sscanf(args, " %31s %f%n", control, &value, &n) == 2 && g_ncmds < 64) {
        SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(control);
        SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(control);
        if (!strncmp(control, "ff", 2)) {
            g_cmds[g_ncmds].kind = CMD_FF;
            snprintf(g_cmds[g_ncmds].name, sizeof g_cmds[0].name, "%s", control);
        } else if (!strcmp(control, "plug")) {
            g_cmds[g_ncmds].kind = CMD_PLUG;
        } else if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            g_cmds[g_ncmds].kind = CMD_AXIS;
            g_cmds[g_ncmds].control = axis;
        } else if (button != SDL_CONTROLLER_BUTTON_INVALID) {
            g_cmds[g_ncmds].kind = CMD_BUTTON;
            g_cmds[g_ncmds].control = button;
        } else {
            dk_log("test: pad: unknown control %s", control);
            args += n;
            continue;
        }
        g_cmds[g_ncmds++].value = value;
        args += n;
    }
    LeaveCriticalSection(&g_cmd_lock);
}

/* ---- per frame ---- */

void pad_post_key(int vk, int up)
{
    HWND hwnd = GAME_HWND;
    LPARAM lp = 1 | ((LPARAM)MapVirtualKeyA(vk, 0) << 16);
    if (hwnd) {
        PostMessageA(hwnd, up ? WM_KEYUP : WM_KEYDOWN, vk, up ? lp | ((LPARAM)3 << 30) : lp);
    }
}

static void post_escape(int up) { pad_post_key(VK_ESCAPE, up); }

SDL_GameController *pad_controller(int slot) { return slot >= 0 && slot < MAX_JOY ? g_pads[slot].gc : NULL; }
int pad_frontend(void) { return FE_VISIBLE != 0; }
int pad_racing(void) { return RACE_DRAWN && !FE_VISIBLE; }
int pad_race_drawn(void) { return RACE_DRAWN != 0; }

int pad_any_button(SDL_GameControllerButton b)
{
    int i;
    for (i = 0; i < MAX_JOY; i++) {
        if (g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, b)) {
            return 1;
        }
    }
    return 0;
}

/* SDL events (hotplug), at most every 4 ms, game thread only */
static void pump(void)
{
    static DWORD last;
    SDL_Event ev[16];
    DWORD now;
    int n, i, slot;
    if (g_sdl != 1 || GetCurrentThreadId() != g_thread) {
        return;
    }
    now = timeGetTime();
    if (now - last < 4) {
        return;
    }
    last = now;
    test_apply();
    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED)) > 0) {
        for (i = 0; i < n; i++) {
            if (ev[i].type == SDL_CONTROLLERDEVICEADDED) {
                open_pad(ev[i].cdevice.which, g_nslots);
            } else if (ev[i].type == SDL_CONTROLLERDEVICEREMOVED && (slot = find_id(ev[i].cdevice.which)) >= 0) {
                close_pad(slot);
            }
        }
    }
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    padfe_frame();
    padrumble_frame();

    /* Start = Escape while a race runs (pause; in menus B goes back, and Escape on the title quits) */
    for (i = 0; i < MAX_JOY; i++) {
        int down = g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, SDL_CONTROLLER_BUTTON_START);
        if (down && !g_pads[i].start && RACE_DRAWN) {
            g_pads[i].start = 1;
            if (FE_VISIBLE && padfe_start()) {
                g_pads[i].start = 2;     /* resumed the pause menu: no Escape (it would pause again) */
                continue;
            }
            post_escape(0);
        } else if (!down && g_pads[i].start) {
            if (g_pads[i].start == 1) {
                post_escape(1);
            }
            g_pads[i].start = 0;
        }
    }
}

static int sdl_init(void)
{
    if (g_sdl) {
        return g_sdl == 1;
    }
    g_thread = GetCurrentThreadId();
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (dk_unit) {
        /* test units share the PC's real controllers: only the virtual one */
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    } else {
        /* Xbox pads through XInput, not SDL's RawInput driver: that one can only rumble once it has matched the device
           with its XInput/WGI twin by input ("Controller isn't correlated yet"), so rumble failed (2026-10-21, an Xbox
           One controller: -1 through RawInput, 0 and felt through XInput). PlayStation/Switch pads stay on HIDAPI. */
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    }
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        dk_log("gamepad: SDL_Init failed: %s", SDL_GetError());
        g_sdl = -1;
        return 0;
    }
    {
        /* optional extra mappings (https://github.com/mdqinc/SDL_GameControllerDB) next to the exe */
        char path[MAX_PATH + 32], *slash;
        GetModuleFileNameA(NULL, path, MAX_PATH);
        slash = strrchr(path, '\\');
        strcpy(slash ? slash + 1 : path, "gamecontrollerdb.txt");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            int n = SDL_GameControllerAddMappingsFromFile(path);
            dk_log("gamepad: %d mappings from %s", n, path);
        }
    }
    {
        SDL_version v;
        SDL_GetVersion(&v);
        dk_log("gamepad: SDL %d.%d.%d", v.major, v.minor, v.patch);
    }
    g_sdl = 1;
    return 1;
}

/* ---- fake joystick (IDirectInputDevice2A) ---- */

typedef struct {
    const void *const *vtbl;
    int slot;
    LONG min, max;                   /* DIPROP_RANGE */
    DWORD deadzone;                  /* DIPROP_DEADZONE, 0..10000 */
} pad_dev;

static pad_dev g_devs[MAX_JOY];

static double stick(SDL_GameController *gc, SDL_GameControllerAxis axis, double dz)
{
    double v = SDL_GameControllerGetAxis(gc, axis) / 32767.0;
    double a = fabs(v);
    if (v < -1) {
        v = -1;
    }
    if (a <= dz) {
        return 0;
    }
    a = (a - dz) / (1 - dz);
    return v < 0 ? -(a > 1 ? 1 : a) : (a > 1 ? 1 : a);
}

/* the race's analog pedals are 2 * Y (gas for Y < 0, brake for Y > 0, JOY bits "up"/"down" past
   +-0.5 switch to the digital ramp), so a trigger maps to half of Y and only a full pull to all of it */
static double pedals(SDL_GameController *gc)
{
    double rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0;
    double lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0;
    rt = rt < 0.05 ? 0 : rt;
    lt = lt < 0.05 ? 0 : lt;
    if (rt >= 0.97) {
        return -1;
    }
    if (lt >= 0.97) {
        return 1;
    }
    return (lt - rt) * 0.5;
}

static LONG to_range(const pad_dev *d, double v)
{
    return d->min + (LONG)floor((v + 1) * 0.5 * (double)(d->max - d->min) + 0.5);
}

static void read_state(pad_dev *d, DIJOYSTATE *js)
{
    SDL_GameController *gc = g_pads[d->slot].gc;
    double x = 0, y = 0;
    unsigned char btn[GAME_BUTTONS];
    int i;
    memset(js, 0, sizeof *js);
    for (i = 0; i < 4; i++) {
        js->rgdwPOV[i] = 0xffffffff;
    }
    if (gc) {
        double dz = g_deadzone / 100.0;
        if (d->deadzone / 10000.0 > dz) {
            dz = d->deadzone / 10000.0;      /* the game's own dead zone option */
        }
        if (dz > 0.9) {
            dz = 0.9;
        }
        x = stick(gc, SDL_CONTROLLER_AXIS_LEFTX, dz);
        y = (g_trigger_throttle && RACE_DRAWN && !FE_VISIBLE) ? pedals(gc) : stick(gc, SDL_CONTROLLER_AXIS_LEFTY, dz);
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            x = -1;
        } else if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            x = 1;
        }
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            y = -1;
        } else if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            y = 1;
        }
        for (i = 0; i < GAME_BUTTONS; i++) {
            btn[i] = SDL_GameControllerGetButton(gc, BUTTON_MAP[i]) ? 0x80 : 0;
        }
        padfe_filter(d->slot, &x, &y, btn);
        memcpy(js->rgbButtons, btn, GAME_BUTTONS);
    }
    js->lX = to_range(d, x);
    js->lY = to_range(d, y);
    js->lZ = js->lRx = js->lRy = js->lRz = to_range(d, 0);
}

static void instance(int slot, DIDEVICEINSTANCEA *di)
{
    memset(di, 0, sizeof *di);
    di->dwSize = sizeof *di;
    di->guidInstance = PAD_GUID;
    di->guidInstance.Data4[7] = (BYTE)slot;
    if (g_pads[slot].gc) {
        di->guidProduct.Data1 = MAKELONG(SDL_GameControllerGetVendor(g_pads[slot].gc), SDL_GameControllerGetProduct(g_pads[slot].gc));
    }
    memcpy(di->guidProduct.Data4, "\0\0PIDVID", 8);
    di->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_TRADITIONAL << 8) | DIDEVTYPE_HID;   /* analog in the game */
    pad_name(slot, di->tszProductName, sizeof di->tszProductName);
    snprintf(di->tszInstanceName, sizeof di->tszInstanceName, "%s", di->tszProductName);
}

static HRESULT WINAPI dev_QueryInterface(pad_dev *d, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_DIDevice2A) || IsEqualGUID(iid, &IID_DIDeviceA) || IsEqualGUID(iid, &IID_Unknown)) {
        *out = d;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dev_AddRef(pad_dev *d) { (void)d; return 2; }      /* static objects */
static ULONG WINAPI dev_Release(pad_dev *d) { (void)d; return 1; }

static HRESULT WINAPI dev_GetCapabilities(pad_dev *d, DIDEVCAPS *caps)
{
    DIDEVICEINSTANCEA di;
    DWORD size = caps->dwSize;
    if (size < 0x18) {
        return DIERR_INVALIDPARAM;
    }
    instance(d->slot, &di);
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED;
    caps->dwDevType = di.dwDevType;
    caps->dwAxes = 2;
    caps->dwButtons = GAME_BUTTONS;
    return DI_OK;
}

static HRESULT WINAPI dev_EnumObjects(pad_dev *d, LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *ref, DWORD flags)
{
    DIDEVICEOBJECTINSTANCEA o;
    int i;
    (void)d;
    for (i = 0; i < 2 + GAME_BUTTONS; i++) {
        memset(&o, 0, sizeof o);
        o.dwSize = sizeof o;
        if (i < 2) {
            if (flags && !(flags & DIDFT_AXIS)) {
                continue;
            }
            o.guidType = i ? AXIS_Y : AXIS_X;
            o.dwOfs = i ? DIJOFS_Y : DIJOFS_X;
            o.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
            strcpy(o.tszName, i ? "Y Axis" : "X Axis");
        } else {
            if (flags && !(flags & DIDFT_BUTTON)) {
                continue;
            }
            o.guidType = BUTTON;
            o.dwOfs = DIJOFS_BUTTON(i - 2);
            o.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i - 2);
            sprintf(o.tszName, "Button %d", i - 1);
        }
        if (cb(&o, ref) == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetProperty(pad_dev *d, REFGUID prop, DIPROPHEADER *h)
{
    if ((uintptr_t)prop == 4 && h->dwSize >= sizeof(DIPROPRANGE)) {
        ((DIPROPRANGE *)h)->lMin = d->min;
        ((DIPROPRANGE *)h)->lMax = d->max;
        return DI_OK;
    }
    if ((uintptr_t)prop == 5 && h->dwSize >= sizeof(DIPROPDWORD)) {
        ((DIPROPDWORD *)h)->dwData = d->deadzone;
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SetProperty(pad_dev *d, REFGUID prop, const DIPROPHEADER *h)
{
    if ((uintptr_t)prop == 4 && h->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE *r = (const DIPROPRANGE *)h;
        if (r->lMin >= r->lMax) {
            return DIERR_INVALIDPARAM;
        }
        d->min = r->lMin;
        d->max = r->lMax;
    } else if ((uintptr_t)prop == 5 && h->dwSize >= sizeof(DIPROPDWORD)) {
        DWORD v = ((const DIPROPDWORD *)h)->dwData;
        d->deadzone = v > 10000 ? 10000 : v;
    }
    return DI_OK;      /* autocenter, buffer size, ...: nothing to do */
}

static HRESULT WINAPI dev_Acquire(pad_dev *d) { (void)d; return DI_OK; }
static HRESULT WINAPI dev_Unacquire(pad_dev *d) { (void)d; return DI_OK; }

static HRESULT WINAPI dev_GetDeviceState(pad_dev *d, DWORD size, void *data)
{
    if (size != sizeof(DIJOYSTATE)) {
        return DIERR_INVALIDPARAM;
    }
    pump();
    read_state(d, data);
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceData(pad_dev *d, DWORD size, void *data, DWORD *n, DWORD flags)
{
    (void)d; (void)size; (void)data; (void)flags;
    if (n) {
        *n = 0;
    }
    return DIERR_NOTBUFFERED;
}

static HRESULT WINAPI dev_SetDataFormat(pad_dev *d, const DIDATAFORMAT *f)
{
    (void)d;
    return f && f->dwDataSize == sizeof(DIJOYSTATE) ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI dev_SetEventNotification(pad_dev *d, HANDLE e) { (void)d; (void)e; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_SetCooperativeLevel(pad_dev *d, HWND w, DWORD f) { (void)d; (void)w; (void)f; return DI_OK; }

static HRESULT WINAPI dev_GetObjectInfo(pad_dev *d, void *o, DWORD obj, DWORD how)
{
    (void)d; (void)o; (void)obj; (void)how;
    return DIERR_OBJECTNOTFOUND;
}

static HRESULT WINAPI dev_GetDeviceInfo(pad_dev *d, DIDEVICEINSTANCEA *di)
{
    DWORD size = di->dwSize;
    DIDEVICEINSTANCEA full;
    if (size < 0x234 || size > sizeof full) {      /* DirectInput 3 size .. 5 size */
        return DIERR_INVALIDPARAM;
    }
    instance(d->slot, &full);
    memcpy(di, &full, size);
    di->dwSize = size;
    return DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(pad_dev *d, HWND w, DWORD f) { (void)d; (void)w; (void)f; return DI_OK; }
static HRESULT WINAPI dev_Initialize(pad_dev *d, HINSTANCE i, DWORD v, REFGUID g) { (void)d; (void)i; (void)v; (void)g; return DI_OK; }

static HRESULT WINAPI dev_CreateEffect(pad_dev *d, REFGUID g, const void *e, void **out, void *outer)
{
    (void)d; (void)g; (void)e; (void)outer;
    if (out) {
        *out = NULL;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_EnumEffects(pad_dev *d, void *cb, void *ref, DWORD type) { (void)d; (void)cb; (void)ref; (void)type; return DI_OK; }
static HRESULT WINAPI dev_GetEffectInfo(pad_dev *d, void *info, REFGUID g) { (void)d; (void)info; (void)g; return DIERR_DEVICENOTREG; }
static HRESULT WINAPI dev_GetForceFeedbackState(pad_dev *d, DWORD *s) { (void)d; (void)s; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_SendForceFeedbackCommand(pad_dev *d, DWORD c) { (void)d; (void)c; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_EnumCreatedEffectObjects(pad_dev *d, void *cb, void *ref, DWORD f) { (void)d; (void)cb; (void)ref; (void)f; return DI_OK; }
static HRESULT WINAPI dev_Escape(pad_dev *d, void *e) { (void)d; (void)e; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(pad_dev *d)
{
    (void)d;
    pump();
    return DI_OK;
}

static HRESULT WINAPI dev_SendDeviceData(pad_dev *d, DWORD size, const void *data, DWORD *n, DWORD f)
{
    (void)d; (void)size; (void)data; (void)n; (void)f;
    return DIERR_UNSUPPORTED;
}

static const void *const DEV_VTBL[] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
    dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo,
    dev_RunControlPanel, dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo,
    dev_GetForceFeedbackState, dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape,
    dev_Poll, dev_SendDeviceData,
};

/* ---- IDirectInputA wrapper ---- */

typedef struct {
    const void *const *vtbl;
    IDirectInputA *real;
} di_t;

static int pad_slot(REFGUID g)
{
    GUID base = *g;
    int slot = base.Data4[7];
    base.Data4[7] = 0;
    return IsEqualGUID(&base, &PAD_GUID) && slot < g_nslots ? slot : -1;
}

static HRESULT WINAPI di_QueryInterface(di_t *w, REFIID iid, void **out)
{
    return IDirectInput_QueryInterface(w->real, iid, out);
}

static ULONG WINAPI di_AddRef(di_t *w) { return IDirectInput_AddRef(w->real); }
static ULONG WINAPI di_Release(di_t *w) { return IDirectInput_Release(w->real); }

static HRESULT WINAPI di_CreateDevice(di_t *w, REFGUID g, IDirectInputDeviceA **out, IUnknown *outer)
{
    int slot = pad_slot(g);
    if (slot >= 0) {
        g_devs[slot].vtbl = DEV_VTBL;
        g_devs[slot].slot = slot;
        g_devs[slot].min = 0;
        g_devs[slot].max = 0xffff;
        g_devs[slot].deadzone = 0;
        *out = (IDirectInputDeviceA *)&g_devs[slot];
        return DI_OK;
    }
    return IDirectInput_CreateDevice(w->real, g, out, outer);
}

typedef struct {
    DIDEVICEINSTANCEA dev[16];
    int n;
} devlist_t;

static BOOL CALLBACK collect(LPCDIDEVICEINSTANCEA di, void *ref)
{
    devlist_t *l = ref;
    if (l->n < 16) {
        memset(&l->dev[l->n], 0, sizeof l->dev[0]);
        memcpy(&l->dev[l->n], di, di->dwSize < sizeof l->dev[0] ? di->dwSize : sizeof l->dev[0]);
        l->n++;
    }
    return DIENUM_CONTINUE;
}

/* a DirectInput device that SDL drives as one of its gamepads (same USB vendor/product) */
static int is_sdl_pad(const DIDEVICEINSTANCEA *di)
{
    int i;
    if (memcmp(di->guidProduct.Data4 + 2, "PIDVID", 6) != 0) {
        return 0;
    }
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)
            && di->guidProduct.Data1 == (DWORD)MAKELONG(SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i))) {
            return 1;
        }
    }
    return 0;
}

static HRESULT WINAPI di_EnumDevices(di_t *w, DWORD type, LPDIENUMDEVICESCALLBACKA cb, void *ref, DWORD flags)
{
    static devlist_t real;
    DIDEVICEINSTANCEA offer[MAX_JOY];
    int i, n = 0, pads = 0, nreal;
    HRESULT hr;
    if ((type & 0xff) != DIDEVTYPE_JOYSTICK) {
        return IDirectInput_EnumDevices(w->real, type, cb, ref, flags);
    }
    real.n = 0;
    hr = IDirectInput_EnumDevices(w->real, type, collect, &real, flags);
    if (FAILED(hr)) {
        return hr;
    }
    if (sdl_init()) {
        int had = g_nslots;
        g_nslots = MAX_JOY;
        for (i = 0; i < SDL_NumJoysticks(); i++) {
            open_pad(i, MAX_JOY);
        }
        for (i = 0; i < MAX_JOY; i++) {
            if (g_pads[i].gc) {
                pads = i + 1;
            }
        }
        /* at least one slot (for a gamepad plugged in later); the game enumerates again when a race
           starts, and slots never go away, so the joystick numbers the options refer to stay put */
        g_nslots = pads > had ? pads : had > 0 ? had : 1;
    }
    nreal = 0;
    for (i = 0; i < real.n; i++) {
        if (g_sdl == 1 && is_sdl_pad(&real.dev[i])) {
            dk_log("gamepad: %s (DirectInput) hidden, SDL drives it", real.dev[i].tszProductName);
            continue;
        }
        real.dev[nreal++] = real.dev[i];
    }
    if (nreal > MAX_JOY - g_nslots) {
        dk_log("gamepad: %d DirectInput joysticks, only %d offered", nreal, MAX_JOY - g_nslots);
        nreal = MAX_JOY - g_nslots;
    }
    g_nreal = nreal;
    /* the other joysticks first, as without the patch (the frontend is driven by joystick 0) */
    for (i = 0; i < nreal; i++) {
        offer[n++] = real.dev[i];
    }
    for (i = 0; i < g_nslots; i++) {
        instance(i, &offer[n++]);
    }
    for (i = 0; i < n; i++) {
        dk_log("gamepad: joystick %d = %s%s", i, offer[i].tszProductName, pad_slot(&offer[i].guidInstance) >= 0 ? " (SDL)" : "");
        if (cb(&offer[i], ref) == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI di_GetDeviceStatus(di_t *w, REFGUID g)
{
    return pad_slot(g) >= 0 ? DI_OK : IDirectInput_GetDeviceStatus(w->real, g);
}

static HRESULT WINAPI di_RunControlPanel(di_t *w, HWND hwnd, DWORD flags)
{
    return IDirectInput_RunControlPanel(w->real, hwnd, flags);
}

static HRESULT WINAPI di_Initialize(di_t *w, HINSTANCE inst, DWORD ver)
{
    return IDirectInput_Initialize(w->real, inst, ver);
}

static const void *const DI_VTBL[] = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
    di_RunControlPanel, di_Initialize,
};

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn real_DirectInputCreateA;

static HRESULT WINAPI hk_DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    HRESULT hr = real_DirectInputCreateA(inst, ver, out, outer);
    if (hr == DI_OK && out && *out && !outer) {
        di_t *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
        if (w) {
            w->vtbl = DI_VTBL;
            w->real = *out;
            *out = w;
        }
    }
    return hr;
}

/* ---- keyboard injection (Start as Escape, a button skips an FMV) ---- */

/* any face/Start/shoulder button on any pad: a gamepad "confirm" to skip the intro movie */
static int confirm_held(void)
{
    static const SDL_GameControllerButton b[] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    };
    int i, j;
    for (i = 0; i < MAX_JOY; i++) {
        for (j = 0; g_pads[i].gc && j < (int)(sizeof b / sizeof b[0]); j++) {
            if (SDL_GameControllerGetButton(g_pads[i].gc, b[j])) {
                return 1;
            }
        }
    }
    return 0;
}

/* the VK bound to CONFIRM_BITS in JOY_ReadKeyboard's active table, so the injection follows a rebind */
static int confirm_vk(void)
{
    const uint32_t *t = KBD_TABLE;
    uint32_t i, n = KBD_COUNT;
    for (i = 0; t && i < n && n <= 64; i++) {
        if (t[i * 2 + 1] == CONFIRM_BITS) {
            return (int)t[i * 2];
        }
    }
    return 'W';     /* the default table binds accelerate (= confirm) to W */
}

static SHORT (WINAPI *real_GetAsyncKeyState)(int);

static SHORT WINAPI hk_GetAsyncKeyState(int vk)
{
    SHORT r = real_GetAsyncKeyState(vk);
    pump();
    if (vk == VK_ESCAPE) {
        int i;
        for (i = 0; i < MAX_JOY; i++) {
            if (g_pads[i].start == 1) {
                r |= (SHORT)0x8000;
            }
        }
    }
    if (g_sdl == 1 && vk == confirm_vk() && FMV_PLAYING() && fmv_screen() && confirm_held()) {
        r |= (SHORT)0x8000;      /* a gamepad button skips the intro movie */
    }
    return r;
}

/* ---- joystick defaults: deploy powerup on X, rear view on Y ---- */

/* The car controls' joystick defaults (CFG_ResetControls 0x487f10, joystick half from 0x487f7f; the CFG object
   0x7045c8 keeps them at +0xfbca8 and dk.db stores them) are, in the order of Options - Control: left, right, up,
   down, then buttons 2..7 = fire gun A, deploy powerup RB, gear up LB, gear down B, change view Back, rear view
   Start (button n = bit 0x10 << n). Deploy powerup goes to button 0 = X and rear view to button 1 = Y instead
   (Start pauses). A dk.db still holding the original set (or the one with only the powerup moved, from earlier
   versions of the patch) is updated after CFG_Load (0x488760) and saved (CFG_Save 0x4887a0: the License Screen's
   constructor 0x46e770 loads dk.db again); any other set is the player's and stays. */
#define JOY_BINDS ((volatile uint32_t *)0x800270)
#define BIND_POWERUP 5
#define BIND_REAR_VIEW 9

static void (*real_CFG_Load)(void) = (void (*)(void))0x488760;

static int binds_are(const uint32_t *set)
{
    int i;
    for (i = 0; i < 10 && JOY_BINDS[i] == set[i]; i++) {
    }
    return i == 10;
}

static void hk_CFG_Load(void)
{
    static const uint32_t orig[10] = { 4, 8, 1, 2, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800 };
    static const uint32_t old_x[10] = { 4, 8, 1, 2, 0x40, 0x10, 0x100, 0x200, 0x400, 0x800 };
    real_CFG_Load();
    if (binds_are(orig) || binds_are(old_x)) {
        JOY_BINDS[BIND_POWERUP] = 0x10;
        JOY_BINDS[BIND_REAR_VIEW] = 0x20;
        ((void (*)(void))0x4887a0)();
        dk_log("gamepad: joystick defaults updated (deploy powerup on X, rear view on Y)");
    }
}

static void joy_defaults_install(void)
{
    static const uint8_t pow_old[4] = { 0x80, 0, 0, 0 }, pow_new[4] = { 0x10, 0, 0, 0 };
    static const uint8_t rear_old[4] = { 0, 0x08, 0, 0 }, rear_new[4] = { 0x20, 0, 0, 0 };
    static const uint8_t call1[5] = { 0xe8, 0x80, 0x63, 0x01, 0x00 }, call2[5] = { 0xe8, 0xcf, 0x9f, 0x01, 0x00 };
    if (dk_patch(0x487fb7, pow_old, pow_new, 4) && dk_patch(0x487fdf, rear_old, rear_new, 4)) {
        if (dk_expect(0x4723db, call1, 5)) {
            dk_call(0x4723db, hk_CFG_Load);      /* GAME_BootSequence */
        }
        if (dk_expect(0x46e78c, call2, 5)) {
            dk_call(0x46e78c, hk_CFG_Load);       /* the License Screen */
        }
    }
}

void dk_gamepad_install(void)
{
    if (!dk_ini_int("Gamepad", "UseSDL", 1)) {
        return;
    }
    g_deadzone = dk_ini_int("Gamepad", "Deadzone", 10);
    g_deadzone = g_deadzone < 0 ? 0 : g_deadzone > 90 ? 90 : g_deadzone;
    g_trigger_throttle = dk_ini_int("Gamepad", "TriggerThrottle", 1);
    real_DirectInputCreateA = dk_hook_import("DINPUT.dll", "DirectInputCreateA", hk_DirectInputCreateA);
    if (!real_DirectInputCreateA) {
        return;
    }
    real_GetAsyncKeyState = dk_hook_import("USER32.dll", "GetAsyncKeyState", hk_GetAsyncKeyState);
    if (!real_GetAsyncKeyState) {
        real_GetAsyncKeyState = GetAsyncKeyState;
    }
    InitializeCriticalSection(&g_cmd_lock);
    g_on = 1;
    padfe_install();
    padrumble_install();
    joy_defaults_install();
    dk_log("gamepad: SDL gamepads on (dead zone %d%%, %s)", g_deadzone,
           g_trigger_throttle ? "triggers accelerate/brake" : "left stick accelerates/brakes");
}
