/* ASI plugin build (mcm2patch.asi), for an ASI loader such as Ultimate ASI Loader: the loader LoadLibrary()s
 * every .asi it finds (game folder, scripts\, plugins\) before MCM2.EXE runs its own code, and DllMain
 * applies the same patches as the dinput.dll proxy. Settings come from mcm2patch.ini next to the .asi, the
 * log goes to mcm2patch.log there.
 *
 * Without the proxy the game's DirectInput object is not wrapped at creation, so the gamepad module hooks
 * DINPUT.dll!DirectInputCreateEx in the exe's import table instead (gamepad.c). */
#include <windows.h>
#include <string.h>
#include "mcm2patch.h"

static void patch_init(HINSTANCE dll)
{
    char ini[MAX_PATH], log[MAX_PATH + 16], *slash;
    GetModuleFileNameA(dll, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, ini);
    strcat(ini, "mcm2patch.ini");
    strcat(log, "mcm2patch.log");
    DeleteFileA(log);
    mcm_set_files(ini, log);

    if (mcm_patch_start()) {
        mcm_install();
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
