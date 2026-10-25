/* dgVoodoo 2 settings the game needs ([Video] ConfigureDgVoodoo).
 *
 * The game runs its menus in 640x480 and races in the resolution chosen in Options - Graphics, always full screen
 * (DDSCL_EXCLUSIVE). Through dgVoodoo 2 (DDraw.dll, D3DImm.dll and dgVoodoo.conf next to MCM2.EXE) that only
 * looks right with:
 *   [General]    FullScreenMode = true, ScalingMode = stretched_ar   full screen; a 4:3 picture (the menus, a
 *                                                                     4:3 race resolution) pillarboxed, not stretched
 *   [DirectX]    Resolution = unforced                                the game's resolution decides the shape: a
 *                                                                     forced size (h:2880, v:2160; max; desktop)
 *                                                                     squeezes every mode into that shape
 *   [DirectXExt] ExtraEnumeratedResolutions = max, max_16_9, ...      the wide modes: dgVoodoo refuses a mode it did
 *                                                                     not list, and lists few of its own
 * so those keys are set in the game folder's dgVoodoo.conf (text format) when it differs, before dgVoodoo reads it
 * (it does so when the game creates DirectDraw, long after this runs). The original file is kept once as
 * dgVoodoo.conf.mcm2patch.bak. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"

#define WIDE_MODES "max, max_16_9, 3840x2160, 3440x1440, 2560x1440, 2560x1080, 1920x1200, 1920x1080, " \
                   "1680x1050, 1600x900, 1440x900, 1366x768, 1280x800, 1280x720"

typedef struct { const char *section, *key, *value; } setting_t;

static const setting_t SETTINGS[] = {
    { "General", "FullScreenMode", "true" },
    { "General", "ScalingMode", "stretched_ar" },
    { "DirectX", "Resolution", "unforced" },
    { "DirectXExt", "ExtraEnumeratedResolutions", WIDE_MODES },
};
#define NSETTINGS ((int)(sizeof SETTINGS / sizeof SETTINGS[0]))

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* the file with every setting at its value; returns the number of lines changed or added */
static int apply(const char *in, size_t len, char **out, size_t *out_len)
{
    size_t cap = len + 4096, n = 0;
    char *o = malloc(cap), section[64] = "";
    const char *p = in, *end = in + len;
    int done[NSETTINGS] = { 0 }, changed = 0, i;
    if (!o) {
        return -1;
    }
#define EMIT(s, l) do { size_t l_ = (l); if (n + l_ >= cap) { cap = (n + l_) * 2; o = realloc(o, cap); } \
                        memcpy(o + n, (s), l_); n += l_; } while (0)
#define FLUSH_MISSING() do { for (i = 0; i < NSETTINGS; i++) if (!done[i] && !_stricmp(section, SETTINGS[i].section)) { \
                                 char b_[512]; int k_ = snprintf(b_, sizeof b_, "%-36s = %s\r\n", SETTINGS[i].key, SETTINGS[i].value); \
                                 EMIT(b_, (size_t)k_); done[i] = 1; changed++; } } while (0)
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t l = eol ? (size_t)(eol - p + 1) : (size_t)(end - p);
        char line[1024], *t;
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        t = trim(line);
        if (*t == '[') {
            /* a new section: settings missing from the one that ends go at its end */
            char *close = strchr(t, ']');
            FLUSH_MISSING();
            if (close) {
                *close = '\0';
                snprintf(section, sizeof section, "%s", t + 1);
            }
            EMIT(p, l);
        } else if (*t && *t != ';' && strchr(t, '=')) {
            char *eq = strchr(t, '='), *key, *val;
            int hit = -1;
            *eq = '\0';
            key = trim(t);
            val = trim(eq + 1);
            for (i = 0; i < NSETTINGS; i++) {
                if (!_stricmp(section, SETTINGS[i].section) && !_stricmp(key, SETTINGS[i].key)) {
                    hit = i;
                }
            }
            if (hit >= 0 && !done[hit] && _stricmp(val, SETTINGS[hit].value)) {
                char nl[512];
                int k = snprintf(nl, sizeof nl, "%-36s = %s\r\n", SETTINGS[hit].key, SETTINGS[hit].value);
                mcm_log("dgvoodoo: [%s] %s = %s (was %s)", section, SETTINGS[hit].key, SETTINGS[hit].value, val);
                EMIT(nl, (size_t)k);
                changed++;
            } else {
                EMIT(p, l);
            }
            if (hit >= 0) {
                done[hit] = 1;
            }
        } else {
            EMIT(p, l);
        }
        p += l;
    }
    FLUSH_MISSING();
#undef FLUSH_MISSING
#undef EMIT
    *out = o;
    *out_len = n;
    return changed;
}

void mcm_dgvoodoo_install(void)
{
    char path[MAX_PATH + 32], bak[MAX_PATH + 64];
    FILE *f;
    long len;
    char *in, *out = NULL;
    size_t out_len;
    int changed;
    if (!mcm_ini_int("Video", "ConfigureDgVoodoo", 1)) {
        return;
    }
    snprintf(path, sizeof path, "%sdgVoodoo.conf", mcm_game_dir());
    f = fopen(path, "rb");
    if (!f) {
        return;                                   /* no dgVoodoo here (or its settings live elsewhere) */
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    in = malloc((size_t)len + 1);
    if (!in || fread(in, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(in);
        return;
    }
    fclose(f);
    in[len] = '\0';
    if ((long)strlen(in) != len) {
        mcm_log("dgvoodoo: dgVoodoo.conf is in the binary format, left alone (save it as text in dgVoodooCpl)");
        free(in);
        return;
    }
    changed = apply(in, (size_t)len, &out, &out_len);
    if (changed > 0) {
        snprintf(bak, sizeof bak, "%s.mcm2patch.bak", path);
        CopyFileA(path, bak, TRUE);               /* the user's original, once */
        f = fopen(path, "wb");
        if (f && fwrite(out, 1, out_len, f) == out_len) {
            mcm_log("dgvoodoo: %d settings of dgVoodoo.conf set (full screen, 4:3 pillarboxed, wide modes)", changed);
        } else {
            mcm_log("dgvoodoo: cannot write %s", path);
        }
        if (f) {
            fclose(f);
        }
    }
    free(out);
    free(in);
}
