/* ASI plugin build (mcm2patch.asi), for an ASI loader such as Ultimate ASI Loader: the loader LoadLibrary()s
 * every .asi it finds (game folder, scripts\, plugins\) before MCM2.EXE runs its own code, and DllMain
 * applies the same patches as the dinput.dll proxy, with the Interposer embedded the same way. Settings come
 * from mcm2patch.ini next to the .asi, the log goes to mcm2patch.log there.
 *
 * Without the proxy the game's DirectInput object is not wrapped at creation, so the gamepad module hooks
 * DINPUT.dll!DirectInputCreateEx in the exe's import table instead (gamepad.c). */
#include <windows.h>
#include "mcm2patch.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        mcm_standalone_init(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        mcm_standalone_detach(reserved != NULL);
    }
    return TRUE;
}
