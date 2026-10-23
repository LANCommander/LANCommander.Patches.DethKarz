/* Widescreen support (docs/widescreen.md).
 *
 * 1. Modes: the Vertigo Glide DLL only knows 4:3 Glide resolutions. Its unused 960x720 case is
 *    turned into Glide's only wide mode, GR_RESOLUTION_856x480, and 856x480 is added to its mode
 *    list. Every renderer's mode list is also trimmed to what the exe's caps table holds (50).
 * 2. Hor+: the camera FOV is horizontal (tan(FOV/2) at cam+0xc4, aspect w/h at cam+0xe0), so a
 *    wider screen showed less vertically. tan(FOV/2) is widened by aspect / (4/3) wherever it is
 *    computed, so the vertical view stays the original 4:3 one.
 * 3. HUD: HUD_Render lays the HUD out in a virtual screen of max(viewport, 640x480) pixels with
 *    edge-anchored elements, so it never grew with the resolution. With ScaleHud it gets a virtual
 *    screen 480 high and as wide as the aspect, so it scales with the screen height and keeps its
 *    proportions while staying anchored to the edges.
 * 4. Menus: the frontend is laid out for 640x480 and stretched by FE_g_ScaleX/Y. With CenterMenus its
 *    viewport is made 4:3 and centred, so the scales become uniform and its 3D (the DLLs map 3D to
 *    the current viewport) lands in the middle. 2D primitives take absolute screen coordinates, so
 *    the device DLL's 2D entry points are wrapped to shift x by the viewport's offset while the
 *    frontend draws. The menu's 3D sub-viewports (car select etc.) get the same shift, and the side
 *    bars are cleared with a full-screen viewport when no race is drawn underneath. The cursor
 *    lives in 640x480 space (DirectInput deltas), so it needs nothing. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

#define THISCALL __attribute__((thiscall))

#define VTX_HMODULE   (*(HMODULE *)0x87a438)
#define VTX_QUERYCAPS (*(char (**)(void *))0x87a1f0)
#define INIT_W        (*(volatile uint16_t *)0x50efb4)   /* GFX_g_InitParams: the device DLL writes back the real mode */
#define INIT_H        (*(volatile uint16_t *)0x50efb6)
#define HUD_VP_W      (*(float *)0x514fe0)
#define HUD_VP_H      (*(float *)0x515108)
#define HUD_VIRT_W    (*(float *)0x515228)
#define HUD_VIRT_H    (*(float *)0x5150e4)
#define HUD_SCALE_X   (*(float *)0x51522c)
#define HUD_SCALE_Y   (*(float *)0x515230)
#define HUD_HALF_W    (*(float *)0x5150e8)
#define VTX_RENDERRECT   (*(void **)0x87a23c)
#define VTX_RENDERTRI2DC (*(void **)0x87a260)   /* RenderTriangleColor2D */
#define VTX_RENDERLINE   (*(void **)0x87a2b8)
#define VTX_CLEARVP      (*(char (**)(char *))0x87a2c0)
#define FE_VP         (*(char **)0x50ef6c)    /* frontend viewport (FE_g_Viewport) */
#define VP_LIST       (*(char ***)0x8d4204)   /* every viewport, deleted together by FUN_004c26f0 */
#define VP_COUNT      (*(int *)0x8d420c)

#define CAPS_MODES    0x78    /* caps: +0 mode count, +0x78 modes of {w, h, bpp} */
#define CAPS_MAX      50      /* caps blocks are 0x2d0 bytes in the exe (device list stride 0x4c4, caps at +0x1f4) */
#define GLIDE_STAMP   0x3692f835u   /* Vertigo 3Dfx (Glide 2).dll */

static char (*const VTX_ResolveExports)(void) = (void *)0x4a0480;
static char (*const VP_Create)(char **out, void *params) = (void *)0x4c2390;   /* params: +4 x, +6 y, +8 w, +10 h (u16) */
static void (*const FE_Render)(char race_drawn) = (void *)0x44ae00;

static int g_modes, g_fov, g_hud, g_menus;
static int g_fe_off;     /* x of the centred 4:3 frontend viewport, 0 = menus stretched */
static int g_in_fe;      /* inside FE_Render */
static char *g_full_vp;  /* full-screen viewport for clearing the side bars */
static char (*g_query_caps)(void *);
static int g_glide;   /* the loaded device DLL is the patched Glide one */
static int g_gl_glide;   /* ... and it runs on the OpenGL Glide (any resolution) */

