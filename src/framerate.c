/* High refresh rates.
 *
 * The game has no frame cap of its own: it draws as fast as Flip lets it (vsync) and runs everything on a
 * variable time step. Each frame the main loop (0x469600) reads the clock, takes dt = (now - last) * 0.001 s
 * (app+0x2d8 / app+0x2dc), clamps it to 0.2 s and hands it to the simulation (app+0x2f0, 0x43dea0).
 *
 * The clock is its only timeGetTime call (0x4c41d0, wrapped in timeBeginPeriod(1) / timeEndPeriod(1)).
 * timeGetTime follows the system timer resolution, which on today's Windows can be as coarse as 15.6 ms for a
 * process that does not keep it raised, and even at 1 ms a 144 Hz frame (6.9 ms) is measured as 6 or 7 ms:
 * frames get uneven time steps, or none at all, which shows as stutter. Here:
 *   - the clock is QueryPerformanceCounter based (same millisecond scale, never jumps backwards)
 *   - the frame time step comes from the performance counter itself, to the microsecond (0x469629), whenever
 *     the game measures from its previous frame (it restarts its clock after a loading screen: those frames
 *     keep the game's own value)
 *   - [Framerate] MaxFps caps the frame rate: 0 = the desktop refresh rate (default), -1 = no cap, or a number.
 *     The cap matters when vsync is off (dgVoodoo's ForceVerticalSync, or a windowed game); with vsync the
 *     display already paces the frames.
 *   - [Framerate] AiRate: the computer riders' driving (0x414000, BikeAI.cpp: path, steering, throttle; called at
 *     0x497bd7 per rider and frame) runs at most that many times per second, with the time gathered since its last
 *     run. It was tuned for the frame rates of 2000 and goes wrong with short time steps: at 240 Hz the riders
 *     weave (heading changes 74 degrees per second against 30) and ride at two thirds of their speed, barely moving
 *     off the start (measured with the test harness at a fixed 1/240 s step). The bikes' physics is elsewhere and
 *     still runs every frame, so they move smoothly; they keep the last steering and throttle between runs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"
#include "d3d.h"

#define TIMER_FN       0x4c41d0u    /* DWORD Timer(void): the game's clock, ms */
#define FRAME_NOW_CALL 0x46961eu    /* call Timer: the main loop's "now" (app+0x2d8) */
#define FRAME_DT       0x469629u    /* dt = (app+0x2d8 - app+0x2dc) * 0.001 -> st0, [ebp-4] at 0x469658 */
#define FRAME_DT_DONE  0x469658u

static LARGE_INTEGER g_freq, g_start;
static DWORD g_base;                /* timeGetTime at start: the clock goes on from there */
static double g_now_s, g_prev_s;    /* precise times of the frame's "now" and of the previous frame's */
static DWORD g_now_ms, g_prev_ms;
static int g_have_prev;
static double g_frame_s;            /* MaxFps: seconds per frame, 0 = no cap */
static double g_next_s;
static float g_test_dt;             /* test harness (MCM_TEST_DT): every measured step is this long, in seconds */

static double seconds(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_start.QuadPart) / (double)g_freq.QuadPart;
}

static DWORD __cdecl timer_ms(void)
{
    return g_base + (DWORD)(seconds() * 1000.0);
}

/* the main loop's "now": the same clock, and the exact time remembered for the time step */
static DWORD __cdecl frame_now_ms(void)
{
    double s = seconds();
    g_now_s = s;
    g_now_ms = g_base + (DWORD)(s * 1000.0);
    return g_now_ms;
}

/* the frame's time step in seconds (left in st0 for the game) */
static float __cdecl frame_dt(char *app)
{
    DWORD now_ms = *(DWORD *)(app + 0x2d8), last_ms = *(DWORD *)(app + 0x2dc);
    float dt = (float)(now_ms - last_ms) * 0.001f;
    if (g_have_prev && now_ms == g_now_ms && last_ms == g_prev_ms && g_now_s >= g_prev_s) {
        dt = g_test_dt > 0 ? g_test_dt : (float)(g_now_s - g_prev_s);
    }
    g_prev_ms = now_ms;
    g_prev_s = now_ms == g_now_ms ? g_now_s : (double)(now_ms - g_base) * 0.001;
    g_have_prev = 1;
    return dt;
}

/* replaces 0x469629..0x469657: edi = app; what the original left in eax, ecx, edx for the code after it */
__attribute__((naked)) static void frame_dt_stub(void)
{
    __asm__ volatile(
        "push %%edi\n\t"
        "call %P0\n\t"                 /* st0 = dt */
        "add $4, %%esp\n\t"
        "mov 0x6615f8, %%eax\n\t"
        "mov 0x5724f0, %%edx\n\t"
        "lea (%%eax,%%eax,8), %%ecx\n\t"
        "push $0x469658\n\t"
        "ret\n\t"
        :: "i"(frame_dt));
}

