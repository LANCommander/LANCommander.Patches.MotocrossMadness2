/* [Stats] MatchStatistics: races reported to LANCommander's match statistics through the LANCommander Interposer's
 * match API. Where the reports go is the Interposer's business: MatchStatistics.ServerUrl / Token / GameId in its
 * Config.yml (or [Stats] ServerUrl ... here, handed over with InterposerMatchConfigure); else LANCommander's
 * launcher, when it started the game; else they are only logged.
 *
 * The Interposer is the one embedded in dinput.dll / the ASI, or the one that loaded the plugin build
 * (mcm_interposer()); its functions are looked up by name, so an Interposer without the match API (or built without
 * MATCHES) just means no statistics.
 *
 * One match per race (one event: a Supercross / Nationals / Baja / Enduro race, a Stunt Quarry or Tag session), from
 * the gate drop to the final standings. Single Event and Tournament races only: Practice and Ghost races never end,
 * replays are not races. By default only network races are reported, by the host (ReportAsClient: by the players who
 * joined too); SinglePlayerRaces=1 reports races against the computer as well.
 *
 * The game (app = *(char **)0x5743ac):
 *   app+0x2d74 race type: 0 Stunt Quarry, 1 Baja, 2 Nationals, 3 Supercross, 4 Tag, 5 Enduro
 *   app+0x2d70 race mode: 0 Practice, 1 Single Event, 2 Tournament, 4 Ghost Race
 *   app+0x2d90 laps, app+0x2eb0 Stunt / Tag time limit (minutes, float), app+0x2da6 the track's folder (char[]),
 *   app+0xc6c + type * 0x80 its display name, app+0x3428 a replay is playing, app+8 CNet (network game; +0x10 == 1
 *   hosting), app+0x570 the EventManager (+0x48 byte races done in the tournament; app+0x2d7c races in it; records
 *   EM+0x50 + i * 0x50, count EM+0x4c: +0 dpid, +0x28 tournament points, +0x30 sub-index byte).
 *   BikeRace (the event's race, event + 0x34): vtable 0x555ddc, slot 10 (0x555e04) Update 0x41e0b0
 *   int __thiscall (br, float dt) runs the race each frame (RaceStatus inside); byte br+0x18a goes 0 -> 1 at the
 *   gate drop, 1 -> 0 on Restart Race. Riders: void *__thiscall 0x421420(br, int *index) from index 0, NULL at the end.
 *   Rider (KrustyBike): +0x5e0 name, +0x11bc dpid, +0x11c0 sub-index byte, +0x734 AI, +0x735 remote, +0x736 ghost,
 *   +0x4a0 left the game, +0x7a0 u16 laps done, +0x7a4 finished (0 -> 1, then 3), and floats in seconds of race time:
 *   +0x74c last lap, +0x750 best lap, +0x754 race time (frozen at the finish; FLT_MAX for a network rider still out
 *   at the end), +0x768 stunt score, +0x788 biggest stunt, +0x760 longest tag hold, +0x764 total tag hold;
 *   +0x784 int position.
 *   EventManager::FinalizeResults 0x45f990 (__fastcall, ecx EM), called once per completed race at 0x45f63f, sets
 *   the final positions; EndRace 0x45ddd0 (__thiscall(EM, mode), mode 2 = the event is destroyed) also ends a race
 *   quit from the pause menu, without FinalizeResults.
 * In a single-player race the computer riders still out when the player finishes get a finishing time made up by
 * the game (reported with "estimated": true). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "mcm2patch.h"
#include "interposer/LANCommander.Interposer.h"

typedef BOOL (*configure_fn)(const InterposerMatchConfig *);
typedef BOOL (*get_config_fn)(const wchar_t *, wchar_t *, DWORD);
typedef DWORD (*start_fn)(const InterposerMatchInfo *);
typedef BOOL (*joined_fn)(DWORD, const char *, const char *, const char *, long long);
typedef BOOL (*left_fn)(DWORD, const char *, long long);
typedef BOOL (*event_fn)(DWORD, const char *, long long, long long, const char *, const char *, const char *);
typedef BOOL (*stat_fn)(DWORD, const char *, const char *, const char *, double);
typedef BOOL (*result_fn)(DWORD, const InterposerMatchResult *);
typedef BOOL (*end_fn)(DWORD, const char *, BOOL);

static configure_fn g_configure;
static get_config_fn g_get_config;
static start_fn g_start;
static joined_fn g_joined;
static left_fn g_left;
static event_fn g_event;
static stat_fn g_set_stat;
static result_fn g_set_result;
static end_fn g_end;

static int g_single_player, g_as_client, g_kind_configured;

/* ---------------------------------------------------------------- text */

