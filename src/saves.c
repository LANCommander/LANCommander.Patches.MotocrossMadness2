/* Profiles, replays and logs in the user's Saved Games folder instead of the install folder.
 *
 * Folder: <Saved Games>\Microsoft\Motocross Madness 2 (paths.c). The game names everything relative to the
 * install folder (its working directory); these are moved:
 *     ui\profile\<name>\<name>.prf   -> Profiles\<name>\<name>.prf   player profile (career, bike setup, taunts)
 *     ui\profile\<name>\control.ctl  -> Profiles\<name>\control.ctl  controls (an INI file)
 *     Record\*.vcr, Record\*.gho     -> Record\...                   saved replays and ghosts
 *     VCRtape.dat, VCRghost.dat,     -> ...                          the replay / ghost being recorded
 *     VCRgtemp.dat
 *     errorlog.txt, scnmgr.log       -> ...                          crash report, scene manager log
 * plus the game's start-up check that its folder is writable (GetTempFileNameA(".", "tst") at 0x4a1f8a,
 * "cannot write to the hard disk" otherwise), which now tests the save folder. ProfileTemplate\ (the new
 * profile template) and the shipped files stay where they are.
 *
 * Existing profiles and records are copied over on the first start (when Profiles\ does not exist yet).
 * Reading a file that only exists in the install folder falls back to it.
 *
 * Hooks: the exe's KERNEL32 imports for files, folders, find and the private profile (INI) functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"

static const char *g_dir;           /* save folder, with trailing backslash */
static char g_cwd[MAX_PATH];        /* working directory at startup, with trailing backslash */
static int g_on;

static const struct { const char *game; const char *saved; int folder; } MAP[] = {
    {"ui\\profile", "Profiles", 1},
    {"record", "Record", 1},
    {"vcrtape.dat", "VCRtape.dat", 0},
    {"vcrghost.dat", "VCRghost.dat", 0},
    {"vcrgtemp.dat", "VCRgtemp.dat", 0},
    {"errorlog.txt", "errorlog.txt", 0},
    {"scnmgr.log", "scnmgr.log", 0},
};

/* ---------------------------------------------------------------- path mapping */

static int has_prefix_i(const char *s, const char *prefix)
{
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

/* game path -> place in the save folder (dest) and in the install (orig). 0 if the path is not one we move. */
static int map_path(const char *path, char *dest, char *orig)
{
    char full[MAX_PATH];
    const char *rel, *game = mcm_game_dir();
    size_t i;
    if (!g_on || !path || !path[0] || (path[0] == '\\' && path[1] == '\\')) {
        return 0;
    }
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL)) {
        return 0;
    }
    if (has_prefix_i(full, game)) {
        rel = full + strlen(game);
    } else if (has_prefix_i(full, g_cwd)) {
        rel = full + strlen(g_cwd);
    } else {
        return 0;
    }
    for (i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        size_t n = strlen(MAP[i].game);
        if (_strnicmp(rel, MAP[i].game, n) || (rel[n] && !(MAP[i].folder && rel[n] == '\\'))) {
            continue;
        }
        snprintf(dest, MAX_PATH, "%s%s%s", g_dir, MAP[i].saved, rel + n);
        snprintf(orig, MAX_PATH, "%s%s", game, rel);
        return 1;
    }
    return 0;
}

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* a path to read: the save folder's copy, or the install's if only that one exists */
static const char *read_path(const char *path, char *dest, char *orig)
{
    if (!map_path(path, dest, orig)) {
        return path;
    }
    return exists(dest) || !exists(orig) ? dest : orig;
}

/* a path to write: the save folder's, with its folders created */
static const char *write_path(const char *path, char *dest, char *orig)
{
    if (!map_path(path, dest, orig)) {
        return path;
    }
    mcm_make_dirs(dest);
    return dest;
}

/* ---------------------------------------------------------------- hooks */

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static BOOL (WINAPI *real_CreateDirectoryA)(LPCSTR, LPSECURITY_ATTRIBUTES);
static BOOL (WINAPI *real_RemoveDirectoryA)(LPCSTR);
static BOOL (WINAPI *real_DeleteFileA)(LPCSTR);
static BOOL (WINAPI *real_CopyFileA)(LPCSTR, LPCSTR, BOOL);
static DWORD (WINAPI *real_GetPrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
static UINT (WINAPI *real_GetPrivateProfileIntA)(LPCSTR, LPCSTR, INT, LPCSTR);
static BOOL (WINAPI *real_WritePrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
static UINT (WINAPI *real_GetTempFileNameA)(LPCSTR, LPCSTR, UINT, LPSTR);

static HANDLE WINAPI hk_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    int write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0
                || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING || disp == OPEN_ALWAYS;
    const char *p = path;
    if (map_path(path, dest, orig)) {
        if (write) {
            mcm_make_dirs(dest);
            /* an update of a file only the install has (OPEN_EXISTING / OPEN_ALWAYS): bring it over first */
            if (!exists(dest) && disp != CREATE_ALWAYS && disp != CREATE_NEW && disp != TRUNCATE_EXISTING
                && real_CopyFileA(orig, dest, TRUE)) {
                mcm_log("saves: copied %s over", orig + strlen(mcm_game_dir()));
            }
            p = dest;
        } else {
            p = exists(dest) || !exists(orig) ? dest : orig;
        }
    } else if (write && path && path[0] != '\\' && !has_prefix_i(path, g_dir)) {
        mcm_log("saves: write open of %s was not redirected", path);
    }
    return real_CreateFileA(p, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    /* listings (profiles, replays) come from the save folder only: the first start copied the old ones over */
    return real_FindFirstFileA(map_path(pattern, dest, orig) ? dest : pattern, fd);
}

static BOOL WINAPI hk_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    if (map_path(path, dest, orig)) {
        mcm_make_dirs(dest);
        return real_CreateDirectoryA(dest, sa);
    }
    return real_CreateDirectoryA(path, sa);
}

static BOOL WINAPI hk_RemoveDirectoryA(LPCSTR path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_RemoveDirectoryA(map_path(path, dest, orig) ? dest : path);
}

static BOOL WINAPI hk_DeleteFileA(LPCSTR path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_DeleteFileA(map_path(path, dest, orig) ? dest : path);
}

static BOOL WINAPI hk_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    char d1[MAX_PATH], o1[MAX_PATH], d2[MAX_PATH], o2[MAX_PATH];
    return real_CopyFileA(read_path(from, d1, o1), write_path(to, d2, o2), fail_if_exists);
}

