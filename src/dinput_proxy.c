/* dinput.dll proxy: the game imports DINPUT.dll!DirectInputCreateEx (and LoadLibrary()s DINPUT.DLL to look
 * for DirectInputCreateA when it picks its DirectX level), so Windows loads this file from the game folder
 * first. The real DLL is loaded from the system directory and every call forwarded; DllMain applies the
 * patches before MCM2.EXE's entry point runs. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"

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
typedef HRESULT (WINAPI *DirectInputCreateEx_fn)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *Void_fn)(void);

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    REAL(DirectInputCreateA, DirectInputCreate_fn);
    return real(inst, ver, out, outer);
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    REAL(DirectInputCreateW, DirectInputCreate_fn);
    return real(inst, ver, out, outer);
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr;
    REAL(DirectInputCreateEx, DirectInputCreateEx_fn);
    hr = real(inst, ver, iid, out, outer);
    if (hr == 0 && out && *out) {
        *out = mcm_gamepad_wrap_dinput(*out);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    REAL(DllGetClassObject, DllGetClassObject_fn);
    return real(clsid, iid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;   /* the patch stays loaded for the life of the process */
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void)
{
    REAL(DllRegisterServer, Void_fn);
    return real();
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void)
{
    REAL(DllUnregisterServer, Void_fn);
    return real();
}

/* mcm2patch.ini / mcm2patch.log next to this DLL */
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
