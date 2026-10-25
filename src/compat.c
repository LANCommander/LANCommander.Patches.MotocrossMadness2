/* What the Windows XP compatibility mode used to paper over.
 *
 * Without the compatibility layer MCM2.EXE sees today's Windows and today's PC:
 *   - GetVersionExA / GetVersion report Windows 8 (6.2, the most an exe without a manifest is told). The game
 *     itself only tests for NT (and picks its DirectX level from what loads, 0x4494f0). The XP layer reported
 *     5.1 SP3; so does this module, to the exe and to BLADE.DLL, its DirectDraw layer.
 *   - GlobalMemoryStatus caps every figure at 2 GB - 1 for an exe that is not large address aware, and the
 *     game adds them up in 32 bits: the start-up check at 0x5261d4 (dwAvailPhys + dwAvailPageFile < 64 MB
 *     = "not enough memory" warning) wraps around on any machine with 4 GB or more, and the texture cache
 *     (0x4e2d99) is sized from the same figures. The XP layer included GlobalMemoryStatus2GB; this module
 *     reports at most 1 GB per figure, so sums of two still fit.
 *   - The engine sound crashes when DirectSound cannot lock its buffer (below).
 *   - The crash handler (0x460fad) writes errorlog.txt into the install folder (saves.c moves it) and calls
 *     GetUserNameA; nothing to do there.
 * The other parts of what used to need the compatibility mode are in their own modules: the registry
 * (registry.c: HKLM writes need administrator rights), a writable game folder (saves.c), the 3-D card dialog
 * (device.c) and the Indeo movies (movie.c: the crash at 0x44c468).
 * [Compat] WindowsVersion=0 / MemoryStatus=0 / SoundLock=0 turn the fixes here off. */
#include <string.h>
#include "mcm2patch.h"

#define XP_MAJOR 5
#define XP_MINOR 1
#define XP_BUILD 2600
#define MEM_CAP  0x40000000u   /* 1 GB */