/* --- 1. modes --- */

typedef struct { int w, h, bpp; } vmode_t;

static void add_mode(vmode_t *m, int *n, int max, int w, int h, int bpp)
{
    int i, j;
    for (i = 0; i < *n; i++) {
        if (m[i].w == w && m[i].h == h && m[i].bpp == bpp) {
            return;
        }
    }
    if (*n >= max) {
        return;
    }
    for (i = 0; i < *n && (m[i].w < w || (m[i].w == w && m[i].h < h)); i++) {
    }
    for (j = *n; j > i; j--) {
        m[j] = m[j - 1];
    }
    m[i].w = w;
    m[i].h = h;
    m[i].bpp = bpp;
    (*n)++;
}

/* VTX_QueryCaps wrapper: the DLL fills a big scratch block, then at most CAPS_MAX modes go back
   (16-bit ones first: the game's own default, and what Glide offers). */
static char query_caps(void *caps)
{
    static unsigned char big[CAPS_MODES + 1024 * sizeof(vmode_t)];
    vmode_t *src = (vmode_t *)(big + CAPS_MODES), *dst = (vmode_t *)((char *)caps + CAPS_MODES);
    int n, i, kept = 0, pass;
    char r;
    memset(big, 0, sizeof big);
    memcpy(big, caps, CAPS_MODES);
    r = g_query_caps(big);
    n = *(int *)big;
    if (n < 0 || n > 1024) {
        n = 0;
    }
    memcpy(caps, big, CAPS_MODES);
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            if ((src[i].bpp == 16) == (pass == 0)) {
                add_mode(dst, &kept, CAPS_MAX, src[i].w, src[i].h, src[i].bpp);
            }
        }
    }
    if (g_glide || g_gl_glide) {
        add_mode(dst, &kept, CAPS_MAX, 856, 480, 16);
    }
    if (g_gl_glide) {
        static int logged;
        int gl[32][2], m = dk_opengl_modes(gl, 32);
        for (i = 0; i < m; i++) {
            add_mode(dst, &kept, CAPS_MAX, gl[i][0], gl[i][1], 16);
        }
        if (!logged) {
            char list[1024];
            int len = 0;
            logged = 1;
            for (i = 0; i < kept && len < (int)sizeof list - 16; i++) {
                len += snprintf(list + len, sizeof list - len, " %dx%d", dst[i].w, dst[i].h);
            }
            dk_log("opengl: modes%s", list);
        }
    }
    *(int *)caps = kept;
    if (n > CAPS_MAX) {
        dk_log("widescreen: renderer reported %d modes, kept %d", n, kept);
    }
    return r;
}

/* GFX_AddModeCallback (0x45e1f0, stdcall) labels each Options - Graphics mode "%dx%d %d-bit" (entries of
   0x24 bytes at 0x535960: label[0x18], w, h, bpp). Glide modes are 16-bit, but the OpenGL Glide renders into
   an RGBA8 framebuffer, so with it the label says 32-bit (the mode itself stays 16-bit for the game). */
#define GFX_MODE_COUNT (*(int *)0x537dfc)
#define GFX_MODE_LABEL(i) ((char *)0x535960 + (i) * 0x24)
static int (__stdcall *const GFX_AddModeCallback)(int *mode) = (void *)0x45e1f0;

static int __stdcall add_mode_label(int *mode)
{
    int r = GFX_AddModeCallback(mode);
    if (g_gl_glide && GFX_MODE_COUNT > 0 && mode[2] == 16) {
        snprintf(GFX_MODE_LABEL(GFX_MODE_COUNT - 1), 0x18, "%dx%d 32-bit", mode[0], mode[1]);
    }
    return r;
}

/* Vertigo Glide Init (RVA 0x14e0) maps the width to a Glide resolution; 960 -> 9 (960x720) becomes
   856 -> 0xa (856x480). 960 then falls back to 640x480 like any unknown width; it isn't listed. */
