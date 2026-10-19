/* Headless Glide 2 for the test harness (build/headless/glide2x.dll).
 *
 * With DK_HEADLESS set, test.c loads this before the game loads "Vertigo 3Dfx (Glide 2).dll", so that
 * DLL's glide2x.dll import binds to it instead of nGlide: no window, no display mode change, no GPU.
 * It reports one Voodoo2 and does nothing; frames are paced at about 60 per second in grBufferSwap so
 * a dozen instances don't each spin a core. Only the 54 functions the Vertigo DLL imports exist. */
#include <windows.h>
#include <string.h>

#define API __stdcall
typedef int FxBool;

typedef struct {
    int num_sst;
    struct {
        int type;                                  /* GR_SSTTYPE_Voodoo2 = 3 */
        struct {
            int fbRam, fbiRev, nTexelfx;
            FxBool sliDetect;
            struct { int tmuRev, tmuRam; } tmu[3];
        } cfg;
    } sst[4];
} GrHwConfiguration;

static int g_width = 640, g_height = 480;
static DWORD g_last_swap;

static void hw_config(GrHwConfiguration *hw)
{
    memset(hw, 0, sizeof *hw);
    hw->num_sst = 1;
    hw->sst[0].type = 3;
    hw->sst[0].cfg.fbRam = 4;
    hw->sst[0].cfg.fbiRev = 2;
    hw->sst[0].cfg.nTexelfx = 1;
    hw->sst[0].cfg.tmu[0].tmuRev = 1;
    hw->sst[0].cfg.tmu[0].tmuRam = 4;
}

FxBool API grSstQueryBoards(GrHwConfiguration *hw) { hw->num_sst = 1; return 1; }
FxBool API grSstQueryHardware(GrHwConfiguration *hw) { hw_config(hw); return 1; }
void API grGlideInit(void) {}
void API grGlideShutdown(void) {}
void API grGlideGetVersion(char *v) { strcpy(v, "Glide Version 2.46 (dkpatch headless)"); }
void API grSstSelect(int which) { (void)which; }

FxBool API grSstWinOpen(int hwnd, int res, int refresh, int cformat, int origin, int ncolor, int naux)
{
    static const short modes[][2] = {
        { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 }, { 640, 200 }, { 640, 350 }, { 640, 400 },
        { 640, 480 }, { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 }, { 1024, 768 }, { 1280, 1024 },
        { 1600, 1200 }, { 400, 300 },
    };
    (void)hwnd; (void)refresh; (void)cformat; (void)origin; (void)ncolor; (void)naux;
    if (res >= 0 && res < (int)(sizeof modes / sizeof modes[0])) {
        g_width = modes[res][0];
        g_height = modes[res][1];
    }
    return 1;
}
void API grSstWinClose(void) {}
unsigned API grSstScreenWidth(void) { return (unsigned)g_width; }
unsigned API grSstScreenHeight(void) { return (unsigned)g_height; }
FxBool API grSstControl(unsigned code) { (void)code; return 1; }
void API grSstIdle(void) {}
void API grSstPerfStats(void *stats) { memset(stats, 0, 5 * sizeof(unsigned)); }
void API grSstResetPerfStats(void) {}

void API grBufferSwap(int interval)
{
    DWORD now = GetTickCount();
    (void)interval;
    if (now - g_last_swap < 16) {
        Sleep(16 - (now - g_last_swap));
    }
    g_last_swap = GetTickCount();
}
void API grBufferClear(unsigned color, unsigned char alpha, unsigned short depth) { (void)color; (void)alpha; (void)depth; }

FxBool API grLfbReadRegion(int buf, unsigned x, unsigned y, unsigned w, unsigned h, unsigned stride, void *dst)
{
    unsigned row;
    (void)buf; (void)x; (void)y;
    for (row = 0; row < h; row++) {
        memset((char *)dst + row * stride, 0, w * 2);
    }
    return 1;
}
FxBool API grLfbWriteRegion(int buf, unsigned x, unsigned y, int fmt, unsigned w, unsigned h, int stride, void *src)
{
    (void)buf; (void)x; (void)y; (void)fmt; (void)w; (void)h; (void)stride; (void)src;
    return 1;
}

