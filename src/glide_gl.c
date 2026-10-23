/* Glide 2 on OpenGL 3.3 (build/dkgl/glide2x.dll, docs/opengl.md).
 *
 * "Vertigo 3Dfx (Glide 2).dll" is the game's only hardware renderer that works today; it rasterises
 * pre-transformed vertices through 54 Glide 2 functions. This DLL implements exactly those on OpenGL,
 * so the game renders natively: the Glide screen is a virtual coordinate space (the game's mode,
 * e.g. 856x480 or the desktop size), drawn into an offscreen framebuffer at the window's real
 * resolution (optionally supersampled / multisampled) and presented letterboxed in the game window,
 * which is made borderless fullscreen (Alt+Enter toggles a window).
 *
 * Emulated Voodoo state: one TMU with 4 MB of texture memory (textures are cached GL textures keyed by
 * their start address; a download over a range drops what was there), the colour/alpha/texture
 * combiners in a generic shader, fog table, chroma key, Z or W buffering (depth is written from the
 * shader as screen-linear ooz or oow, like the hardware), clip window, LFB region read/write.
 * dkpatch configures it through dkgl_configure (resolution for GR_RESOLUTION_NONE, vsync, MSAA...). */
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glide_gl.h"

#define API __stdcall
typedef int FxBool;
typedef uint32_t FxU32;
typedef uint32_t GrColor_t;

/* ------------------------------------------------------------------ GL loading */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_RGBA8 0x8058
#define GL_TRIANGLES 0x0004
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_NEVER 0x0200
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_TEXTURE0 0x84C0
#define GL_MULTISAMPLE 0x809D
#define GL_MAX_SAMPLES 0x8D57
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

/* opengl32 exports (GL 1.1) */
#define GL11(X) \
    X(void, glBindTexture, (unsigned, unsigned)) \
    X(void, glBlendFunc, (unsigned, unsigned)) \
    X(void, glClear, (unsigned)) \
    X(void, glClearColor, (float, float, float, float)) \
    X(void, glClearDepth, (double)) \
    X(void, glColorMask, (unsigned char, unsigned char, unsigned char, unsigned char)) \
    X(void, glDeleteTextures, (int, const unsigned *)) \
    X(void, glDepthFunc, (unsigned)) \
    X(void, glDepthMask, (unsigned char)) \
    X(void, glDisable, (unsigned)) \
    X(void, glDrawArrays, (unsigned, int, int)) \
    X(void, glEnable, (unsigned)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glGenTextures, (int, unsigned *)) \
    X(unsigned, glGetError, (void)) \
    X(void, glGetFloatv, (unsigned, float *)) \
    X(void, glGetIntegerv, (unsigned, int *)) \
    X(const char *, glGetString, (unsigned)) \
    X(void, glPixelStorei, (unsigned, int)) \
    X(void, glReadPixels, (int, int, int, int, unsigned, unsigned, void *)) \
    X(void, glScissor, (int, int, int, int)) \
    X(void, glTexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *)) \
    X(void, glTexParameteri, (unsigned, unsigned, int)) \
    X(void, glTexSubImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *)) \
    X(void, glViewport, (int, int, int, int))
/* GL 2.0+ entry points (wglGetProcAddress) */
#define GLEXT(X) \
    X(void, glActiveTexture, (unsigned)) \
    X(void, glAttachShader, (unsigned, unsigned)) \
    X(void, glBindBuffer, (unsigned, unsigned)) \
    X(void, glBindFramebuffer, (unsigned, unsigned)) \
    X(void, glBindRenderbuffer, (unsigned, unsigned)) \
    X(void, glBindSampler, (unsigned, unsigned)) \
    X(void, glBindVertexArray, (unsigned)) \
    X(void, glBlendFuncSeparate, (unsigned, unsigned, unsigned, unsigned)) \
    X(void, glBlitFramebuffer, (int, int, int, int, int, int, int, int, unsigned, unsigned)) \
    X(void, glBufferData, (unsigned, GLsizeiptr, const void *, unsigned)) \
    X(unsigned, glCheckFramebufferStatus, (unsigned)) \
    X(void, glCompileShader, (unsigned)) \
    X(unsigned, glCreateProgram, (void)) \
    X(unsigned, glCreateShader, (unsigned)) \
    X(void, glDeleteFramebuffers, (int, const unsigned *)) \
    X(void, glDeleteRenderbuffers, (int, const unsigned *)) \
    X(void, glDeleteSamplers, (int, const unsigned *)) \
    X(void, glEnableVertexAttribArray, (unsigned)) \
    X(void, glFramebufferRenderbuffer, (unsigned, unsigned, unsigned, unsigned)) \
    X(void, glGenBuffers, (int, unsigned *)) \
    X(void, glGenFramebuffers, (int, unsigned *)) \
    X(void, glGenRenderbuffers, (int, unsigned *)) \
    X(void, glGenSamplers, (int, unsigned *)) \
    X(void, glGenVertexArrays, (int, unsigned *)) \
    X(void, glGetProgramInfoLog, (unsigned, int, int *, GLchar *)) \
    X(void, glGetProgramiv, (unsigned, unsigned, int *)) \
    X(void, glGetShaderInfoLog, (unsigned, int, int *, GLchar *)) \
    X(void, glGetShaderiv, (unsigned, unsigned, int *)) \
    X(int, glGetUniformLocation, (unsigned, const GLchar *)) \
    X(void, glLinkProgram, (unsigned)) \
    X(void, glRenderbufferStorageMultisample, (unsigned, int, unsigned, int, int)) \
    X(void, glSamplerParameterf, (unsigned, unsigned, float)) \
    X(void, glSamplerParameteri, (unsigned, unsigned, int)) \
    X(void, glShaderSource, (unsigned, int, const GLchar *const *, const int *)) \
    X(void, glUniform1f, (int, float)) \
    X(void, glUniform1fv, (int, int, const float *)) \
    X(void, glUniform1i, (int, int)) \
    X(void, glUniform2f, (int, float, float)) \
    X(void, glUniform4f, (int, float, float, float, float)) \
    X(void, glUniform4i, (int, int, int, int, int)) \
    X(void, glUseProgram, (unsigned)) \
    X(void, glVertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void *))

#define DECL(ret, name, args) static ret(WINAPI *p_##name) args;
GL11(DECL)
GLEXT(DECL)
#undef DECL

static HGLRC(WINAPI *p_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
static BOOL(WINAPI *p_wglSwapIntervalEXT)(int);

static int gl_load(void)
{
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    int ok = 1;
#define LOAD11(ret, name, args) if (!(p_##name = (void *)GetProcAddress(gl, #name))) { dkgl_log("missing %s", #name); ok = 0; }
#define LOADX(ret, name, args) if (!(p_##name = (void *)wglGetProcAddress(#name))) { dkgl_log("missing %s", #name); ok = 0; }
    GL11(LOAD11)
    GLEXT(LOADX)
#undef LOAD11
#undef LOADX
    p_wglSwapIntervalEXT = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    return ok;
}

/* ------------------------------------------------------------------ config + log */

static dkgl_config_t g_cfg = { sizeof(dkgl_config_t), 1, 1, 0, 1, 1, 16, 0, 0, NULL };

void dkgl_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (!g_cfg.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_cfg.log(line);
}

/* settings changed while the screen is open (Options - Graphics): applied by present() on the render thread */
static dkgl_config_t g_pending;
static volatile LONG g_pending_set;
static int g_open, g_fullscreen;

__declspec(dllexport) void __cdecl dkgl_configure(const dkgl_config_t *c)
{
    if (!c || c->size != sizeof *c) {
        return;
    }
    if (g_open) {
        g_pending = *c;
        InterlockedExchange(&g_pending_set, 1);
    } else {
        g_cfg = *c;
    }
}

/* the settings in use (fullscreen as toggled by Alt+Enter) */
__declspec(dllexport) void __cdecl dkgl_get_config(dkgl_config_t *out)
{
    *out = InterlockedCompareExchange(&g_pending_set, 0, 0) ? g_pending : g_cfg;
    if (g_open && !InterlockedCompareExchange(&g_pending_set, 0, 0)) {
        out->fullscreen = g_fullscreen;
    }
}

/* the next presented frame is written to path as a 24-bit BMP at the framebuffer's resolution (any thread) */
static char g_shot_path[MAX_PATH];
static volatile LONG g_shot_req;

__declspec(dllexport) void __cdecl dkgl_request_screenshot(const char *path)
{
    if (!InterlockedCompareExchange(&g_shot_req, 0, 0)) {
        lstrcpynA(g_shot_path, path, sizeof g_shot_path);
        InterlockedExchange(&g_shot_req, 1);
    }
}

__declspec(dllexport) void __cdecl dkgl_set_virtual_size(int w, int h)
{
    g_cfg.virt_w = w;
    g_cfg.virt_h = h;
}

/* ------------------------------------------------------------------ Glide constants */

enum { GR_COLORFORMAT_ARGB, GR_COLORFORMAT_ABGR, GR_COLORFORMAT_RGBA, GR_COLORFORMAT_BGRA };
enum { GR_ORIGIN_UPPER_LEFT, GR_ORIGIN_LOWER_LEFT };
enum { GR_DEPTHBUFFER_DISABLE, GR_DEPTHBUFFER_ZBUFFER, GR_DEPTHBUFFER_WBUFFER };
enum { GR_CULL_DISABLE, GR_CULL_NEGATIVE, GR_CULL_POSITIVE };
enum { GR_FOG_DISABLE, GR_FOG_WITH_ITERATED_ALPHA, GR_FOG_WITH_TABLE };
enum { GR_TEXTABLE_NCC0, GR_TEXTABLE_NCC1, GR_TEXTABLE_PALETTE };
#define GR_TEXFMT_RGB_332 0x0
#define GR_TEXFMT_YIQ_422 0x1
#define GR_TEXFMT_ALPHA_8 0x2
#define GR_TEXFMT_INTENSITY_8 0x3
#define GR_TEXFMT_ALPHA_INTENSITY_44 0x4
#define GR_TEXFMT_P_8 0x5
#define GR_TEXFMT_ARGB_8332 0x8
#define GR_TEXFMT_AYIQ_8422 0x9
#define GR_TEXFMT_RGB_565 0xa
#define GR_TEXFMT_ARGB_1555 0xb
#define GR_TEXFMT_ARGB_4444 0xc
#define GR_TEXFMT_ALPHA_INTENSITY_88 0xd
#define GR_TEXFMT_AP_88 0xe
#define GR_RESOLUTION_NONE 0xff
#define FOG_TABLE_SIZE 64
#define TEX_MEM (4u << 20)

typedef struct { float sow, tow, oow; } GrTmuVertex;
typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex tmuvtx[3];
} GrVertex;

