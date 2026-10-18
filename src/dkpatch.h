#ifndef DKPATCH_H
#define DKPATCH_H
#include <windows.h>
#include <stdint.h>

int dk_patch_start(void);            /* exe check + Enabled; 1 = go ahead and install the modules */
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

void *dk_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */

/* patch modules */
void dk_net_install(void);
void dk_widescreen_install(void);

/* test harness (tools/mp.py), only when DK_UNIT is set */
extern int dk_unit;
void dk_test_install(void);
void dk_test_wrap_dinput(void *direct_input);

#define DK_ADDR(a) ((void *)(uintptr_t)(a))
#endif