static void ansi_to_utf8(const char *ansi, char *out, size_t size)
{
    wchar_t wide[256];
    out[0] = '\0';
    if (MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, sizeof wide / sizeof wide[0]) > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL);
    }
}

/* a JSON string (quotes included) from game text */
static size_t json_str(char *out, size_t size, const char *ansi)
{
    char utf8[512];
    const unsigned char *s;
    size_t n = 0;
    ansi_to_utf8(ansi ? ansi : "", utf8, sizeof utf8);
    if (size < 3) {
        return 0;
    }
    out[n++] = '"';
    for (s = (const unsigned char *)utf8; *s && n + 8 < size; s++) {
        if (*s == '"' || *s == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*s;
        } else if (*s < 0x20) {
            n += snprintf(out + n, size - n, "\\u%04x", *s);
        } else {
            out[n++] = (char)*s;
        }
    }
    out[n++] = '"';
    out[n] = '\0';
    return n;
}

/* seconds of race time as whole milliseconds; -1 for none (0, negative, FLT_MAX, NaN) */
static long long ms_of(float seconds)
{
    if (!(seconds > 0.0f) || seconds > 1e6f) {
        return -1;
    }
    return (long long)(seconds * 1000.0f + 0.5f);
}

/* ---------------------------------------------------------------- the game */

#define APP_RACE_MODE   0x2d70
#define APP_RACE_TYPE   0x2d74
#define APP_LAPS        0x2d90
#define APP_TOUR_RACES  0x2d7c
#define APP_TRACK       0x2da6
#define APP_TRACK_NAME  0xc6c
#define APP_TIME_LIMIT  0x2eb0
#define APP_REPLAY      0x3428
#define APP_NET         0x8
#define APP_EVENTS      0x570

#define BR_VTABLE_UPDATE 0x555e04u
#define FN_BR_UPDATE     0x41e0b0u
#define FN_NEXT_RIDER    0x421420u
#define CALL_FINALIZE    0x45f63fu
#define FN_FINALIZE      0x45f990u
#define FN_END_RACE      0x45ddd0u

#define R_NAME     0x5e0
#define R_DPID     0x11bc
#define R_SUB      0x11c0
#define R_AI       0x734
#define R_REMOTE   0x735
#define R_GHOST    0x736
#define R_LEFT     0x4a0
#define R_LAST_LAP 0x74c
#define R_BEST_LAP 0x750
#define R_TIME     0x754
#define R_TAG_LONG 0x760
#define R_TAG_TIME 0x764
#define R_SCORE    0x768
#define R_POSITION 0x784
#define R_BIG_STUNT 0x788
#define R_LAPS     0x7a0
#define R_FINISHED 0x7a4
#define R_SIZE     0x11c4

enum { TYPE_STUNT, TYPE_BAJA, TYPE_NATIONALS, TYPE_SUPERCROSS, TYPE_TAG, TYPE_ENDURO };
static const char *const TYPES[] = { "stunt_quarry", "baja", "nationals", "supercross", "tag", "enduro" };
static const char *const MODES[] = { "practice", "single_event", "tournament", "unknown", "ghost_race" };

#define F(p, off) (*(const float *)((const char *)(p) + (off)))
#define I(p, off) (*(const int *)((const char *)(p) + (off)))
#define B(p, off) (*(const unsigned char *)((const char *)(p) + (off)))

static char *app(void) { return MCM_APP; }

static int race_type(void) { return I(app(), APP_RACE_TYPE); }
static int lap_race(void)
{
    int t = race_type();
    return t == TYPE_BAJA || t == TYPE_NATIONALS || t == TYPE_SUPERCROSS || t == TYPE_ENDURO;
}

