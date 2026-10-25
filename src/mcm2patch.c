/* Core of the patch: verifies the exe, reads the settings, installs the patch modules.
 * Settings and log go to mcm2patch.ini / mcm2patch.log next to the DLL (dinput.dll proxy and ASI builds), or
 * to the LANCommander Interposer's Config.yml / session log (plugin build, see interposer_plugin.c). Settings.ini in
 * the save folder (same sections and keys as mcm2patch.ini) overrides either, per key. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"

#define MCM_TIMESTAMP 0x63732172u   /* MCM2.EXE 2.10 without SafeDisc (1,576,960 bytes) */
#define MCM_ENTRY     0x13d6f5u

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];
static int (*g_config_int)(const char *section, const char *key, int def);
static void (*g_log_line)(const char *line);
static int (*g_config_str)(const char *section, const char *key, char *out, int size);
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;

void mcm_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line))
{
    g_config_int = config_int;
    g_log_line = log_line;
}

void mcm_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size))
{
    g_config_str = config_str;
}

void mcm_log(const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log_line) {
        g_log_line(line);
        return;
    }
    if (!g_log_cs_ready) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
    }
    EnterCriticalSection(&g_log_cs);
    {
        FILE *f = fopen(g_log, "a");
        if (f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

int mcm_write(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(MCM_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        mcm_log("VirtualProtect %08x failed (%lu)", addr, GetLastError());
        return 0;
    }
    memcpy(MCM_ADDR(addr), bytes, len);
    VirtualProtect(MCM_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), MCM_ADDR(addr), len);
    return 1;
}

int mcm_expect(uint32_t addr, const void *bytes, size_t len)
{
    return memcmp(MCM_ADDR(addr), bytes, len) == 0;
}

int mcm_patch(uint32_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!mcm_expect(addr, expect, len)) {
        mcm_log("patch %08x: original bytes differ, skipped", addr);
        return 0;
    }
    return mcm_write(addr, bytes, len);
}

static int rel32(uint32_t from, uint8_t op, const void *to)
{
    uint8_t b[5];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    return mcm_write(from, b, 5);
}

int mcm_jmp(uint32_t from, const void *to) { return rel32(from, 0xe9, to); }
int mcm_call(uint32_t from, const void *to) { return rel32(from, 0xe8, to); }

int mcm_redirect_call(uint32_t site, uint32_t old_target, const void *to)
{
    uint8_t old[5];
    int32_t rel = (int32_t)(old_target - (site + 5));
    old[0] = 0xe8;
    memcpy(old + 1, &rel, 4);
    if (!mcm_expect(site, old, 5)) {
        mcm_log("call at %08x does not go to %08x, left alone", site, old_target);
        return 0;
    }
    return mcm_call(site, to);
}

const char *mcm_ini_file(void)
{
    if (!g_ini[0]) {   /* Interposer plugin: the game's own settings still go to mcm2patch.ini */
        snprintf(g_ini, sizeof g_ini, "%smcm2patch.ini", mcm_game_dir());
    }
    return g_ini;
}

/* Settings.ini in the save folder (the install folder without one); NULL while the save folder is being worked out */
const char *mcm_user_ini_file(void)
{
    static char path[MAX_PATH];
    const char *dir;
    if (!path[0]) {
        if (!(dir = mcm_save_dir_ready())) {
            return NULL;
        }
        snprintf(path, sizeof path, "%sSettings.ini", dir[0] ? dir : mcm_game_dir());
    }
    return path;
}

/* the value of [section] key in Settings.ini, which wins over mcm2patch.ini / the Interposer's Config.yml */
static int user_value(const char *section, const char *key, char *out, int size)
{
    const char *user = mcm_user_ini_file();
    if (!user) {
        return 0;
    }
    GetPrivateProfileStringA(section, key, "\x01", out, size, user);
    return strcmp(out, "\x01") != 0;
}

int mcm_ini_int(const char *section, const char *key, int def)
{
    char value[64], *end;
    long n;
    if (user_value(section, key, value, sizeof value)) {
        n = strtol(value, &end, 10);
        return end == value ? def : (int)n;
    }
    if (g_config_int) {
        return g_config_int(section, key, def);
    }
    return (int)GetPrivateProfileIntA(section, key, def, mcm_ini_file());
}

/* text setting; out is left empty if the key is missing. Returns 1 if a non-empty value was read. */
int mcm_ini_str(const char *section, const char *key, char *out, int size)
{
    out[0] = '\0';
    if (user_value(section, key, out, size)) {
        return out[0] != '\0';
    }
    out[0] = '\0';
    if (g_config_int) {   /* Interposer backend */
        return g_config_str ? g_config_str(section, key, out, size) : 0;
    }
    GetPrivateProfileStringA(section, key, "", out, size, mcm_ini_file());
    return out[0] != '\0';
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == MCM_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == MCM_ENTRY;
}

int mcm_patch_start(void)
{
    if (!exe_is_supported()) {
        mcm_log("mcm2patch: not the supported MCM2.EXE; no patches applied");
        return 0;
    }
    if (!mcm_ini_int("General", "Enabled", 1)) {
        mcm_log("mcm2patch: disabled in the settings");
        return 0;
    }
    {
        /* the dinput.dll proxy, the ASI and the Interposer plugin carry the same patches: only the first one loaded applies them */
        char name[48];
        snprintf(name, sizeof name, "Local\\mcm2patch_%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) {
            mcm_log("mcm2patch: another copy of the patch is already loaded; this one does nothing");
            return 0;
        }
    }
    mcm_log("mcm2patch " MCM_VERSION ": MCM2.EXE recognised, installing");
    return 1;
}

/* an empty Settings.ini that says what it is for */
static void scaffold_user_ini(void)
{
    const char *path = mcm_user_ini_file();
    FILE *f;
    if (!path || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    f = fopen(path, "w");
    if (!f) {
        mcm_log("settings: cannot create %s", path);
        return;
    }
    fputs("; Motocross Madness 2 settings of this player (mcm2patch). Any [Section] Key= of mcm2patch.ini next to\n"
          "; MCM2.EXE can be set here too and wins over that file, except [Saves]. A game setting ([Game], [Graphics])\n"
          "; that is set here is also saved here when the game changes it. For example:\n"
          ";\n"
          "; [Profile]\n"
          "; Name=Player\n"
          ";\n"
          "; [Gamepad]\n"
          "; Sensitivity=50\n"
          ";\n"
          "; [Graphics]\n"
          "; TerrainDetailTextureMethod=1\n", f);
    fclose(f);
    mcm_log("settings: created %s", path);
}

void mcm_install(void)
{
    scaffold_user_ini();
    mcm_crashlog_install();
    mcm_compat_install();
    mcm_saves_install();
    mcm_registry_install();
    mcm_profile_install();
    mcm_device_install();
    mcm_dgvoodoo_install();
    mcm_movie_install();
    mcm_d3d_install();
    mcm_widescreen_install();
    mcm_hud_install();
    mcm_lod_install();
    mcm_framerate_install();
    mcm_players_install();
    mcm_gamepad_install();
    mcm_test_install();
}

void mcm_set_files(const char *ini, const char *log)
{
    lstrcpynA(g_ini, ini, MAX_PATH);
    lstrcpynA(g_log, log, MAX_PATH);
}
