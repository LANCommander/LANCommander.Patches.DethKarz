/* OpenGL renderer (docs/opengl.md): dkpatch\glide2x.dll (src/glide_gl.c) is loaded before the game's
 * Glide device DLL ("Vertigo 3Dfx (Glide 2).dll"), so that DLL's glide2x.dll import binds to it instead
 * of nGlide. The Vertigo DLL's Init only knows the eight Glide 2 resolutions (other widths become
 * 640x480); with OpenGL every width takes its GR_RESOLUTION_NONE path instead and the Glide screen is
 * whatever mode the game asked for (Init's params), so the mode list can offer the desktop resolutions.
 * widescreen.c calls dk_opengl_device_loaded from its VTX_ResolveExports hook and adds the modes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dkpatch.h"
#include "glide_gl.h"

#define GLIDE_STAMP 0x3692f835u   /* Vertigo 3Dfx (Glide 2).dll */
#define VTX_TABLE   0x87a1b0      /* device function pointers, filled by VTX_ResolveExports */
#define VTX_SLOTS   162

int dk_opengl_on;
static int g_active;   /* the device in use is the Vertigo Glide DLL on our OpenGL Glide */
static dkgl_set_virtual_size_fn g_set_size;
static dkgl_configure_fn g_configure;
static dkgl_get_config_fn g_get_config;

static int device_loaded(char *base);
static int (*g_init)(void *params);

static void gl_log(const char *line)
{
    dk_log("opengl: %s", line);
}

/* Vertigo Init(params): +4 width, +6 height (u16) */
static int init_wrap(void *params)
{
    uint16_t w = *(uint16_t *)((char *)params + 4), h = *(uint16_t *)((char *)params + 6);
    g_set_size(w, h);
    return g_init(params);
}

int dk_opengl_device_loaded(char *base)
{
    g_active = device_loaded(base);
    return g_active;
}

/* Options - Graphics (gfxmenu.c): the settings in use, and new ones (applied from the next frame) */
int dk_opengl_get(dkgl_config_t *c)
{
    if (!g_active || !g_get_config) {
        return 0;
    }
    g_get_config(c);
    return 1;
}

/* interp.c: the game's hidden frame costs no GPU time (glide_gl.c dkgl_discard; also the test harness's null Glide).
   0 if the loaded glide2x.dll has no such switch (nGlide). */
int dk_opengl_discard(int on)
{
    static void(__cdecl * discard)(int);
    static int looked;
    if (!looked) {
        HMODULE glide = GetModuleHandleA("glide2x.dll");
        looked = 1;
        discard = glide ? (void(__cdecl *)(int))(void *)GetProcAddress(glide, "dkgl_discard") : NULL;
        if (getenv("DKI_NODISCARD")) {   /* test switch: as with nGlide */
            discard = NULL;
        }
        dk_log("opengl: %s", discard ? "the game's hidden frames are not drawn (dkgl_discard)"
                                     : "this glide2x.dll draws the game's hidden frames too (no dkgl_discard)");
    }
    if (!discard) {
        return 0;
    }
    discard(on);
    return 1;
}

void dk_opengl_set(const dkgl_config_t *c)
{
    if (g_active && g_configure) {
        g_configure(c);
    }
}

