/* Local multiplayer test harness: several game instances on one PC (tools/mp.py drives them).
 *
 * Only active when the environment variable DK_UNIT=1..16 is set:
 * - Multiple instances: the "dethkarz semaphore" single-instance check (FUN_00472220) passes.
 * - Background instances keep running: deactivation messages never reach the game's window proc,
 *   and GetActiveWindow/GetForegroundWindow report the game's own window.
 * - Input: the game reads menu keys from WM_KEYDOWN (last VK, 0x53d30c), WM_CHAR (text entry) and
 *   GetAsyncKeyState (held navigation keys); DirectInput devices become background/non-exclusive,
 *   joysticks are hidden. The real keyboard and mouse are masked and scripted keys are fed to all
 *   of these. Commands come from dkcmd_unit<N>.txt (written by tools/mp.py, deleted once read):
 *     tap <VK>[,<VK>...] [hold ms]     press (up to 4 keys together) and release; queued, 150 ms gap
 *     text <ascii>                     types it (WM_CHAR)
 *     char <hex>                       one WM_CHAR (e.g. 0d to confirm a text field)
 *     ready                            lobby: mark this player ready (the row's tick is mouse-only)
 *     drop <ms>                        network outage: for <ms>, this unit's DragNet traffic is
 *                                      discarded in both directions (DirectPlay system messages pass)
 *     slow <ms>                        sleep this long every frame (emulates a slow PC; 0 = off)
 * - No idle timeouts: no attract-mode demo from the title screen, no fall back to the title from a menu.
 * - Unhandled exceptions are logged as CRASH: lines; message boxes are logged as MSGBOX: lines instead
 *   of being shown (answered with their default button).
 * - Headless (DK_HEADLESS=1 as well, tools/mp.py's default): nothing reaches the screen, speakers or
 *   system settings. headless\glide2x.dll (a null Glide, src/nullglide.c) is loaded before the game's
 *   Glide device DLL so nGlide never runs (no fullscreen switch, splash or GPU use); the game window is
 *   never shown or activated (activation is faked), its DirectDraw is an in-memory fake (fakeddraw.c,
 *   so the display mode is never changed); DirectSound, CD audio (MCI) and the system mixer
 *   are refused. Screenshots then show nothing: leave a unit visible for those.
 * Log: dkpatch_unit<N>.log. */
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

#define THISCALL __attribute__((thiscall))
#define QUEUE 512
#define DPERR_NOMESSAGES ((HRESULT)0x887700beL)

int dk_unit;
static int g_headless;
static HWND g_hwnd;
static WNDPROC g_game_wndproc;
static char g_cmd_path[MAX_PATH + 32];

/* ---- network outage ---- */

static volatile DWORD g_drop_until, g_trace_until;
static volatile long g_dropped_send, g_dropped_recv;

static int dropping(void)
{
    return g_drop_until && (int)(g_drop_until - timeGetTime()) > 0;
}

/* replaces DN_DPSend (0x442150): IDirectPlay4A::Send(dp, local, to, flags, data, len) */
static HRESULT THISCALL hk_DN_DPSend(void *net, DWORD to, void *data, DWORD len, DWORD flags)
{
    void **dp = *(void ***)((char *)net + 0x160);
    if (!dp) {
        return E_NOINTERFACE;
    }
    if (g_trace_until && (int)(g_trace_until - timeGetTime()) > 0 && len >= 8) {
        const unsigned char *b = data;
        dk_log("trace %lu: send to %08lx type %u seq %lu id %02x len %lu%s", (unsigned long)timeGetTime(), to,
               b[0], *(const unsigned long *)(b + 1), b[5], len, dropping() ? " (dropped)" : "");
    }
    if (dropping()) {
        g_dropped_send++;
        return 0;
    }
    return ((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD, void *, DWORD))(*(void ***)dp)[0x68 / 4])(
        dp, *(DWORD *)((char *)net + 0x168), to, flags, data, len);
}

