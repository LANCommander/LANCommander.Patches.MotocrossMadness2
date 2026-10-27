/* The game's registry settings in mcm2patch.ini instead of the registry.
 *
 * MCM2.EXE keeps everything under HKLM\Software\Microsoft\Microsoft Games\Motocross Madness 2: install facts
 * (CDPath, HardDriveRootPath, InstallType, a product id PID, ...), its settings (MRUProfile, UseVideoCardIdx,
 * UseControllerId, ...) and, per DirectDraw device, a "DriverInfo\{guid}" key with the card profile it measures
 * when there is none (Modes, NumberOfModes, TotalVideoMemory, ProfiledCard, ...) next to a few quality switches
 * (TerrainDetailTextureMethod, Use8BitTextures, RenderFog, AllowTriLinear, ...). It also looks for
 * HKLM\SOFTWARE\Rainbow Studios\TestKey (a developer switch, XOR-hidden at 0x5713b4).
 *
 * Writing HKLM needs administrator rights; without them Windows silently moved the writes to the user's
 * VirtualStore. All ADVAPI32 registry imports of the exe are replaced here by a small in-memory registry. Only
 * the settings outlive the session, as plain INI keys:
 *
 *     [Game]                  values of the game's key
 *     MRUProfile=Tester
 *     UseVideoCardIdx=0
 *
 *     [Graphics]              the quality switches of DriverInfo\{guid}, for whichever card is used
 *     TerrainDetailTextureMethod=1
 *
 * They are read from mcm2patch.ini, then from Settings.ini in the save folder (whose keys win), and a change is
 * written back to the file the key came from (mcm2patch.ini for a new key). Install facts are made up at every
 * start (CDPath and HardDriveRootPath are the folder the exe runs from), so nothing ties the settings to one
 * install folder. The card profile goes to VideoCards.bin in the save folder, tagged with the machine set-up it was
 * measured on; without a matching one the cards are measured again, without the game's "your 3-D card will now
 * be tested" prompt. Nothing is taken over from the registry. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"

#define GAME_ROOT    "HKLM\\Software\\Microsoft\\Microsoft Games\\"
#define GAME_KEY     GAME_ROOT "Motocross Madness 2"
#define DRIVER_KEY   GAME_KEY "\\DriverInfo\\"
#define HANDLE_BASE  0x4d430000u   /* fake HKEYs: 'MC' << 16 | slot */
#define MAX_HANDLES  256

#define PROFILE_CARDS 0x4c5460u    /* thiscall on the app: loads each card's DriverInfo profile, measures the cards if one is missing */
#define PROFILE_SITE  0x4c471du    /* its call at start-up */
#define PROFILE_ASK   0x4c4699u    /* no profile yet: "the 3-D card will now be tested" OK / Cancel box, OK goes on at 0x4c46fb */

typedef struct {
    char name[128];
    DWORD type;
    BYTE *data;
    DWORD size;
} value_t;

typedef struct {
    char path[MAX_PATH];
    value_t *vals;
    int nvals, cap;
} rkey_t;

static rkey_t *g_keys;
static int g_nkeys, g_capkeys;
static rkey_t g_graphics;          /* [Graphics]: answered for every DriverInfo\{guid} key */
static char g_handles[MAX_HANDLES][MAX_PATH];
static int g_used[MAX_HANDLES];
static CRITICAL_SECTION g_cs;
static int g_log_calls;
static int g_profiling;
static char g_cards_file[MAX_PATH]; /* VideoCards.bin */
static int g_cards_loaded, g_cards_dirty;

static void load_cards(void);
static void save_cards(void);

/* values of the game's key that describe the install, not a choice: made up at start, never stored */
static const char *const g_install_values[] = {
    "CDPath", "HardDriveRootPath", "InstallType", "InstalledGroup", "VersionType", "IsAdmin", "LangID", "PID",
    "LastControllerId", "FIRSTRUN", "Launched", NULL
};

/* values of a DriverInfo\{guid} key the game measures on the card: kept in VideoCards.bin */
static const char *const g_card_values[] = {
    "DeviceIdentifier", "ProfiledCard", "ProfileVersion", "Modes", "NumberOfModes", "TotalVideoMemory", "IsAGP",
    "PartialTextureBlt", "CanRenderDualTextureInSinglePass", "DisabledWindowed", "DisabledFullScreen",
    "DisabledSoftware", "DisabledHardware", NULL
};

/* text values that can look like numbers */
static const char *const g_string_values[] = {"MRUProfile", "UseControllerId", NULL};

enum { MEMORY, GAME, GRAPHICS };
static const char *const g_sections[] = {NULL, "Game", "Graphics"};

static int in_list(const char *const *list, const char *name)
{
    for (; *list; list++) {
        if (!_stricmp(*list, name)) {
            return 1;
        }
    }
    return 0;
}