typedef struct { int smallLod, largeLod, aspectRatio, format; void *data; } GrTexInfo;

typedef struct {
    int num_sst;
    struct {
        int type;
        struct {
            int fbRam, fbiRev, nTexelfx;
            FxBool sliDetect;
            struct { int tmuRev, tmuRam; } tmu[3];
        } cfg;
    } sst[4];
} GrHwConfiguration;

/* ------------------------------------------------------------------ state */

/* decoded versions kept per palettised texture: the game picks a texture, then loads its palette, and some
   textures are drawn with more than one palette; each change used to decode the texture again */
#define TEX_VERSIONS 4

typedef struct {
    float x, y, ooz, oow;
    float r, g, b, a;
    float s, t;
} vtx_t;

typedef struct {
    uint32_t start, end;
    int small_lod, large_lod, aspect, format;
    int w, h, levels;
    unsigned gl;           /* the GL texture in use (palettised: the version for the palette at the last bind) */
    uint8_t *raw;          /* palettised formats keep their indices and are decoded per palette, when drawn */
    unsigned ver_gl[TEX_VERSIONS];
    uint32_t ver_hash[TEX_VERSIONS];
    int nver, next_ver;
    float smax, tmax;      /* Glide's s/t span (256 on the long side) */
} tex_t;

typedef struct {
    int cc[5], ac[5];      /* function, factor, local, other, invert */
    int tc[6];             /* rgb function, rgb factor, alpha function, alpha factor, rgb invert, alpha invert */
    uint32_t constant, chroma, fogcolor;
    int chroma_on, fog_mode;
    int blend[4];
    int depth_mode, depth_func, depth_mask, cmask_rgb, cmask_a;
    int cull;
    int clip[4];
    int clamp_s, clamp_t, minf, magf, mip_mode, lod_blend;
    float lod_bias;
    tex_t *tex;
} state_t;

static HWND g_hwnd;
static HDC g_dc;
static HGLRC g_rc;
static WNDPROC g_old_proc;
static int g_cformat, g_origin;
static int g_vw = 640, g_vh = 480;         /* Glide screen */
static int g_rw, g_rh;                     /* offscreen framebuffer */
static unsigned g_fbo, g_fbo_color, g_fbo_depth, g_resolve_fbo, g_resolve_color;
static int g_samples;
static unsigned g_prog, g_blit_prog, g_vao, g_vbo, g_white;
static unsigned g_samplers[64];
static float g_aniso_max;
static state_t S;                          /* current Glide state */
static int g_dirty = 1;
static vtx_t g_batch[4096 * 3];
static int g_nbatch;
static float g_fog[FOG_TABLE_SIZE];
static uint32_t g_palette[256], g_pal_hash;
static tex_t g_tex[2048];
static int g_ntex;
static uint32_t g_frames;
static int g_swap = -1;   /* the swap interval set, -1 = none yet (grBufferSwap) */

static struct {
    int xf, stscale, cc, ac, tc, flags, constant, chroma, fogcol, fog, fogmode, depthmode, wscale, tex;
} U;

/* ------------------------------------------------------------------ shaders */

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"     /* x, y, ooz, oow */
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_st;\n"
    "uniform vec4 u_xf;\n"
    "uniform vec2 u_stscale;\n"
    "noperspective out vec4 v_col;\n"
    "noperspective out vec2 v_depth;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    float w = 1.0 / max(a_pos.w, 1e-20);\n"
    "    vec2 ndc = vec2(a_pos.x * u_xf.x + u_xf.y, a_pos.y * u_xf.z + u_xf.w);\n"
    "    gl_Position = vec4(ndc * w, 0.0, w);\n"
    "    v_col = clamp(a_col / 255.0, 0.0, 1.0);\n"
    "    v_depth = a_pos.zw;\n"
    "    v_uv = a_st * u_stscale;\n"
    "}\n";

/* Glide's combine units: f(other, local, factor) for TMU0 (other = 0, there is no upstream TMU),
   then colour and alpha with local = iterated/constant(/depth) and other = iterated/texture/constant. */
static const char *FS =
    "#version 330 core\n"
    "noperspective in vec4 v_col;\n"
    "noperspective in vec2 v_depth;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform ivec4 u_cc, u_ac, u_tc, u_flags;\n"   /* flags: cc invert, ac invert, tc invert bits, chroma */
    "uniform vec4 u_const, u_chroma, u_fogcol;\n"
    "uniform float u_fog[64];\n"
    "uniform int u_fogmode, u_depthmode;\n"
    "uniform float u_wscale;\n"
    "out vec4 o_col;\n"
    "vec3 cfn(int f, vec3 fac, vec3 o, vec3 l, float la) {\n"
    "    if (f == 1) return l;\n"
    "    if (f == 2) return vec3(la);\n"
    "    if (f == 3) return fac * o;\n"
    "    if (f == 4) return fac * o + l;\n"
    "    if (f == 5) return fac * o + la;\n"
    "    if (f == 6) return fac * (o - l);\n"
    "    if (f == 7) return fac * (o - l) + l;\n"
    "    if (f == 8) return fac * (o - l) + la;\n"
    "    if (f == 9) return -fac * l + l;\n"
    "    if (f == 16) return -fac * l + la;\n"
    "    return vec3(0.0);\n"
    "}\n"
    "float afn(int f, float fac, float o, float l) {\n"
    "    if (f == 1 || f == 2) return l;\n"
    "    if (f == 3) return fac * o;\n"
    "    if (f == 4 || f == 5) return fac * o + l;\n"
    "    if (f == 6) return fac * (o - l);\n"
    "    if (f == 7 || f == 8) return fac * (o - l) + l;\n"
    "    if (f == 9 || f == 16) return -fac * l + l;\n"
    "    return 0.0;\n"
    "}\n"
    "float sel(int f, float l, float la, float oa, float ta) {\n"   /* factor for one channel / alpha */
    "    int b = f & 7; float v = 0.0;\n"
    "    if (b == 1) v = l; else if (b == 2) v = oa; else if (b == 3) v = la; else if (b == 4) v = ta;\n"
    "    return f >= 8 ? 1.0 - v : v;\n"
    "}\n"
    "vec3 sel3(int f, vec3 l, float la, float oa, float ta) {\n"
    "    int b = f & 7; vec3 v = vec3(0.0);\n"
    "    if (b == 1) v = l; else if (b == 2) v = vec3(oa); else if (b == 3) v = vec3(la); else if (b == 4) v = vec3(ta);\n"
    "    return f >= 8 ? 1.0 - v : v;\n"
    "}\n"
    "float fogw(float oow) {\n"
    "    float w = 1.0 / max(oow, 1e-20);\n"
    "    float fi = 0.0;\n"
    "    if (w >= 1.0) {\n"
    "        float k = min(floor(log2(w)), 15.0);\n"
    "        float r = clamp(w / exp2(k), 1.0, 2.0);\n"
    "        float lo, hi, j;\n"
    "        if (r < 8.0/7.0) { j = 0.0; lo = 1.0; hi = 8.0/7.0; }\n"
    "        else if (r < 4.0/3.0) { j = 1.0; lo = 8.0/7.0; hi = 4.0/3.0; }\n"
    "        else if (r < 1.6) { j = 2.0; lo = 4.0/3.0; hi = 1.6; }\n"
    "        else { j = 3.0; lo = 1.6; hi = 2.0; }\n"
    "        fi = clamp(4.0 * k + j + (r - lo) / (hi - lo), 0.0, 63.0);\n"
    "    }\n"
    "    int i0 = int(fi); int i1 = min(i0 + 1, 63);\n"
    "    return mix(u_fog[i0], u_fog[i1], fi - float(i0)) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    /* TMU0 */
    "    vec3 trgb = cfn(u_tc.x, sel3(u_tc.y, t.rgb, t.a, 0.0, t.a), vec3(0.0), t.rgb, t.a);\n"
    "    float ta = afn(u_tc.z, sel(u_tc.w, t.a, t.a, 0.0, t.a), 0.0, t.a);\n"
    "    if ((u_flags.z & 1) != 0) trgb = 1.0 - trgb;\n"
    "    if ((u_flags.z & 2) != 0) ta = 1.0 - ta;\n"
    "    vec4 tex = clamp(vec4(trgb, ta), 0.0, 1.0);\n"
    /* alpha combine inputs (shared with the colour unit's alpha factors) */
    "    float al = u_ac.z == 0 ? v_col.a : u_ac.z == 1 ? u_const.a : clamp(v_depth.x / 65535.0, 0.0, 1.0);\n"
    "    float ao = u_ac.w == 0 ? v_col.a : u_ac.w == 1 ? tex.a : u_const.a;\n"
    "    vec3 cl = u_cc.z == 0 ? v_col.rgb : u_const.rgb;\n"
    "    vec3 co = u_cc.w == 0 ? v_col.rgb : u_cc.w == 1 ? tex.rgb : u_const.rgb;\n"
    "    if (u_flags.w != 0 && all(lessThan(abs(co - u_chroma.rgb), vec3(6.0 / 255.0)))) discard;\n"
    "    vec3 rgb = cfn(u_cc.x, sel3(u_cc.y, cl, al, ao, tex.a), co, cl, al);\n"
    "    float a = afn(u_ac.x, sel(u_ac.y, al, al, ao, tex.a), ao, al);\n"
    "    if (u_flags.x != 0) rgb = 1.0 - rgb;\n"
    "    if (u_flags.y != 0) a = 1.0 - a;\n"
    "    rgb = clamp(rgb, 0.0, 1.0); a = clamp(a, 0.0, 1.0);\n"
    "    if (u_fogmode == 1) rgb = mix(rgb, u_fogcol.rgb, v_col.a);\n"
    "    else if (u_fogmode == 2) rgb = mix(rgb, u_fogcol.rgb, fogw(v_depth.y));\n"
    "    o_col = vec4(rgb, a);\n"
    "    if (u_depthmode == 2) gl_FragDepth = clamp(v_depth.y * u_wscale, 0.0, 1.0);\n"
    "    else gl_FragDepth = clamp(v_depth.x / 65535.0, 0.0, 1.0);\n"
    "}\n";

