/* Core of the patch: verifies the exe, reads the settings, installs the patch modules.
 * Settings and log go to dkpatch.ini / dkpatch.log next to the DLL (dinput.dll proxy build), or
 * to the LANCommander Interposer's Config.yml / session log (plugin build, see interposer_plugin.c). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

#define DK_TIMESTAMP 0x362c0cfeu   /* Dethkarz.exe (GOG), 16 Oct 1998 */

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];
static int (*g_config_int)(const char *section, const char *key, int def);
static void (*g_log_line)(const char *line);

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
    dk_log("dkpatch: Dethkarz.exe recognised, installing");
    return 1;
}

void dk_set_files(const char *ini, const char *log)
{
    lstrcpynA(g_ini, ini, MAX_PATH);
    lstrcpynA(g_log, log, MAX_PATH);
}