static BOOL WINAPI hk_GetVersionExA(LPOSVERSIONINFOA vi)
{
    DWORD size;
    if (!vi || (vi->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) && vi->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    size = vi->dwOSVersionInfoSize;
    memset(vi, 0, size);
    vi->dwOSVersionInfoSize = size;
    vi->dwMajorVersion = XP_MAJOR;
    vi->dwMinorVersion = XP_MINOR;
    vi->dwBuildNumber = XP_BUILD;
    vi->dwPlatformId = VER_PLATFORM_WIN32_NT;
    strcpy(vi->szCSDVersion, "Service Pack 3");
    if (size == sizeof(OSVERSIONINFOEXA)) {
        LPOSVERSIONINFOEXA ex = (LPOSVERSIONINFOEXA)vi;
        ex->wServicePackMajor = 3;
        ex->wProductType = VER_NT_WORKSTATION;
        ex->wSuiteMask = VER_SUITE_SINGLEUSERTS;
    }
    return TRUE;
}

static DWORD WINAPI hk_GetVersion(void)
{
    return (DWORD)XP_BUILD << 16 | XP_MINOR << 8 | XP_MAJOR;
}

static SIZE_T cap(DWORDLONG v)
{
    return (SIZE_T)(v > MEM_CAP ? MEM_CAP : v);
}

static void WINAPI hk_GlobalMemoryStatus(LPMEMORYSTATUS ms)
{
    MEMORYSTATUSEX ex;
    ex.dwLength = sizeof ex;
    if (!GlobalMemoryStatusEx(&ex)) {
        GlobalMemoryStatus(ms);
        return;
    }
    ms->dwLength = sizeof *ms;
    ms->dwMemoryLoad = ex.dwMemoryLoad;
    ms->dwTotalPhys = cap(ex.ullTotalPhys);
    ms->dwAvailPhys = cap(ex.ullAvailPhys);
    ms->dwTotalPageFile = cap(ex.ullTotalPageFile);
    ms->dwAvailPageFile = cap(ex.ullAvailPageFile);
    ms->dwTotalVirtual = cap(ex.ullTotalVirtual);
    ms->dwAvailVirtual = cap(ex.ullAvailVirtual);
}

/* ---------------------------------------------------------------- engine sound buffer lock
 *
 * The engine sound streamer (0x4e9aa0 and two more) locks its whole DirectSound buffer through
 * CSoundBuffer::Lock (0x4c1770) and copies a buffer's worth of samples to the first pointer without looking
 * at the result. When Lock fails (a lost buffer: another program took the audio device, a device change)
 * that pointer is NULL: "Access Violation at 0x4e9e0d". The wrapper is replaced: a lost buffer is restored
 * and locked again, and if that still fails the copy goes to a scratch buffer that Unlock (0x4c17d0)
 * recognises and drops. */

#define SOUND_LOCK   0x4c1770u
#define SOUND_UNLOCK 0x4c17d0u
#define SCRATCH_SIZE (4u << 20)

typedef HRESULT (WINAPI *dsb_lock_fn)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT (WINAPI *dsb_unlock_fn)(void *, void *, DWORD, void *, DWORD);
typedef HRESULT (WINAPI *dsb_restore_fn)(void *);

static BYTE *g_scratch;
static int g_lock_failures;

static int __attribute__((thiscall)) sound_lock(char *self, DWORD offset, DWORD bytes, void **p1, DWORD *n1, void **p2,
                                                DWORD *n2, DWORD flags)
{
    void *buf = *(void **)(self + 0xc);
    void **vt;
    HRESULT hr;
    if (!buf) {
        *p1 = NULL;
        *p2 = NULL;
        return 0;
    }
    vt = *(void ***)buf;
    hr = ((dsb_lock_fn)vt[11])(buf, offset, bytes, p1, n1, p2, n2, flags);
    if (hr == (HRESULT)0x88780096) {           /* DSERR_BUFFERLOST */
        ((dsb_restore_fn)vt[20])(buf);
        hr = ((dsb_lock_fn)vt[11])(buf, offset, bytes, p1, n1, p2, n2, flags);
    }
    if (hr >= 0 && *p1) {
        return 1;
    }
    if (g_lock_failures++ < 4) {
        mcm_log("compat: sound buffer lock failed (%08lx), samples dropped", hr);
    }
    *p1 = g_scratch;
    if (n1) {
        *n1 = bytes && bytes < SCRATCH_SIZE ? bytes : SCRATCH_SIZE;
    }
    *p2 = NULL;
    if (n2) {
        *n2 = 0;
    }
    return 0;
}

static int __attribute__((thiscall)) sound_unlock(char *self, void *p1, DWORD n1, void *p2, DWORD n2)
{
    void *buf = *(void **)(self + 0xc);
    if (!buf || !p1 || p1 == g_scratch) {
        return 0;
    }
    return ((dsb_unlock_fn)(*(void ***)buf)[19])(buf, p1, n1, p2, n2) >= 0;
}

static void fix_sound_lock(void)
{
    static const uint8_t lock_start[] = { 0x8b, 0x41, 0x0c, 0x85, 0xc0, 0x75, 0x19 };
    static const uint8_t unlock_start[] = { 0x8b, 0x41, 0x0c, 0x85, 0xc0, 0x74, 0x2c };
    if (!mcm_expect(SOUND_LOCK, lock_start, sizeof lock_start) || !mcm_expect(SOUND_UNLOCK, unlock_start, sizeof unlock_start)) {
        mcm_log("compat: CSoundBuffer::Lock/Unlock do not match, left alone");
        return;
    }
    g_scratch = VirtualAlloc(NULL, SCRATCH_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_scratch && mcm_jmp(SOUND_LOCK, sound_lock) && mcm_jmp(SOUND_UNLOCK, sound_unlock)) {
        mcm_log("compat: a failed sound buffer lock no longer crashes the engine sound");
    }
}

void mcm_compat_install(void)
{
    if (mcm_ini_int("Compat", "WindowsVersion", 1)) {
        HMODULE blade = GetModuleHandleA("BLADE.DLL");
        mcm_hook_import("KERNEL32.dll", "GetVersionExA", hk_GetVersionExA);
        mcm_hook_import("KERNEL32.dll", "GetVersion", hk_GetVersion);
        if (blade) {
            mcm_hook_import_in(blade, "KERNEL32.dll", "GetVersion", hk_GetVersion);
        }
        mcm_log("compat: Windows version reported as 5.1.2600 (XP SP3)");
    }
    if (mcm_ini_int("Compat", "MemoryStatus", 1)) {
        mcm_hook_import("KERNEL32.dll", "GlobalMemoryStatus", hk_GlobalMemoryStatus);
        mcm_log("compat: GlobalMemoryStatus capped at 1 GB per figure");
    }
    if (mcm_ini_int("Compat", "SoundLock", 1)) {
        fix_sound_lock();
    }
}