static int patch_glide(char *base)
{
    static const uint8_t cmp_old[] = { 0x3d, 0xc0, 0x03, 0x00, 0x00 }, cmp_new[] = { 0x3d, 0x58, 0x03, 0x00, 0x00 };
    static const uint8_t set_old[] = {
        0xc7, 0x05, 0, 0, 0, 0, 0x09, 0x00, 0x00, 0x00,   /* mov [res], 9 (address relocated) */
        0x66, 0xc7, 0x46, 0x04, 0xc0, 0x03,               /* mov word [esi+4], 960 */
        0x66, 0xc7, 0x46, 0x06, 0xd0, 0x02 };             /* mov word [esi+6], 720 */
    uint8_t set[sizeof set_old], expect[sizeof set_old];
    uintptr_t cmp_at = (uintptr_t)base + 0x15d2, set_at = (uintptr_t)base + 0x1656;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GLIDE_STAMP) {
        return 0;
    }
    if (dk_expect(cmp_at, cmp_new, sizeof cmp_new)) {
        return 1;   /* already patched (the DLL is loaded several times) */
    }
    memcpy(set, (void *)set_at, sizeof set);
    memcpy(expect, set_old, sizeof expect);
    memcpy(expect + 2, set + 2, 4);
    if (!dk_expect(set_at, expect, sizeof expect) || !dk_patch(cmp_at, cmp_old, cmp_new, sizeof cmp_new)) {
        dk_log("widescreen: Vertigo Glide DLL differs, 856x480 not added");
        return 0;
    }
    set[6] = 0x0a;
    set[14] = 0x58;   /* 856 */
    set[15] = 0x03;
    set[20] = 0xe0;   /* 480 */
    set[21] = 0x01;
    dk_write(set_at, set, sizeof set);
    return 1;
}

/* --- 4. 2D wrappers (frontend x offset) --- */

typedef int (*rect_fn)(float, float, float, float, float, float, float, float);
typedef int (*tri_fn)(float, float, float, float, float, float, void *, void *, void *, int, int, int);
typedef int (*line_fn)(float, float, float, float);
static rect_fn g_render_rect;
static tri_fn g_render_tri;
static line_fn g_render_line;

static int render_rect(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2)
{
    float o = g_in_fe ? g_fe_off : 0;
    return g_render_rect(x1 + o, y1, x2 + o, y2, u1, v1, u2, v2);
}

static int render_tri(float x1, float y1, float x2, float y2, float x3, float y3, void *c1, void *c2, void *c3,
                      int a, int b, int c)
{
    float o = g_in_fe ? g_fe_off : 0;
    return g_render_tri(x1 + o, y1, x2 + o, y2, x3 + o, y3, c1, c2, c3, a, b, c);
}

static int render_line(float x1, float y1, float x2, float y2)
{
    float o = g_in_fe ? g_fe_off : 0;
    return g_render_line(x1 + o, y1, x2 + o, y2);
}

/* replaces the VTX_ResolveExports call in VTX_LoadDevice (0x4a0442): runs after every LoadLibrary
   of a device DLL */
static char resolve_exports(void)
{
    char r = VTX_ResolveExports();
    if (r) {
        dk_interp_device_loaded(VTX_HMODULE);
    }
    if (r && g_menus) {
        g_render_rect = VTX_RENDERRECT;
        g_render_tri = VTX_RENDERTRI2DC;
        g_render_line = VTX_RENDERLINE;
        VTX_RENDERRECT = (void *)render_rect;
        VTX_RENDERTRI2DC = (void *)render_tri;
        VTX_RENDERLINE = (void *)render_line;
    }
    if (r && dk_opengl_on) {
        g_gl_glide = dk_opengl_device_loaded((char *)VTX_HMODULE);
    }
    if (r && (g_modes || g_gl_glide)) {
        g_glide = g_gl_glide ? 0 : patch_glide((char *)VTX_HMODULE);
        g_query_caps = VTX_QUERYCAPS;
        VTX_QUERYCAPS = query_caps;
    }
    return r;
}

/* --- 2. Hor+ FOV --- */

static float screen_aspect(void)
{
    return INIT_H ? (float)INIT_W / INIT_H : 4.0f / 3;
}

/* With stretched menus every viewport is a 640x480 layout stretched to the screen, so each gets
   the screen's widening. With centred menus only the full-screen (race) one is stretched: it is
   recognised by its camera having the screen's aspect. */