/* LFB writes: a textured quad, no pipeline state */
static const char *BLIT_VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=2) in vec2 a_st;\n"
    "uniform vec4 u_xf;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos.x * u_xf.x + u_xf.y, a_pos.y * u_xf.z + u_xf.w, 0.0, 1.0); v_uv = a_st; }\n";
static const char *BLIT_FS =
    "#version 330 core\n"
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 o_col;\n"
    "void main() { o_col = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

static unsigned compile(unsigned type, const char *src)
{
    unsigned s = p_glCreateShader(type);
    int ok = 0;
    char msg[1024];
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        p_glGetShaderInfoLog(s, sizeof msg, NULL, msg);
        dkgl_log("shader: %s", msg);
    }
    return s;
}

static unsigned link(const char *vs, const char *fs)
{
    unsigned p = p_glCreateProgram();
    int ok = 0;
    char msg[1024];
    p_glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    p_glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        p_glGetProgramInfoLog(p, sizeof msg, NULL, msg);
        dkgl_log("link: %s", msg);
    }
    return p;
}

/* ------------------------------------------------------------------ helpers */

static void color4(uint32_t c, float *out)
{
    int a, r, g, b;
    switch (g_cformat) {
    case GR_COLORFORMAT_ABGR: a = c >> 24; b = (c >> 16) & 255; g = (c >> 8) & 255; r = c & 255; break;
    case GR_COLORFORMAT_RGBA: r = c >> 24; g = (c >> 16) & 255; b = (c >> 8) & 255; a = c & 255; break;
    case GR_COLORFORMAT_BGRA: b = c >> 24; g = (c >> 16) & 255; r = (c >> 8) & 255; a = c & 255; break;
    default: a = c >> 24; r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

static void flush(void);
static void tex_select(tex_t *t);

#define SET(field, value) do { if ((field) != (value)) { flush(); (field) = (value); g_dirty = 1; } } while (0)

/* virtual (Glide) pixel -> framebuffer pixel */
static float sx(void) { return (float)g_rw / g_vw; }
static float sy(void) { return (float)g_rh / g_vh; }

static void scissor_rect(int *x, int *y, int *w, int *h)
{
    int x0 = S.clip[0], y0 = S.clip[1], x1 = S.clip[2], y1 = S.clip[3];
    float fx = sx(), fy = sy();
    *x = (int)floorf(x0 * fx + 0.5f);
    *w = (int)floorf(x1 * fx + 0.5f) - *x;
    if (g_origin == GR_ORIGIN_LOWER_LEFT) {
        *y = (int)floorf(y0 * fy + 0.5f);
        *h = (int)floorf(y1 * fy + 0.5f) - *y;
    } else {
        *y = (int)floorf((g_vh - y1) * fy + 0.5f);
        *h = (int)floorf((g_vh - y0) * fy + 0.5f) - *y;
    }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static void set_xf(int loc)
{
    if (g_origin == GR_ORIGIN_LOWER_LEFT) {
        p_glUniform4f(loc, 2.0f / g_vw, -1.0f, 2.0f / g_vh, -1.0f);
    } else {
        p_glUniform4f(loc, 2.0f / g_vw, -1.0f, -2.0f / g_vh, 1.0f);
    }
}

static unsigned gl_blend(int f, int src)
{
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_SRC_ALPHA;
    case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return GL_DST_ALPHA;
    case 4: return GL_ONE;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 7: return GL_ONE_MINUS_DST_ALPHA;
    case 15: return src ? GL_SRC_ALPHA_SATURATE : GL_ONE;
    default: return GL_ONE;
    }
}

/* W buffering: depth = oow * scale, nearer = larger, so the comparison is reversed */
#define W_SCALE (1.0f / 64.0f)
static unsigned gl_depth_func(int f)
{
    static const unsigned z[8] = { 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207 };
    static const unsigned w[8] = { 0x200, 0x204, 0x202, 0x206, 0x201, 0x205, 0x203, 0x207 };
    return S.depth_mode == GR_DEPTHBUFFER_WBUFFER ? w[f & 7] : z[f & 7];
}

static unsigned sampler_for(void)
{
    int mipped = S.tex && S.tex->levels > 1 && S.mip_mode != 0;
    int bil = S.minf != 0, mag = S.magf != 0;
    int key = (S.clamp_s & 1) | (S.clamp_t & 1) << 1 | bil << 2 | mag << 3 | mipped << 4 | (S.lod_blend != 0) << 5;
    unsigned *smp = &g_samplers[key];
    if (!*smp) {
        unsigned min;
        int smooth = g_cfg.filter && bil;
        p_glGenSamplers(1, smp);
        p_glSamplerParameteri(*smp, GL_TEXTURE_WRAP_S, S.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        p_glSamplerParameteri(*smp, GL_TEXTURE_WRAP_T, S.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        p_glSamplerParameteri(*smp, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
        if (!mipped) {
            min = bil ? GL_LINEAR : GL_NEAREST;
        } else if (bil) {
            min = (S.lod_blend || smooth) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
        } else {
            min = S.lod_blend ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        }
        p_glSamplerParameteri(*smp, GL_TEXTURE_MIN_FILTER, min);
        if (smooth && g_aniso_max > 1.0f && g_cfg.anisotropy > 1) {
            float a = (float)g_cfg.anisotropy;
            p_glSamplerParameterf(*smp, GL_TEXTURE_MAX_ANISOTROPY, a < g_aniso_max ? a : g_aniso_max);
        }
    }
    return *smp;
}

static int g_nodepth = -1;   /* DKGL_NODEPTH: depth test off (debugging) */

static void apply_state(void)
{
    float c[4];
    int x, y, w, h;
    if (!g_dirty) {
        return;
    }
    g_dirty = 0;
    if (g_nodepth < 0) {
        g_nodepth = getenv("DKGL_NODEPTH") != NULL;
    }
    p_glUseProgram(g_prog);
    p_glBindVertexArray(g_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    set_xf(U.xf);
    p_glUniform4i(U.cc, S.cc[0], S.cc[1], S.cc[2], S.cc[3]);
    p_glUniform4i(U.ac, S.ac[0], S.ac[1], S.ac[2], S.ac[3]);
    p_glUniform4i(U.tc, S.tc[0], S.tc[1], S.tc[2], S.tc[3]);
    p_glUniform4i(U.flags, S.cc[4] != 0, S.ac[4] != 0, (S.tc[4] != 0) | (S.tc[5] != 0) << 1, S.chroma_on);
    color4(S.constant, c);
    p_glUniform4f(U.constant, c[0], c[1], c[2], c[3]);
    color4(S.chroma, c);
    p_glUniform4f(U.chroma, c[0], c[1], c[2], c[3]);
    color4(S.fogcolor, c);
    p_glUniform4f(U.fogcol, c[0], c[1], c[2], c[3]);
    p_glUniform1fv(U.fog, FOG_TABLE_SIZE, g_fog);
    p_glUniform1i(U.fogmode, S.fog_mode & 3);
    p_glUniform1i(U.depthmode, S.depth_mode);
    p_glUniform1f(U.wscale, W_SCALE);
    p_glActiveTexture(GL_TEXTURE0);
    if (S.tex) {
        if (S.tex->raw) {
            tex_select(S.tex);
        }
        p_glBindTexture(GL_TEXTURE_2D, S.tex->gl);
        p_glUniform2f(U.stscale, 1.0f / S.tex->smax, 1.0f / S.tex->tmax);
    } else {
        p_glBindTexture(GL_TEXTURE_2D, g_white);
        p_glUniform2f(U.stscale, 1.0f / 256, 1.0f / 256);
    }
    p_glBindSampler(0, sampler_for());
    if (S.blend[0] == 4 && S.blend[1] == 0) {
        p_glDisable(GL_BLEND);
    } else {
        p_glEnable(GL_BLEND);
        p_glBlendFuncSeparate(gl_blend(S.blend[0], 1), gl_blend(S.blend[1], 0), gl_blend(S.blend[2], 1), gl_blend(S.blend[3], 0));
    }
    if (S.depth_mode == GR_DEPTHBUFFER_DISABLE || g_nodepth) {
        p_glDisable(GL_DEPTH_TEST);
    } else {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(gl_depth_func(S.depth_func));
    }
    p_glDepthMask(S.depth_mask != 0);
    p_glColorMask(S.cmask_rgb != 0, S.cmask_rgb != 0, S.cmask_rgb != 0, S.cmask_a != 0);
    scissor_rect(&x, &y, &w, &h);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x, y, w, h);
}

/* dkpatch's interpolation (interp.c) draws the game's own frame of every race tick only for its side effects and never
   shows it: while discarding, triangles, clears and LFB writes do nothing on the GPU (state and textures are still
   tracked), so that frame costs CPU time only and the shown, interpolated frame is the one render per tick. */
static int g_discard;

__declspec(dllexport) void __cdecl dkgl_discard(int on)
{
    flush();
    g_discard = on != 0;
    g_dirty = 1;
}

static void flush(void)
{
    if (!g_nbatch || !g_open || g_discard) {
        g_nbatch = 0;
        return;
    }
    apply_state();
    p_glBufferData(GL_ARRAY_BUFFER, g_nbatch * sizeof(vtx_t), g_batch, GL_STREAM_DRAW);
    p_glDrawArrays(GL_TRIANGLES, 0, g_nbatch);
    g_nbatch = 0;
}

#define SNAP 786432.0f

static void put(const GrVertex *v)
{
    vtx_t *o = &g_batch[g_nbatch++];
    float oow = v->oow;
    /* the Vertigo DLL snaps x/y by adding 3 * 2^18 (1/16 pixel steps) and leaves it there: the Voodoo
       only takes the low bits of the float, so the bias has to come off */
    o->x = v->x >= SNAP / 2 ? v->x - SNAP : v->x;
    o->y = v->y >= SNAP / 2 ? v->y - SNAP : v->y;
    o->ooz = v->ooz;
    o->oow = oow;
    o->r = v->r;
    o->g = v->g;
    o->b = v->b;
    o->a = v->a;
    if (oow != 0.0f) {
        o->s = v->tmuvtx[0].sow / oow;
        o->t = v->tmuvtx[0].tow / oow;
    } else {
        o->s = v->tmuvtx[0].sow;
        o->t = v->tmuvtx[0].tow;
    }
}

static int g_trace;   /* DKGL_TRACE=frame: log that frame's triangles and state */
static int g_ntris;

static void trace_tri(const GrVertex *v)
{
    int i;
    for (i = 0; i < 3; i++) {
        dkgl_log("  v x=%.2f y=%.2f ooz=%.1f oow=%.6f rgba=%.0f,%.0f,%.0f,%.0f st=%.2f,%.2f", v[i].x, v[i].y, v[i].ooz, v[i].oow,
                 v[i].r, v[i].g, v[i].b, v[i].a, v[i].tmuvtx[0].sow, v[i].tmuvtx[0].tow);
    }
    dkgl_log("  cc=%d,%d,%d,%d,%d ac=%d,%d,%d,%d,%d tc=%d,%d,%d,%d blend=%d,%d depth=%d/%d/%d cull=%d tex=%p fog=%d clip=%d,%d,%d,%d const=%08x",
             S.cc[0], S.cc[1], S.cc[2], S.cc[3], S.cc[4], S.ac[0], S.ac[1], S.ac[2], S.ac[3], S.ac[4], S.tc[0], S.tc[1], S.tc[2], S.tc[3],
             S.blend[0], S.blend[1], S.depth_mode, S.depth_func, S.depth_mask, S.cull, (void *)S.tex, S.fog_mode,
             S.clip[0], S.clip[1], S.clip[2], S.clip[3], S.constant);
}

static void tri(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    if (!g_open) {
        return;
    }
    g_ntris++;
    if (g_trace && (int)g_frames == g_trace && (g_ntris <= 30 || g_ntris % 200 == 0)) {
        GrVertex v[3];
        v[0] = *a; v[1] = *b; v[2] = *c;
        trace_tri(v);
    }
    if (S.cull != GR_CULL_DISABLE) {   /* (the snap bias cancels out in the differences) */
        float area = (a->x - b->x) * (b->y - c->y) - (b->x - c->x) * (a->y - b->y);
        if ((S.cull == GR_CULL_NEGATIVE && area < 0) || (S.cull == GR_CULL_POSITIVE && area > 0)) {
            return;
        }
    }
    if (g_nbatch + 3 > (int)(sizeof g_batch / sizeof g_batch[0])) {
        flush();
    }
    put(a);
    put(b);
    put(c);
}

/* ------------------------------------------------------------------ framebuffer + window */

static void destroy_fbo(void)
{
    if (g_fbo) {
        p_glDeleteFramebuffers(1, &g_fbo);
        p_glDeleteRenderbuffers(1, &g_fbo_color);
        p_glDeleteRenderbuffers(1, &g_fbo_depth);
        g_fbo = 0;
    }
    if (g_resolve_fbo) {
        p_glDeleteFramebuffers(1, &g_resolve_fbo);
        p_glDeleteRenderbuffers(1, &g_resolve_color);
        g_resolve_fbo = 0;
    }
}

/* framebuffer size: the largest rectangle of the Glide screen's aspect inside the client area, times
   the supersampling factor */
static void framebuffer_size(int cw, int ch, int *w, int *h)
{
    double a = (double)g_vw / g_vh;
    if (cw <= 0 || ch <= 0) {
        cw = g_vw;
        ch = g_vh;
    }
    if ((double)cw / ch > a) {
        *h = ch;
        *w = (int)(ch * a + 0.5);
    } else {
        *w = cw;
        *h = (int)(cw / a + 0.5);
    }
    if (g_cfg.supersample > 1) {
        *w *= g_cfg.supersample;
        *h *= g_cfg.supersample;
    }
}

static void create_fbo(int w, int h)
{
    int max = 0;
    destroy_fbo();
    g_rw = w;
    g_rh = h;
    g_samples = g_cfg.msaa;
    p_glGetIntegerv(GL_MAX_SAMPLES, &max);
    if (g_samples > max) {
        g_samples = max;
    }
    if (g_samples < 2) {
        g_samples = 0;
    }
    p_glGenFramebuffers(1, &g_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    p_glGenRenderbuffers(1, &g_fbo_color);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_color);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_samples, GL_RGBA8, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_fbo_color);
    p_glGenRenderbuffers(1, &g_fbo_depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_samples, GL_DEPTH_COMPONENT32F, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        dkgl_log("framebuffer %dx%d (%d samples) incomplete", w, h, g_samples);
    }
    if (g_samples) {   /* MSAA can't be scaled by a blit: resolve into a plain one first */
        p_glGenFramebuffers(1, &g_resolve_fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_resolve_fbo);
        p_glGenRenderbuffers(1, &g_resolve_color);
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_resolve_color);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, w, h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_resolve_color);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    p_glViewport(0, 0, w, h);
    p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor(0, 0, 0, 1);
    p_glClearDepth(S.depth_mode == GR_DEPTHBUFFER_WBUFFER ? 0.0 : 1.0);
    p_glColorMask(1, 1, 1, 1);
    p_glDepthMask(1);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g_dirty = 1;
    dkgl_log("framebuffer %dx%d for a %dx%d Glide screen, %d samples", w, h, g_vw, g_vh, g_samples);
}

static void client_size(int *w, int *h)
{
    RECT r;
    GetClientRect(g_hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

static void place_window(int fullscreen)
{
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(mon, &mi);
    g_fullscreen = fullscreen;
    if (fullscreen) {
        style = (style & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME)) | WS_POPUP | WS_VISIBLE;
        SetWindowLongA(g_hwnd, GWL_STYLE, style);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        /* the largest whole multiple of the Glide screen that fits 85 % of the work area (at least 1x) */
        int aw = (mi.rcWork.right - mi.rcWork.left) * 85 / 100, ah = (mi.rcWork.bottom - mi.rcWork.top) * 85 / 100;
        int k = 1, w, h;
        RECT r;
        while (g_vw * (k + 1) <= aw && g_vh * (k + 1) <= ah) {
            k++;
        }
        w = g_vw * k;
        h = g_vh * k;
        if (w > aw || h > ah) {   /* bigger than the screen: scale down keeping the aspect */
            double f = fmin((double)aw / g_vw, (double)ah / g_vh);
            w = (int)(g_vw * f);
            h = (int)(g_vh * f);
        }
        style = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongA(g_hwnd, GWL_STYLE, style);
        r.left = 0;
        r.top = 0;
        r.right = w;
        r.bottom = h;
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2,
                     mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_open && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp == VK_RETURN && (lp & (1 << 29))) {
        if (msg == WM_SYSKEYDOWN && !(lp & (1 << 30))) {
            place_window(!g_fullscreen);
            g_cfg.fullscreen = g_fullscreen;   /* kept when the game reopens the screen (a new resolution) */
        }
        return 0;
    }
    if (g_open && msg == WM_SYSCHAR && wp == VK_RETURN) {
        return 0;   /* no beep */
    }
    return CallWindowProcA(g_old_proc, hwnd, msg, wp, lp);
}

static int create_context(HWND hwnd)
{
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC tmp;
    int pf;
    static const int attrs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    g_hwnd = hwnd;
    g_dc = GetDC(hwnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) {
        dkgl_log("SetPixelFormat failed (%lu)", GetLastError());
        /* already set (the window outlived an earlier context): carry on with it */
    }
    tmp = wglCreateContext(g_dc);
    if (!tmp || !wglMakeCurrent(g_dc, tmp)) {
        dkgl_log("wglCreateContext failed (%lu)", GetLastError());
        return 0;
    }
    p_wglCreateContextAttribsARB = (void *)wglGetProcAddress("wglCreateContextAttribsARB");
    g_rc = p_wglCreateContextAttribsARB ? p_wglCreateContextAttribsARB(g_dc, NULL, attrs) : NULL;
    if (!g_rc) {
        dkgl_log("no OpenGL 3.3 core context");
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(tmp);
        return 0;
    }
    wglMakeCurrent(g_dc, g_rc);
    wglDeleteContext(tmp);
    if (!gl_load()) {
        return 0;
    }
    dkgl_log("OpenGL %s on %s", p_glGetString(GL_VERSION), p_glGetString(GL_RENDERER));
    p_glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g_aniso_max);
    p_glGetError();

    g_prog = link(VS, FS);
    U.xf = p_glGetUniformLocation(g_prog, "u_xf");
    U.stscale = p_glGetUniformLocation(g_prog, "u_stscale");
    U.cc = p_glGetUniformLocation(g_prog, "u_cc");
    U.ac = p_glGetUniformLocation(g_prog, "u_ac");
    U.tc = p_glGetUniformLocation(g_prog, "u_tc");
    U.flags = p_glGetUniformLocation(g_prog, "u_flags");
    U.constant = p_glGetUniformLocation(g_prog, "u_const");
    U.chroma = p_glGetUniformLocation(g_prog, "u_chroma");
    U.fogcol = p_glGetUniformLocation(g_prog, "u_fogcol");
    U.fog = p_glGetUniformLocation(g_prog, "u_fog");
    U.fogmode = p_glGetUniformLocation(g_prog, "u_fogmode");
    U.depthmode = p_glGetUniformLocation(g_prog, "u_depthmode");
    U.wscale = p_glGetUniformLocation(g_prog, "u_wscale");
    U.tex = p_glGetUniformLocation(g_prog, "u_tex");
    p_glUseProgram(g_prog);
    p_glUniform1i(U.tex, 0);
    g_blit_prog = link(BLIT_VS, BLIT_FS);
    p_glUseProgram(g_blit_prog);
    p_glUniform1i(p_glGetUniformLocation(g_blit_prog, "u_tex"), 0);

    p_glGenVertexArrays(1, &g_vao);
    p_glBindVertexArray(g_vao);
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(vtx_t), (void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(vtx_t), (void *)16);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(vtx_t), (void *)32);

    {
        static const uint32_t white = 0xffffffff;
        p_glGenTextures(1, &g_white);
        p_glBindTexture(GL_TEXTURE_2D, g_white);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_swap = -1;   /* the swap interval is set at the next grBufferSwap */
    if (!g_old_proc) {
        g_old_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)wnd_proc);
    }
    return 1;
}

/* ------------------------------------------------------------------ textures */

static int lod_size(int lod) { return 256 >> lod; }

static void tex_dims(int lod, int aspect, int *w, int *h)
{
    int s = lod_size(lod);
    *w = *h = s;
    if (aspect < 3) {
        *h = s >> (3 - aspect);
    } else if (aspect > 3) {
        *w = s >> (aspect - 3);
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int texel_bytes(int format) { return format >= GR_TEXFMT_ARGB_8332 ? 2 : 1; }

static uint32_t tex_mem(int small, int large, int aspect, int format)
{
    uint32_t total = 0;
    int lod, w, h;
    for (lod = large; lod <= small; lod++) {
        tex_dims(lod, aspect, &w, &h);
        total += w * h * texel_bytes(format);
    }
    return (total + 7) & ~7u;
}

static inline uint8_t x5(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t x6(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t x4(unsigned v) { return (uint8_t)(v * 17); }
static inline uint8_t x3(unsigned v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }
static inline uint8_t x2(unsigned v) { return (uint8_t)(v * 85); }

static uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) { return r | g << 8 | b << 16 | a << 24; }

static uint32_t pal(unsigned i, unsigned a)
{
    uint32_t p = g_palette[i & 255];
    return rgba((p >> 16) & 255, (p >> 8) & 255, p & 255, a);
}

static void decode(int format, const uint8_t *src, uint32_t *dst, int n)
{
    const uint16_t *s16 = (const uint16_t *)src;
    int i;
    static int warned;
    for (i = 0; i < n; i++) {
        unsigned v = texel_bytes(format) == 2 ? s16[i] : src[i];
        switch (format) {
        case GR_TEXFMT_RGB_332: dst[i] = rgba(x3(v >> 5), x3((v >> 2) & 7), x2(v & 3), 255); break;
        case GR_TEXFMT_ALPHA_8: dst[i] = rgba(v, v, v, v); break;
        case GR_TEXFMT_INTENSITY_8: dst[i] = rgba(v, v, v, 255); break;
        case GR_TEXFMT_ALPHA_INTENSITY_44: dst[i] = rgba(x4(v & 15), x4(v & 15), x4(v & 15), x4(v >> 4)); break;
        case GR_TEXFMT_P_8: dst[i] = pal(v, 255); break;
        case GR_TEXFMT_ARGB_8332: dst[i] = rgba(x3((v >> 5) & 7), x3((v >> 2) & 7), x2(v & 3), v >> 8); break;
        case GR_TEXFMT_RGB_565: dst[i] = rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); break;
        case GR_TEXFMT_ARGB_1555: dst[i] = rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), v & 0x8000 ? 255 : 0); break;
        case GR_TEXFMT_ARGB_4444: dst[i] = rgba(x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x4(v >> 12)); break;
        case GR_TEXFMT_ALPHA_INTENSITY_88: dst[i] = rgba(v & 255, v & 255, v & 255, v >> 8); break;
        case GR_TEXFMT_AP_88: dst[i] = pal(v & 255, v >> 8); break;
        default:
            if (!warned) {
                warned = 1;
                dkgl_log("texture format %d not supported (YIQ)", format);
            }
            dst[i] = rgba(128, 128, 128, 255);
            break;
        }
    }
}

static int palettised(int format) { return format == GR_TEXFMT_P_8 || format == GR_TEXFMT_AP_88; }

static void upload(tex_t *t, const uint8_t *data)
{
    static uint32_t buf[256 * 256];
    int lod, level = 0, w, h, bpp = texel_bytes(t->format);
    p_glBindTexture(GL_TEXTURE_2D, t->gl);
    for (lod = t->large_lod; lod <= t->small_lod; lod++, level++) {
        tex_dims(lod, t->aspect, &w, &h);
        decode(t->format, data, buf, w * h);
        p_glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        data += w * h * bpp;
    }
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
}

/* a palettised texture: the version decoded with the current palette (decoded now if there is none; the oldest of
   TEX_VERSIONS is reused). Binds texture unit 0. */
static void tex_select(tex_t *t)
{
    int k;
    for (k = 0; k < t->nver; k++) {
        if (t->ver_hash[k] == g_pal_hash) {
            t->gl = t->ver_gl[k];
            return;
        }
    }
    if (t->nver < TEX_VERSIONS) {
        k = t->nver++;
        p_glGenTextures(1, &t->ver_gl[k]);
    } else {
        k = t->next_ver;
        t->next_ver = (k + 1) % TEX_VERSIONS;
    }
    t->ver_hash[k] = g_pal_hash;
    t->gl = t->ver_gl[k];
    upload(t, t->raw);
}

static void tex_free(tex_t *t)
{
    if (S.tex == t) {
        flush();
        S.tex = NULL;
        g_dirty = 1;
    }
    if (t->raw) {
        p_glDeleteTextures(t->nver, t->ver_gl);
    } else {
        p_glDeleteTextures(1, &t->gl);
    }
    free(t->raw);
    *t = g_tex[--g_ntex];
    if (S.tex == &g_tex[g_ntex]) {
        S.tex = t;
    }
}

static tex_t *tex_find(uint32_t start, const GrTexInfo *info)
{
    int i;
    for (i = 0; i < g_ntex; i++) {
        tex_t *t = &g_tex[i];
        if (t->start == start && t->small_lod == info->smallLod && t->large_lod == info->largeLod &&
            t->aspect == info->aspectRatio && t->format == info->format) {
            return t;
        }
    }
    return NULL;
}

static void tex_download(uint32_t start, const GrTexInfo *info)
{
    uint32_t size = tex_mem(info->smallLod, info->largeLod, info->aspectRatio, info->format), end = start + size;
    tex_t *t;
    int i;
    flush();
    for (i = g_ntex - 1; i >= 0; i--) {   /* whatever the download overwrites is gone */
        if (g_tex[i].start < end && start < g_tex[i].end) {
            tex_free(&g_tex[i]);
        }
    }
    if (g_ntex == (int)(sizeof g_tex / sizeof g_tex[0])) {
        tex_free(&g_tex[0]);
    }
    t = &g_tex[g_ntex++];
    memset(t, 0, sizeof *t);
    t->start = start;
    t->end = end;
    t->small_lod = info->smallLod;
    t->large_lod = info->largeLod;
    t->aspect = info->aspectRatio;
    t->format = info->format;
    t->levels = info->smallLod - info->largeLod + 1;
    tex_dims(info->largeLod, info->aspectRatio, &t->w, &t->h);
    t->smax = t->w >= t->h ? 256.0f : 256.0f * t->w / t->h;
    t->tmax = t->h >= t->w ? 256.0f : 256.0f * t->h / t->w;
    if (palettised(t->format) && (t->raw = malloc(size)) != NULL) {
        memcpy(t->raw, info->data, size);   /* decoded when drawn (tex_select) */
    } else {
        p_glGenTextures(1, &t->gl);
        upload(t, info->data);
    }
    g_dirty = 1;
}

/* ------------------------------------------------------------------ LFB */

static void draw_image(int x, int y, int w, int h, const uint32_t *pixels)
{
    static unsigned tex;
    vtx_t q[6];
    int i;
    flush();
    if (!tex) {
        p_glGenTextures(1, &tex);
    }
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    p_glBindSampler(0, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    memset(q, 0, sizeof q);
    /* row 0 of the image is at Glide y (rows go up with a lower-left origin, down otherwise) */
    {
        float x0 = (float)x, x1 = (float)(x + w), y0 = (float)y, y1 = (float)(y + h);
        float px[6] = { x0, x1, x1, x0, x1, x0 }, py[6] = { y0, y0, y1, y0, y1, y1 };
        float u[6] = { 0, 1, 1, 0, 1, 0 }, v[6] = { 0, 0, 1, 0, 1, 1 };
        for (i = 0; i < 6; i++) {
            q[i].x = px[i];
            q[i].y = py[i];
            q[i].s = u[i];
            q[i].t = v[i];
            q[i].oow = 1;
        }
    }
    p_glUseProgram(g_blit_prog);
    set_xf(p_glGetUniformLocation(g_blit_prog, "u_xf"));
    p_glDisable(GL_BLEND);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(1, 1, 1, 1);
    p_glBindVertexArray(g_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    p_glDrawArrays(GL_TRIANGLES, 0, 6);
    g_dirty = 1;
}

static void present(void);

static void write_screenshot(unsigned src)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    int stride = (g_rw * 3 + 3) & ~3, y;
    uint8_t *px = malloc((size_t)g_rw * g_rh * 4), *row = calloc(1, stride);
    FILE *f = fopen(g_shot_path, "wb");
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    p_glReadPixels(0, 0, g_rw, g_rh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (f) {
        memset(&fh, 0, sizeof fh);
        memset(&ih, 0, sizeof ih);
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + stride * g_rh;
        ih.biSize = sizeof ih;
        ih.biWidth = g_rw;
        ih.biHeight = g_rh;   /* bottom-up, like GL */
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (y = 0; y < g_rh; y++) {
            int x;
            for (x = 0; x < g_rw; x++) {
                row[x * 3] = px[(y * g_rw + x) * 4 + 2];
                row[x * 3 + 1] = px[(y * g_rw + x) * 4 + 1];
                row[x * 3 + 2] = px[(y * g_rw + x) * 4];
            }
            fwrite(row, stride, 1, f);
        }
        fclose(f);
    }
    free(px);
    free(row);
    dkgl_log("screenshot %s (%dx%d)", g_shot_path, g_rw, g_rh);
}

/* ------------------------------------------------------------------ Glide API */

FxBool API grSstQueryBoards(GrHwConfiguration *hw) { hw->num_sst = 1; return 1; }

FxBool API grSstQueryHardware(GrHwConfiguration *hw)
{
    memset(hw, 0, sizeof *hw);
    hw->num_sst = 1;
    hw->sst[0].type = 3;               /* GR_SSTTYPE_Voodoo2 */
    hw->sst[0].cfg.fbRam = 4;
    hw->sst[0].cfg.fbiRev = 2;
    hw->sst[0].cfg.nTexelfx = 1;
    hw->sst[0].cfg.tmu[0].tmuRev = 1;
    hw->sst[0].cfg.tmu[0].tmuRam = TEX_MEM >> 20;
    return 1;
}

void API grGlideInit(void) {}
void API grGlideShutdown(void) {}
void API grGlideGetVersion(char *v) { strcpy(v, "Glide Version 2.46 (dkpatch OpenGL)"); }
void API grSstSelect(int which) { (void)which; }

static void reset_state(void)
{
    int i;
    memset(&S, 0, sizeof S);
    S.cc[0] = 1;                /* GR_COMBINE_FUNCTION_LOCAL, iterated */
    S.ac[0] = 1;
    S.tc[0] = 1;
    S.tc[2] = 1;
    S.blend[0] = 4;             /* ONE, ZERO */
    S.blend[2] = 4;
    S.depth_func = 1;           /* LESS */
    S.depth_mask = 0;
    S.cmask_rgb = 1;
    S.cmask_a = 1;
    S.clip[2] = g_vw;
    S.clip[3] = g_vh;
    S.minf = S.magf = 1;
    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        g_fog[i] = 0;
    }
    g_dirty = 1;
}

static BOOL CALLBACK find_window(HWND hwnd, LPARAM out)
{
    if (!GetWindow(hwnd, GW_OWNER) && IsWindowVisible(hwnd)) {
        *(HWND *)out = hwnd;
        return FALSE;
    }
    return TRUE;
}

FxBool API grSstWinOpen(int hwnd, int res, int refresh, int cformat, int origin, int ncolor, int naux)
{
    static const short modes[][2] = {
        { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 }, { 640, 200 }, { 640, 350 }, { 640, 400 },
        { 640, 480 }, { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 }, { 1024, 768 }, { 1280, 1024 },
        { 1600, 1200 }, { 400, 300 },
    };
    HWND wnd = (HWND)(intptr_t)hwnd;
    int cw, ch, w, h;
    (void)refresh; (void)ncolor; (void)naux;
    if (res >= 0 && res < (int)(sizeof modes / sizeof modes[0])) {
        g_vw = modes[res][0];
        g_vh = modes[res][1];
    } else if (res == GR_RESOLUTION_NONE && g_cfg.virt_w > 0 && g_cfg.virt_h > 0) {
        g_vw = g_cfg.virt_w;
        g_vh = g_cfg.virt_h;
    } else {
        g_vw = 640;
        g_vh = 480;
    }
    if (!wnd) {   /* the Vertigo DLL passes 0 when fullscreen: use the game's main window */
        EnumThreadWindows(GetCurrentThreadId(), find_window, (LPARAM)&wnd);
    }
    if (!wnd) {
        wnd = GetActiveWindow();
    }
    if (!wnd) {
        dkgl_log("grSstWinOpen: no window");
        return 0;
    }
    if (wnd != g_hwnd || !g_rc) {
        if (g_rc) {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(g_rc);
            g_rc = NULL;
        }
        if (!create_context(wnd)) {
            return 0;
        }
    }
    g_cformat = cformat;
    g_origin = origin;
    g_open = 1;
    reset_state();
    place_window(g_cfg.fullscreen);
    client_size(&cw, &ch);
    framebuffer_size(cw, ch, &w, &h);
    create_fbo(w, h);
    dkgl_log("grSstWinOpen res %d -> %dx%d, window %dx%d, %s", res, g_vw, g_vh, cw, ch, g_fullscreen ? "fullscreen" : "windowed");
    return 1;
}

void API grSstWinClose(void)
{
    if (!g_open) {
        return;
    }
    flush();
    while (g_ntex) {
        tex_free(&g_tex[g_ntex - 1]);
    }
    destroy_fbo();
    g_open = 0;
}

FxBool API grSstControl(FxU32 code) { (void)code; return 1; }
void API grSstIdle(void) { flush(); }
void API grSstPerfStats(void *stats) { memset(stats, 0, 20); }
void API grSstResetPerfStats(void) {}
FxU32 API grSstScreenWidth(void) { return g_vw; }
FxU32 API grSstScreenHeight(void) { return g_vh; }

static int g_fbo_stale;

/* new settings from dkgl_configure while the screen is open */
static void apply_pending(void)
{
    dkgl_config_t c = g_pending;
    int i;
    InterlockedExchange(&g_pending_set, 0);
    c.virt_w = g_cfg.virt_w;
    c.virt_h = g_cfg.virt_h;
    c.log = g_cfg.log;
    if (c.vsync != g_cfg.vsync) {
        g_swap = -1;
    }
    if (c.msaa != g_cfg.msaa || c.supersample != g_cfg.supersample) {
        g_fbo_stale = 1;
    }
    if (c.filter != g_cfg.filter || c.anisotropy != g_cfg.anisotropy) {
        for (i = 0; i < (int)(sizeof g_samplers / sizeof g_samplers[0]); i++) {
            if (g_samplers[i]) {
                p_glDeleteSamplers(1, &g_samplers[i]);
                g_samplers[i] = 0;
            }
        }
        g_dirty = 1;
    }
    g_cfg = c;
    if (c.fullscreen != g_fullscreen) {
        place_window(c.fullscreen);
    }
    dkgl_log("settings: Fullscreen=%d VSync=%d MSAA=%d Supersample=%d SmoothTextures=%d Anisotropy=%d", c.fullscreen,
             c.vsync, c.msaa, c.supersample, c.filter, c.anisotropy);
}

static void present(void)
{
    int cw, ch, w, h, x, y;
    unsigned src = g_fbo;
    if (!g_open) {
        return;
    }
    flush();
    client_size(&cw, &ch);
    if (cw <= 0 || ch <= 0) {
        SwapBuffers(g_dc);
        return;
    }
    /* blits obey the scissor test: with the last draw's clip still set (the centred 4:3 menu), only that part
       was resolved and the bars kept an old frame (e.g. the race before going back to the menus) */
    p_glDisable(GL_SCISSOR_TEST);
    if (g_samples) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve_fbo);
        p_glBlitFramebuffer(0, 0, g_rw, g_rh, 0, 0, g_rw, g_rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        src = g_resolve_fbo;
    }
    /* letterbox the frame (the Glide screen's aspect) into the client area */
    if ((double)cw / ch > (double)g_vw / g_vh) {
        h = ch;
        w = (int)((double)ch * g_vw / g_vh + 0.5);
    } else {
        w = cw;
        h = (int)((double)cw * g_vh / g_vw + 0.5);
    }
    x = (cw - w) / 2;
    y = (ch - h) / 2;
    if (InterlockedCompareExchange(&g_shot_req, 0, 0)) {
        write_screenshot(src);
        InterlockedExchange(&g_shot_req, 0);
        if (getenv("DKGL_TRACE")) {   /* trace the frame after a screenshot */
            g_trace = (int)g_frames + 1;
        }
    }
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(1, 1, 1, 1);
    p_glViewport(0, 0, cw, ch);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    p_glBlitFramebuffer(0, 0, g_rw, g_rh, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, (w == g_rw && h == g_rh) ? GL_NEAREST : GL_LINEAR);
    SwapBuffers(g_dc);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    p_glViewport(0, 0, g_rw, g_rh);
    g_dirty = 1;
    if (InterlockedCompareExchange(&g_pending_set, 0, 0)) {
        apply_pending();
        client_size(&cw, &ch);
    }
    /* the window changed size (Alt+Enter, a different monitor) or MSAA/supersampling changed: follow it */
    framebuffer_size(cw, ch, &w, &h);
    if (w != g_rw || h != g_rh || g_fbo_stale) {
        g_fbo_stale = 0;
        create_fbo(w, h);
    }
    if (++g_frames == 1) {
        const char *t = getenv("DKGL_TRACE");
        g_trace = t ? atoi(t) : 0;
        dkgl_log("first frame presented");
    }
    if (g_trace && (int)g_frames == g_trace + 1) {
        dkgl_log("frame %d: %d triangles, %d textures", g_trace, g_ntris, g_ntex);
    }
    g_ntris = 0;
}

/* interval: the game's "video sync" option (Options - Graphics - Customise Performance; the device DLL passes it
   through, VTX_EndScene -> grBufferSwap): the swap interval, unless the config forces one */
void API grBufferSwap(int interval)
{
    int want = g_cfg.vsync >= 0 ? g_cfg.vsync != 0 : interval > 0;
    if (want != g_swap && p_wglSwapIntervalEXT) {
        p_wglSwapIntervalEXT(want);
        if (g_swap != -1 || g_frames) {
            dkgl_log("vsync %s", want ? "on" : "off");
        }
        g_swap = want;
    }
    present();
}

void API grBufferClear(GrColor_t color, GrColor_t alpha, uint16_t depth)
{
    float c[4];
    int x, y, w, h;
    unsigned mask = 0;
    if (!g_open || g_discard) {
        return;
    }
    flush();
    if (g_trace && (int)g_frames == g_trace) {
        dkgl_log("  clear color=%08x alpha=%02x depth=%04x masks rgb=%d a=%d depth=%d mode=%d clip=%d,%d,%d,%d", color, alpha, depth,
                 S.cmask_rgb, S.cmask_a, S.depth_mask, S.depth_mode, S.clip[0], S.clip[1], S.clip[2], S.clip[3]);
    }
    color4(color, c);
    scissor_rect(&x, &y, &w, &h);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x, y, w, h);
    if (S.cmask_rgb || S.cmask_a) {
        p_glColorMask(S.cmask_rgb != 0, S.cmask_rgb != 0, S.cmask_rgb != 0, S.cmask_a != 0);
        p_glClearColor(c[0], c[1], c[2], (alpha & 255) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (S.depth_mask && S.depth_mode != GR_DEPTHBUFFER_DISABLE) {
        p_glDepthMask(1);
        /* W buffer: depth is oow * scale (nearer = larger). The Vertigo DLL clears it with values like 0x92c0 or
           0xaf40 (the Voodoo's 4.12 float of w, a far plane at w ~ 600..2000), yet its world is drawn at w ~ 5000:
           the engine clips its own far plane, so a W clear is always "farthest". */
        p_glClearDepth(S.depth_mode == GR_DEPTHBUFFER_WBUFFER ? 0.0 : depth / 65535.0);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) {
        p_glClear(mask);
    }
    g_dirty = 1;
}

void API grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx, FxU32 maxy)
{
    if ((int)minx != S.clip[0] || (int)miny != S.clip[1] || (int)maxx != S.clip[2] || (int)maxy != S.clip[3]) {
        flush();
        S.clip[0] = minx;
        S.clip[1] = miny;
        S.clip[2] = maxx;
        S.clip[3] = maxy;
        g_dirty = 1;
    }
}

void API grColorCombine(int function, int factor, int local, int other, FxBool invert)
{
    SET(S.cc[0], function); SET(S.cc[1], factor); SET(S.cc[2], local); SET(S.cc[3], other); SET(S.cc[4], invert);
}

void API grAlphaCombine(int function, int factor, int local, int other, FxBool invert)
{
    SET(S.ac[0], function); SET(S.ac[1], factor); SET(S.ac[2], local); SET(S.ac[3], other); SET(S.ac[4], invert);
}

void API grTexCombine(int tmu, int rgb_function, int rgb_factor, int alpha_function, int alpha_factor, FxBool rgb_invert, FxBool alpha_invert)
{
    if (tmu != 0) {
        return;
    }
    SET(S.tc[0], rgb_function); SET(S.tc[1], rgb_factor); SET(S.tc[2], alpha_function); SET(S.tc[3], alpha_factor);
    SET(S.tc[4], rgb_invert); SET(S.tc[5], alpha_invert);
}

void API grAlphaBlendFunction(int rgb_sf, int rgb_df, int alpha_sf, int alpha_df)
{
    SET(S.blend[0], rgb_sf); SET(S.blend[1], rgb_df); SET(S.blend[2], alpha_sf); SET(S.blend[3], alpha_df);
}

void API grConstantColorValue(GrColor_t c) { SET(S.constant, c); }
void API grChromakeyMode(int mode) { SET(S.chroma_on, mode != 0); }
void API grChromakeyValue(GrColor_t c) { SET(S.chroma, c); }
void API grColorMask(FxBool rgb, FxBool a) { SET(S.cmask_rgb, rgb != 0); SET(S.cmask_a, a != 0); }
void API grCullMode(int mode) { S.cull = mode; }
void API grDepthBufferFunction(int f) { SET(S.depth_func, f); }
void API grDepthMask(FxBool on) { SET(S.depth_mask, on != 0); }
void API grDitherMode(int mode) { (void)mode; }
void API grFogColorValue(GrColor_t c) { SET(S.fogcolor, c); }
void API grFogMode(int mode) { SET(S.fog_mode, mode); }

void API grDepthBufferMode(int mode)
{
    if (mode > GR_DEPTHBUFFER_WBUFFER) {
        mode -= 2;   /* the compare-to-bias variants */
    }
    SET(S.depth_mode, mode);
}

void API grFogTable(const uint8_t *table)
{
    int i;
    flush();
    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        g_fog[i] = table[i];
    }
    g_dirty = 1;
}