static int device_loaded(char *base)
{
    /* Init (RVA 0x14e0) at 0x15d2: cmp eax, 960 starts the width -> Glide resolution chain; jump
       straight to its default case (0x1685: res = 7, 640x480), which becomes res = GR_RESOLUTION_NONE
       keeping the requested width and height, then on to 0x16db as before. */
    static const uint8_t cmp_old[] = { 0x3d, 0xc0, 0x03, 0x00, 0x00 };
    static const uint8_t def_old[] = {
        0x89, 0x3d, 0, 0, 0, 0,                   /* mov [res], edi (7; address relocated) */
        0x66, 0xc7, 0x46, 0x04, 0x80, 0x02,       /* mov word [esi+4], 640 */
        0x66, 0xc7, 0x46, 0x06, 0xe0, 0x01,       /* mov word [esi+6], 480 */
        0xeb, 0x42 };                             /* jmp 0x16db */
    uint8_t def_new[sizeof def_old], expect[sizeof def_old], jmp[5] = { 0xe9 };
    uintptr_t cmp_at = (uintptr_t)base + 0x15d2, def_at = (uintptr_t)base + 0x1685;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    void *init = (void *)GetProcAddress((HMODULE)base, "Init");
    void **table = (void **)VTX_TABLE;
    int32_t rel = (int32_t)(def_at - (cmp_at + 5));
    int i;
    if (nt->FileHeader.TimeDateStamp != GLIDE_STAMP || !GetProcAddress(GetModuleHandleA("glide2x.dll"), "dkgl_configure")) {
        return 0;
    }
    memcpy(jmp + 1, &rel, 4);
    memcpy(expect, def_old, sizeof expect);
    memcpy(expect + 2, (void *)(def_at + 2), 4);
    if (!dk_expect(cmp_at, jmp, 5)) {   /* not patched yet (the DLL is loaded several times) */
        if (!dk_expect(cmp_at, cmp_old, 5) || !dk_expect(def_at, expect, sizeof expect)) {
            dk_log("opengl: Vertigo Glide DLL differs, resolutions stay Glide's");
            return 0;
        }
        memset(def_new, 0x90, sizeof def_new);
        def_new[0] = 0xc7;                        /* mov dword [res], 0xff */
        def_new[1] = 0x05;
        memcpy(def_new + 2, expect + 2, 4);
        def_new[6] = 0xff;
        def_new[7] = def_new[8] = def_new[9] = 0;
        def_new[10] = 0xeb;                       /* jmp 0x16db */
        def_new[11] = 0x4a;
        dk_write(def_at, def_new, sizeof def_new);
        dk_write(cmp_at, jmp, 5);
    }
    for (i = 0; i < VTX_SLOTS && init; i++) {
        if (table[i] == init) {
            g_init = init;
            table[i] = (void *)init_wrap;
            break;
        }
    }
    if (!g_init) {
        dk_log("opengl: Init not found in the device table");
        return 0;
    }
    return 1;
}

/* the resolutions offered on top of the Glide DLL's 4:3 ones: common modes up to the desktop's size */
int dk_opengl_modes(int (*out)[2], int max)
{
    static const short common[][2] = {
        { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 },
        { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 },
        { 2560, 1600 }, { 3440, 1440 }, { 3840, 1600 }, { 3840, 2160 }, { 5120, 1440 }, { 5120, 2880 },
    };
    int dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN), n = 0, i;
    for (i = 0; i < (int)(sizeof common / sizeof common[0]) && n < max; i++) {
        if (common[i][0] <= dw && common[i][1] <= dh) {
            out[n][0] = common[i][0];
            out[n][1] = common[i][1];
            n++;
        }
    }
    if (n < max) {
        out[n][0] = dw;
        out[n][1] = dh;
        n++;
    }
    return n;
}

/* The renderer choice. The game keeps it as the binary value B3D_DeviceInfo (0x134 bytes: device DLL name, w/h/bpp
 * at +0x104, then 0x24 bytes GFX_Startup copies into the render setup at 0x50efd4) under HKLM\SOFTWARE\Melbourne
 * House\DethKarZ\Device, through REG_ReadBinary 0x47eb30 / REG_WriteBinary 0x47ec60, whose only callers are
 * GFX_LoadDeviceFromRegistry (0x4725a0) and GFX_SaveMode (0x406790). Those four calls go to the settings instead:
 * [Video] Device (the DLL), DeviceWidth/DeviceHeight/DeviceBpp, and DeviceData (the last 0x24 bytes as hex, empty
 * when all zero). The registry is never read or written for it.
 *
 * First start: GFX_LoadDeviceFromRegistry shows the device / mode dialog when there is no stored choice. With
 * SkipDeviceDialog the choice is stored as the dialog would (DLL name, w/h/bpp; no DirectDraw adapter) for the
 * Glide device at the desktop size with OpenGL (else dk.db's mode), the detail level picked like the dialog's OK
 * does, and dk.db saved. "-c" on the command line (0x806e1d) still opens the dialog. */
#define DEV_KEY       ((const char *)0x4fc4dc)   /* "SOFTWARE\Melbourne House\DethKarZ\Device" */
#define DEV_VALUE     ((const char *)0x4fc508)   /* "B3D_DeviceInfo" */
#define DEV_SIZE      0x134
#define DEV_TAIL      0x110
#define DEV_DIALOG    (*(volatile char *)0x806e1d)
#define CFG           ((void *)0x7045c8)
#define CFG_RES       ((int *)(0x7045c8 + 0xa4))   /* w, h, bpp */
#define GLIDE_DEVICE  "Vertigo 3Dfx (Glide 2).dll"

static char (*const GFX_LoadDeviceFromRegistry)(void) = (void *)0x4725a0;
static char (*const GFX_DetailForDevice)(const char *info) = (void *)0x454480;
static void (__attribute__((thiscall)) *const CFG_SetResolution)(void *cfg, int w, int h, int bpp) = (void *)0x489660;
static void (__attribute__((thiscall)) *const CFG_SetDetail)(void *cfg, int level) = (void *)0x4893d0;
static void (*const CFG_Save)(void) = (void *)0x4887a0;