static float fov_scale(const char *cam)
{
    float s = screen_aspect(), k = s * 0.75f;
    if (!g_fov || k <= 1) {
        return 1;
    }
    if (g_fe_off && fabsf(*(const float *)(cam + 0xe0) - s) > 0.01f) {
        return 1;
    }
    return k;
}

/* called by tan_thunk in place of crt_tan in CAM_CopyParams / CAM_SetFov (esi = the camera; the
   result goes to cam+0xc4) */
float __attribute__((used)) dk_tan_half_fov(double x, const char *cam)
{
    return (float)(tan(x) * fov_scale(cam));
}

void tan_thunk(void);
__asm__(".text\n_tan_thunk:\n"
        "    push %esi\n"
        "    push 12(%esp)\n"     /* the double argument, high then low dword */
        "    push 12(%esp)\n"
        "    call _dk_tan_half_fov\n"
        "    add $12, %esp\n"
        "    ret\n");

/* replaces VP_AttachCamera (0x4c2760, cdecl): the original plus the widened tan(FOV/2) */
static char attach_camera(char *vp, char *cam)
{
    *(char **)(vp + 0x20) = cam;
    *(uint32_t *)(cam + 0x10) |= 0x60000c0;   /* projection and frustum dirty */
    *(float *)(cam + 0xe0) = (float)*(uint16_t *)(vp + 8) / *(uint16_t *)(vp + 10);
    if (*(float *)(cam + 0xc0) > 0) {
        *(float *)(cam + 0xc4) = dk_tan_half_fov(*(float *)(cam + 0xc0) * 0.5 * 0.017453292519943295, cam);
    }
    return 1;
}

/* --- 3. HUD --- */

/* replaces HUD_Render's layout block 0x41faf3..0x41fb81 (VpW/VpH are already stored) */
static void hud_layout(void)
{
    if (g_hud && HUD_VP_H > 0) {
        HUD_VIRT_H = 480;
        HUD_VIRT_W = 480 * HUD_VP_W / HUD_VP_H;
    } else {
        HUD_VIRT_W = HUD_VP_W > 640 ? HUD_VP_W : 640;
        HUD_VIRT_H = HUD_VP_H > 480 ? HUD_VP_H : 480;
    }
    HUD_SCALE_X = HUD_VP_W / HUD_VIRT_W;
    HUD_SCALE_Y = HUD_VP_H / HUD_VIRT_H;
    HUD_HALF_W = HUD_VIRT_W * 0.5f;
}

/* --- 4. menus --- */

/* replaces VP_Create for the frontend viewport in FUN_00403540 (0x4035fc): full screen -> centred 4:3,
   plus a full-screen twin for clearing the bars */
static char fe_viewport_create(char **out, uint8_t *params)
{
    uint16_t *x = (uint16_t *)(params + 4), *w = (uint16_t *)(params + 8), h = *(uint16_t *)(params + 10);
    g_fe_off = 0;
    if (g_menus && *w * 3 > h * 4 + 3) {
        int nw = (h * 4 + 1) / 3;
        VP_Create(&g_full_vp, params);   /* freed with all the others by FUN_004c26f0 */
        g_fe_off = (*w - nw) / 2;
        *x = (uint16_t)g_fe_off;
        *w = (uint16_t)nw;
    }
    return VP_Create(out, params);
}

/* replaces VP_Create for the frontend's 3D sub-viewports, whose x is 640x480-space * FE_g_ScaleX */
static char fe_subviewport_create(char **out, uint8_t *params)
{
    *(uint16_t *)(params + 4) += (uint16_t)g_fe_off;
    return VP_Create(out, params);
}

static int vp_alive(const char *vp)
{
    int i;
    for (i = 0; vp && i < VP_COUNT; i++) {
        if (VP_LIST[i] == vp) {
            return 1;
        }
    }
    return 0;
}

/* replaces the FE_Render call in the render loop (0x406a87) */
static void fe_render(char race_drawn)
{
    if (g_fe_off && !race_drawn && FE_VP && vp_alive(g_full_vp)) {
        if (!*(char **)(g_full_vp + 0x20)) {
            *(char **)(g_full_vp + 0x20) = *(char **)(FE_VP + 0x20);   /* Glide's clear reads the camera's far plane */
        }
        VTX_CLEARVP(g_full_vp);
    }
    g_in_fe = 1;
    FE_Render(race_drawn);
    g_in_fe = 0;
}