/* DriverInfo\{guid} */
static int is_card_key(const char *path)
{
    size_t n = strlen(DRIVER_KEY);
    return !_strnicmp(path, DRIVER_KEY, n) && path[n] && !strchr(path + n, '\\');
}

/* where a value of key path is kept: MEMORY (card values: also VideoCards.bin), GAME or GRAPHICS */
static int place(const char *path, const char *name)
{
    if (!_stricmp(path, GAME_KEY)) {
        return in_list(g_install_values, name) ? MEMORY : GAME;
    }
    if (is_card_key(path)) {
        return in_list(g_card_values, name) ? MEMORY : GRAPHICS;
    }
    return MEMORY;
}

/* ---------------------------------------------------------------- store */

static rkey_t *find_key(const char *path)
{
    int i;
    for (i = 0; i < g_nkeys; i++) {
        if (!_stricmp(g_keys[i].path, path)) {
            return &g_keys[i];
        }
    }
    return NULL;
}

/* a key exists if it was created, or is the parent of one that was */
static int key_exists(const char *path)
{
    size_t n = strlen(path);
    int i;
    if (!strchr(path, '\\')) {
        return 1;                       /* HKLM, HKCU, ... */
    }
    for (i = 0; i < g_nkeys; i++) {
        if (!_strnicmp(g_keys[i].path, path, n) && (g_keys[i].path[n] == '\0' || g_keys[i].path[n] == '\\')) {
            return 1;
        }
    }
    return 0;
}

static rkey_t *create_key(const char *path, int *created)
{
    rkey_t *k = find_key(path);
    if (created) {
        *created = !key_exists(path);
    }
    if (k) {
        return k;
    }
    if (g_nkeys == g_capkeys) {
        g_capkeys = g_capkeys ? g_capkeys * 2 : 32;
        g_keys = realloc(g_keys, g_capkeys * sizeof *g_keys);
    }
    k = &g_keys[g_nkeys++];
    memset(k, 0, sizeof *k);
    lstrcpynA(k->path, path, MAX_PATH);
    return k;
}

static value_t *find_value(rkey_t *k, const char *name)
{
    int i;
    for (i = 0; k && i < k->nvals; i++) {
        if (!_stricmp(k->vals[i].name, name)) {
            return &k->vals[i];
        }
    }
    return NULL;
}

static value_t *set_value(rkey_t *k, const char *name, DWORD type, const void *data, DWORD size)
{
    value_t *v = find_value(k, name);
    if (!v) {
        if (k->nvals == k->cap) {
            k->cap = k->cap ? k->cap * 2 : 16;
            k->vals = realloc(k->vals, k->cap * sizeof *k->vals);
        }
        v = &k->vals[k->nvals++];
        memset(v, 0, sizeof *v);
        lstrcpynA(v->name, name, sizeof v->name);
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    if (size) {
        memcpy(v->data, data, size);
    }
    return v;
}

static void set_string(rkey_t *k, const char *name, const char *s)
{
    set_value(k, name, REG_SZ, s, (DWORD)strlen(s) + 1);
}

static void set_dword(rkey_t *k, const char *name, DWORD d)
{
    set_value(k, name, REG_DWORD, &d, 4);
}

static void free_values(rkey_t *k)
{
    int j;
    for (j = 0; j < k->nvals; j++) {
        free(k->vals[j].data);
    }
    free(k->vals);
    k->vals = NULL;
    k->nvals = k->cap = 0;
}

static void delete_tree(const char *path)
{
    size_t n = strlen(path);
    int i;
    for (i = g_nkeys - 1; i >= 0; i--) {
        if (!_strnicmp(g_keys[i].path, path, n) && (g_keys[i].path[n] == '\0' || g_keys[i].path[n] == '\\')) {
            free_values(&g_keys[i]);
            g_keys[i] = g_keys[--g_nkeys];
        }
    }
}

/* ---------------------------------------------------------------- INI */

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "hex:0102..." or "hex(<type>):..." (what a value that is neither a number nor one line of text is written as) */
static void set_hex(rkey_t *k, const char *name, const char *text)
{
    DWORD type = REG_BINARY, size = 0;
    const char *p = text + 3;
    BYTE *data;
    if (*p == '(') {
        char *end;
        type = strtoul(p + 1, &end, 16);
        p = strchr(end, ')');
        p = p ? p + 1 : text + 3;
    }
    if (*p == ':') {
        p++;
    }
    data = malloc(strlen(p) / 2 + 1);
    while (hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
        data[size++] = (BYTE)(hexval(p[0]) << 4 | hexval(p[1]));
        p += 2;
        while (*p == ',' || *p == ' ') {
            p++;
        }
    }
    set_value(k, name, type, data, size);
    free(data);
}

/* an INI value: a number is a DWORD, anything else text */
static void set_from_text(rkey_t *k, const char *name, const char *text)
{
    char *end = NULL;
    unsigned long n = 0;
    if (!_strnicmp(text, "hex:", 4) || !_strnicmp(text, "hex(", 4)) {
        set_hex(k, name, text);
        return;
    }
    if (*text && !in_list(g_string_values, name)) {
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            n = strtoul(text + 2, &end, 16);
        } else if (text[0] == '-') {
            n = (unsigned long)strtol(text, &end, 10);
        } else {
            n = strtoul(text, &end, 10);
        }
    }
    if (end && end != text && *end == '\0') {
        set_dword(k, name, (DWORD)n);
    } else {
        set_string(k, name, text);
    }
}