void API grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c) { tri(a, b, c); }

void API grAADrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c, FxBool ab, FxBool bc, FxBool ca)
{
    (void)ab; (void)bc; (void)ca;
    tri(a, b, c);
}

/* a line is a quad, one Glide pixel wide per 480 lines of screen height: the game's lines (the race's minimap,
   menu frames) were drawn for 640x480 and would thin out to a hairline at high resolutions */
void API grDrawLine(const GrVertex *a, const GrVertex *b)
{
    GrVertex q[4];
    float dx = b->x - a->x, dy = b->y - a->y, len = sqrtf(dx * dx + dy * dy), nx, ny;
    float half = g_vh > 480 ? 0.5f * g_vh / 480 : 0.5f;
    int cull = S.cull;
    if (!g_open) {
        return;
    }
    if (len < 1e-6f) {
        nx = half;
        ny = 0;
    } else {
        nx = -dy / len * half;
        ny = dx / len * half;
    }
    q[0] = *a; q[1] = *a; q[2] = *b; q[3] = *b;
    q[0].x += nx; q[0].y += ny;
    q[1].x -= nx; q[1].y -= ny;
    q[2].x -= nx; q[2].y -= ny;
    q[3].x += nx; q[3].y += ny;
    S.cull = GR_CULL_DISABLE;
    tri(&q[0], &q[1], &q[2]);
    tri(&q[0], &q[2], &q[3]);
    S.cull = cull;
}