static int is_call(uint32_t at, uint32_t to)
{
    uint8_t b[5] = { 0xe8 };
    int32_t rel = (int32_t)(to - (at + 5));
    memcpy(b + 1, &rel, 4);
    return dk_expect(at, b, 5);
}

/* The minimap (HUD_Minimap 0x41e710, in HUD space): the track spline as lines and a triangle per car, centred
   at (VirtW - 100, 120) on the camera and turned with it. HUD_MinimapProject (0x41e620) maps 1000 world units
   (0x4fc97c) to 60 HUD pixels (0x4fc970), whatever the track's size; the car triangles are +-3 (the player's
   red one, 0x4efab8/0x4efabc) and +-2 (the others, 0x4efac0/0x4efac4). MinimapScale (percent) scales both
   by pointing those reads at scaled copies; the constants are shared with other HUD code, so they stay. */
static float g_map_px, g_map_mark[4];

static void minimap_install(void)
{
    static const uint32_t px_sites[] = { 0x41e6c8, 0x41e6d0 };
    static const struct { uint32_t at, old; } marks[] = {
        { 0x41e9b0, 0x4efab8 }, { 0x41e9f8, 0x4efab8 }, { 0x41e9cd, 0x4efabc }, { 0x41e9e6, 0x4efabc },
        { 0x41ea52, 0x4efac0 }, { 0x41ea9a, 0x4efac0 }, { 0x41ea6f, 0x4efac4 }, { 0x41ea88, 0x4efac4 },
    };
    static const uint32_t mark_src[4] = { 0x4efab8, 0x4efabc, 0x4efac0, 0x4efac4 };
    int pct = dk_ini_int("Widescreen", "MinimapScale", 140), i, j, ok;
    float f, *p;
    if (pct == 100) {
        return;
    }
    pct = pct < 25 ? 25 : pct > 400 ? 400 : pct;
    f = pct / 100.0f;
    g_map_px = *(float *)0x4fc970 * f;
    for (i = 0; i < 4; i++) {
        g_map_mark[i] = *(float *)(uintptr_t)mark_src[i] * f;
    }
    ok = *(float *)0x4fc970 == 60.0f;
    for (i = 0; i < 2; i++) {
        static const uint8_t fmul[2] = { 0xd8, 0x0d };
        ok = ok && dk_expect(px_sites[i], fmul, 2) && *(uint32_t *)(uintptr_t)(px_sites[i] + 2) == 0x4fc970;
    }
    for (i = 0; i < 8; i++) {
        static const uint8_t fsub[2] = { 0xd8, 0x25 };
        ok = ok && dk_expect(marks[i].at, fsub, 2) && *(uint32_t *)(uintptr_t)(marks[i].at + 2) == marks[i].old;
    }
    if (!ok) {
        dk_log("widescreen: minimap code differs, MinimapScale skipped");
        return;
    }
    p = &g_map_px;
    for (i = 0; i < 2; i++) {
        dk_write(px_sites[i] + 2, &p, 4);
    }
    for (i = 0; i < 8; i++) {
        for (j = 0; mark_src[j] != marks[i].old; j++) {
        }
        p = &g_map_mark[j];
        dk_write(marks[i].at + 2, &p, 4);
    }
    dk_log("widescreen: minimap at %d%%", pct);
}