/* wraps the DN_DPReceive call in DN_ReceiveOne (0x445d59); msg+0x18 = sender DPID (0 = DirectPlay) */
static HRESULT THISCALL hk_DN_DPReceive(void *net, char *msg)
{
    HRESULT hr = ((HRESULT (THISCALL *)(void *, char *))0x442090)(net, msg);
    if (hr >= 0 && *(DWORD *)(msg + 0x18) != 0 && dropping()) {
        g_dropped_recv++;
        return DPERR_NOMESSAGES;      /* what DN_ReceiveOne returns for a filtered message */
    }
    return hr;
}

/* game-thread actions: the main loop's DN_NetPoll call (0x48f195) runs them first */
static volatile LONG g_want_ready;

static char mainloop_poll(int arg)
{
    if (InterlockedExchange(&g_want_ready, 0)) {
        int slot = *(volatile int *)0x524c34;          /* DN_g_LocalSlot */
        ((void (*)(int, int))0x4406d0)(slot, 1);        /* set ready (host: roster broadcast) */
        dk_log("test: slot %d ready", slot);
    }
    return ((char (*)(int))0x43f480)(arg);
}

/* replaces the two FMV play calls (0x479c10, cdecl) of the FMV screen: no intro movies */
static int skip_fmv(const char *path, int a, int b, int w, int h)
{
    (void)a; (void)b; (void)w; (void)h;
    dk_log("test: skipped %s", path);
    return 0;
}

/* ---- windows ---- */

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y,
                                      int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND hwnd;
    if (g_headless && !parent) {
        style &= ~WS_VISIBLE;
        ex = (ex | WS_EX_NOACTIVATE) & ~WS_EX_TOPMOST;
    }
    hwnd = CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (!g_hwnd && !parent) {
        g_hwnd = hwnd;
    }
    if (hwnd == g_hwnd) {
        char t[64];
        sprintf(t, "Dethkarz unit %d", dk_unit);
        SetWindowTextA(hwnd, t);
    }
    return hwnd;
}

static HWND WINAPI hk_GetActiveWindow(void)
{
    return g_hwnd;
}