static void value_text(const value_t *v, char *out, size_t size)
{
    DWORD b;
    size_t n;
    if (v->type == REG_DWORD && v->size == 4) {
        snprintf(out, size, "%ld", (long)*(DWORD *)v->data);
        return;
    }
    if (v->type == REG_SZ && v->size && v->data[v->size - 1] == '\0' && strlen((char *)v->data) == v->size - 1
        && !strpbrk((char *)v->data, "\r\n") && strncmp((char *)v->data, "hex", 3)) {
        lstrcpynA(out, (char *)v->data, (int)size);
        return;
    }
    if (v->type == REG_BINARY) {
        snprintf(out, size, "hex:");
    } else {
        snprintf(out, size, "hex(%lx):", v->type);
    }
    for (b = 0, n = strlen(out); b < v->size && n + 3 <= size; b++, n += 2) {
        snprintf(out + n, 3, "%02x", v->data[b]);
    }
}

static void trim(char *s)
{
    char *p = s;
    size_t n;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {   /* like GetPrivateProfileString */
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

/* every key=value of [section] in file into k */
static int load_section(const char *file, const char *section, rkey_t *k)
{
    static char buf[32768];
    char *p;
    int count = 0;
    if (!file || !GetPrivateProfileSectionA(section, buf, sizeof buf, file)) {
        return 0;
    }
    for (p = buf; *p; p += strlen(p) + 1) {
        char line[1024], *eq;
        lstrcpynA(line, p, sizeof line);
        eq = strchr(line, '=');
        if (line[0] == ';' || line[0] == '#' || !eq) {
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        if (line[0]) {
            set_from_text(k, line, eq + 1);
            count++;
        }
    }
    return count;
}

static int ini_has(const char *file, const char *section, const char *name, char *out, int size)
{
    if (!file) {
        return 0;
    }
    GetPrivateProfileStringA(section, name, "\x01", out, size, file);
    return strcmp(out, "\x01") != 0;
}

/* writes a setting back: to Settings.ini if it came from there, else to mcm2patch.ini */
static void persist(int where, const value_t *v)
{
    static char text[4096], old[4096];
    const char *section = g_sections[where], *file = mcm_ini_file(), *user = mcm_user_ini_file();
    if (where == MEMORY || !v) {
        return;
    }
    value_text(v, text, sizeof text);
    if (ini_has(user, section, v->name, old, sizeof old)) {
        file = user;
    } else if (!ini_has(file, section, v->name, old, sizeof old)) {
        old[0] = '\x01';
        old[1] = '\0';
    }
    if (strcmp(old, text) && !WritePrivateProfileStringA(section, v->name, text, file)) {
        mcm_log("registry: cannot write [%s] %s to %s (%lu)", section, v->name, file, GetLastError());
    }
}

/* a value the game (or another module) sets */
static void store(const char *path, const char *name, DWORD type, const void *data, DWORD size)
{
    static DWORD probed;
    rkey_t *k = create_key(path, NULL);
    value_t *v = find_value(k, name);
    int probe, where = place(path, name);
    if (where == MEMORY && is_card_key(path) && (!v || v->type != type || v->size != size || memcmp(v->data, data, size))) {
        g_cards_dirty = 1;              /* saved now, or when the measuring is done */
    }
    v = set_value(k, name, type, data, size);
    if (!g_profiling) {
        save_cards();
    }
    /* the first race start tests dual texturing (0x50ba16) and, if the card can, sets TerrainDetailTextureMethod
     * to 1 right after storing the result: a default too */
    probe = probed && GetTickCount() - probed < 1000;
    probed = !_stricmp(name, "CanRenderDualTextureInSinglePass") ? GetTickCount() | 1 : 0;
    switch (where) {
    case GAME:
        persist(GAME, v);
        break;
    case GRAPHICS:
        /* while measuring the cards the game writes its defaults (Use8BitTextures, TextureCacheLimit): stored only
         * where there is no [Graphics] value yet */
        if ((!g_profiling && !probe) || !find_value(&g_graphics, name)) {
            persist(GRAPHICS, set_value(&g_graphics, name, type, data, size));
        }
        break;
    }
}

static value_t *lookup(const char *path, const char *name)
{
    value_t *v = place(path, name) == GRAPHICS ? find_value(&g_graphics, name) : NULL;
    return v ? v : find_value(find_key(path), name);
}

static void load_settings(void)
{
    rkey_t *k = create_key(GAME_KEY, NULL);
    const char *files[2];
    int i;
    files[0] = mcm_ini_file();
    files[1] = mcm_user_ini_file();
    for (i = 0; i < 2; i++) {
        load_section(files[i], "Game", k);
        load_section(files[i], "Graphics", &g_graphics);
    }
}

/* ---------------------------------------------------------------- card profiles */

/* VideoCards.bin in the save folder keeps the DriverInfo\{guid} values the game measured on the cards, so it does
 * not measure them at every start (about 2 s). The file is tagged with what they were measured on: the primary
 * display adapter, the desktop mode, and the dgVoodoo files next to the exe. On another machine or set-up the
 * file is left unused and the cards are measured again. Layout (little endian):
 *
 *     u32 magic 'MCCV', u32 version, u64 tag, u32 keys,
 *     per key:   u16 length, name ("{guid}"), u32 values,
 *     per value: u8 length, name, u32 type, u32 size, data */
#define CARDS_MAGIC   0x5643434du
#define CARDS_VERSION 2u      /* 2: measured with the video memory capped (d3d.c), 1 lost the wide modes on big cards */

static unsigned long long g_cards_tag;

static unsigned long long fnv(unsigned long long h, const void *data, size_t n)
{
    const BYTE *p = data;
    while (n--) {
        h = (h ^ *p++) * 0x100000001b3ull;
    }
    return h;
}

static unsigned long long tag_file(unsigned long long h, const char *name, int contents)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    snprintf(path, sizeof path, "%s%s", mcm_game_dir(), name);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
        return fnv(h, "-", 1);
    }
    if (contents) {     /* dgVoodoo.conf: dgvoodoo.c rewrites it at every start, mostly with the same text */
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        BYTE buf[4096];
        DWORD got;
        if (f != INVALID_HANDLE_VALUE) {
            while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) {
                h = fnv(h, buf, got);
            }
            CloseHandle(f);
        }
        return h;
    }
    h = fnv(h, &a.nFileSizeLow, sizeof a.nFileSizeLow);
    return fnv(h, &a.ftLastWriteTime, sizeof a.ftLastWriteTime);
}