static unsigned char g_dev[DEV_SIZE];
static int g_dev_state;   /* 0 = not loaded yet, 1 = none stored, 2 = g_dev holds it */

static void dev_load(void)
{
    char data[2 * (DEV_SIZE - DEV_TAIL) + 8];
    int v, i;
    memset(g_dev, 0, sizeof g_dev);
    g_dev_state = 1;
    if (!dk_ini_str("Video", "Device", (char *)g_dev, 0x104)) {
        return;
    }
    v = dk_ini_int("Video", "DeviceWidth", 640);
    memcpy(g_dev + 0x104, &v, 4);
    v = dk_ini_int("Video", "DeviceHeight", 480);
    memcpy(g_dev + 0x108, &v, 4);
    v = dk_ini_int("Video", "DeviceBpp", 16);
    memcpy(g_dev + 0x10c, &v, 4);
    dk_ini_str("Video", "DeviceData", data, sizeof data);
    for (i = 0; i < DEV_SIZE - DEV_TAIL && data[2 * i] && data[2 * i + 1]; i++) {
        unsigned x = 0;
        sscanf(data + 2 * i, "%2x", &x);
        g_dev[DEV_TAIL + i] = (unsigned char)x;
    }
    g_dev_state = 2;
}

/* REG_ReadBinary's arguments (cdecl): 1 = found */
static int dev_read(const char *key, const char *name, void *buf, DWORD size, int root)
{
    (void)key, (void)name, (void)root;
    if (!g_dev_state) {
        dev_load();
    }
    if (g_dev_state != 2) {
        return 0;
    }
    memcpy(buf, g_dev, size < DEV_SIZE ? size : DEV_SIZE);
    return 1;
}

/* REG_WriteBinary's: kept for the session even if the settings can't be written (the game quits on a failure) */
static int dev_write(const char *key, const char *name, const void *buf, DWORD size, int root)
{
    char data[2 * (DEV_SIZE - DEV_TAIL) + 1] = "", name_[0x104];
    int i, w, h, bpp, ok, nonzero = 0;
    (void)key, (void)name, (void)root;
    memset(g_dev, 0, sizeof g_dev);
    memcpy(g_dev, buf, size < DEV_SIZE ? size : DEV_SIZE);
    g_dev_state = 2;
    lstrcpynA(name_, (const char *)g_dev, sizeof name_);
    memcpy(&w, g_dev + 0x104, 4);
    memcpy(&h, g_dev + 0x108, 4);
    memcpy(&bpp, g_dev + 0x10c, 4);
    for (i = 0; i < DEV_SIZE - DEV_TAIL; i++) {
        nonzero |= g_dev[DEV_TAIL + i];
    }
    for (i = 0; nonzero && i < DEV_SIZE - DEV_TAIL; i++) {
        sprintf(data + 2 * i, "%02x", g_dev[DEV_TAIL + i]);
    }
    ok = dk_ini_set_str("Video", "Device", name_);
    ok &= dk_ini_set_int("Video", "DeviceWidth", w);
    ok &= dk_ini_set_int("Video", "DeviceHeight", h);
    ok &= dk_ini_set_int("Video", "DeviceBpp", bpp);
    ok &= dk_ini_set_str("Video", "DeviceData", data);
    dk_log(ok ? "device: stored %s at %dx%dx%d in the settings" : "device: cannot store %s at %dx%dx%d in the settings",
           name_, w, h, bpp);
    return 1;
}

static char load_device(void)
{
    unsigned char info[DEV_SIZE];
    if (!DEV_DIALOG && !dev_read(DEV_KEY, DEV_VALUE, info, sizeof info, 2)) {
        char path[MAX_PATH + 64], *slash;
        int w = CFG_RES[0], h = CFG_RES[1], bpp = CFG_RES[2];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        slash = strrchr(path, '\\');
        strcpy(slash ? slash + 1 : path, "Device Libraries\\" GLIDE_DEVICE);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            dk_log("device: %s missing, the game asks for a device", path);
            return GFX_LoadDeviceFromRegistry();
        }
        if (dk_opengl_on) {
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
            bpp = 16;   /* the mode list's (Glide) depth; OpenGL draws into an RGBA8 framebuffer whatever it says */
        }
        memset(info, 0, sizeof info);
        strcpy((char *)info, GLIDE_DEVICE);
        memcpy(info + 0x104, &w, 4);
        memcpy(info + 0x108, &h, 4);
        memcpy(info + 0x10c, &bpp, 4);
        if (dev_write(DEV_KEY, DEV_VALUE, info, sizeof info, 2)) {
            int detail = (unsigned char)GFX_DetailForDevice((char *)info);
            CFG_SetResolution(CFG, w, h, bpp);
            CFG_SetDetail(CFG, detail);
            CFG_Save();
            dk_log("device: first start, %s at %dx%dx%d, detail %d (no device dialog)", GLIDE_DEVICE, w, h, bpp, detail);
        }
    }
    return GFX_LoadDeviceFromRegistry();
}

