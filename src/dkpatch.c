/* Core of the patch: verifies the exe, reads the settings, installs the patch modules.
 * Settings and log go to dkpatch.ini / dkpatch.log next to the DLL (dinput.dll proxy build), or
 * to the LANCommander Interposer's Config.yml / session log (plugin build, see interposer_plugin.c). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

#define DK_TIMESTAMP 0x362c0cfeu   /* Dethkarz.exe (GOG), 16 Oct 1998 */

int dk_unit;   /* test harness unit (DK_UNIT), 0 = normal game; set by the dinput.dll proxy */

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];
static int (*g_config_int)(const char *section, const char *key, int def);
static void (*g_log_line)(const char *line);
static int (*g_config_str)(const char *section, const char *key, char *out, int size);

void dk_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line))
{
    g_config_int = config_int;
    g_log_line = log_line;
}

void dk_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log_line) {
        char line[512];
        vsnprintf(line, sizeof line, fmt, ap);
        g_log_line(line);
    } else {
        FILE *f = fopen(g_log, "a");
        if (f) {
            vfprintf(f, fmt, ap);
            fputc('\n', f);
            fclose(f);
        }
    }
    va_end(ap);
}

int dk_write(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(DK_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        dk_log("VirtualProtect %08x failed (%lu)", addr, GetLastError());
        return 0;
    }
    memcpy(DK_ADDR(addr), bytes, len);
    VirtualProtect(DK_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), DK_ADDR(addr), len);
    return 1;
}

int dk_expect(uint32_t addr, const void *bytes, size_t len)
{
    return memcmp(DK_ADDR(addr), bytes, len) == 0;
}

int dk_patch(uint32_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!dk_expect(addr, expect, len)) {
        dk_log("patch %08x: original bytes differ, skipped", addr);
        return 0;
    }
    return dk_write(addr, bytes, len);
}

static int rel32(uint32_t from, uint8_t op, const void *to)
{
    uint8_t b[5];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    return dk_write(from, b, 5);
}

int dk_jmp(uint32_t from, const void *to) { return rel32(from, 0xe9, to); }
int dk_call(uint32_t from, const void *to) { return rel32(from, 0xe8, to); }

int dk_ini_int(const char *section, const char *key, int def)
{
    if (g_config_int) {
        return g_config_int(section, key, def);
    }
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}

void dk_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size))
{
    g_config_str = config_str;
}

static int (*g_config_set)(const char *section, const char *key, int value);

void dk_set_write_backend(int (*config_set)(const char *section, const char *key, int value))
{
    g_config_set = config_set;
}

/* stores a setting (the in-game options): dkpatch.ini keeps its comments, the Interposer updates Config.yml */
int dk_ini_set_int(const char *section, const char *key, int value)
{
    char text[16];
    if (g_config_int) {
        return g_config_set ? g_config_set(section, key, value) : 0;
    }
    snprintf(text, sizeof text, "%d", value);
    return WritePrivateProfileStringA(section, key, text, g_ini) != 0;
}

static int (*g_config_set_str)(const char *section, const char *key, const char *value);

void dk_set_string_write_backend(int (*config_set_str)(const char *section, const char *key, const char *value))
{
    g_config_set_str = config_set_str;
}

/* stores a text setting (the renderer choice, opengl.c) */
int dk_ini_set_str(const char *section, const char *key, const char *value)
{
    if (g_config_int) {
        return g_config_set_str ? g_config_set_str(section, key, value) : 0;
    }
    return WritePrivateProfileStringA(section, key, value, g_ini) != 0;
}

/* text setting; out is left empty if the key is missing. Returns 1 if a non-empty value was read. */
int dk_ini_str(const char *section, const char *key, char *out, int size)
{
    out[0] = '\0';
    if (g_config_int) {   /* Interposer backend */
        return g_config_str ? g_config_str(section, key, out, size) : 0;
    }
    GetPrivateProfileStringA(section, key, "", out, size, g_ini);
    return out[0] != '\0';
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == DK_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == 0xcb820;
}

int dk_patch_start(void)
{
    if (!exe_is_supported()) {
        dk_log("dkpatch: not the supported Dethkarz.exe; no patches applied");
        return 0;
    }
    if (!dk_ini_int("General", "Enabled", 1)) {
        dk_log("dkpatch: disabled in the settings");
        return 0;
    }
    {
        /* the dinput.dll proxy, the ASI and the Interposer plugin carry the same patches: only the first one loaded applies them */
        char name[48];
        snprintf(name, sizeof name, "Local\\dkpatch_%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) {
            dk_log("dkpatch: another copy of the patch is already loaded; this one does nothing");
            return 0;
        }
    }
    dk_log("dkpatch " DK_VERSION ": Dethkarz.exe recognised, installing");
    return 1;
}

void dk_install(void)
{
    dk_players_install();
    dk_net_install();
    dk_opengl_install();
    dk_device_install();
    dk_widescreen_install();
    dk_gamepad_install();
    dk_saves_install();
    dk_lod_install();
    dk_gfxmenu_install();
    dk_interp_install();
    dk_tickrate_install();
}

void dk_set_files(const char *ini, const char *log)
{
    lstrcpynA(g_ini, ini, MAX_PATH);
    lstrcpynA(g_log, log, MAX_PATH);
}
