#ifndef MCM2PATCH_H
#define MCM2PATCH_H
#include <windows.h>
#include <stdint.h>

#ifndef MCM_VERSION
#define MCM_VERSION "dev"            /* build.sh passes the release tag */
#endif

int mcm_patch_start(void);           /* exe check + Enabled + not loaded twice; 1 = go ahead and install the modules */
void mcm_install(void);              /* installs the patch modules */
void mcm_log(const char *fmt, ...);

/* where settings and log lines go: mcm2patch.ini / mcm2patch.log (proxy, ASI), or callbacks (Interposer plugin) */
void mcm_set_files(const char *ini, const char *log);
void mcm_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line));
void mcm_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size));

/* memory helpers (all addresses are MCM2.EXE VAs, image base 0x400000, no relocations) */
int mcm_write(uint32_t addr, const void *bytes, size_t len);
int mcm_expect(uint32_t addr, const void *bytes, size_t len);   /* 1 if memory matches */
int mcm_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int mcm_jmp(uint32_t from, const void *to);   /* E9 rel32 at from */
int mcm_call(uint32_t from, const void *to);  /* E8 rel32 at from */
int mcm_redirect_call(uint32_t site, uint32_t old_target, const void *to);   /* E8 rel32 at site, if it calls old_target */

/* settings: [section] key in Settings.ini in the save folder, else in mcm2patch.ini (or Plugins.MotocrossMadness2.<key>
 * in the Interposer's Config.yml) */
int mcm_ini_int(const char *section, const char *key, int def);
int mcm_ini_str(const char *section, const char *key, char *out, int size);   /* "" if missing */
const char *mcm_ini_file(void);      /* mcm2patch.ini (next to MCM2.EXE with the Interposer plugin) */
const char *mcm_user_ini_file(void); /* Settings.ini; NULL while the save folder is not known yet */

void *mcm_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */
void *mcm_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement);

/* paths.c: the install folder and the user's save folder (both with a trailing backslash) */
const char *mcm_game_dir(void);
const char *mcm_save_dir(void);      /* "" when saves are not redirected */
const char *mcm_save_dir_ready(void); /* mcm_save_dir(), or NULL from within its own setting lookups */
void mcm_make_dirs(const char *path); /* creates every missing parent folder of path (and path itself if it ends in \) */

/* registry.c: a value of the game's settings key (sub = "" or e.g. "DriverInfo\\{guid}"), INI or registry */
int mcm_reg_get_dword(const char *sub, const char *name, int def);
void mcm_reg_set_dword(const char *sub, const char *name, DWORD value);
void mcm_reg_set_string(const char *sub, const char *name, const char *value);

/* patch modules */
void mcm_compat_install(void);       /* compat.c: what the Windows XP compatibility mode used to paper over */
void mcm_registry_install(void);     /* registry.c: the game's registry settings in mcm2patch.ini / Settings.ini */
void mcm_saves_install(void);        /* saves.c: profiles, replays and logs in Saved Games */
void mcm_profile_install(void);      /* profile.c: [Profile] Name= creates and loads that profile */
void mcm_device_install(void);       /* device.c: no 3-D card dialog */
void mcm_dgvoodoo_install(void);     /* dgvoodoo.c: the dgVoodoo 2 settings the game needs */
void mcm_movie_install(void);        /* movie.c: Indeo 5 movies without a system codec, no crash without one */
void mcm_d3d_install(void);          /* d3d.c: DirectDraw / Direct3D hooks for the video modules */
void mcm_test_install(void);         /* testharness.c: scripted input and screenshots, only with MCM_TEST set */
void mcm_crashlog_install(void);     /* crashlog.c: [Debug] CrashLog=1 logs crashes with a call chain */
void mcm_widescreen_install(void);   /* widescreen.c */
void mcm_hud_install(void);          /* hud.c: the HUD and in-race dialogs scaled with the screen */
void mcm_lod_install(void);          /* lod.c: detailed trees and models further away */
void mcm_framerate_install(void);    /* framerate.c */
void mcm_players_install(void);      /* players.c: more than 8 players in network games */
void mcm_gamepad_install(void);      /* gamepad.c: SDL gamepads + rumble */

/* gamepad.c: the IDirectInput7A the game created (wrapped to add the SDL gamepads) */
void *mcm_gamepad_wrap_dinput(void *direct_input);

#define MCM_ADDR(a) ((void *)(uintptr_t)(a))
#define MCM_APP (*(char **)0x5743ac)  /* the application object (CMcmApp, "PCL" vtable at 0x55b0e0) */
#endif