void dk_widescreen_install(void)
{
    static const uint8_t call_resolve[] = { 0xe8, 0x39, 0x00, 0x00, 0x00 };   /* call 0x4a0480 */
    static const uint8_t call_tan1[] = { 0xe8, 0xc4, 0x41, 0x03, 0x00 };      /* 0x496dbb: call crt_tan */
    static const uint8_t call_tan2[] = { 0xe8, 0x8b, 0x40, 0x03, 0x00 };      /* 0x496ef4 */
    static const uint8_t attach[] = { 0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x44, 0x24, 0x08 };
    static const uint8_t hud[] = { 0xd9, 0x05, 0xe0, 0x4f, 0x51, 0x00, 0xd8, 0x1d, 0xfc, 0xfa, 0x4e, 0x00 };
    static const uint32_t sub_vp[] = { 0x46568f, 0x4678a0, 0x46cef2, 0x46f684, 0x470b00 };
    static const uint8_t call_fe_render[] = { 0xe8, 0x74, 0x43, 0x04, 0x00 };   /* call FE_Render */
    static const uint8_t intro_prologue[] = { 0x83, 0xec, 0x34 };               /* FMV_PlayBootIntro (0x472830) */
    static const uint8_t ret_true[] = { 0xb0, 0x01, 0xc3 };                     /* mov al,1 ; ret */
    int i, ok;
    g_modes = dk_ini_int("Widescreen", "Modes", 1);
    g_menus = dk_ini_int("Widescreen", "CenterMenus", 1);
    g_fov = dk_ini_int("Widescreen", "HorPlusFov", 1);
    g_hud = dk_ini_int("Widescreen", "ScaleHud", 1);

    /* The boot intro (data\fmv\dk_intro.vbc) is the one place the game changes the display mode itself: a
       DirectDraw exclusive-fullscreen 640x480x8 switch (FMV_PlayBootIntro 0x472830 -> 0x493a80), straight to
       the GPU, not through nGlide. Modern monitors often reject that mode ("signal out of range") at startup.
       Everything else renders through the Vertigo device DLL (Glide/nGlide, which presents at the desktop
       resolution). 1 = skip that intro (its window/DirectDraw/surfaces are self-contained, used nowhere else),
       so the game goes straight to the Glide frontend. The function returns success. */
    if (dk_ini_int("Video", "SkipIntro", 1)) {
        if (dk_patch(0x472830, intro_prologue, ret_true, sizeof ret_true)) {
            dk_log("video: boot intro skipped (no 640x480x8 mode change)");
        }
    }

    if ((g_modes || g_menus || dk_opengl_on) && dk_expect(0x4a0442, call_resolve, sizeof call_resolve)) {
        dk_call(0x4a0442, resolve_exports);
        if (dk_opengl_on) {
            static const uint8_t push_cb[] = { 0x68, 0xf0, 0xe1, 0x45, 0x00 };   /* 0x45e1b0: push GFX_AddModeCallback */
            uint8_t push_new[5] = { 0x68 };
            void *cb = (void *)add_mode_label;
            memcpy(push_new + 1, &cb, 4);
            dk_patch(0x45e1b0, push_cb, push_new, sizeof push_new);
        }
    } else {
        g_modes = g_menus = 0;
    }
    if (g_fov) {
        if (dk_expect(0x496dbb, call_tan1, 5) && dk_expect(0x496ef4, call_tan2, 5)
            && dk_expect(0x4c2760, attach, sizeof attach)) {
            dk_call(0x496dbb, tan_thunk);
            dk_call(0x496ef4, tan_thunk);
            dk_jmp(0x4c2760, attach_camera);
        } else {
            dk_log("widescreen: camera code differs, HorPlusFov skipped");
            g_fov = 0;
        }
    }
    if (dk_expect(0x41faf3, hud, sizeof hud)) {
        uint8_t jmp[5] = { 0xe9 };
        int32_t rel = 0x41fb81 - (0x41faf8 + 5);
        dk_call(0x41faf3, hud_layout);
        memcpy(jmp + 1, &rel, 4);
        dk_write(0x41faf8, jmp, 5);
    } else {
        dk_log("widescreen: HUD code differs, ScaleHud skipped");
        g_hud = 0;
    }
    if (g_menus) {
        ok = dk_expect(0x406a87, call_fe_render, sizeof call_fe_render) && is_call(0x4035fc, 0x4c2390);
        for (i = 0; i < 5; i++) {
            ok = ok && is_call(sub_vp[i], 0x4c2390);
        }
        if (ok) {
            dk_call(0x406a87, fe_render);
            dk_call(0x4035fc, fe_viewport_create);
            for (i = 0; i < 5; i++) {
                dk_call(sub_vp[i], fe_subviewport_create);
            }
        } else {
            dk_log("widescreen: frontend code differs, CenterMenus skipped");
            g_menus = 0;
        }
    }
    minimap_install();
    dk_log("widescreen: Modes=%d HorPlusFov=%d ScaleHud=%d CenterMenus=%d", g_modes, g_fov, g_hud, g_menus);
}
