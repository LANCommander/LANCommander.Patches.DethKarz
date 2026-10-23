/* Options - Graphics: the patch's own video settings as extra rows of the game's menu (docs/gfxmenu.md).
 *
 * The screen (descriptor 0x4fee70: create 0x45da20, update 0x45dea0, after-update 0x45df20) builds its four
 * option rows with FE_CreateCycler (0x4525c0), 25 units apart from y = 380 (640x480 space, y up). Right after
 * the fourth (0x45dcc1, before OK / CUSTOMISE PERFORMANCE / CANCEL, so they stay in the widget order) more
 * rows are added below: the OpenGL ones (only when the game renders through the OpenGL Glide). They show the settings
 * in use; OK applies them at once (OpenGL from the next frame) and stores them in dkpatch.ini / Config.yml, like the
 * game stores its own rows in dk.db. CANCEL drops them.
 *
 * The game's third row, the frame rate (15/30/60fps = the tick rate, which tickrate.c fixes at 60 Hz), becomes
 * "max framerate" ([Graphics] MaxFps, interp.c: frames drawn between the ticks): its creation (0x45dc61), load
 * (0x45e05b) and store (0x45e15c, OK and CUSTOMISE PERFORMANCE) call ours instead of the game's.
 *
 * What the game's Customise Performance screen already has isn't repeated here: its "video sync" is the OpenGL
 * renderer's VSync (glide_gl.c grBufferSwap), its "bilinear" / "mipmapping" decide the filtering (the anisotropic
 * filtering row adds to it), and its "car LOD" row gets the vehicle draw distance (lod.c) as extra steps above
 * "high" (carlod_*). */
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"
#include "glide_gl.h"

#define THISCALL __attribute__((thiscall))

#define GFX_DESC          0x4fee70                       /* Options - Graphics Screen */
#define GFX_OK_PRESSED    (*(volatile char *)0x537e58)   /* set by OK, read by the after-update */
#define GFX_CUSTOMISE     (*(char **)0x537e14)           /* CUSTOMISE PERFORMANCE button */
#define WIDGET_ACTIVATED  0x16                           /* clicked / Enter (read and cleared by 0x44d690) */
#define CYCLER_INDEX      0x84
#define ROW_STYLE_A       ((const fe_style_t *)0x526c48)
#define ROW_STYLE_B       ((const fe_style_t *)0x526508)
#define CFG               ((void *)0x7045c8)

typedef struct { uint32_t d[10]; } fe_style_t;           /* passed by value */

/* (x, y, label, value column left/right, value names (kept, not copied), count, styles) -> the row */
static char *(*const FE_CreateCycler)(float x, float y, const char *label, float vx0, float vx1, const char *const *names,
                                      int n, fe_style_t a, fe_style_t b) = (void *)0x4525c0;
static int (THISCALL *const CYCLER_GetIndex)(char *w) = (int (THISCALL *)(char *))0x452770;
static int (THISCALL *const CFG_GetCarLod)(void *cfg) = (int (THISCALL *)(void *))0x489810;

enum { ROW_FULLSCREEN, ROW_MSAA, ROW_SUPERSAMPLE, ROW_ANISO, ROWS };

static const char *const onoff_names[] = { "off", "on" };
static const int onoff_values[] = { 0, 1 };
static const char *const msaa_names[] = { "off", "2x", "4x", "8x" };
static const int msaa_values[] = { 0, 2, 4, 8 };
static const char *const ss_names[] = { "off", "2x", "3x" };
static const int ss_values[] = { 1, 2, 3 };
/* [OpenGL] Anisotropy, on what the game draws bilinear (its "bilinear" option); 1 = off */
static const char *const aniso_names[] = { "off", "2x", "4x", "8x", "16x" };
static const int aniso_values[] = { 1, 2, 4, 8, 16 };

static struct {
    const char *label;
    const char *const *names;
    const int *values;
    int n;
    int opengl;     /* only with the OpenGL renderer */
    char *widget;   /* the row on the open screen, NULL if not shown */
    int shown;      /* the value it was opened with */
} g_rows[ROWS] = {
    { "fullscreen:", onoff_names, onoff_values, 2, 1, NULL, 0 },
    { "antialiasing:", msaa_names, msaa_values, 4, 1, NULL, 0 },
    { "supersampling:", ss_names, ss_values, 3, 1, NULL, 0 },
    { "anisotropic filtering:", aniso_names, aniso_values, 5, 1, NULL, 0 },
};

static char (*g_update)(void);
static char (*g_after_update)(void);

