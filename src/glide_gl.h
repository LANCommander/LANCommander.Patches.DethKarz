/* Interface between dkpatch and its OpenGL Glide (src/glide_gl.c, dkpatch\glide2x.dll) */
#ifndef GLIDE_GL_H
#define GLIDE_GL_H

typedef struct {
    int size;            /* sizeof(dkgl_config_t) */
    int fullscreen;      /* 1 = borderless fullscreen on the window's monitor, 0 = a window (Alt+Enter toggles) */
    int vsync;           /* -1 = the game's video sync option (grBufferSwap's interval), 0 / 1 = off / on */
    int msaa;            /* samples, 0 = off */
    int supersample;     /* render at this multiple of the window size */
    int filter;          /* 1 = trilinear + anisotropic where the game asks for bilinear */
    int anisotropy;
    int virt_w, virt_h;  /* Glide screen for GR_RESOLUTION_NONE (any resolution) */
    void (*log)(const char *line);
} dkgl_config_t;

typedef void(__cdecl *dkgl_configure_fn)(const dkgl_config_t *c);   /* while the screen is open: applied at the next frame */
typedef void(__cdecl *dkgl_get_config_fn)(dkgl_config_t *out);
typedef void(__cdecl *dkgl_set_virtual_size_fn)(int w, int h);

void dkgl_log(const char *fmt, ...);

/* dkpatch side (opengl.c): 0 / nothing unless the game renders through the OpenGL Glide */
int dk_opengl_get(dkgl_config_t *c);
void dk_opengl_set(const dkgl_config_t *c);
#endif