void API grAADrawLine(const GrVertex *a, const GrVertex *b) { grDrawLine(a, b); }

FxU32 API grTexMinAddress(int tmu) { (void)tmu; return 0; }
FxU32 API grTexMaxAddress(int tmu) { (void)tmu; return TEX_MEM - 8; }

FxU32 API grTexCalcMemRequired(int small, int large, int aspect, int format)
{
    return tex_mem(small, large, aspect, format);
}

void API grTexDownloadMipMap(int tmu, FxU32 start, FxU32 evenOdd, GrTexInfo *info)
{
    (void)evenOdd;
    if (tmu != 0 || !g_open || !info || !info->data) {
        return;
    }
    tex_download(start, info);
}

void API grTexSource(int tmu, FxU32 start, FxU32 evenOdd, GrTexInfo *info)
{
    tex_t *t;
    (void)evenOdd;
    if (tmu != 0 || !g_open) {
        return;
    }
    t = tex_find(start, info);
    if (t != S.tex) {
        flush();
        S.tex = t;
        g_dirty = 1;
    }
}

static uint32_t hash_palette(void)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 256; i++) {
        h = (h ^ g_palette[i]) * 16777619u;
    }
    return h;
}

void API grTexDownloadTablePartial(int tmu, int type, const uint32_t *data, int start, int end)
{
    int i;
    if (tmu != 0 || type != GR_TEXTABLE_PALETTE || !data) {
        return;
    }
    flush();   /* what is batched was drawn with the old palette */
    for (i = start; i <= end && i < 256; i++) {
        g_palette[i] = data[i];
    }
    g_pal_hash = hash_palette();
    if (S.tex && S.tex->raw) {
        g_dirty = 1;   /* apply_state picks the texture's version for this palette */
    }
}