static DWORD WINAPI hk_GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_GetPrivateProfileStringA(sec, key, def, out, size, read_path(file, dest, orig));
}

static UINT WINAPI hk_GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT def, LPCSTR file)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_GetPrivateProfileIntA(sec, key, def, read_path(file, dest, orig));
}

static BOOL WINAPI hk_WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR value, LPCSTR file)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    if (map_path(file, dest, orig)) {
        mcm_make_dirs(dest);
        if (!exists(dest)) {
            real_CopyFileA(orig, dest, TRUE);
        }
        file = dest;
    }
    return real_WritePrivateProfileStringA(sec, key, value, file);
}

/* the start-up "is the game folder writable" test, and any other temporary file in the game folder */
static UINT WINAPI hk_GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out)
{
    char full[MAX_PATH];
    if (g_on && dir && GetFullPathNameA(dir, MAX_PATH, full, NULL)) {
        size_t n = strlen(full);
        if (n && full[n - 1] != '\\') {
            full[n] = '\\';
            full[n + 1] = '\0';
        }
        if (!_stricmp(full, mcm_game_dir()) || !_stricmp(full, g_cwd)) {
            dir = g_dir;
        }
    }
    return real_GetTempFileNameA(dir, prefix, unique, out);
}

/* ---------------------------------------------------------------- migration */

static int copy_tree(const char *from, const char *to)
{
    char pat[MAX_PATH], a[2 * MAX_PATH], b[2 * MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CreateDirectoryA(to, NULL);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || !_stricmp(fd.cFileName, "EMPTYFIL")) {
            continue;
        }
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += copy_tree(a, b);
        } else if (CopyFileA(a, b, TRUE)) {
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void migrate(void)
{
    char from[MAX_PATH], to[MAX_PATH];
    int n;
    snprintf(to, sizeof to, "%sProfiles", g_dir);
    if (exists(to)) {
        return;
    }
    snprintf(from, sizeof from, "%sui\\profile", mcm_game_dir());
    n = copy_tree(from, to);
    CreateDirectoryA(to, NULL);
    snprintf(from, sizeof from, "%sRecord", mcm_game_dir());
    snprintf(to, sizeof to, "%sRecord", g_dir);
    n += copy_tree(from, to);
    CreateDirectoryA(to, NULL);
    mcm_log("saves: first start, copied %d files (profiles, records) from the install folder", n);
}

/* ---------------------------------------------------------------- install */

#define HOOK(name) real_##name = mcm_hook_import("KERNEL32.dll", #name, hk_##name)

void mcm_saves_install(void)
{
    size_t n;
    g_dir = mcm_save_dir();
    if (!g_dir[0]) {
        mcm_log("saves: profiles and records stay in the install folder");
        return;
    }
    GetCurrentDirectoryA(MAX_PATH - 1, g_cwd);
    n = strlen(g_cwd);
    if (n && g_cwd[n - 1] != '\\') {
        strcat(g_cwd, "\\");
    }
    HOOK(CreateFileA);
    HOOK(FindFirstFileA);
    HOOK(CreateDirectoryA);
    HOOK(RemoveDirectoryA);
    HOOK(DeleteFileA);
    HOOK(CopyFileA);
    HOOK(GetPrivateProfileStringA);
    HOOK(GetPrivateProfileIntA);
    HOOK(WritePrivateProfileStringA);
    HOOK(GetTempFileNameA);
    if (!real_CreateFileA || !real_FindFirstFileA || !real_CopyFileA) {
        mcm_log("saves: file imports missing, redirection is off");
        return;
    }
    if (!real_CreateDirectoryA) real_CreateDirectoryA = CreateDirectoryA;
    if (!real_RemoveDirectoryA) real_RemoveDirectoryA = RemoveDirectoryA;
    if (!real_DeleteFileA) real_DeleteFileA = DeleteFileA;
    if (!real_GetPrivateProfileStringA) real_GetPrivateProfileStringA = GetPrivateProfileStringA;
    if (!real_GetPrivateProfileIntA) real_GetPrivateProfileIntA = GetPrivateProfileIntA;
    if (!real_WritePrivateProfileStringA) real_WritePrivateProfileStringA = WritePrivateProfileStringA;
    if (!real_GetTempFileNameA) real_GetTempFileNameA = GetTempFileNameA;
    g_on = 1;
    migrate();
    mcm_log("saves: folder %s", g_dir);
}