static unsigned long long cards_tag(void)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    DISPLAY_DEVICEA dd;
    DEVMODEA dm;
    DWORD i;
    for (i = 0; dd.cb = sizeof dd, EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            h = fnv(h, dd.DeviceString, strlen(dd.DeviceString));
            h = fnv(h, dd.DeviceID, strlen(dd.DeviceID));
            break;
        }
    }
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        h = fnv(h, &dm.dmPelsWidth, sizeof dm.dmPelsWidth);
        h = fnv(h, &dm.dmPelsHeight, sizeof dm.dmPelsHeight);
        h = fnv(h, &dm.dmBitsPerPel, sizeof dm.dmBitsPerPel);
    }
    h = tag_file(h, "DDraw.dll", 0);
    h = tag_file(h, "D3DImm.dll", 0);
    return tag_file(h, "dgVoodoo.conf", 1);
}

typedef struct {
    BYTE *p, *end;
} reader_t;

static int take(reader_t *r, void *out, DWORD n)
{
    if ((DWORD)(r->end - r->p) < n) {
        return 0;
    }
    memcpy(out, r->p, n);
    r->p += n;
    return 1;
}

/* at the game's first registry call (every module, dgvoodoo.c included, is installed by then) */
static void load_cards(void)
{
    HANDLE f;
    DWORD size, got = 0, head[2], keys = 0, i;
    unsigned long long tag = 0;
    BYTE *buf;
    reader_t r;
    int ok = 1, count = 0;
    g_cards_loaded = 1;
    g_cards_tag = cards_tag();
    f = CreateFileA(g_cards_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        mcm_log("registry: no %s yet, the 3-D cards are measured", g_cards_file);
        return;
    }
    size = GetFileSize(f, NULL);
    buf = malloc(size != INVALID_FILE_SIZE && size ? size : 1);
    if (size != INVALID_FILE_SIZE) {
        ReadFile(f, buf, size, &got, NULL);
    }
    CloseHandle(f);
    r.p = buf;
    r.end = buf + got;
    if (!take(&r, head, 8) || !take(&r, &tag, 8) || !take(&r, &keys, 4) || head[0] != CARDS_MAGIC
        || head[1] != CARDS_VERSION || tag != g_cards_tag) {
        mcm_log("registry: %s is from another machine or set-up, the 3-D cards are measured again", g_cards_file);
        free(buf);
        return;
    }
    for (i = 0; ok && i < keys; i++) {
        WORD klen = 0;
        DWORD vals = 0, j;
        char name[64], path[MAX_PATH];
        rkey_t *k;
        ok = take(&r, &klen, 2) && klen < sizeof name && take(&r, name, klen) && take(&r, &vals, 4);
        if (!ok) {
            break;
        }
        name[klen] = '\0';
        snprintf(path, sizeof path, DRIVER_KEY "%s", name);
        k = create_key(path, NULL);
        for (j = 0; ok && j < vals; j++) {
            BYTE vlen = 0;
            char vname[256];
            DWORD type = 0, vsize = 0;
            ok = take(&r, &vlen, 1) && take(&r, vname, vlen) && take(&r, &type, 4) && take(&r, &vsize, 4)
                 && vsize <= (DWORD)(r.end - r.p);
            if (ok) {
                vname[vlen] = '\0';
                if (place(path, vname) == MEMORY) {
                    set_value(k, vname, type, r.p, vsize);
                    count++;
                }
                r.p += vsize;
            }
        }
    }
    free(buf);
    if (!ok) {
        mcm_log("registry: %s is damaged, the 3-D cards are measured again", g_cards_file);
        delete_tree(GAME_KEY "\\DriverInfo");
        return;
    }
    mcm_log("registry: %d 3-D card values from %s", count, g_cards_file);
}