/* ---------------------------------------------------------------- computer riders */

#define AI_DRIVE      0x414000u    /* int __thiscall BikeAI::Drive(float dt) */
#define AI_DRIVE_CALL 0x497bd7u
#define AI_SLOTS      16           /* the races have at most 11 riders */

static float g_ai_step;            /* seconds between two runs, 0 = every frame */
static struct { void *bike; float time; } g_ai[AI_SLOTS];
static int g_ai_next;

static int __fastcall ai_drive(char *bike, void *unused, float dt)
{
    int i;
    for (i = 0; i < AI_SLOTS && g_ai[i].bike != bike; i++) {
    }
    if (i == AI_SLOTS) {           /* a rider not seen yet (or a new race): drives now, then every step */
        i = g_ai_next;
        g_ai_next = (g_ai_next + 1) % AI_SLOTS;
        g_ai[i].bike = bike;
        g_ai[i].time = 0;
    } else {
        g_ai[i].time += dt;
        if (g_ai[i].time < g_ai_step) {
            return 1;              /* what Drive returns; the caller does not use it */
        }
        dt = g_ai[i].time > 0.2f ? 0.2f : g_ai[i].time;   /* the main loop's own cap */
        g_ai[i].time = 0;
    }
    return ((int (__fastcall *)(char *, void *, float))AI_DRIVE)(bike, unused, dt);
}

static void install_ai(int hz)
{
    if (hz <= 0) {
        return;
    }
    g_ai_step = 1.0f / (float)hz;
    if (mcm_redirect_call(AI_DRIVE_CALL, AI_DRIVE, ai_drive)) {
        mcm_log("framerate: computer riders drive at most %d times per second", hz);
    } else {
        mcm_log("framerate: the computer riders' call (0x497bd7) does not match, left alone");
    }
}

/* ---------------------------------------------------------------- frame cap */

static void on_present(void *surface)
{
    double now, wait;
    (void)surface;
    if (g_frame_s <= 0) {
        return;
    }
    now = seconds();
    if (g_next_s <= 0 || now - g_next_s > 0.25) {
        g_next_s = now;               /* first frame, or after a long one (loading): start over */
    }
    wait = g_next_s - now;
    if (wait > 0.002) {
        Sleep((DWORD)((wait - 0.0015) * 1000.0));
    }
    while (seconds() < g_next_s) {
        YieldProcessor();
    }
    g_next_s += g_frame_s;
}

static int desktop_refresh(void)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        return (int)dm.dmDisplayFrequency;
    }
    return 60;
}

void mcm_framerate_install(void)
{
    static const uint8_t timer_start[] = { 0x56, 0x6a, 0x01, 0xff, 0x15, 0x98, 0x53, 0x55, 0x00 };
    static const uint8_t dt_start[] = { 0x8b, 0x87, 0xd8, 0x02, 0x00, 0x00, 0x8b, 0x8f, 0xdc, 0x02, 0x00, 0x00, 0x2b, 0xc1 };
    int max_fps;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_base = timeGetTime();

    {
        char v[32];
        if (GetEnvironmentVariableA("MCM_TEST_DT", v, sizeof v) && atof(v) > 0) {
            g_test_dt = (float)atof(v);
            mcm_log("framerate: test time step %.5f s", g_test_dt);
        }
    }
    if (mcm_ini_int("Framerate", "PreciseTimer", 1)) {
        if (mcm_expect(TIMER_FN, timer_start, sizeof timer_start) && mcm_jmp(TIMER_FN, timer_ms)) {
            mcm_log("framerate: the game clock runs on the performance counter");
        } else {
            mcm_log("framerate: the game clock (0x4c41d0) does not match, left alone");
        }
        if (mcm_expect(FRAME_DT, dt_start, sizeof dt_start) && mcm_redirect_call(FRAME_NOW_CALL, TIMER_FN, frame_now_ms)
            && mcm_jmp(FRAME_DT, frame_dt_stub)) {
            mcm_log("framerate: frame time steps measured to the microsecond");
        } else {
            mcm_log("framerate: the main loop (0x469629) does not match, its time step is left alone");
        }
    }

    install_ai(mcm_ini_int("Framerate", "AiRate", 60));

    max_fps = mcm_ini_int("Framerate", "MaxFps", 0);
    if (max_fps == 0) {
        max_fps = desktop_refresh();
    }
    if (max_fps > 0) {
        g_frame_s = 1.0 / max_fps;
        mcm_d3d.on_present = on_present;
        mcm_log("framerate: at most %d frames per second", max_fps);
    } else {
        mcm_log("framerate: no frame cap");
    }
}