/* headless: the game window stays hidden; the activation it would get on being shown is sent instead */
static BOOL WINAPI hk_ShowWindow(HWND hwnd, int cmd)
{
    static int activated;
    if (hwnd != g_hwnd) {
        return ShowWindow(hwnd, cmd);
    }
    if (!activated) {
        activated = 1;
        SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
        SendMessageA(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        SendMessageA(hwnd, WM_SETFOCUS, 0, 0);
    }
    return FALSE;
}

static HWND WINAPI hk_SetFocus(HWND hwnd)
{
    (void)hwnd;
    return g_hwnd;
}

static BOOL WINAPI hk_SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    if (hwnd == g_hwnd) {
        flags = (flags | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW;
    }
    return SetWindowPos(hwnd, after, x, y, w, h, flags);
}

static int WINAPI hk_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type)
{
    char line[512], *p;
    (void)hwnd;
    snprintf(line, sizeof line, "%s", text ? text : "");
    for (p = line; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    dk_log("MSGBOX: [%s] %s", caption ? caption : "", line);
    switch (type & 0xf) {
    case MB_YESNO: case MB_YESNOCANCEL: return IDYES;
    case MB_RETRYCANCEL: return IDCANCEL;
    case MB_ABORTRETRYIGNORE: return IDIGNORE;
    default: return IDOK;
    }
}

/* the game's debug output (e.g. GetDXVersion's reasons) goes to the unit's log */
static void WINAPI hk_OutputDebugStringA(LPCSTR text)
{
    char line[256];
    size_t n;
    lstrcpynA(line, text ? text : "", sizeof line);
    n = strlen(line);
    while (n && (line[n - 1] == 10 || line[n - 1] == 13)) {
        line[--n] = 0;
    }
    if (n && strncmp(line, "Polys drawn", 11) != 0) {      /* the renderer's stats, every few seconds */
        dk_log("debug: %s", line);
    }
}

static HRESULT WINAPI hk_DirectSoundCreate(const GUID *guid, void **out, void *outer)
{
    (void)guid;
    (void)outer;
    if (out) {
        *out = NULL;
    }
    return (HRESULT)0x88780078L;       /* DSERR_NODRIVER: no sound card */
}

static DWORD WINAPI hk_mciSendCommandA(UINT id, UINT msg, DWORD_PTR flags, DWORD_PTR params)
{
    (void)id; (void)msg; (void)flags; (void)params;
    return 276;                         /* MCIERR_DEVICE_NOT_READY: no CD audio */
}

static UINT WINAPI hk_mixerSetControlDetails(void *mixer, void *details, DWORD flags)
{
    (void)mixer; (void)details; (void)flags;
    return 0;                           /* leave the system volume alone */
}

/* the main window's DirectDraw (0x493c43, exclusive fullscreen + display mode) becomes an in-memory one;
   the renderer list's DirectDraw probing (0x4a1bfb) stays real, it never sets a mode */
static HRESULT (WINAPI *real_DirectDrawCreate)(GUID *, void **, void *);

static HRESULT WINAPI hk_DirectDrawCreate(GUID *guid, void **out, void *outer)
{
    if (__builtin_return_address(0) == (void *)0x493c48) {
        *out = dk_fake_ddraw();
        return 0;
    }
    return real_DirectDrawCreate(guid, out, outer);
}

/* GetDXVersion (0x4c4010) creates a real primary surface, which fails while another program holds the
   display (and then the game warns "requires DirectX 6"): headless units report DirectX 7 on NT */
static void fake_dx_version(DWORD *version, DWORD *platform)
{
    *version = 0x700;
    *platform = 2;
}

static void install_headless(const char *dir)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%sheadless\\glide2x.dll", dir);
    if (!LoadLibraryA(path)) {
        dk_log("test: headless: cannot load %s (%lu), nGlide will be used", path, GetLastError());
    }
    {
        static const uint8_t call_dxver[] = { 0xe8, 0xde, 0x1c, 0x05, 0x00 };   /* call GetDXVersion */
        if (dk_expect(0x47232d, call_dxver, 5)) {
            dk_call(0x47232d, (void *)fake_dx_version);
        }
    }
    real_DirectDrawCreate = dk_hook_import("DDRAW.dll", "DirectDrawCreate", hk_DirectDrawCreate);
    dk_hook_import("USER32.dll", "ShowWindow", hk_ShowWindow);
    dk_hook_import("USER32.dll", "SetFocus", hk_SetFocus);
    dk_hook_import("USER32.dll", "SetWindowPos", hk_SetWindowPos);
    dk_hook_import("DSOUND.dll", "DirectSoundCreate", hk_DirectSoundCreate);
    dk_hook_import("_INMM.dll", "mciSendCommandA", hk_mciSendCommandA);
    dk_hook_import("_INMM.dll", "mixerSetControlDetails", hk_mixerSetControlDetails);
    dk_log("test: headless");
}

static HANDLE WINAPI hk_OpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
    (void)access;
    (void)inherit;
    (void)name;
    return NULL;      /* "no other instance is running" */
}

static volatile unsigned char g_down[4];       /* currently held VKs, 0 = none */

static SHORT WINAPI hk_GetAsyncKeyState(int vk)
{
    int i;
    for (i = 0; i < 4; i++) {        /* the real keyboard is shared by every instance: scripted keys only */
        if (vk && g_down[i] == vk) {
            return (SHORT)0x8000;
        }
    }
    return 0;
}

static LRESULT CALLBACK test_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_hwnd && !GetParent(hwnd)) {
        g_hwnd = hwnd;      /* the game shows its window while CreateWindowExA is still running */
    }
    if (g_headless && hwnd == g_hwnd && msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *pos = (WINDOWPOS *)lp;   /* whoever shows or raises it: stays hidden and inactive */
        if (pos->flags & SWP_SHOWWINDOW) {
            static int logged;
            if (!logged++) {
                dk_log("test: headless: kept the window hidden (shown from %p)", __builtin_return_address(0));
            }
        }
        pos->flags = (pos->flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE | SWP_NOZORDER;
    }
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (!wp) {
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            return 0;
        }
        break;
    }
    return CallWindowProcA(g_game_wndproc, hwnd, msg, wp, lp);
}