void API grTexDownloadTable(int tmu, int type, const uint32_t *data)
{
    grTexDownloadTablePartial(tmu, type, data, 0, 255);
}

void API grTexClampMode(int tmu, int s, int t)
{
    if (tmu == 0) {
        SET(S.clamp_s, s); SET(S.clamp_t, t);
    }
}

void API grTexFilterMode(int tmu, int minf, int magf)
{
    if (tmu == 0) {
        SET(S.minf, minf); SET(S.magf, magf);
    }
}

void API grTexMipMapMode(int tmu, int mode, FxBool lod_blend)
{
    if (tmu == 0) {
        SET(S.mip_mode, mode); SET(S.lod_blend, lod_blend != 0);
    }
}

void API grTexLodBiasValue(int tmu, float bias) { (void)tmu; S.lod_bias = bias; }
void API grTexDetailControl(int tmu, int lod_bias, uint8_t detail_scale, float detail_max)
{
    (void)tmu; (void)lod_bias; (void)detail_scale; (void)detail_max;
}

/* LFB: 16-bit 565/555/1555 or 32-bit 888/8888 source pixels */
FxBool API grLfbWriteRegion(int buffer, FxU32 x, FxU32 y, int fmt, FxU32 w, FxU32 h, int stride, const void *data)
{
    uint32_t *px;
    FxU32 i, j;
    if (!g_open || g_discard || !w || !h || buffer > 1 || w > 4096 || h > 4096) {
        return buffer <= 1;
    }
    px = malloc(w * h * 4);
    for (j = 0; j < h; j++) {
        const uint8_t *row = (const uint8_t *)data + j * stride;
        for (i = 0; i < w; i++) {
            uint32_t v;
            switch (fmt) {
            case 0: v = ((const uint16_t *)row)[i]; px[j * w + i] = rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); break;
            case 1:
            case 2: v = ((const uint16_t *)row)[i]; px[j * w + i] = rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255); break;
            case 4:
            case 5: v = ((const uint32_t *)row)[i]; px[j * w + i] = rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, 255); break;
            default: px[j * w + i] = 0; break;
            }
        }
    }
    draw_image(x, y, w, h, px);
    free(px);
    if (buffer == 0) {   /* the front buffer: show it now */
        present();
    }
    return 1;
}