static int readable(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }

typedef char *(__thiscall *next_rider_fn)(void *br, int *index);

/* ---------------------------------------------------------------- the open match */

#define MAX_RIDERS 16
static struct {
    char *bike;
    char key[96];
    char name[64];                       /* UTF-8 */
    int human, local;
    unsigned laps;
    int finished, left;
} g_riders[MAX_RIDERS];
static int g_nriders;

static DWORD g_match;
static char *g_br;                       /* the BikeRace of the open match */
static double g_clock;                   /* seconds of race time since the gate drop */
static int g_network, g_host;

static int g_local_done;                 /* the player of this computer finished (single player: the race is over) */

static long long game_time(void) { return (long long)(g_clock * 1000.0); }

/* +0x7a4 is briefly non-zero at the start of a race too (seen 3 s after the gate with no lap done): in a lap race a
 * rider has finished once its laps are done, or (single player) when the player's finish ended the race for it */
static int rider_finished(const char *bike)
{
    if (!(I(bike, R_FINISHED) & 1)) {
        return 0;
    }
    if (race_type() == 0 || race_type() == 4) {
        return 1;
    }
    return *(const unsigned short *)(bike + R_LAPS) >= (unsigned)I(app(), APP_LAPS) || (!g_network && g_local_done);
}

static void rider_key(char *bike, int human, char *key, size_t size, char *name, size_t name_size)
{
    char raw[32];
    int i;
    lstrcpynA(raw, (const char *)bike + R_NAME, sizeof raw);
    ansi_to_utf8(raw, name, name_size);
    if (!name[0]) {
        snprintf(name, name_size, "Rider %d", I(bike, R_DPID));
    }
    snprintf(key, size, human ? "name:%s" : "bot:%s", name);
    for (i = 0; i < g_nriders; i++) {   /* two riders of the same name: tell them apart by their ids */
        if (!strcmp(g_riders[i].key, key)) {
            snprintf(key, size, human ? "name:%s#%d.%d" : "bot:%s#%d.%d", name, I(bike, R_DPID), B(bike, R_SUB));
            break;
        }
    }
}

/* the rider's entry, added (and joined to the match) if it is new */
static int rider_index(char *bike)
{
    int i;
    for (i = 0; i < g_nriders; i++) {
        if (g_riders[i].bike == bike) {
            return i;
        }
    }
    if (g_nriders >= MAX_RIDERS || !readable(bike, R_SIZE) || B(bike, R_GHOST)) {
        return -1;
    }
    i = g_nriders;
    memset(&g_riders[i], 0, sizeof g_riders[0]);
    g_riders[i].bike = bike;
    g_riders[i].human = !B(bike, R_AI);
    g_riders[i].local = !B(bike, R_AI) && !B(bike, R_REMOTE);
    rider_key(bike, g_riders[i].human, g_riders[i].key, sizeof g_riders[i].key, g_riders[i].name,
              sizeof g_riders[i].name);
    g_riders[i].laps = *(const unsigned short *)(bike + R_LAPS);
    g_riders[i].finished = rider_finished(bike);
    g_riders[i].left = I(bike, R_LEFT) != 0;
    g_nriders++;
    g_joined(g_match, g_riders[i].key, g_riders[i].name, NULL, game_time());
    return i;
}

static void event(const char *type, const char *actor, const char *payload)
{
    if (!g_match) {
        return;
    }
    if (!g_event(g_match, type, game_time(), -1, actor, NULL, payload)) {
        mcm_log("matchstats: event %s refused (payload %s)", type, payload ? payload : "none");
    } else {
        mcm_log("matchstats: %s %s %s", type, actor ? actor : "-", payload ? payload : "");
    }
}

static void end_match(const char *reason, BOOL abandoned)
{
    if (!g_match) {
        return;
    }
    g_end(g_match, reason, abandoned);
    mcm_log("matchstats: match %lu ended (%s%s) after %.3f s", g_match, reason, abandoned ? ", abandoned" : "",
            g_clock);
    g_match = 0;
    g_br = NULL;
}

/* ---------------------------------------------------------------- start */

