/* dinput.dll proxy: the game imports DINPUT.dll!DirectInputCreateA, so Windows loads this
 * file from the game folder first. The real DLL is loaded from the system directory and the
 * call forwarded; DllMain applies the patches before Dethkarz.exe's entry point runs. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dkpatch.h"

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn real_DirectInputCreateA;

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            dk_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    if (!real_DirectInputCreateA) {
        real_DirectInputCreateA = (DirectInputCreateA_fn)(void (*)(void))real_proc("DirectInputCreateA");
        if (!real_DirectInputCreateA) {
            return E_FAIL;
        }
    }
    {
        HRESULT hr = real_DirectInputCreateA(inst, ver, out, outer);
        if (hr == 0 && out) {
            dk_test_wrap_dinput(*out);
        }
        return hr;
    }
}

/* dkpatch.ini / dkpatch.log next to this DLL (dkpatch_unit<N>.log with the test harness) */
static void patch_init(HINSTANCE dll)
{
    char ini[MAX_PATH], log[MAX_PATH + 32], unit[8], *slash;
    GetModuleFileNameA(dll, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, ini);
    strcat(ini, "dkpatch.ini");
    if (GetEnvironmentVariableA("DK_UNIT", unit, sizeof unit) && atoi(unit) >= 1 && atoi(unit) <= 16) {
        dk_unit = atoi(unit);
        sprintf(log + strlen(log), "dkpatch_unit%d.log", dk_unit);
    } else {
        strcat(log, "dkpatch.log");
    }
    DeleteFileA(log);
    dk_set_files(ini, log);

    if (dk_patch_start()) {
        dk_test_install();
        dk_players_install();
        dk_net_install();
        dk_widescreen_install();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        patch_init(inst);
    }
    return TRUE;
}
