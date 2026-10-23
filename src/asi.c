/* ASI plugin build (dkpatch.asi), for an ASI loader such as Ultimate ASI Loader: the loader LoadLibrary()s
 * every .asi it finds (game folder, scripts\, plugins\) before Dethkarz.exe runs its own code, and DllMain
 * applies the same patches as the dinput.dll proxy. Settings come from dkpatch.ini next to the .asi, the log
 * goes to dkpatch.log there. No test harness in this build. */
#include <windows.h>
#include <string.h>
#include "dkpatch.h"

static void patch_init(HINSTANCE dll)
{
    char ini[MAX_PATH], log[MAX_PATH + 16], *slash;
    GetModuleFileNameA(dll, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, ini);
    strcat(ini, "dkpatch.ini");
    strcat(log, "dkpatch.log");
    DeleteFileA(log);
    dk_set_files(ini, log);

    if (dk_patch_start()) {
        dk_install();
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