static void set_reporter_kind(const char *kind)
{
    InterposerMatchConfig config;
    if (g_kind_configured || !g_configure) {
        return;
    }
    memset(&config, 0, sizeof config);
    config.structSize = sizeof config;
    config.enabled = -1;
    config.reporterKind = kind;
    g_configure(&config);
}

static void start_match(char *br)
{
    InterposerMatchInfo info;
    char *a = app(), *net = *(char **)(a + APP_NET), *em = *(char **)(a + APP_EVENTS);
    char track[64], track_name[256], name_json[300], settings[512], *bike;
    int type = race_type(), mode = I(a, APP_RACE_MODE), index = 0;
    g_network = net != NULL;
    g_host = g_network && readable(net, 0x14) && I(net, 0x10) == 1;
    if (mode != 1 && mode != 2) {
        mcm_log("matchstats: %s race, not reported", (unsigned)mode < 5 ? MODES[mode] : "unknown");
        return;
    }
    if ((unsigned)type > TYPE_ENDURO) {
        mcm_log("matchstats: unknown race type %d, not reported", type);
        return;
    }
    if (!g_network && !g_single_player) {
        mcm_log("matchstats: single-player race, not reported (SinglePlayerRaces=0)");
        return;
    }
    if (g_network && !g_host && !g_as_client) {
        mcm_log("matchstats: network race joined, reported by the host (ReportAsClient=0)");
        return;
    }

    {
        char raw[0x80];
        lstrcpynA(raw, a + APP_TRACK, sizeof track);
        ansi_to_utf8(raw, track, sizeof track);
        lstrcpynA(raw, a + APP_TRACK_NAME + type * 0x80, sizeof raw);
        json_str(name_json, sizeof name_json, raw);
        ansi_to_utf8(raw, track_name, sizeof track_name);
    }
    if (type == TYPE_STUNT || type == TYPE_TAG) {
        snprintf(settings, sizeof settings, "{\"race_mode\":\"%s\",\"time_limit_ms\":%lld,\"track_name\":%s",
                 MODES[mode], (long long)(F(a, APP_TIME_LIMIT) * 60000.0f), name_json);
    } else {
        snprintf(settings, sizeof settings, "{\"race_mode\":\"%s\",\"laps\":%d,\"track_name\":%s", MODES[mode],
                 I(a, APP_LAPS), name_json);
    }
    if (mode == 2 && readable(em, 0x50)) {
        size_t n = strlen(settings);
        snprintf(settings + n, sizeof settings - n, ",\"tournament_race\":%d,\"tournament_races\":%d",
                 B(em, 0x48) + 1, I(a, APP_TOUR_RACES));
    }
    strncat(settings, "}", sizeof settings - strlen(settings) - 1);

    memset(&info, 0, sizeof info);
    info.structSize = sizeof info;
    info.map = track[0] ? track : NULL;
    info.mode = TYPES[type];
    info.gameVersion = "2.10";
    info.settingsJson = settings;
    info.metadataJson = g_network ? "{\"patch\":\"mcm2patch " MCM_VERSION "\",\"network\":true}"
                                  : "{\"patch\":\"mcm2patch " MCM_VERSION "\",\"network\":false}";
    set_reporter_kind(g_network && !g_host ? "Client" : "Host");
    if (!(g_match = g_start(&info))) {
        mcm_log("matchstats: the Interposer did not start a match (match statistics not built in, or turned off)");
        return;
    }
    g_br = br;
    g_clock = 0;
    g_nriders = 0;
    g_local_done = 0;
    while ((bike = ((next_rider_fn)FN_NEXT_RIDER)(br, &index)) != NULL && index < 64) {
        rider_index(bike);
    }
    mcm_log("matchstats: match %lu started: %s on %s (%s), %s, %s, %d riders", g_match, TYPES[type], track,
            track_name, MODES[mode], g_network ? (g_host ? "network, hosting" : "network, joined") : "single player",
            g_nriders);
}

/* ---------------------------------------------------------------- during the race */