static ATOM WINAPI hk_RegisterClassA(const WNDCLASSA *wc)
{
    WNDCLASSA copy = *wc;
    if (!g_game_wndproc) {
        g_game_wndproc = wc->lpfnWndProc;
        copy.lpfnWndProc = test_wndproc;
    }
    return RegisterClassA(&copy);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    dk_log("CRASH: exception %08lx at %08lx (access %s %08lx) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx "
           "esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           ep->ExceptionRecord->ExceptionCode, (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
           ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
           ep->ExceptionRecord->NumberParameters >= 2 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0,
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    {
        /* likely return addresses: stack words pointing into the exe's .text */
        const uint32_t *sp = (const uint32_t *)(uintptr_t)c->Esp;
        char line[512];
        int i, n = 0, len = 0;
        line[0] = '\0';
        for (i = 0; i < 512 && n < 24; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
                break;
            }
            if (sp[i] >= 0x401000 && sp[i] < 0x4e9512) {
                len += sprintf(line + len, " %08lx", (unsigned long)sp[i]);
                n++;
            }
        }
        dk_log("CRASH stack:%s", line);
    }
    TerminateProcess(GetCurrentProcess(), 0xdead);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    (void)f;   /* keep crash_filter in place */
    return NULL;
}

/* ---- DirectInput ---- */

static struct { unsigned char dik[4]; int hold; } g_queue[QUEUE];
static volatile int g_q_head, g_q_tail;
static CRITICAL_SECTION g_lock;
static volatile int g_slow_ms;

typedef HRESULT (WINAPI *GetDeviceState_fn)(void *, DWORD, void *);
typedef HRESULT (WINAPI *SetCooperativeLevel_fn)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *CreateDevice_fn)(void *, const GUID *, void **, void *);
typedef BOOL (CALLBACK *EnumCb)(const void *inst, void *ref);
typedef HRESULT (WINAPI *EnumDevices_fn)(void *, DWORD, EnumCb, void *, DWORD);
static GetDeviceState_fn real_GetDeviceState;
static SetCooperativeLevel_fn real_SetCooperativeLevel;
static CreateDevice_fn real_CreateDevice;
static EnumDevices_fn real_EnumDevices;
static EnumCb g_enum_cb;

static int vk_extended(unsigned vk)
{
    return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE
        || vk == VK_RCONTROL || vk == VK_RMENU;
}

static unsigned char vk_dik(unsigned vk)
{
    return (unsigned char)(MapVirtualKeyA(vk, 0) | (vk_extended(vk) ? 0x80 : 0));
}

static void post_key(unsigned vk, int up)
{
    LPARAM lp = 1 | ((LPARAM)MapVirtualKeyA(vk, 0) << 16) | (vk_extended(vk) ? (1 << 24) : 0);
    if (up) {
        lp |= (LPARAM)3 << 30;
    }
    if (g_hwnd) {
        PostMessageA(g_hwnd, up ? WM_KEYUP : WM_KEYDOWN, vk, lp);
    }
}

static HRESULT WINAPI hk_GetDeviceState(void *dev, DWORD size, void *data)
{
    HRESULT hr = real_GetDeviceState(dev, size, data);
    if (hr != 0) {
        memset(data, 0, size);
        hr = 0;
    }
    if (size == 256) {          /* keyboard: scripted keys only */
        int i;
        if (g_slow_ms > 0) {
            Sleep(g_slow_ms);   /* the keyboard is polled once per frame */
        }
        memset(data, 0, 256);
        for (i = 0; i < 4; i++) {
            if (g_down[i]) {
                ((unsigned char *)data)[vk_dik(g_down[i])] = 0x80;
            }
        }
    } else {
        memset(data, 0, size);  /* mouse: still */
    }
    return hr;
}

static HRESULT WINAPI hk_SetCooperativeLevel(void *dev, HWND hwnd, DWORD flags)
{
    (void)flags;
    return real_SetCooperativeLevel(dev, hwnd, 0x8 | 0x2);   /* DISCL_BACKGROUND | DISCL_NONEXCLUSIVE */
}

static void patch_vtable(void **vtbl, int slot, void *fn, void **old)
{
    if (vtbl[slot] == fn) {
        return;
    }
    *old = vtbl[slot];
    dk_write((uint32_t)(uintptr_t)&vtbl[slot], &fn, 4);
}