static int index_of(int row, int value)
{
    int i, best = 0, d, best_d = 0x7fffffff;
    for (i = 0; i < g_rows[row].n; i++) {
        d = g_rows[row].values[i] - value;
        d = d < 0 ? -d : d;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static int row_value(int row)
{
    char *w = g_rows[row].widget;
    int i = w ? *(int *)(w + CYCLER_INDEX) : -1;
    return i >= 0 && i < g_rows[row].n ? g_rows[row].values[i] : g_rows[row].shown;
}

/* called at 0x45dcc1 by rows_thunk, after the fourth row */
void __attribute__((used)) dk_gfxmenu_create_rows(void)
{
    dkgl_config_t c;
    int gl = dk_opengl_get(&c), i;
    float y = 280;
    g_rows[ROW_FULLSCREEN].shown = c.fullscreen;
    g_rows[ROW_MSAA].shown = c.msaa;
    g_rows[ROW_SUPERSAMPLE].shown = c.supersample < 1 ? 1 : c.supersample;
    g_rows[ROW_ANISO].shown = c.anisotropy < 2 ? 1 : c.anisotropy;
    for (i = 0; i < ROWS; i++) {
        char *w;
        g_rows[i].widget = NULL;
        if (g_rows[i].opengl && !gl) {
            continue;
        }
        w = FE_CreateCycler(40, y, g_rows[i].label, 340, 620, g_rows[i].names, g_rows[i].n, *ROW_STYLE_A, *ROW_STYLE_B);
        if (w) {
            void(THISCALL * set_index)(char *, int) = (*(void ***)w)[0x3c / 4];
            set_index(w, index_of(i, g_rows[i].shown));
            g_rows[i].widget = w;
        }
        y -= 25;
    }
}

void rows_thunk(void);
__asm__(".text\n_rows_thunk:\n"
        "    mov %eax, 0x535518\n"   /* the instruction replaced: the fourth row's pointer */
        "    pushal\n"
        "    call _dk_gfxmenu_create_rows\n"
        "    popal\n"
        "    ret\n");

static void store(int row, const char *section, const char *key, int value)
{
    if (value != g_rows[row].shown && !dk_ini_set_int(section, key, value)) {
        dk_log("gfxmenu: could not store %s=%d", key, value);
    }
}

static void apply(void)
{
    dkgl_config_t c, old;
    if (!g_rows[ROW_FULLSCREEN].widget || !dk_opengl_get(&c)) {
        return;
    }
    old = c;
    c.fullscreen = row_value(ROW_FULLSCREEN);
    c.msaa = row_value(ROW_MSAA);
    c.supersample = row_value(ROW_SUPERSAMPLE);
    c.anisotropy = row_value(ROW_ANISO);
    if (!memcmp(&c, &old, sizeof c)) {
        return;
    }
    dk_opengl_set(&c);
    store(ROW_FULLSCREEN, "OpenGL", "Fullscreen", c.fullscreen);
    store(ROW_MSAA, "OpenGL", "MSAA", c.msaa);
    store(ROW_SUPERSAMPLE, "OpenGL", "Supersample", c.supersample);
    store(ROW_ANISO, "OpenGL", "Anisotropy", c.anisotropy);
    dk_log("gfxmenu: Fullscreen=%d MSAA=%d Supersample=%d Anisotropy=%d", c.fullscreen, c.msaa, c.supersample,
           c.anisotropy);
}

/* screen update: CUSTOMISE PERFORMANCE stores the game's rows before it leaves, so ours are applied too */
static char gfx_update(void)
{
    if (GFX_CUSTOMISE && GFX_CUSTOMISE[WIDGET_ACTIVATED]) {
        apply();
    }
    return g_update();
}

/* after-update: OK was pressed (the game then applies its rows and leaves) */
static char gfx_after_update(void)
{
    if (GFX_OK_PRESSED) {
        apply();
    }
    return g_after_update();
}

/* ---- the frame rate row (the game's third) as "max framerate" ----
 * Values: the desktop refresh rate (MaxFps 0), 60 (one frame per tick, as the original), every refresh rate above 60
 * the display reports, the ini's value if it is another, no limit (-1); built when the screen opens. Without the
 * interpolation hooks (ExperimentalTickRate set, or the main loop differs) one value: what the frames follow. */
#define FPS_MAX 20
static char fps_text[FPS_MAX][24];
static const char *fps_names[FPS_MAX];
static int fps_values[FPS_MAX], fps_n, fps_shown;

/* kept in order: the refresh rate (0) first, then rising, no limit (-1) last */
static void fps_add(int value, const char *fmt, int hz)
{
    int i, j;
    for (i = 0; i < fps_n; i++) {
        if (fps_values[i] == value) {
            return;
        }
    }
    if (fps_n == FPS_MAX) {
        return;
    }
    for (i = 0; i < fps_n && (fps_values[i] == 0 || (value != 0 && fps_values[i] > 0 && (value < 0 || fps_values[i] < value)));
         i++) {
    }
    for (j = fps_n; j > i; j--) {
        fps_values[j] = fps_values[j - 1];
        memcpy(fps_text[j], fps_text[j - 1], sizeof fps_text[j]);
    }
    snprintf(fps_text[i], sizeof fps_text[i], fmt, hz);
    fps_values[i] = value;
    fps_n++;
    for (j = 0; j < fps_n; j++) {
        fps_names[j] = fps_text[j];
    }
}

static void fps_build(void)
{
    int rates[FPS_MAX], n, k;
    fps_n = 0;
    if (!dk_interp_available()) {
        fps_add(60, dk_ini_int("Graphics", "ExperimentalTickRate", 0) ? "tick rate" : "60", 0);
        fps_shown = 60;
        return;
    }
    fps_shown = dk_interp_max_fps();
    if (fps_shown > 0 && fps_shown < 60) {
        fps_shown = 60;
    }
    fps_add(0, "display (%d)", dk_interp_refresh());
    fps_add(60, "60", 0);
    n = dk_tickrate_rates(rates, FPS_MAX);
    for (k = 0; k < n; k++) {
        if (rates[k] > 60) {
            fps_add(rates[k], "%d", rates[k]);
        }
    }
    if (fps_shown > 60) {
        fps_add(fps_shown, "%d", fps_shown);
    }
    fps_add(-1, "unlimited", 0);
}

/* 0x45dc61: FE_CreateCycler for the frame rate row */
static char *fps_create_row(float x, float y, const char *label, float vx0, float vx1, const char *const *names, int n,
                            fe_style_t a, fe_style_t b)
{
    (void)label, (void)names, (void)n;
    fps_build();
    return FE_CreateCycler(x, y, "max framerate:", vx0, vx1, fps_names, fps_n, a, b);
}

/* 0x45e05b: the row's index (was CFG_GetFrameRate) */
static int THISCALL fps_index(void *cfg)
{
    int i;
    (void)cfg;
    for (i = 0; i < fps_n; i++) {
        if (fps_values[i] == fps_shown) {
            return i;
        }
    }
    return 0;
}

/* 0x45e15c: OK or CUSTOMISE PERFORMANCE (was CFG_SetFrameRate: the frame rate option stays as stored, the tick 60) */
static void THISCALL fps_store(void *cfg, int index)
{
    int v;
    (void)cfg;
    if (index < 0 || index >= fps_n || !dk_interp_available() || (v = fps_values[index]) == fps_shown) {
        return;
    }
    dk_interp_set_max_fps(v);
    if (!dk_ini_set_int("Graphics", "MaxFps", v)) {
        dk_log("gfxmenu: could not store MaxFps=%d", v);
    }
    fps_shown = v;
    dk_log("gfxmenu: MaxFps=%d", v);
}

static int fps_install(void)
{
    static const uint8_t create[] = { 0xe8, 0x5a, 0x49, 0xff, 0xff };   /* 0x45dc61: call FE_CreateCycler */
    static const uint8_t names[] = { 0x68, 0x88, 0x7e, 0x53, 0x00 };    /* 0x45dc38: push 0x537e88 (15/30/60fps) */
    static const uint8_t load[] = { 0xe8, 0xe0, 0xb7, 0x02, 0x00 };     /* 0x45e05b: call CFG_GetFrameRate */
    static const uint8_t save[] = { 0xe8, 0xef, 0xb6, 0x02, 0x00 };     /* 0x45e15c: call CFG_SetFrameRate */
    if (!dk_expect(0x45dc61, create, 5) || !dk_expect(0x45dc38, names, 5) || !dk_expect(0x45e05b, load, 5)
        || !dk_expect(0x45e15c, save, 5)) {
        return 0;
    }
    dk_call(0x45dc61, (void *)fps_create_row);
    dk_call(0x45e05b, (void *)fps_index);
    dk_call(0x45e15c, (void *)fps_store);
    return 1;
}

/* ---- Customise Performance (FUN_0045e3a0 creates it): the "car LOD" row with the vehicle draw distance ----
 * The game's row (0x537e48, names 0x537ed0 = high / medium / low, index = CFG car LOD 0..2, CFG_GetCarLod 0x489810,
 * setter 0x489830) decides which detail models are loaded; [Graphics] VehicleDrawDistance (lod.c) how far away they
 * stay. One row: maximum (always the most detailed), ultra (1000 %), very high (400 %), high (the original), medium,
 * low (car LOD 1 / 2, original distances). The load (FUN_0045eb30, 0x45ec69) and the "changed?" check (FUN_0045ef10,
 * 0x45f052) read the row's index for the settings in use instead of the car LOD, so a distance change asks to save
 * like any other; the store (FUN_0045ed20, 0x45ee61, only when saving) gets the car LOD and applies the distance. */
static const char *const carlod_names[] = { "maximum", "ultra", "very high", "high", "medium", "low" };
static const int carlod_lod[] = { 0, 0, 0, 0, 1, 2 };
static const int carlod_pct[] = { 0, 1000, 400, 100, 100, 100 };

static int THISCALL carlod_index(void *cfg)
{
    int lod = CFG_GetCarLod(cfg), p = dk_lod_percent;
    if (lod > 0) {
        return lod + 3 > 5 ? 5 : lod + 3;
    }
    return p == 0 ? 0 : p >= 1000 ? 1 : p > 100 ? 2 : 3;
}

static int THISCALL carlod_store(char *row)
{
    int i = CYCLER_GetIndex(row);
    if (i < 0 || i > 5) {
        return i;
    }
    if (carlod_pct[i] != dk_lod_percent) {
        dk_lod_set_percent(carlod_pct[i]);
        if (!dk_ini_set_int("Graphics", "VehicleDrawDistance", carlod_pct[i])) {
            dk_log("gfxmenu: could not store VehicleDrawDistance=%d", carlod_pct[i]);
        }
        dk_log("gfxmenu: car LOD %s: VehicleDrawDistance=%d", carlod_names[i], carlod_pct[i]);
    }
    return carlod_lod[i];
}

static void carlod_install(void)
{
    static const uint8_t names[] = { 0x6a, 0x03, 0x68, 0xd0, 0x7e, 0x53, 0x00 };   /* 0x45e4a4: push 3; push 0x537ed0 */
    static const uint8_t get_lod[] = { 0xe8, 0xa2, 0xab, 0x02, 0x00 };             /* 0x45ec69: call 0x489810 */
    static const uint8_t get_lod2[] = { 0xe8, 0xb9, 0xa7, 0x02, 0x00 };            /* 0x45f052: call 0x489810 */
    static const uint8_t get_idx[] = { 0xe8, 0x0a, 0x39, 0xff, 0xff };             /* 0x45ee61: call 0x452770 */
    uint8_t push[7] = { 0x6a, 0x06, 0x68, 0, 0, 0, 0 };
    const char *const *p = carlod_names;
    if (!dk_expect(0x45e4a4, names, 7) || !dk_expect(0x45ec69, get_lod, 5) || !dk_expect(0x45f052, get_lod2, 5)
        || !dk_expect(0x45ee61, get_idx, 5)) {
        dk_log("gfxmenu: customise performance screen differs, car LOD row unchanged");
        return;
    }
    memcpy(push + 3, &p, 4);
    dk_write(0x45e4a4, push, 7);
    dk_call(0x45ec69, (void *)carlod_index);
    dk_call(0x45f052, (void *)carlod_index);
    dk_call(0x45ee61, (void *)carlod_store);
}

void dk_gfxmenu_install(void)
{
    static const uint8_t store_row4[] = { 0xa3, 0x18, 0x55, 0x53, 0x00 };   /* 0x45dcc1: mov [0x535518], eax */
    void **desc = (void **)GFX_DESC;
    if (!dk_ini_int("Graphics", "MenuOptions", 1)) {
        return;
    }
    carlod_install();
    if (!dk_expect(0x45dcc1, store_row4, sizeof store_row4) || desc[0] != (void *)0x45da20 || desc[2] != (void *)0x45dea0
        || desc[4] != (void *)0x45df20) {
        dk_log("gfxmenu: graphics options screen differs, no extra rows");
        return;
    }
    if (!fps_install()) {
        dk_log("gfxmenu: frame rate row differs, left as it is");
    }
    g_update = desc[2];
    g_after_update = desc[4];
    dk_call(0x45dcc1, rows_thunk);
    {
        void *update = (void *)gfx_update, *after = (void *)gfx_after_update;
        dk_write(GFX_DESC + 8, &update, 4);
        dk_write(GFX_DESC + 16, &after, 4);
    }
    dk_log("gfxmenu: max framerate and extra rows in Options - Graphics, car LOD with the draw distance in Customise Performance");
}