/* reads back RGB565 at Glide resolution (screenshots) */
FxBool API grLfbReadRegion(int buffer, FxU32 x, FxU32 y, FxU32 w, FxU32 h, FxU32 stride, void *data)
{
    static unsigned fbo, rb;
    uint8_t *rgb;
    FxU32 i, j;
    (void)buffer;
    if (!g_open || !w || !h) {
        return 0;
    }
    flush();
    if (!fbo) {
        p_glGenFramebuffers(1, &fbo);
        p_glGenRenderbuffers(1, &rb);
    }
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, g_vw, g_vh);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    p_glDisable(GL_SCISSOR_TEST);
    if (g_samples) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve_fbo);
        p_glBlitFramebuffer(0, 0, g_rw, g_rh, 0, 0, g_rw, g_rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_resolve_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    } else {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    }
    p_glBlitFramebuffer(0, 0, g_rw, g_rh, 0, 0, g_vw, g_vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    rgb = malloc(w * h * 4);
    /* GL rows go up from the bottom, like Glide's lower-left origin */
    p_glReadPixels(x, g_origin == GR_ORIGIN_LOWER_LEFT ? (int)y : (int)(g_vh - y - h), w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgb);
    for (j = 0; j < h; j++) {
        uint16_t *out = (uint16_t *)((uint8_t *)data + j * stride);
        const uint8_t *in = rgb + (g_origin == GR_ORIGIN_LOWER_LEFT ? j : h - 1 - j) * w * 4;
        for (i = 0; i < w; i++) {
            out[i] = (uint16_t)((in[i * 4] >> 3) << 11 | (in[i * 4 + 1] >> 2) << 5 | in[i * 4 + 2] >> 3);
        }
    }
    free(rgb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    g_dirty = 1;
    return 1;
}

/* fog table helpers (gu*), as in Glide's gu.c */
static float fog_index_to_w(int i) { return (float)(pow(2.0, 3.0 + (double)(i >> 2)) / (8 - (i & 3))); }

void API guFogGenerateExp(uint8_t *table, float density)
{
    int i;
    float scale = 1.0f - expf(-density * fog_index_to_w(FOG_TABLE_SIZE - 1));
    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        float f = (1.0f - expf(-density * fog_index_to_w(i))) / scale;
        table[i] = (uint8_t)(f > 1 ? 255 : f < 0 ? 0 : f * 255);
    }
}

void API guFogGenerateExp2(uint8_t *table, float density)
{
    int i;
    float d = density * fog_index_to_w(FOG_TABLE_SIZE - 1), scale = 1.0f - expf(-d * d);
    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        float dp = density * fog_index_to_w(i), f = (1.0f - expf(-dp * dp)) / scale;
        table[i] = (uint8_t)(f > 1 ? 255 : f < 0 ? 0 : f * 255);
    }
}

void API guFogGenerateLinear(uint8_t *table, float near_w, float far_w)
{
    int i;
    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        float f = (fog_index_to_w(i) - near_w) / (far_w - near_w);
        table[i] = (uint8_t)(f > 1 ? 255 : f < 0 ? 0 : f * 255);
    }
}