static HRESULT WINAPI hk_CreateDevice(void *di, const GUID *guid, void **out, void *outer)
{
    HRESULT hr = real_CreateDevice(di, guid, out, outer);
    if (hr == 0 && *out) {
        void **vtbl = *(void ***)*out;
        patch_vtable(vtbl, 9, hk_GetDeviceState, (void **)&real_GetDeviceState);
        patch_vtable(vtbl, 13, hk_SetCooperativeLevel, (void **)&real_SetCooperativeLevel);
    }
    return hr;
}

static BOOL CALLBACK filter_cb(const void *inst, void *ref)
{
    DWORD type = *(const DWORD *)((const char *)inst + 4 + 16 + 16);  /* DIDEVICEINSTANCEA.dwDevType */
    if ((type & 0xff) == 4 || (type & 0xff) == 5) {                  /* DIDEVTYPE_JOYSTICK / _DEVICE */
        return TRUE;
    }
    return g_enum_cb(inst, ref);
}

static HRESULT WINAPI hk_EnumDevices(void *di, DWORD type, EnumCb cb, void *ref, DWORD flags)
{
    g_enum_cb = cb;
    return real_EnumDevices(di, type, filter_cb, ref, flags);
}

/* called by the dinput proxy with the IDirectInputA the game gets */
void dk_test_wrap_dinput(void *di)
{
    if (!dk_unit || !di) {
        return;
    }
    patch_vtable(*(void ***)di, 3, hk_CreateDevice, (void **)&real_CreateDevice);
    patch_vtable(*(void ***)di, 4, hk_EnumDevices, (void **)&real_EnumDevices);
}

static void push4(const unsigned char dik[4], int hold)
{
    EnterCriticalSection(&g_lock);
    if ((g_q_tail + 1) % QUEUE != g_q_head) {
        memcpy(g_queue[g_q_tail].dik, dik, 4);
        g_queue[g_q_tail].hold = hold;
        g_q_tail = (g_q_tail + 1) % QUEUE;
    }
    LeaveCriticalSection(&g_lock);
}

static void read_commands(void)
{
    char line[256];
    FILE *f = fopen(g_cmd_path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned k[4] = {0, 0, 0, 0};
        int hold = 120;
        if (strncmp(line, "tap ", 4) == 0) {
            unsigned char d[4] = {0, 0, 0, 0};
            char *keys = line + 4, *sp = strchr(keys, ' ');
            int n, i;
            if (sp) {
                *sp = '\0';
                hold = atoi(sp + 1);
            }
            n = sscanf(keys, "%x,%x,%x,%x", &k[0], &k[1], &k[2], &k[3]);
            for (i = 0; i < n; i++) {
                d[i] = (unsigned char)k[i];
            }
            push4(d, hold > 0 ? hold : 120);
        } else if (sscanf(line, "drop %d", &hold) == 1) {
            g_drop_until = timeGetTime() + (DWORD)hold;
            g_trace_until = g_drop_until + 20000;
            dk_log("test: network outage for %d ms", hold);
        } else if (sscanf(line, "slow %d", &hold) == 1) {
            g_slow_ms = hold;
            dk_log("test: slow %d ms per frame", hold);
        } else if (strncmp(line, "ready", 5) == 0) {
            g_want_ready = 1;
        } else if (sscanf(line, "char %x", &k[0]) == 1) {
            if (g_hwnd) {
                PostMessageA(g_hwnd, WM_CHAR, k[0], 1);
            }
        } else if (strncmp(line, "text ", 5) == 0) {
            char *c;
            for (c = line + 5; *c && *c != '\n' && *c != '\r'; c++) {
                if (g_hwnd) {
                    PostMessageA(g_hwnd, WM_CHAR, (unsigned char)*c, 1);
                    Sleep(30);
                }
            }
        }
    }
    fclose(f);
    DeleteFileA(g_cmd_path);
}