/* laps, finishes and players leaving, from what changed in the riders over one BikeRace::Update */
static void track_riders(char *br)
{
    char payload[256], *bike;
    int index = 0, i, local_finished_now = 0;
    while ((bike = ((next_rider_fn)FN_NEXT_RIDER)(br, &index)) != NULL && index < 64) {
        rider_index(bike);
    }
    for (i = 0; i < g_nriders; i++) {   /* the local rider first: computer riders it ends the race for are estimated */
        if (g_riders[i].local && !g_riders[i].finished && rider_finished(g_riders[i].bike)) {
            local_finished_now = 1;
            g_local_done = 1;
        }
    }
    for (i = 0; i < g_nriders; i++) {
        char *b = g_riders[i].bike;
        unsigned laps = *(const unsigned short *)(b + R_LAPS);
        int finished = rider_finished(b), left = I(b, R_LEFT) != 0;
        if (lap_race() && laps > g_riders[i].laps && laps < 1000) {
            snprintf(payload, sizeof payload, "{\"lap\":%u,\"lap_ms\":%lld,\"race_ms\":%lld,\"position\":%d}", laps,
                     ms_of(F(b, R_LAST_LAP)), ms_of(F(b, R_TIME)), I(b, R_POSITION));
            event("lap_completed", g_riders[i].key, payload);
        }
        g_riders[i].laps = laps;
        if (finished && !g_riders[i].finished) {
            int estimated = !g_network && !g_riders[i].human && local_finished_now;
            if (lap_race()) {
                snprintf(payload, sizeof payload, "{\"position\":%d,\"race_ms\":%lld,\"laps\":%u%s}",
                         I(b, R_POSITION), ms_of(F(b, R_TIME)), laps, estimated ? ",\"estimated\":true" : "");
                event("finished", g_riders[i].key, payload);
            }
        }
        g_riders[i].finished = finished;
        if (left && !g_riders[i].left) {
            g_left(g_match, g_riders[i].key, game_time());
            event("left", g_riders[i].key, NULL);
        }
        g_riders[i].left = left;
    }
}

typedef int (__thiscall *update_fn)(void *br, float dt);
static update_fn g_update;

static int __thiscall update_hook(char *br, float dt)
{
    int dropped = B(br, 0x18a), result;
    result = g_update(br, dt);
    if (I(app(), APP_REPLAY)) {
        return result;
    }
    if (dropped && !B(br, 0x18a) && g_match && g_br == br) {
        end_match("restarted", TRUE);   /* Restart Race: the gate drops again, a new match then */
        return result;
    }
    if (!dropped && B(br, 0x18a) && !g_match) {
        start_match(br);
    }
    if (g_match && g_br == br) {
        if (dt > 0.0f && dt < 1.0f) {
            g_clock += dt;
        }
        track_riders(br);
    }
    return result;
}

/* ---------------------------------------------------------------- the end */

static int tournament_points(char *em, char *bike)
{
    int i, n;
    if (I(app(), APP_RACE_MODE) != 2 || !readable(em, 0x50)) {
        return -1;
    }
    n = I(em, 0x4c);
    for (i = 0; i < n && i < 16; i++) {
        char *rec = em + 0x50 + i * 0x50;
        if (I(rec, 0) == I(bike, R_DPID) && B(rec, 0x30) == B(bike, R_SUB)) {
            return I(rec, 0x28);
        }
    }
    return -1;
}