static void put(BYTE **buf, DWORD *len, DWORD *cap, const void *data, DWORD n)
{
    if (*len + n > *cap) {
        *cap = (*len + n) * 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, data, n);
    *len += n;
}

static void save_cards(void)
{
    BYTE *buf = NULL;
    DWORD len = 0, cap = 0, keys = 0, done = 0;
    DWORD head[2] = {CARDS_MAGIC, CARDS_VERSION};
    char tmp[MAX_PATH + 8];
    HANDLE f;
    int i, j, ok;
    if (!g_cards_dirty || !g_cards_file[0]) {
        return;
    }
    g_cards_dirty = 0;
    put(&buf, &len, &cap, head, 8);
    put(&buf, &len, &cap, &g_cards_tag, 8);
    put(&buf, &len, &cap, &keys, 4);
    for (i = 0; i < g_nkeys; i++) {
        rkey_t *k = &g_keys[i];
        const char *name = k->path + strlen(DRIVER_KEY);
        WORD klen;
        DWORD vals = 0, at;
        if (!is_card_key(k->path) || strlen(name) >= 64) {
            continue;
        }
        klen = (WORD)strlen(name);
        put(&buf, &len, &cap, &klen, 2);
        put(&buf, &len, &cap, name, klen);
        at = len;
        put(&buf, &len, &cap, &vals, 4);
        for (j = 0; j < k->nvals; j++) {
            value_t *v = &k->vals[j];
            BYTE vlen = (BYTE)strlen(v->name);
            if (place(k->path, v->name) != MEMORY) {
                continue;
            }
            put(&buf, &len, &cap, &vlen, 1);
            put(&buf, &len, &cap, v->name, vlen);
            put(&buf, &len, &cap, &v->type, 4);
            put(&buf, &len, &cap, &v->size, 4);
            put(&buf, &len, &cap, v->data, v->size);
            vals++;
        }
        memcpy(buf + at, &vals, 4);
        keys++;
    }
    memcpy(buf + 16, &keys, 4);
    snprintf(tmp, sizeof tmp, "%s.new", g_cards_file);
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, len, &done, NULL) && done == len;
    if (f != INVALID_HANDLE_VALUE) {
        ok = CloseHandle(f) && ok;
    }
    if (!ok || !MoveFileExA(tmp, g_cards_file, MOVEFILE_REPLACE_EXISTING)) {
        mcm_log("registry: cannot write %s (%lu)", g_cards_file, GetLastError());
    }
    free(buf);
}

