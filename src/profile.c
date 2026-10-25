/* A player profile chosen from the settings ([Profile] Name=), for launchers that know the player's name.
 *
 * The game keeps one folder per profile (ui\profile\<name>\, the save folder's Profiles\<name>\ with saves.c):
 * <name>.prf (binary; the name in a 16-byte, NUL-padded field at offset 0) and control.ctl, both started from
 * the install's ProfileTemplate\. It loads the profile named by the MRUProfile string in its registry key and
 * otherwise opens "Create New Profile" on every start.
 *
 * With a name set, the profile is created from the template if it does not exist yet (or its name field is
 * corrected when the folder was renamed), and MRUProfile is set to it before the game starts. The name may use
 * %VARIABLES%; characters a file name cannot hold become _ and it is cut to the game's 15 characters. This is
 * what LANCommander's ChangeName.ps1 did by hand. */
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"

#define NAME_MAX_LEN 15

static void sanitize(char *s)
{
    char *p;
    for (p = s; *p; p++) {
        if (strchr("<>:\"/\\|?*", *p) || (unsigned char)*p < 32) {
            *p = '_';
        }
    }
    if (strlen(s) > NAME_MAX_LEN) {
        s[NAME_MAX_LEN] = '\0';
    }
    while ((p = s + strlen(s)) > s && (p[-1] == ' ' || p[-1] == '.')) {
        p[-1] = '\0';               /* Windows drops trailing dots and spaces from folder names */
    }
}

void mcm_profile_install(void)
{
    char raw[MAX_PATH], name[MAX_PATH], dir[MAX_PATH], prf[2 * MAX_PATH], from[MAX_PATH];
    const char *save = mcm_save_dir();
    if (!mcm_ini_str("Profile", "Name", raw, sizeof raw)) {
        return;
    }
    if (!ExpandEnvironmentStringsA(raw, name, sizeof name)) {
        lstrcpynA(name, raw, sizeof name);
    }
    sanitize(name);
    if (!name[0]) {
        mcm_log("profile: [Profile] Name \"%s\" has no usable characters", raw);
        return;
    }
    if (save[0]) {
        snprintf(dir, sizeof dir, "%sProfiles\\%s\\", save, name);
    } else {
        snprintf(dir, sizeof dir, "%sui\\profile\\%s\\", mcm_game_dir(), name);
    }
    snprintf(prf, sizeof prf, "%s%s.prf", dir, name);
    if (GetFileAttributesA(prf) != INVALID_FILE_ATTRIBUTES) {
        /* a profile renamed outside the game (a launcher's name change) still has the old name inside */
        char field[16], want[16];
        DWORD done = 0;
        HANDLE h = CreateFileA(prf, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        memset(want, 0, sizeof want);
        memcpy(want, name, strlen(name));
        if (h != INVALID_HANDLE_VALUE) {
            if (ReadFile(h, field, sizeof field, &done, NULL) && done == sizeof field && memcmp(field, want, sizeof want)) {
                SetFilePointer(h, 0, NULL, FILE_BEGIN);
                WriteFile(h, want, sizeof want, &done, NULL);
                mcm_log("profile: name inside %s.prf set to \"%s\"", name, name);
            }
            CloseHandle(h);
        }
    } else {
        HANDLE h;
        char field[16];
        DWORD done;
        mcm_make_dirs(dir);
        snprintf(from, sizeof from, "%sProfileTemplate\\ProfileTemplate.prf", mcm_game_dir());
        if (!CopyFileA(from, prf, TRUE)) {
            mcm_log("profile: cannot create %s from %s (%lu)", prf, from, GetLastError());
            return;
        }
        memset(field, 0, sizeof field);
        memcpy(field, name, strlen(name));
        h = CreateFileA(prf, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, field, sizeof field, &done, NULL);
            CloseHandle(h);
        }
        snprintf(from, sizeof from, "%sProfileTemplate\\control.ctl", mcm_game_dir());
        snprintf(prf, sizeof prf, "%scontrol.ctl", dir);
        CopyFileA(from, prf, TRUE);
        mcm_log("profile: created profile \"%s\"", name);
    }
    mcm_reg_set_string("", "MRUProfile", name);
    mcm_log("profile: \"%s\" is loaded at start", name);
}
