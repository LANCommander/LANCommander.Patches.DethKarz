/* Vehicle draw distance (docs/lod.md): the distance at which cars switch to their low-detail models.
 *
 * Only the cars (bodies and wheels) have LOD models. Each has up to 8 levels with a squared switch distance
 * per level (distance.lod of the car * CAR_g_LodDistScale, 1.0 or 0.4 in split screen). Every frame
 * ENT_CullAndSelectLod (0x407ec0) picks the first level whose threshold exceeds d^2 * tan(FOV/2)^2, d being
 * the camera distance. That is a world-space measure (the car's size relative to the screen width), not
 * pixels, so a higher resolution never brought more detail. The product is computed at 0x408000..0x408013;
 * that block is replaced by a call to lod_thunk, which scales it by (100 / VehicleDrawDistance)^2.
 *
 * Hor+ (widescreen.c) widens tan(FOV/2) on a wide screen, which made the cars switch earlier (by 4/3 at
 * 16:9). The full-screen race camera's metric is taken relative to the screen height instead, i.e. as at
 * 4:3, so 100 keeps the original distances at any aspect. */
#include <math.h>
#include <string.h>
#include "dkpatch.h"

#define INIT_W (*(volatile uint16_t *)0x50efb4)   /* GFX_g_InitParams: the mode in use */
#define INIT_H (*(volatile uint16_t *)0x50efb6)

int dk_lod_percent = 100;
static float g_scale = 1;   /* (100 / percent)^2, 0 = always the most detailed level */
static int g_horplus;

void dk_lod_set_percent(int pct)
{
    if (pct < 0) {
        pct = 100;
    } else if (pct > 0 && pct < 25) {
        pct = 25;
    } else if (pct > 1000) {
        pct = 1000;
    }
    dk_lod_percent = pct;
    g_scale = pct ? (100.0f / pct) * (100.0f / pct) : 0;
}

/* the LOD metric: tan(FOV/2)^2 * d^2 (original), scaled */
float __attribute__((used)) dk_lod_metric(float tan_half, const char *cam, float dist2)
{
    float m = tan_half * tan_half * dist2 * g_scale;
    if (g_horplus && INIT_H) {
        float screen = (float)INIT_W / INIT_H, aspect = *(const float *)(cam + 0xe0);
        if (screen > 4.0f / 3 && fabsf(aspect - screen) < 0.01f) {
            float k = 4.0f / 3 / screen;
            m *= k * k;
        }
    }
    return m;
}

/* at 0x408000 (20 bytes): eax = tan(FOV/2) (cam+0xc4, as bits), edx = camera, [esp+0x24] = d^2; the result
   goes to [esp+0x28]. ecx (the loop counter) and the flags (jle at 0x408014) must survive. */
void lod_thunk(void);
__asm__(".text\n_lod_thunk:\n"
        "    pushfl\n"
        "    push %ecx\n"
        "    push %edx\n"
        "    push 0x34(%esp)\n"   /* d^2: 0x24 + 4 (return) + 12 (saved) */
        "    push %edx\n"
        "    push %eax\n"
        "    call _dk_lod_metric\n"
        "    add $12, %esp\n"
        "    fstps 0x38(%esp)\n"  /* [esp+0x28] of the caller */
        "    pop %edx\n"
        "    pop %ecx\n"
        "    popfl\n"
        "    ret\n");

void dk_lod_install(void)
{
    static const uint8_t old[] = {
        0x89, 0x44, 0x24, 0x28,   /* mov [esp+0x28], eax */
        0xd9, 0x44, 0x24, 0x28,   /* fld dword [esp+0x28] */
        0xd8, 0x4c, 0x24, 0x28,   /* fmul dword [esp+0x28] */
        0xd8, 0x4c, 0x24, 0x24,   /* fmul dword [esp+0x24] */
        0xd9, 0x5c, 0x24, 0x28 }; /* fstp dword [esp+0x28] */
    uint8_t code[sizeof old];
    int32_t rel = (int32_t)((uintptr_t)lod_thunk - (0x408000 + 5));
    dk_lod_set_percent(dk_ini_int("Graphics", "VehicleDrawDistance", 400));
    g_horplus = dk_ini_int("Widescreen", "HorPlusFov", 1);
    memset(code, 0x90, sizeof code);
    code[0] = 0xe8;
    memcpy(code + 1, &rel, 4);
    if (!dk_patch(0x408000, old, code, sizeof code)) {
        dk_log("lod: LOD selection code differs, VehicleDrawDistance skipped");
        return;
    }
    dk_log("lod: VehicleDrawDistance=%d%s", dk_lod_percent, dk_lod_percent ? "%" : " (always full detail)");
}