static void make_guid(char *out)
{
    GUID g;
    HMODULE ole = LoadLibraryA("ole32.dll");
    HRESULT (WINAPI *create)(GUID *) = ole ? (HRESULT (WINAPI *)(GUID *))(void (*)(void))GetProcAddress(ole, "CoCreateGuid") : NULL;
    if (!create || create(&g) != S_OK) {
        DWORD t = GetTickCount() ^ GetCurrentProcessId();
        memset(&g, 0, sizeof g);
        g.Data1 = t;
        g.Data2 = (WORD)(t >> 7);
    }
    sprintf(out, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
            g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/* the install facts Setup wrote (a [Game] value wins, except for the paths and IsAdmin) */
static void install_values(void)
{
    rkey_t *k = create_key(GAME_KEY, NULL);
    char dir[MAX_PATH];
    size_t n;
    value_t *v;
    static const struct { const char *name; DWORD value; } dwords[] = {
        {"InstalledGroup", 3}, {"LangID", 9}, {"AllowFreezeCamera", 1},
    };
    static const struct { const char *name; const char *value; } strings[] = {
        {"InstallType", "Full"}, {"VersionType", "RetailVersion"},
    };
    size_t i;

    /* where the game is now, not where it was installed: without a trailing backslash, like Setup wrote it */
    lstrcpynA(dir, mcm_game_dir(), MAX_PATH);
    n = strlen(dir);
    if (n > 3 && dir[n - 1] == '\\') {
        dir[n - 1] = '\0';
    }
    set_string(k, "CDPath", dir);
    set_string(k, "HardDriveRootPath", dir);
    set_dword(k, "IsAdmin", 1);
    for (i = 0; i < sizeof dwords / sizeof dwords[0]; i++) {
        if (!find_value(k, dwords[i].name)) {
            set_dword(k, dwords[i].name, dwords[i].value);
        }
    }
    for (i = 0; i < sizeof strings / sizeof strings[0]; i++) {
        if (!find_value(k, strings[i].name)) {
            set_string(k, strings[i].name, strings[i].value);
        }
    }
    if (!find_value(k, "PID")) {
        char guid[40];
        make_guid(guid);
        set_string(k, "PID", guid);
    }
    /* the controller of the last session: the game resets the controls when it differs from UseControllerId */
    if (!find_value(k, "LastControllerId") && (v = find_value(k, "UseControllerId")) != NULL) {
        set_value(k, "LastControllerId", v->type, v->data, v->size);
    }
}

/* ---------------------------------------------------------------- handles */

static const char *root_name(HKEY h)
{
    switch ((uintptr_t)h) {
    case (uintptr_t)HKEY_CLASSES_ROOT: return "HKCR";
    case (uintptr_t)HKEY_CURRENT_USER: return "HKCU";
    case (uintptr_t)HKEY_LOCAL_MACHINE: return "HKLM";
    case (uintptr_t)HKEY_USERS: return "HKU";
    case (uintptr_t)HKEY_CURRENT_CONFIG: return "HKCC";
    }
    return NULL;
}

/* the key path of handle h + sub; 0 if h is not a key we know */
static int resolve(HKEY h, const char *sub, char *out)
{
    const char *base = root_name(h);
    uintptr_t slot = (uintptr_t)h - HANDLE_BASE;
    if (!g_cards_loaded) {
        load_cards();
    }
    size_t n;
    if (!base) {
        if (slot >= MAX_HANDLES || !g_used[slot]) {
            return 0;
        }
        base = g_handles[slot];
    }
    lstrcpynA(out, base, MAX_PATH);
    while (sub && *sub == '\\') {
        sub++;
    }
    if (sub && *sub) {
        n = strlen(out);
        snprintf(out + n, MAX_PATH - n, "\\%s", sub);
    }
    n = strlen(out);
    while (n && out[n - 1] == '\\') {
        out[--n] = '\0';
    }
    return 1;
}

static HKEY new_handle(const char *path)
{
    int i;
    for (i = 0; i < MAX_HANDLES; i++) {
        if (!g_used[i]) {
            g_used[i] = 1;
            lstrcpynA(g_handles[i], path, MAX_PATH);
            return (HKEY)(uintptr_t)(HANDLE_BASE + i);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- the API */

static LONG WINAPI hk_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD opts, REGSAM sam, PHKEY out)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    (void)opts;
    (void)sam;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (key_exists(path)) {
        *out = new_handle(path);
        r = *out ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        mcm_log("registry: open %s -> %ld", sub ? sub : "", r);
    }
    return r;
}

static LONG WINAPI hk_RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opts, REGSAM sam,
                                      LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    char path[MAX_PATH];
    LONG r = ERROR_SUCCESS;
    int created = 0;
    (void)res; (void)cls; (void)opts; (void)sam; (void)sa;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else {
        create_key(path, &created);
        *out = new_handle(path);
        if (!*out) {
            r = ERROR_OUTOFMEMORY;
        }
        if (disp) {
            *disp = created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
        }
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static const char *short_path(const char *path)
{
    size_t n = strlen(GAME_ROOT);
    return !_strnicmp(path, GAME_ROOT, n) ? path + n : path;
}

static LONG WINAPI hk_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    value_t *v;
    (void)res;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if ((v = lookup(path, name ? name : "")) != NULL) {
        if (type) {
            *type = v->type;
        }
        if (data && cb && *cb < v->size) {
            r = ERROR_MORE_DATA;
        } else {
            if (data) {
                memcpy(data, v->data, v->size);
            }
            r = ERROR_SUCCESS;
        }
        if (cb) {
            *cb = v->size;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        mcm_log("registry: query %s\\%s -> %ld", short_path(path), name ? name : "", r);
    }
    return r;
}

static LONG WINAPI hk_RegSetValueExA(HKEY h, LPCSTR name, DWORD res, DWORD type, const BYTE *data, DWORD cb)
{
    char path[MAX_PATH];
    LONG r = ERROR_SUCCESS;
    (void)res;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else {
        store(path, name ? name : "", type, data, data ? cb : 0);
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        mcm_log("registry: set %s\\%s (type %lu, %lu bytes) -> %ld", short_path(path), name ? name : "", type, cb, r);
    }
    return r;
}

/* the immediate subkeys of path, sorted (the registry enumerates them in order) */
static int children(const char *path, char names[][MAX_PATH], int max)
{
    size_t n = strlen(path);
    int i, j, count = 0;
    for (i = 0; i < g_nkeys; i++) {
        const char *p = g_keys[i].path, *end;
        char child[MAX_PATH];
        int dup = 0;
        if (_strnicmp(p, path, n) || p[n] != '\\') {
            continue;
        }
        p += n + 1;
        end = strchr(p, '\\');
        lstrcpynA(child, p, end ? (int)(end - p) + 1 : MAX_PATH);
        for (j = 0; j < count; j++) {
            dup |= !_stricmp(names[j], child);
        }
        if (!dup && count < max) {
            strcpy(names[count++], child);
        }
    }
    for (i = 1; i < count; i++) {
        for (j = i; j > 0 && _stricmp(names[j - 1], names[j]) > 0; j--) {
            char t[MAX_PATH];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
        }
    }
    return count;
}

static char g_children[128][MAX_PATH];

static LONG WINAPI hk_RegEnumKeyA(HKEY h, DWORD index, LPSTR name, DWORD cch)
{
    char path[MAX_PATH];
    LONG r;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if ((int)index >= children(path, g_children, 128)) {
        r = ERROR_NO_MORE_ITEMS;
    } else if (strlen(g_children[index]) + 1 > cch) {
        r = ERROR_MORE_DATA;
    } else {
        strcpy(name, g_children[index]);
        r = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static LONG WINAPI hk_RegQueryInfoKeyA(HKEY h, LPSTR cls, LPDWORD ccls, LPDWORD res, LPDWORD subkeys, LPDWORD maxsub,
                                       LPDWORD maxcls, LPDWORD values, LPDWORD maxname, LPDWORD maxvalue,
                                       LPDWORD secdesc, PFILETIME written)
{
    char path[MAX_PATH];
    LONG r = ERROR_SUCCESS;
    (void)res;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else {
        rkey_t *k = find_key(path);
        int n = children(path, g_children, 128), i;
        DWORD ms = 0, mn = 0, mv = 0;
        for (i = 0; i < n; i++) {
            ms = strlen(g_children[i]) > ms ? (DWORD)strlen(g_children[i]) : ms;
        }
        for (i = 0; k && i < k->nvals; i++) {
            mn = strlen(k->vals[i].name) > mn ? (DWORD)strlen(k->vals[i].name) : mn;
            mv = k->vals[i].size > mv ? k->vals[i].size : mv;
        }
        if (cls && ccls && *ccls) {
            cls[0] = '\0';
        }
        if (ccls) *ccls = 0;
        if (subkeys) *subkeys = (DWORD)n;
        if (maxsub) *maxsub = ms;
        if (maxcls) *maxcls = 0;
        if (values) *values = k ? (DWORD)k->nvals : 0;
        if (maxname) *maxname = mn;
        if (maxvalue) *maxvalue = mv;
        if (secdesc) *secdesc = 0;
        if (written) GetSystemTimeAsFileTime(written);
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static LONG WINAPI hk_RegDeleteKeyA(HKEY h, LPCSTR sub)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (strchr(path, '\\') && key_exists(path)) {
        delete_tree(path);              /* the stored settings stay */
        g_cards_dirty = 1;
        save_cards();
        r = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static LONG WINAPI hk_RegCloseKey(HKEY h)
{
    uintptr_t slot = (uintptr_t)h - HANDLE_BASE;
    LONG r = ERROR_SUCCESS;
    EnterCriticalSection(&g_cs);
    if (slot < MAX_HANDLES && g_used[slot]) {
        g_used[slot] = 0;
    } else if (!root_name(h)) {
        r = ERROR_INVALID_HANDLE;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static int __fastcall profile_cards(char *app, void *unused)
{
    int r;
    (void)unused;
    g_profiling = 1;
    r = ((int (__fastcall *)(char *, void *))PROFILE_CARDS)(app, NULL);
    g_profiling = 0;
    EnterCriticalSection(&g_cs);
    save_cards();
    LeaveCriticalSection(&g_cs);
    return r;
}

/* ---------------------------------------------------------------- for the other modules */

static int g_installed;

/* a DWORD of the game's key (HKLM\Software\Microsoft\Microsoft Games\Motocross Madness 2\<sub>), wherever it lives */
int mcm_reg_get_dword(const char *sub, const char *name, int def)
{
    char path[MAX_PATH];
    int r = def;
    snprintf(path, sizeof path, GAME_KEY "%s%s", sub && *sub ? "\\" : "", sub ? sub : "");
    if (g_installed) {
        value_t *v;
        EnterCriticalSection(&g_cs);
        v = lookup(path, name);
        if (v && v->size == 4) {
            r = *(int *)v->data;
        }
        LeaveCriticalSection(&g_cs);
    } else {
        HKEY h;
        DWORD d, size = 4, type;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path + 5, 0, KEY_READ, &h) == ERROR_SUCCESS) {
            if (RegQueryValueExA(h, name, NULL, &type, (BYTE *)&d, &size) == ERROR_SUCCESS && type == REG_DWORD) {
                r = (int)d;
            }
            RegCloseKey(h);
        }
    }
    return r;
}

static void reg_set(const char *sub, const char *name, DWORD type, const void *data, DWORD size)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, GAME_KEY "%s%s", sub && *sub ? "\\" : "", sub ? sub : "");
    if (g_installed) {
        EnterCriticalSection(&g_cs);
        store(path, name, type, data, size);
        LeaveCriticalSection(&g_cs);
    } else {
        HKEY h;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path + 5, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(h, name, 0, type, data, size);
            RegCloseKey(h);
        }
    }
}

void mcm_reg_set_dword(const char *sub, const char *name, DWORD value)
{
    reg_set(sub, name, REG_DWORD, &value, 4);
}

void mcm_reg_set_string(const char *sub, const char *name, const char *value)
{
    reg_set(sub, name, REG_SZ, value, (DWORD)strlen(value) + 1);
}

/* ---------------------------------------------------------------- install */

void mcm_registry_install(void)
{
    rkey_t *k;
    const char *dir;
    if (!mcm_ini_int("Registry", "UseFile", 1)) {
        mcm_log("registry: UseFile=0, the game uses the registry");
        return;
    }
    InitializeCriticalSection(&g_cs);
    g_log_calls = mcm_ini_int("Registry", "LogAccess", 0);

    dir = mcm_save_dir();
    snprintf(g_cards_file, sizeof g_cards_file, "%sVideoCards.bin", dir[0] ? dir : mcm_game_dir());
    load_settings();
    k = find_key(GAME_KEY);
    mcm_log("registry: %d [Game] and %d [Graphics] settings (%s, %s)", k->nvals, g_graphics.nvals, mcm_ini_file(),
            mcm_user_ini_file() ? mcm_user_ini_file() : "-");
    install_values();
    if (!mcm_redirect_call(PROFILE_SITE, PROFILE_CARDS, profile_cards)) {
        mcm_log("registry: card profiling not found; its defaults may replace [Graphics] values");
    }
    {
        /* the card is measured at every start now: without asking first (jmp short 0x4c46ff, past the box's
         * ShowCursor(0), whose function pointer the skipped code loads) */
        static const BYTE ask[6] = {0x8b, 0x86, 0x20, 0x04, 0x00, 0x00}, go[2] = {0xeb, 0x64};
        if (mcm_expect(PROFILE_ASK, ask, sizeof ask)) {
            mcm_write(PROFILE_ASK, go, sizeof go);
        } else {
            mcm_log("registry: card test prompt not found, the game may ask at start");
        }
    }

    mcm_hook_import("ADVAPI32.dll", "RegOpenKeyExA", hk_RegOpenKeyExA);
    mcm_hook_import("ADVAPI32.dll", "RegCreateKeyExA", hk_RegCreateKeyExA);
    mcm_hook_import("ADVAPI32.dll", "RegQueryValueExA", hk_RegQueryValueExA);
    mcm_hook_import("ADVAPI32.dll", "RegSetValueExA", hk_RegSetValueExA);
    mcm_hook_import("ADVAPI32.dll", "RegEnumKeyA", hk_RegEnumKeyA);
    mcm_hook_import("ADVAPI32.dll", "RegQueryInfoKeyA", hk_RegQueryInfoKeyA);
    mcm_hook_import("ADVAPI32.dll", "RegDeleteKeyA", hk_RegDeleteKeyA);
    mcm_hook_import("ADVAPI32.dll", "RegCloseKey", hk_RegCloseKey);
    g_installed = 1;
    mcm_log("registry: hooks installed");
}
