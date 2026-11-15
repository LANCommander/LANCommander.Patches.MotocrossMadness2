/* dinput.dll proxy: the game imports DINPUT.dll!DirectInputCreateEx (and LoadLibrary()s DINPUT.DLL to look
 * for DirectInputCreateA when it picks its DirectX level), so Windows loads this file from the game folder
 * first. DllMain applies the patches before MCM2.EXE's entry point runs.
 *
 * DirectInputCreateA / Ex go through the Interposer embedded in this DLL (InterposerDirectInputCreate*), which keeps
 * its DirectInput options (DeviceFilter, FixLegacyDeviceEnumeration) in effect and otherwise calls the real DLL in
 * the system directory; exporting them is also how another Interposer in the game recognises this DLL and leaves its
 * exports alone. Everything else is forwarded to the real DLL. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"
#include "interposer/LANCommander.Interposer.h"

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            mcm_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

#define REAL(name, type) static type real; if (!real) { real = (type)(void (*)(void))real_proc(#name); if (!real) return E_FAIL; }

typedef HRESULT (WINAPI *DirectInputCreate_fn)(HINSTANCE, DWORD, void **, void *);
typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *Void_fn)(void);

HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    return InterposerDirectInputCreateA(inst, ver, out, outer);
}

HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    REAL(DirectInputCreateW, DirectInputCreate_fn);
    return real(inst, ver, out, outer);
}

HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr = InterposerDirectInputCreateEx(inst, ver, iid, out, outer);
    if (hr == 0 && out && *out) {
        *out = mcm_gamepad_wrap_dinput(*out);
    }
    return hr;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    REAL(DllGetClassObject, DllGetClassObject_fn);
    return real(clsid, iid, out);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;   /* the patch stays loaded for the life of the process */
}

HRESULT WINAPI DllRegisterServer(void)
{
    REAL(DllRegisterServer, Void_fn);
    return real();
}

HRESULT WINAPI DllUnregisterServer(void)
{
    REAL(DllUnregisterServer, Void_fn);
    return real();
}

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