/* texture memory: the Vertigo DLL places textures itself between these */
unsigned API grTexMinAddress(int tmu) { (void)tmu; return 0; }
unsigned API grTexMaxAddress(int tmu) { (void)tmu; return 0x400000 - 0x20000; }

/* bytes for mipmap levels largeLod..smallLod (GR_LOD_256 = 0 .. GR_LOD_1 = 8) at an aspect
   (GR_ASPECT_8x1 = 0 .. 1x1 = 3 .. 1x8 = 6); formats 8 and up are 16 bits per texel */
unsigned API grTexCalcMemRequired(int small_lod, int large_lod, int aspect, int fmt)
{
    unsigned total = 0;
    int lod, shift = aspect < 3 ? 3 - aspect : aspect - 3;
    for (lod = large_lod; lod <= small_lod && lod <= 8; lod++) {
        unsigned big = 256u >> lod, other = big >> shift;
        if (other == 0) {
            other = 1;
        }
        total += big * other * (fmt >= 8 ? 2 : 1);
    }
    return (total + 7) & ~7u;
}

void API grTexSource(int tmu, unsigned start, unsigned even_odd, void *info) { (void)tmu; (void)start; (void)even_odd; (void)info; }
void API grTexDownloadMipMap(int tmu, unsigned start, unsigned even_odd, void *info) { (void)tmu; (void)start; (void)even_odd; (void)info; }
void API grTexDownloadTable(int tmu, int type, void *data) { (void)tmu; (void)type; (void)data; }
void API grTexDownloadTablePartial(int tmu, int type, void *data, int start, int end) { (void)tmu; (void)type; (void)data; (void)start; (void)end; }
void API grTexCombine(int tmu, int a, int b, int c, int d, int e, int f) { (void)tmu; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void API grTexFilterMode(int tmu, int a, int b) { (void)tmu; (void)a; (void)b; }
void API grTexClampMode(int tmu, int a, int b) { (void)tmu; (void)a; (void)b; }
void API grTexMipMapMode(int tmu, int a, int b) { (void)tmu; (void)a; (void)b; }
void API grTexDetailControl(int tmu, int a, int b, float c) { (void)tmu; (void)a; (void)b; (void)c; }
void API grTexLodBiasValue(int tmu, float bias) { (void)tmu; (void)bias; }

void API grDrawTriangle(const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; }
void API grAADrawTriangle(const void *a, const void *b, const void *c, int ab, int bc, int ca) { (void)a; (void)b; (void)c; (void)ab; (void)bc; (void)ca; }
void API grDrawLine(const void *a, const void *b) { (void)a; (void)b; }
void API grAADrawLine(const void *a, const void *b) { (void)a; (void)b; }

void API grColorCombine(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void API grAlphaCombine(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void API grAlphaBlendFunction(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
void API grColorMask(int rgb, int alpha) { (void)rgb; (void)alpha; }
void API grConstantColorValue(unsigned v) { (void)v; }
void API grChromakeyMode(int mode) { (void)mode; }
void API grChromakeyValue(unsigned v) { (void)v; }
void API grClipWindow(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; }
void API grDepthBufferMode(int mode) { (void)mode; }
void API grDepthBufferFunction(int fn) { (void)fn; }
void API grDepthMask(int on) { (void)on; }
void API grDitherMode(int mode) { (void)mode; }
void API grCullMode(int mode) { (void)mode; }

void API grFogMode(int mode) { (void)mode; }
void API grFogColorValue(unsigned v) { (void)v; }
void API grFogTable(const unsigned char *table) { (void)table; }
void API guFogGenerateLinear(unsigned char *table, float near_z, float far_z) { (void)near_z; (void)far_z; memset(table, 0, 64); }
void API guFogGenerateExp(unsigned char *table, float density) { (void)density; memset(table, 0, 64); }
void API guFogGenerateExp2(unsigned char *table, float density) { (void)density; memset(table, 0, 64); }