static void report_results(char *em)
{
    int type = race_type(), i;
    track_riders(g_br);
    for (i = 0; i < g_nriders; i++) {
        char *b = g_riders[i].bike, payload[160];
        const char *key = g_riders[i].key;
        InterposerMatchResult res;
        long long time = ms_of(F(b, R_TIME)), best = ms_of(F(b, R_BEST_LAP));
        int points = tournament_points(em, b);
        memset(&res, 0, sizeof res);
        res.structSize = sizeof res;
        res.playerKey = key;
        res.rank = I(b, R_POSITION);
        res.timeMs = -1;
        res.status = INTERPOSER_MATCH_RESULT_FINISHED;
        if (lap_race()) {
            if (rider_finished(b) && time >= 0) {
                res.timeMs = time;
            } else {
                res.status = INTERPOSER_MATCH_RESULT_DID_NOT_FINISH;
            }
            g_set_stat(g_match, key, NULL, "laps", g_riders[i].laps);
            if (best >= 0) {
                g_set_stat(g_match, key, NULL, "best_lap_ms", (double)best);
            }
            if (res.timeMs >= 0) {
                g_set_stat(g_match, key, NULL, "race_time_ms", (double)res.timeMs);
            }
        } else if (type == TYPE_STUNT) {
            res.hasScore = 1;
            res.score = F(b, R_SCORE);
            g_set_stat(g_match, key, NULL, "score", F(b, R_SCORE));
            g_set_stat(g_match, key, NULL, "biggest_stunt", F(b, R_BIG_STUNT));
        } else {   /* Tag: the time holding the flag */
            long long held = ms_of(F(b, R_TAG_TIME)), longest = ms_of(F(b, R_TAG_LONG));
            res.hasScore = 1;
            res.score = held > 0 ? (double)held : 0;
            g_set_stat(g_match, key, NULL, "tag_time_ms", held > 0 ? (double)held : 0);
            g_set_stat(g_match, key, NULL, "longest_tag_ms", longest > 0 ? (double)longest : 0);
        }
        if (g_riders[i].left) {
            res.status = INTERPOSER_MATCH_RESULT_FORFEITED;
        }
        if (points >= 0) {
            g_set_stat(g_match, key, NULL, "tournament_points", points);
            snprintf(payload, sizeof payload, "{\"human\":%s,\"tournament_points\":%d}",
                     g_riders[i].human ? "true" : "false", points);
        } else {
            snprintf(payload, sizeof payload, "{\"human\":%s}", g_riders[i].human ? "true" : "false");
        }
        res.payloadJson = payload;
        if (res.rank <= 0 || res.rank > 99) {
            res.rank = g_nriders;
        }
        g_set_result(g_match, &res);
        mcm_log("matchstats: %d. %s (%s) %s time %lld ms, best lap %lld ms, score %.0f", res.rank, g_riders[i].name,
                g_riders[i].human ? "human" : "computer",
                res.status == INTERPOSER_MATCH_RESULT_FINISHED ? "finished" : "did not finish", res.timeMs, best,
                res.hasScore ? res.score : 0.0);
    }
    end_match("completed", FALSE);
}

typedef int (__fastcall *finalize_fn)(void *em, void *edx);

static int __fastcall finalize_hook(void *em, void *edx)
{
    int result = ((finalize_fn)(uintptr_t)FN_FINALIZE)(em, edx);
    if (g_match && g_br && !I(app(), APP_REPLAY)) {
        report_results((char *)em);
    }
    return result;
}

typedef int (__thiscall *end_race_fn)(void *em, int mode);
static end_race_fn g_end_race;           /* trampoline: the original's first instruction, then a jump back */

static int __thiscall end_race_hook(void *em, int mode)
{
    if (mode == 2 && g_match) {
        end_match("quit", TRUE);         /* the event is destroyed without final standings */
    }
    return g_end_race(em, mode);
}

/* ---------------------------------------------------------------- install */