static DWORD WINAPI input_thread(LPVOID arg)
{
    int was_dropping = 0;
    (void)arg;
    for (;;) {
        int d = dropping();
        if (was_dropping && !d) {
            dk_log("test: outage over, dropped %ld sends / %ld receives", g_dropped_send, g_dropped_recv);
            g_drop_until = 0;
        }
        was_dropping = d;
        read_commands();
        if (g_q_head != g_q_tail) {
            unsigned char dik[4];
            int hold;
            EnterCriticalSection(&g_lock);
            memcpy(dik, g_queue[g_q_head].dik, 4);
            hold = g_queue[g_q_head].hold;
            g_q_head = (g_q_head + 1) % QUEUE;
            LeaveCriticalSection(&g_lock);
            {
                int i;
                memcpy((void *)g_down, dik, 4);
                for (i = 0; i < 4 && dik[i]; i++) {
                    post_key(dik[i], 0);
                }
                Sleep(hold);
                memset((void *)g_down, 0, 4);
                for (i = 0; i < 4 && dik[i]; i++) {
                    post_key(dik[i], 1);
                }
            }
            Sleep(150);
        } else {
            Sleep(50);
        }
    }
    return 0;
}

void dk_test_install(void)
{
    char dir[MAX_PATH], *slash;
    if (!dk_unit) {
        return;
    }
    dk_log("test: unit %d", dk_unit);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
    dk_hook_import("KERNEL32.dll", "SetUnhandledExceptionFilter", hk_SetUnhandledExceptionFilter);
    dk_hook_import("KERNEL32.dll", "OpenMutexA", hk_OpenMutexA);
    dk_hook_import("USER32.dll", "CreateWindowExA", hk_CreateWindowExA);
    dk_hook_import("USER32.dll", "GetActiveWindow", hk_GetActiveWindow);
    dk_hook_import("USER32.dll", "GetAsyncKeyState", hk_GetAsyncKeyState);
    dk_hook_import("USER32.dll", "RegisterClassA", hk_RegisterClassA);
    dk_hook_import("USER32.dll", "MessageBoxA", hk_MessageBoxA);
    dk_hook_import("KERNEL32.dll", "OutputDebugStringA", hk_OutputDebugStringA);

    {
        static const uint8_t send_old[] = { 0x8b, 0x81, 0x60, 0x01, 0x00, 0x00 };   /* DN_DPSend */
        static const uint8_t recv_old[] = { 0xe8, 0x32, 0xc3, 0xff, 0xff };         /* call DN_DPReceive */
        if (dk_expect(0x442150, send_old, 6) && dk_expect(0x445d59, recv_old, 5)) {
            dk_jmp(0x442150, hk_DN_DPSend);
            dk_call(0x445d59, hk_DN_DPReceive);
        } else {
            dk_log("test: DN_DPSend/DN_DPReceive differ, no outage injection");
        }
    }

    {
        static const uint8_t poll[] = { 0xe8, 0xe6, 0x02, 0xfb, 0xff };   /* call DN_NetPoll */
        if (dk_expect(0x48f195, poll, 5)) {
            dk_call(0x48f195, mainloop_poll);
        }
    }
    {
        static const uint8_t logo[] = { 0xe8, 0x0c, 0xb6, 0x00, 0x00 }, intro[] = { 0xe8, 0xd7, 0xb4, 0x00, 0x00 };
        if (dk_expect(0x46e5ff, logo, 5) && dk_expect(0x46e734, intro, 5)) {
            dk_call(0x46e5ff, skip_fmv);
            dk_call(0x46e734, skip_fmv);
        }
    }

    {
        /* no idle timeouts: the title screen would start attract-mode demo races (0x46ea09: sbb/inc ->
           xor eax,eax) and a menu would fall back to the title (0x4555b0: jb -> jmp) while units wait */
        static const uint8_t demo_old[] = { 0x1b, 0xc0, 0x40 }, demo_new[] = { 0x31, 0xc0, 0x90 };
        static const uint8_t back_old[] = { 0x72, 0x12 }, back_new[] = { 0xeb, 0x12 };
        dk_patch(0x46ea09, demo_old, demo_new, 3);
        dk_patch(0x4555b0, back_old, back_new, 2);
    }

    GetModuleFileNameA(NULL, dir, MAX_PATH);
    slash = strrchr(dir, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    {
        char v[8];
        g_headless = GetEnvironmentVariableA("DK_HEADLESS", v, sizeof v) && v[0] != '0';
        if (g_headless) {
            install_headless(dir);
        }
    }
    InitializeCriticalSection(&g_lock);
    snprintf(g_cmd_path, sizeof g_cmd_path, "%sdkcmd_unit%d.txt", dir, dk_unit);
    DeleteFileA(g_cmd_path);
    CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
}
