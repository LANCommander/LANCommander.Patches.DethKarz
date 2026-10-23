#ifndef DKPATCH_H
#define DKPATCH_H
#include <windows.h>
#include <stdint.h>

#ifndef DK_VERSION
#define DK_VERSION "dev"             /* build.sh passes the release tag */
#endif

int dk_patch_start(void);            /* exe check + Enabled + not loaded twice; 1 = go ahead and install the modules */
void dk_install(void);               /* installs the patch modules */
void dk_log(const char *fmt, ...);

/* where settings and log lines go: dkpatch.ini / dkpatch.log (proxy), or callbacks (Interposer plugin) */
void dk_set_files(const char *ini, const char *log);
void dk_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line));

/* memory helpers (all addresses are Dethkarz.exe VAs, image base 0x400000, no relocations) */
int dk_write(uint32_t addr, const void *bytes, size_t len);
int dk_expect(uint32_t addr, const void *bytes, size_t len);   /* 1 if memory matches */
int dk_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int dk_jmp(uint32_t from, const void *to);    /* E9 rel32 at from */
int dk_call(uint32_t from, const void *to);   /* E8 rel32 at from */

/* settings: [section] key in dkpatch.ini, or Plugins.Dethkarz.<key> in the Interposer's Config.yml */
int dk_ini_int(const char *section, const char *key, int def);
int dk_ini_str(const char *section, const char *key, char *out, int size);   /* "" if missing */
void dk_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size));
int dk_ini_set_int(const char *section, const char *key, int value);   /* 1 if stored */
void dk_set_write_backend(int (*config_set)(const char *section, const char *key, int value));
int dk_ini_set_str(const char *section, const char *key, const char *value);   /* 1 if stored */
void dk_set_string_write_backend(int (*config_set_str)(const char *section, const char *key, const char *value));

void *dk_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */
void *dk_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement);

/* patch modules */
void dk_net_install(void);
void dk_widescreen_install(void);
void dk_players_install(void);
void dk_gamepad_install(void);
void dk_saves_install(void);
void dk_opengl_install(void);
void dk_device_install(void);   /* opengl.c: renderer choice in the settings, no device dialog at the first start */

/* opengl.c: the OpenGL Glide is loaded; widescreen.c lets it see device loads and adds its modes */
extern int dk_opengl_on;
int dk_opengl_device_loaded(char *base);
int dk_opengl_discard(int on);   /* interp.c: draw nothing until 0 (the hidden game frame); 0 = not supported */
int dk_opengl_modes(int (*out)[2], int max);

/* lod.c: vehicle draw distance in percent of the original (0 = always full detail); the setter applies at once */
extern int dk_lod_percent;
void dk_lod_install(void);
void dk_lod_set_percent(int pct);

/* interp.c: frames drawn between game ticks, interpolated ([Graphics] MaxFps, docs/framerate.md) */
void dk_interp_install(void);
void dk_interp_device_loaded(HMODULE device);   /* widescreen.c: the renderer DLL was loaded */
int dk_interp_available(void);     /* gfxmenu.c: Options - Graphics "max framerate" */
int dk_interp_max_fps(void);       /* MaxFps: 0 the refresh rate, -1 no limit, 60 or less one frame per tick, or a cap */
void dk_interp_set_max_fps(int setting);
int dk_interp_refresh(void);       /* the desktop refresh rate */

/* tickrate.c: the game ticks at 60 Hz (also in network games); [Graphics] ExperimentalTickRate, ini only, makes single-player
   races tick at the display refresh rate instead (docs/framerate.md) */
void dk_tickrate_install(void);
int dk_tickrate_rates(int *out, int max);   /* the refresh rates the display supports, highest first */

/* gfxmenu.c: the patch's video settings as rows in Options - Graphics */
void dk_gfxmenu_install(void);

/* players.c: the player count and where the lockstep channels are (DN_g_Net+0x318 unless relocated) */
extern int dk_max_players;
extern uintptr_t dk_net_channels;

/* test harness (tools/mp.py), only when DK_UNIT is set */
extern int dk_unit;
void dk_test_install(void);
void dk_test_wrap_dinput(void *direct_input);
void *dk_fake_ddraw(void);   /* fakeddraw.c: headless IDirectDraw */
void dk_gamepad_command(const char *args);   /* gamepad.c: drive the virtual SDL gamepad */

#define DK_ADDR(a) ((void *)(uintptr_t)(a))
#endif