static int install_hooks(void)
{
    static const uint8_t end_race_prologue[] = { 0xa1, 0xac, 0x43, 0x57, 0x00 };   /* mov eax, [0x5743ac] */
    uint32_t *slot = (uint32_t *)MCM_ADDR(BR_VTABLE_UPDATE), hook = (uint32_t)(uintptr_t)update_hook;
    uint8_t *tramp;
    int32_t rel;
    if (*slot != FN_BR_UPDATE || !mcm_expect(FN_END_RACE, end_race_prologue, 5)
        || !mcm_expect(CALL_FINALIZE, "\xe8\x4c\x03\x00\x00", 5)) {
        mcm_log("matchstats: the race code differs from the supported MCM2.EXE, no match statistics");
        return 0;
    }
    if (!(tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
        return 0;
    }
    memcpy(tramp, end_race_prologue, 5);
    tramp[5] = 0xe9;
    rel = (int32_t)((FN_END_RACE + 5) - ((uintptr_t)tramp + 10));
    memcpy(tramp + 6, &rel, 4);
    g_end_race = (end_race_fn)(void *)tramp;
    g_update = (update_fn)(uintptr_t)FN_BR_UPDATE;
    return mcm_write(BR_VTABLE_UPDATE, &hook, 4) && mcm_redirect_call(CALL_FINALIZE, FN_FINALIZE, (void *)finalize_hook)
        && mcm_jmp(FN_END_RACE, (void *)end_race_hook);
}

static void configure(void)
{
    InterposerMatchConfig config;
    char url[512], token[512], game[64], server[64], name[128];
    wchar_t kind[32];
    mcm_ini_str("Stats", "ServerUrl", url, sizeof url);
    mcm_ini_str("Stats", "Token", token, sizeof token);
    mcm_ini_str("Stats", "GameId", game, sizeof game);
    mcm_ini_str("Stats", "ServerId", server, sizeof server);
    mcm_ini_str("Stats", "ReporterName", name, sizeof name);
    g_kind_configured = g_get_config && g_get_config(L"MatchStatistics.ReporterKind", kind, 32) && kind[0];
    if (!url[0] && !token[0] && !game[0] && !server[0] && !name[0]) {
        return;   /* all from the Interposer's Config.yml */
    }
    memset(&config, 0, sizeof config);
    config.structSize = sizeof config;
    config.enabled = -1;
    config.serverUrl = url[0] ? url : NULL;
    config.token = token[0] ? token : NULL;
    config.gameId = game[0] ? game : NULL;
    config.serverId = server[0] ? server : NULL;
    config.reporterName = name[0] ? name : NULL;
    if (!g_configure || !g_configure(&config)) {
        mcm_log("matchstats: the [Stats] server settings were refused (see the Interposer's log); Config.yml's are "
                "used");
    } else {
        mcm_log("matchstats: reporting to %s", url[0] ? url : "the server in Config.yml");
    }
}

#define RESOLVE(var, type, name) (var = (type)(void (*)(void))GetProcAddress(interposer, name))

void mcm_matchstats_install(void)
{
    HMODULE interposer = mcm_interposer();
    if (!mcm_ini_int("Stats", "MatchStatistics", 1)) {
        return;
    }
    if (!interposer) {
        mcm_log("matchstats: no LANCommander Interposer, no match statistics");
        return;
    }
    RESOLVE(g_configure, configure_fn, "InterposerMatchConfigure");
    if (GetProcAddress(interposer, "InterposerSetConfigString")) {   /* see interposer_plugin.c */
        RESOLVE(g_get_config, get_config_fn, "InterposerGetConfigString");
    }
    if (!RESOLVE(g_start, start_fn, "InterposerMatchStart") || !RESOLVE(g_joined, joined_fn, "InterposerMatchPlayerJoined")
        || !RESOLVE(g_left, left_fn, "InterposerMatchPlayerLeft") || !RESOLVE(g_event, event_fn, "InterposerMatchEvent")
        || !RESOLVE(g_set_stat, stat_fn, "InterposerMatchSetStatistic")
        || !RESOLVE(g_set_result, result_fn, "InterposerMatchSetResult") || !RESOLVE(g_end, end_fn, "InterposerMatchEnd")) {
        mcm_log("matchstats: the Interposer has no match API (too old), no match statistics");
        return;
    }
    {
        typedef DWORD (*features_fn)(void);
        features_fn features = (features_fn)(void (*)(void))GetProcAddress(interposer, "InterposerGetFeatures");
        if (features && !(features() & INTERPOSER_FEATURE_FLAG_MATCHES)) {
            mcm_log("matchstats: the Interposer was built without match statistics, no match statistics");
            return;
        }
    }
    g_single_player = mcm_ini_int("Stats", "SinglePlayerRaces", 0);
    g_as_client = mcm_ini_int("Stats", "ReportAsClient", 0);
    if (!install_hooks()) {
        return;
    }
    configure();
    mcm_log("matchstats: on (single-player races %s, as a client %s)", g_single_player ? "too" : "no",
            g_as_client ? "too" : "no");
}

void mcm_matchstats_shutdown(BOOL process_terminating)
{
    (void)process_terminating;
    if (g_match && g_end) {
        end_match("game_closed", TRUE);
    }
}
