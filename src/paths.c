/* The install folder and the user's save folder.
 *
 * Save folder: <Saved Games>\Microsoft\Motocross Madness 2 (SHGetKnownFolderPath(FOLDERID_SavedGames), which is
 * %USERPROFILE%\Saved Games unless the user moved it). [Saves] Folder= overrides it; [Saves] Redirect=0 turns
 * the redirection off (mcm_save_dir() is then ""). The [Saves] settings come from mcm2patch.ini only: Settings.ini
 * (mcm2patch.c), which can override every other setting, lives in the folder they choose. */
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#include "mcm2patch.h"

/* FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}, defined here to avoid needing -luuid */
static const GUID MCM_FOLDERID_SavedGames = {0x4C5C32FF, 0xBB9D, 0x43B0, {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}};
typedef HRESULT (WINAPI *SHGetKnownFolderPath_fn)(const GUID *, DWORD, HANDLE, PWSTR *);

static char g_game[MAX_PATH];
static char g_save[MAX_PATH];
static int g_save_done;              /* 1 = being worked out, 2 = done */

static void add_slash(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] != '\\' && n + 1 < MAX_PATH) {
        s[n] = '\\';
        s[n + 1] = '\0';
    }
}

void mcm_make_dirs(const char *path)
{
    char tmp[MAX_PATH], *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++) {          /* skip "C:\" */
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
}

const char *mcm_game_dir(void)
{
    if (!g_game[0]) {
        char *slash;
        GetModuleFileNameA(NULL, g_game, MAX_PATH);
        slash = strrchr(g_game, '\\');
        if (slash) {
            slash[1] = '\0';
        }
    }
    return g_game;
}

static int saved_games_folder(char *out)
{
    SHGetKnownFolderPath_fn fn;
    HMODULE shell = LoadLibraryA("shell32.dll");
    PWSTR w = NULL;
    int ok = 0;
    fn = shell ? (SHGetKnownFolderPath_fn)(void (*)(void))GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
    if (fn && fn(&MCM_FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &w) == S_OK && w) {
        ok = WideCharToMultiByte(CP_ACP, 0, w, -1, out, MAX_PATH - 48, NULL, NULL) > 0;
        CoTaskMemFree(w);
    }
    if (!ok) {   /* no known folders (Wine without a prefix, a stripped down Windows): %USERPROFILE%\Saved Games */
        DWORD n = GetEnvironmentVariableA("USERPROFILE", out, MAX_PATH - 64);
        ok = n && n < MAX_PATH - 64;
        if (ok) {
            add_slash(out);
            strcat(out, "Saved Games");
        }
    }
    if (ok) {
        add_slash(out);
        strcat(out, "Microsoft\\Motocross Madness 2");
    }
    return ok;
}

static void find_save_dir(void)
{
    char folder[MAX_PATH], ovr[MAX_PATH];
    if (!mcm_ini_int("Saves", "Redirect", 1)) {
        return;
    }
    ovr[0] = '\0';
    mcm_ini_str("Saves", "Folder", ovr, sizeof ovr);
    if (ovr[0]) {
        char exp[MAX_PATH];
        DWORD n = ExpandEnvironmentStringsA(ovr, exp, sizeof exp);
        if (!n || n > sizeof exp || !GetFullPathNameA(exp, MAX_PATH - 48, folder, NULL)) {
            folder[0] = '\0';
        }
    } else if (!saved_games_folder(folder)) {
        folder[0] = '\0';
    }
    if (!folder[0]) {
        mcm_log("saves: no usable save folder, files stay in the install folder");
        return;
    }
    add_slash(folder);
    mcm_make_dirs(folder);
    if (GetFileAttributesA(folder) == INVALID_FILE_ATTRIBUTES) {
        mcm_log("saves: cannot create %s (%lu), files stay in the install folder", folder, GetLastError());
        return;
    }
    lstrcpynA(g_save, folder, MAX_PATH);
}

const char *mcm_save_dir(void)
{
    if (!g_save_done) {
        g_save_done = 1;
        find_save_dir();
        g_save_done = 2;
    }
    return g_save;
}

const char *mcm_save_dir_ready(void)
{
    return g_save_done == 1 ? NULL : mcm_save_dir();
}
