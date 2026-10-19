/* Headless test harness: an in-memory DirectDraw for the game's main window.
 *
 * Dethkarz.exe opens its window with DirectDraw in exclusive fullscreen mode and sets the display mode
 * (0x493c30: SetCooperativeLevel 0x13, SetDisplayMode, a flipping primary), whatever the renderer. With
 * DK_HEADLESS that DirectDrawCreate call gets these objects instead: surfaces are plain memory, the
 * display is never touched. Only the IDirectDraw / IDirectDrawSurface / IDirectDrawPalette (v1) methods
 * exist; the game doesn't QueryInterface them. */
#include <stdlib.h>
#include <string.h>
#include "dkpatch.h"

#define DD_OK 0
#define DDERR_NOTFOUND ((HRESULT)0x887600ffL)

typedef struct fake { void **vtbl; LONG refs; } fake_t;

typedef struct surf {
    void **vtbl;
    LONG refs;
    int w, h, bpp, pitch;
    unsigned char *bits;
    struct surf *back;      /* the primary's back buffer */
    HDC dc;
} surf_t;

typedef struct { void **vtbl; LONG refs; PALETTEENTRY entries[256]; } pal_t;

static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;

/* ---- shared stubs (stdcall: the callee pops this + n arguments) ---- */
static HRESULT WINAPI ok1(void *t) { (void)t; return DD_OK; }
static HRESULT WINAPI ok2(void *t, void *a) { (void)t; (void)a; return DD_OK; }
static HRESULT WINAPI ok3(void *t, void *a, void *b) { (void)t; (void)a; (void)b; return DD_OK; }
static HRESULT WINAPI ok4(void *t, void *a, void *b, void *c) { (void)t; (void)a; (void)b; (void)c; return DD_OK; }
static HRESULT WINAPI ok5(void *t, void *a, void *b, void *c, void *d) { (void)t; (void)a; (void)b; (void)c; (void)d; return DD_OK; }
static HRESULT WINAPI ok6(void *t, void *a, void *b, void *c, void *d, void *e) { (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; return DD_OK; }
static HRESULT WINAPI no_interface(void *t, const void *iid, void **out) { (void)t; (void)iid; *out = NULL; return E_NOINTERFACE; }
static ULONG WINAPI add_ref(fake_t *t) { return (ULONG)InterlockedIncrement(&t->refs); }
static ULONG WINAPI release_plain(fake_t *t) { return (ULONG)InterlockedDecrement(&t->refs); }  /* kept: tiny */

/* ---- surface ---- */

static void *g_surf_vtbl[36];

static surf_t *new_surface(int w, int h, int bpp)
{
    surf_t *s = calloc(1, sizeof *s);
    s->vtbl = g_surf_vtbl;
    s->refs = 1;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->pitch = (w * (bpp / 8) + 3) & ~3;
    s->bits = calloc((size_t)s->pitch, (size_t)h);
    return s;
}

static ULONG WINAPI surf_release(surf_t *s)
{
    LONG n = InterlockedDecrement(&s->refs);
    if (n == 0) {
        if (s->back) {
            ((ULONG (WINAPI *)(surf_t *))s->back->vtbl[2])(s->back);
        }
        free(s->bits);
        free(s);
    }
    return (ULONG)n;
}

static void pixel_format(const surf_t *s, unsigned char *pf)
{
    DWORD *d = (DWORD *)pf;
    memset(pf, 0, 32);
    d[0] = 32;
    d[3] = (DWORD)s->bpp;
    if (s->bpp == 8) {
        d[1] = 0x40 | 0x20;                       /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    } else {
        d[1] = 0x40;                              /* DDPF_RGB */
        if (s->bpp == 16) {
            d[4] = 0xf800; d[5] = 0x07e0; d[6] = 0x001f;
        } else {
            d[4] = 0xff0000; d[5] = 0x00ff00; d[6] = 0x0000ff;
        }
    }
}

static void fill_desc(const surf_t *s, unsigned char *desc)
{
    DWORD size = *(DWORD *)desc;
    memset(desc + 4, 0, (size > 0x6c ? 0x6c : size) - 4);
    *(DWORD *)(desc + 4) = 0x1 | 0x2 | 0x4 | 0x8 | 0x800 | 0x1000;   /* CAPS HEIGHT WIDTH PITCH LPSURFACE PIXELFORMAT */
    *(DWORD *)(desc + 0x08) = (DWORD)s->h;
    *(DWORD *)(desc + 0x0c) = (DWORD)s->w;
    *(LONG *)(desc + 0x10) = s->pitch;
    *(void **)(desc + 0x24) = s->bits;
    pixel_format(s, desc + 0x48);
}

static HRESULT WINAPI surf_get_attached(surf_t *s, void *caps, surf_t **out)
{
    (void)caps;
    if (!s->back) {
        *out = NULL;
        return DDERR_NOTFOUND;
    }
    InterlockedIncrement(&s->back->refs);
    *out = s->back;
    return DD_OK;
}

static HRESULT WINAPI surf_get_dc(surf_t *s, HDC *dc)
{
    s->dc = CreateCompatibleDC(NULL);
    *dc = s->dc;
    return DD_OK;
}

static HRESULT WINAPI surf_release_dc(surf_t *s, HDC dc)
{
    DeleteDC(dc);
    if (s->dc == dc) {
        s->dc = NULL;
    }
    return DD_OK;
}

static HRESULT WINAPI surf_get_pf(surf_t *s, void *pf) { pixel_format(s, pf); return DD_OK; }
static HRESULT WINAPI surf_get_desc(surf_t *s, void *desc) { fill_desc(s, desc); return DD_OK; }
static HRESULT WINAPI surf_lock(surf_t *s, void *rect, void *desc, DWORD flags, HANDLE ev)
{
    (void)rect; (void)flags; (void)ev;
    fill_desc(s, desc);
    return DD_OK;
}
static HRESULT WINAPI surf_get_caps(surf_t *s, DWORD *caps) { (void)s; *caps = 0x200; return DD_OK; }   /* PRIMARYSURFACE */

/* ---- palette ---- */

static void *g_pal_vtbl[7];

static HRESULT WINAPI pal_get_entries(pal_t *p, DWORD flags, DWORD base, DWORD n, PALETTEENTRY *e)
{
    (void)flags;
    if (base < 256) {
        memcpy(e, p->entries + base, (base + n > 256 ? 256 - base : n) * sizeof *e);
    }
    return DD_OK;
}

static HRESULT WINAPI pal_set_entries(pal_t *p, DWORD flags, DWORD base, DWORD n, const PALETTEENTRY *e)
{
    (void)flags;
    if (base < 256) {
        memcpy(p->entries + base, e, (base + n > 256 ? 256 - base : n) * sizeof *e);
    }
    return DD_OK;
}

/* ---- DirectDraw ---- */

static void *g_dd_vtbl[23];
static fake_t g_dd = { g_dd_vtbl, 1 };

static HRESULT WINAPI dd_create_palette(void *t, DWORD flags, const PALETTEENTRY *e, pal_t **out, void *outer)
{
    pal_t *p = calloc(1, sizeof *p);
    (void)t; (void)outer;
    p->vtbl = g_pal_vtbl;
    p->refs = 1;
    if (e) {
        memcpy(p->entries, e, ((flags & 0x4) ? 256 : 16) * sizeof *e);   /* DDPCAPS_8BIT */
    }
    *out = p;
    return DD_OK;
}

static HRESULT WINAPI dd_create_surface(void *t, const unsigned char *desc, surf_t **out, void *outer)
{
    DWORD flags = *(const DWORD *)(desc + 4), caps = *(const DWORD *)(desc + 0x68);
    int w = (flags & 0x4) ? (int)*(const DWORD *)(desc + 0x0c) : g_mode_w;
    int h = (flags & 0x2) ? (int)*(const DWORD *)(desc + 0x08) : g_mode_h;
    int bpp = (flags & 0x1000) ? (int)*(const DWORD *)(desc + 0x48 + 0x0c) : g_mode_bpp;
    surf_t *s;
    (void)t; (void)outer;
    s = new_surface(w, h, bpp ? bpp : 16);
    if ((caps & 0x200) && (flags & 0x20) && *(const DWORD *)(desc + 0x14)) {   /* primary with back buffer(s) */
        s->back = new_surface(w, h, bpp ? bpp : 16);
    }
    *out = s;
    return DD_OK;
}

static HRESULT WINAPI dd_set_mode(void *t, DWORD w, DWORD h, DWORD bpp)
{
    (void)t;
    g_mode_w = (int)w;
    g_mode_h = (int)h;
    g_mode_bpp = (int)bpp;
    dk_log("test: headless DirectDraw %lux%lux%lu (not applied)", w, h, bpp);
    return DD_OK;
}

static HRESULT WINAPI dd_get_mode(void *t, unsigned char *desc)
{
    surf_t s;
    (void)t;
    memset(&s, 0, sizeof s);
    s.w = g_mode_w;
    s.h = g_mode_h;
    s.bpp = g_mode_bpp;
    s.pitch = g_mode_w * g_mode_bpp / 8;
    fill_desc(&s, desc);
    return DD_OK;
}

static HRESULT WINAPI dd_get_caps(void *t, DWORD *drv, DWORD *hel)
{
    (void)t;
    if (drv) {
        memset(drv + 1, 0, drv[0] - 4);
    }
    if (hel) {
        memset(hel + 1, 0, hel[0] - 4);
    }
    return DD_OK;
}

void *dk_fake_ddraw(void)
{
    static int init;
    int i;
    if (!init) {
        static const struct { int slot; void *fn; } dd[] = {
            { 0, no_interface }, { 1, add_ref }, { 2, release_plain }, { 3, ok1 }, { 4, ok4 },
            { 5, dd_create_palette }, { 6, dd_create_surface }, { 7, ok3 }, { 8, ok5 }, { 9, ok5 }, { 10, ok1 },
            { 11, dd_get_caps }, { 12, dd_get_mode }, { 13, ok3 }, { 14, ok2 }, { 15, ok2 }, { 16, ok2 },
            { 17, ok2 }, { 18, ok2 }, { 19, ok1 }, { 20, ok3 }, { 21, dd_set_mode }, { 22, ok3 },
        };
        static const struct { int slot; void *fn; } su[] = {
            { 0, no_interface }, { 1, add_ref }, { 2, surf_release }, { 3, ok2 }, { 4, ok2 }, { 5, ok6 }, { 6, ok4 },
            { 7, ok6 }, { 8, ok3 }, { 9, ok3 }, { 10, ok4 }, { 11, ok3 }, { 12, surf_get_attached }, { 13, ok2 },
            { 14, surf_get_caps }, { 15, ok2 }, { 16, ok3 }, { 17, surf_get_dc }, { 18, ok2 }, { 19, ok3 },
            { 20, ok2 }, { 21, surf_get_pf }, { 22, surf_get_desc }, { 23, ok3 }, { 24, ok1 }, { 25, surf_lock },
            { 26, surf_release_dc }, { 27, ok1 }, { 28, ok2 }, { 29, ok3 }, { 30, ok3 }, { 31, ok2 }, { 32, ok2 },
            { 33, ok6 }, { 34, ok2 }, { 35, ok3 },
        };
        static const struct { int slot; void *fn; } pa[] = {
            { 0, no_interface }, { 1, add_ref }, { 2, release_plain }, { 3, ok2 }, { 4, pal_get_entries },
            { 5, ok4 }, { 6, pal_set_entries },
        };
        for (i = 0; i < (int)(sizeof dd / sizeof dd[0]); i++) {
            g_dd_vtbl[dd[i].slot] = dd[i].fn;
        }
        for (i = 0; i < (int)(sizeof su / sizeof su[0]); i++) {
            g_surf_vtbl[su[i].slot] = su[i].fn;
        }
        for (i = 0; i < (int)(sizeof pa / sizeof pa[0]); i++) {
            g_pal_vtbl[pa[i].slot] = pa[i].fn;
        }
        init = 1;
    }
    InterlockedIncrement(&g_dd.refs);
    return &g_dd;
}