static int redirect_call(uint32_t at, uint32_t old_target, const void *to)
{
    uint8_t b[5] = { 0xe8 };
    int32_t rel = (int32_t)(old_target - (at + 5));
    memcpy(b + 1, &rel, 4);
    return dk_expect(at, b, 5) && dk_call(at, to);
}

void dk_device_install(void)
{
    static const uint8_t call_load[] = { 0xe8, 0x21, 0x00, 0x00, 0x00 };   /* 0x47257a: call 0x4725a0 */
    static const struct { uint32_t at, target; } sites[] = {
        { 0x4725dc, 0x47eb30 }, { 0x472674, 0x47ec60 },   /* GFX_LoadDeviceFromRegistry */
        { 0x4067f4, 0x47eb30 }, { 0x406827, 0x47ec60 },   /* GFX_SaveMode */
    };
    size_t i;
    for (i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        if (!redirect_call(sites[i].at, sites[i].target, sites[i].target == 0x47eb30 ? (void *)dev_read : (void *)dev_write)) {
            dk_log("device: call at 0x%x differs, the renderer choice stays in the registry", sites[i].at);
        }
    }
    if (!dk_ini_int("Video", "SkipDeviceDialog", 1)) {
        return;
    }
    if (!dk_expect(0x47257a, call_load, sizeof call_load)) {
        dk_log("device: start-up code differs, the device dialog stays");
        return;
    }
    dk_call(0x47257a, load_device);
}

void dk_opengl_install(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 32], *slash;
    HMODULE gl;
    dkgl_configure_fn configure;
    dkgl_config_t c;
    BOOL(WINAPI * set_dpi_ctx)(HANDLE);
    if (!dk_ini_int("OpenGL", "UseOpenGL", 1)) {
        return;
    }
    if (GetModuleHandleA("glide2x.dll")) {
        dk_log("opengl: a glide2x.dll is already loaded (headless test unit?), OpenGL not used");
        return;
    }
    GetModuleFileNameA(NULL, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash) {
        slash[1] = 0;
    }
    snprintf(path, sizeof path, "%sdkpatch\\glide2x.dll", dir);
    gl = LoadLibraryA(path);
    configure = gl ? (dkgl_configure_fn)(void *)GetProcAddress(gl, "dkgl_configure") : NULL;
    g_get_config = gl ? (dkgl_get_config_fn)(void *)GetProcAddress(gl, "dkgl_get_config") : NULL;
    g_set_size = gl ? (dkgl_set_virtual_size_fn)(void *)GetProcAddress(gl, "dkgl_set_virtual_size") : NULL;
    if (!configure || !g_set_size) {
        dk_log("opengl: cannot load %s (%lu), the game's own Glide (nGlide) is used", path, GetLastError());
        return;
    }
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.fullscreen = dk_ini_int("OpenGL", "Fullscreen", 1);
    c.vsync = -1;   /* VSync is the game's "video sync" option (Customise Performance), see glide_gl.c grBufferSwap */
    c.msaa = dk_ini_int("OpenGL", "MSAA", 4);
    c.supersample = dk_ini_int("OpenGL", "Supersample", 1);
    c.filter = dk_ini_int("OpenGL", "SmoothTextures", 1);
    c.anisotropy = dk_ini_int("OpenGL", "Anisotropy", 16);
    c.log = gl_log;
    if (getenv("DK_WINDOWED")) {   /* tools/mp.py --show units */
        c.fullscreen = 0;
    }
    configure(&c);
    g_configure = configure;
    /* real pixels: without this Windows scales a DPI-unaware game's window up (blurry) */
    set_dpi_ctx = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
    if (!set_dpi_ctx || !set_dpi_ctx((HANDLE)-4)) {
        SetProcessDPIAware();
    }
    dk_opengl_on = 1;
    dk_log("opengl: %s loaded (Fullscreen=%d MSAA=%d Supersample=%d SmoothTextures=%d Anisotropy=%d)", path,
           c.fullscreen, c.msaa, c.supersample, c.filter, c.anisotropy);
}
