/* Gamepads through SDL2, with rumble.
 *
 * MCM2's input (PCControl.cpp 0x4c3990): DirectInputCreateEx(IID_IDirectInput7A), a keyboard and a mouse
 * device, then EnumDevices(DIDEVTYPE_JOYSTICK, ATTACHEDONLY) -> one JoystickDevice (0x4c6ec0, at most 8) per
 * joystick. JoystickDevice::Init (0x4c7080) does CreateDeviceEx(IID_IDirectInputDevice7A), GetDeviceInfo,
 * IDirectInput7::GetDeviceStatus, SetDataFormat(c_dfDIJoystick, 0x55e288), SetCooperativeLevel(EXCLUSIVE |
 * FOREGROUND), GetCapabilities (DIDC_FORCEFEEDBACK -> force feedback on; axes, buttons, POVs), EnumObjects(AXIS)
 * + GetObjectInfo (which of X Y Z Rx Ry Rz exist) and GetProperty(DIPROP_RANGE) per axis (it only uses lMax:
 * the center is (lMax + 1) / 2, so ranges must start at 0), then DIPROP_BUFFERSIZE 16 and Acquire: input is
 * read buffered with GetDeviceData (0x4c7850, "BufferedJoystick", re-Acquire on INPUTLOST / NOTACQUIRED).
 * Devices whose subtype is DIDEVTYPEJOYSTICK_GAMEPAD take a separate path (raw values, 0x4c79ca).
 *
 * Bindings (ui\preset.ctl, the profile's control.ctl): [ControllerN] per controller type (0..2 joysticks,
 * 3 Standard Game Pad, 4 Steering Wheel, 5 Dual Analog Game Pad, 6 keyboard, 7 mouse), KeyK = kind,code for the
 * actions Steer Left/Right, Lean Forward/Back, Accelerate, Brake, Stunts 1-8, Stunts 9-16, Kahuna, Taunting,
 * Thrill Cam, Reverse Cam, Clutch, Back Up; kind 2 = joystick, code -3 X axis, -5 Y, -7 Z, -9 Rx, -11 Ry, -13 Rz
 * (0x4b9e4b), 0.. = buttons. Every joystick preset is: steer X, lean Y, accelerate button 0, brake button 1,
 * stunts buttons 2 and 3, the rest on the keyboard.
 *
 * So the IDirectInput7A the game creates is wrapped: EnumDevices also offers one joystick per SDL gamepad slot
 * (and hides the DirectInput twins of those gamepads), and CreateDevice/CreateDeviceEx of such a joystick
 * returns a fake IDirectInputDevice7A filled from SDL_GameController, laid out for the presets:
 *   X / Y      left stick (D-pad = full deflection)        Z   LT - RT (LT up, RT down, like XInput pads)
 *   Rx / Ry    right stick                                 POV D-pad
 *   buttons    0 A (or RT)  1 B (or LT)  2 X  3 Y  4 LB  5 RB  6 Back  7 Start  8 LS  9 RS  10 RT  11 LT
 * so accelerate / brake are on A / B and on the triggers. Start also presses Escape (pause) and Back the
 * Backspace key (Back Up) through the keyboard ([Gamepad] StartKey / BackKey, DIK codes, 0 = off).
 * Force feedback: pad_rumble.c. Other DirectInput devices (keyboard, mouse, wheels, sticks) are untouched.
 *
 * SDL runs on the game thread: initialised by the first EnumDevices, pumped from the fake devices' reads. The
 * game enumerates once at start-up: every gamepad connected then gets a slot, and [Gamepad] Slots=<n> keeps
 * that many slots offered even when empty, for gamepads plugged in later (default 0: the game makes a new
 * joystick the profile's controller, which would leave a keyboard player without controls). */
#include "pad.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"

/* ---------------------------------------------------------------- GUIDs (no dxguid) */

static const GUID IID_Unknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_DIA = { 0x89521360, 0xaa8a, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DI2A = { 0x5944e662, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DI7A = { 0x9a4cb684, 0x236d, 0x11d3, { 0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae } };
static const GUID IID_DIDevA = { 0x5944e680, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DIDev2A = { 0x5944e682, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DIDev7A = { 0x57d7c6bc, 0x2356, 0x11d3, { 0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae } };
#define OBJ_GUID(d1) { (d1), 0xc9f3, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } }
static const GUID G_XAxis = OBJ_GUID(0xa36d02e0), G_YAxis = OBJ_GUID(0xa36d02e1), G_ZAxis = OBJ_GUID(0xa36d02e2);
static const GUID G_RxAxis = OBJ_GUID(0xa36d02f4), G_RyAxis = OBJ_GUID(0xa36d02f5);
static const GUID G_Button = OBJ_GUID(0xa36d02f0), G_POV = OBJ_GUID(0xa36d02f2);
/* the fake joysticks: {4d434d32-5041-4400-8000-4d43325041 00 0k} ("MCM2PAD", k = slot) */
static const GUID PAD_INSTANCE = { 0x4d434d32, 0x5041, 0x4400, { 0x80, 0x00, 0x4d, 0x43, 0x32, 0x50, 0x41, 0x00 } };
static const GUID PAD_PRODUCT = { 0x4d434d32, 0x5041, 0x4401, { 0x80, 0x00, 0x4d, 0x43, 0x32, 0x50, 0x41, 0x00 } };

/* ---------------------------------------------------------------- settings and SDL slots */

int pad_rumble_strength = 100;
int pad_rumble_sustain = 25;
static int g_on;
static int g_deadzone = 10;
static float g_curve = 2.0f;          /* left stick response exponent, from [Gamepad] Sensitivity */
static int g_trigger_buttons = 1;
static int g_dpad_axes = 1;
static int g_subtype = DIDEVTYPEJOYSTICK_TRADITIONAL;
static int g_hide_twins = 1;
static int g_nslots;                  /* slots offered: the gamepads at start-up, at least g_min_slots */
static int g_min_slots;               /* [Gamepad] Slots: empty slots kept for gamepads plugged in later */
static int g_start_key = 0x01;        /* DIK_ESCAPE */
static int g_back_key = 0x0e;         /* DIK_BACK */
static int g_sdl;                     /* 1 = up, -1 = failed */
static DWORD g_thread;

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
    SDL_JoystickGUID last;
    int had;
    int keys;                         /* bit 0 Start held, bit 1 Back held (keyboard keys down) */
} slot_t;
static slot_t g_pads[PAD_MAX];

SDL_GameController *pad_controller(int slot)
{
    return slot >= 0 && slot < PAD_MAX ? g_pads[slot].gc : NULL;
}

static int find_id(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < PAD_MAX; i++) {
        if (g_pads[i].gc && g_pads[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void open_pad(int index)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    int i, slot = -1;
    if (!SDL_IsGameController(index) || id < 0 || find_id(id) >= 0) {
        return;
    }
    for (i = 0; i < g_nslots && slot < 0; i++) {   /* the slot this pad had before */
        if (!g_pads[i].gc && g_pads[i].had && !memcmp(&g_pads[i].last, &guid, sizeof guid)) {
            slot = i;
        }
    }
    for (i = 0; i < g_nslots && slot < 0; i++) {
        if (!g_pads[i].gc) {
            slot = i;
        }
    }
    if (slot < 0) {
        mcm_log("gamepad: %s connected, but all %d gamepad slots are taken", SDL_GameControllerNameForIndex(index), g_nslots);
        return;
    }
    g_pads[slot].gc = SDL_GameControllerOpen(index);
    if (!g_pads[slot].gc) {
        mcm_log("gamepad: cannot open %s: %s", SDL_GameControllerNameForIndex(index), SDL_GetError());
        return;
    }
    g_pads[slot].id = id;
    g_pads[slot].last = guid;
    g_pads[slot].had = 1;
    mcm_log("gamepad: %s connected as gamepad %d (VID %04x PID %04x, rumble %s)", SDL_GameControllerName(g_pads[slot].gc),
            slot + 1, SDL_GameControllerGetVendor(g_pads[slot].gc), SDL_GameControllerGetProduct(g_pads[slot].gc),
            SDL_GameControllerHasRumble(g_pads[slot].gc) ? "yes" : "no");
}

static void close_pad(int slot)
{
    mcm_log("gamepad: %s disconnected (gamepad %d)", SDL_GameControllerName(g_pads[slot].gc), slot + 1);
    SDL_GameControllerClose(g_pads[slot].gc);
    g_pads[slot].gc = NULL;
}

static int sdl_init(void)
{
    int i, n;
    if (g_sdl) {
        return g_sdl == 1;
    }
    g_thread = GetCurrentThreadId();
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Xbox pads through XInput, not SDL's RawInput driver, which can only rumble once it has matched the device
       with its XInput twin by input (the DethKarz patch found this, 2026-10-21) */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        mcm_log("gamepad: SDL_Init failed: %s", SDL_GetError());
        g_sdl = -1;
        return 0;
    }
    {
        /* optional extra mappings (https://github.com/mdqinc/SDL_GameControllerDB) next to the exe */
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%sgamecontrollerdb.txt", mcm_game_dir());
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            mcm_log("gamepad: %d mappings from %s", SDL_GameControllerAddMappingsFromFile(path), path);
        }
    }
    {
        SDL_version v;
        SDL_GetVersion(&v);
        mcm_log("gamepad: SDL %d.%d.%d", v.major, v.minor, v.patch);
    }
    g_sdl = 1;
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            int connected = 0, k;
            for (k = 0; k < PAD_MAX; k++) {
                connected += g_pads[k].gc != NULL;
            }
            if (connected >= g_nslots && g_nslots < PAD_MAX) {
                g_nslots++;           /* every gamepad there at start-up gets a slot */
            }
            open_pad(i);
        }
    }
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    return 1;
}

/* the keyboard key a pad button stands for, pressed / released through the system (DirectInput sees it) */
static void key(int dik, int down)
{
    INPUT in;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    GetWindowThreadProcessId(fg, &pid);
    if (!dik || pid != GetCurrentProcessId()) {
        return;
    }
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)dik;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof in);
}

static void pump(void)
{
    static DWORD last;
    SDL_Event ev[16];
    DWORD now;
    int n, i, slot;
    if (g_sdl != 1 || GetCurrentThreadId() != g_thread) {
        return;
    }
    now = timeGetTime();
    if (now - last < 2) {
        return;
    }
    last = now;
    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED)) > 0) {
        for (i = 0; i < n; i++) {
            if (ev[i].type == SDL_CONTROLLERDEVICEADDED) {
                open_pad(ev[i].cdevice.which);
            } else if (ev[i].type == SDL_CONTROLLERDEVICEREMOVED && (slot = find_id(ev[i].cdevice.which)) >= 0) {
                close_pad(slot);
            }
        }
    }
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (i = 0; i < PAD_MAX; i++) {
        int start = g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, SDL_CONTROLLER_BUTTON_START);
        int back = g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, SDL_CONTROLLER_BUTTON_BACK);
        if (start != (g_pads[i].keys & 1)) {
            key(g_start_key, start);
        }
        if (back != ((g_pads[i].keys >> 1) & 1)) {
            key(g_back_key, back);
        }
        g_pads[i].keys = start | back << 1;
    }
    padfx_frame();
}

/* ---------------------------------------------------------------- the pad as a joystick */

enum { O_AXIS, O_BUTTON, O_POV };
#define NAXES    5
#define NBUTTONS 12
#define NOBJS    (NAXES + NBUTTONS + 1)

typedef struct { int kind, inst; const GUID *guid; const char *name; } object_t;
static const object_t OBJS[NOBJS] = {
    { O_AXIS, 0, &G_XAxis, "X Axis (left stick)" }, { O_AXIS, 1, &G_YAxis, "Y Axis (left stick)" },
    { O_AXIS, 2, &G_ZAxis, "Z Axis (triggers)" }, { O_AXIS, 3, &G_RxAxis, "X Rotation (right stick)" },
    { O_AXIS, 4, &G_RyAxis, "Y Rotation (right stick)" },
    { O_BUTTON, 0, &G_Button, "A (or RT)" }, { O_BUTTON, 1, &G_Button, "B (or LT)" }, { O_BUTTON, 2, &G_Button, "X" },
    { O_BUTTON, 3, &G_Button, "Y" }, { O_BUTTON, 4, &G_Button, "LB" }, { O_BUTTON, 5, &G_Button, "RB" },
    { O_BUTTON, 6, &G_Button, "Back" }, { O_BUTTON, 7, &G_Button, "Start" }, { O_BUTTON, 8, &G_Button, "Left Stick" },
    { O_BUTTON, 9, &G_Button, "Right Stick" }, { O_BUTTON, 10, &G_Button, "RT" }, { O_BUTTON, 11, &G_Button, "LT" },
    { O_POV, 0, &G_POV, "D-pad" },
};

/* the pad's controls right now: axes -1..1, buttons, POV in hundredths of a degree (-1 = centred) */
typedef struct { float axis[NAXES]; BYTE button[NBUTTONS]; DWORD pov; } padstate_t;

static float stick(Sint16 v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

static void read_pad(int slot, padstate_t *s)
{
    SDL_GameController *gc = pad_controller(slot);
    float lx, ly, mag, dz = g_deadzone / 100.0f, lt, rt;
    int up, down, left, right;
    memset(s, 0, sizeof *s);
    s->pov = (DWORD)-1;
    if (!gc) {
        return;
    }
    lx = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
    ly = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    mag = sqrtf(lx * lx + ly * ly);
    if (mag <= dz) {                  /* radial dead zone, the rest rescaled to the full range */
        lx = ly = 0;
    } else if (dz > 0) {
        float k = (mag > 1 ? 1 : mag);
        k = (k - dz) / (1 - dz) / mag;
        lx *= k;
        ly *= k;
    }
    up = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
    down = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    left = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    right = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (g_curve != 1.0f) {
        /* a thumbstick has a short throw: the response curve gives finer control near the centre and keeps full
           deflection at full lock */
        lx = (lx < 0 ? -1.0f : 1.0f) * powf(fabsf(lx) > 1 ? 1 : fabsf(lx), g_curve);
        ly = (ly < 0 ? -1.0f : 1.0f) * powf(fabsf(ly) > 1 ? 1 : fabsf(ly), g_curve);
    }
    if (g_dpad_axes) {
        if (left != right) lx = left ? -1.0f : 1.0f;
        if (up != down) ly = up ? -1.0f : 1.0f;
    }
    /* the game cubes the steering of a joystick (device class 2 / 3, not the gamepad subtype) before it reaches the
       bike (0x52e471: +0x504 = s * s * s), and keys and mice steer linearly: the stick goes in as the cube root, so
       the response above is what the bike gets. 0.95 undoes the full lock at 95% (0x4c77e0), so the root's steep
       start is not cut off at the top either */
    lx = (lx < 0 ? -0.95f : 0.95f) * cbrtf(fabsf(lx) > 1 ? 1 : fabsf(lx));
    lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
    rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    s->axis[0] = lx;
    s->axis[1] = ly;
    s->axis[2] = lt - rt;
    s->axis[3] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
    s->axis[4] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
    {
        static const SDL_GameControllerButton map[10] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        };
        int i;
        for (i = 0; i < 10; i++) {
            s->button[i] = SDL_GameControllerGetButton(gc, map[i]) ? 0x80 : 0;
        }
        s->button[10] = rt > 0.3f ? 0x80 : 0;
        s->button[11] = lt > 0.3f ? 0x80 : 0;
        if (g_trigger_buttons) {
            s->button[0] |= s->button[10];
            s->button[1] |= s->button[11];
        }
    }
    if (up || down || left || right) {
        int x = right - left, y = down - up;   /* screen: y down */
        static const DWORD dirs[3][3] = { { 31500, 0, 4500 }, { 27000, (DWORD)-1, 9000 }, { 22500, 18000, 13500 } };
        s->pov = dirs[y + 1][x + 1];
    }
}

/* ---------------------------------------------------------------- the fake IDirectInputDevice7A */

#define QUEUE_MAX 256

typedef struct paddev {
    void **vtbl;
    LONG ref;
    int slot;
    int acquired;
    HANDLE event;
    /* the data format the game set: per format object, the pad object (or -1) */
    int nfmt;
    DWORD datasize;
    DWORD fmt_ofs[64], fmt_type[64];
    int fmt_obj[64];
    int obj_fmt[NOBJS];               /* pad object -> format object index (or -1) */
    DWORD last[64];                   /* what the game was told (buffered reads) */
    int primed;
    /* properties */
    LONG rmin[NAXES], rmax[NAXES];
    DWORD dead[NAXES], sat[NAXES];
    DWORD bufsize;
    /* buffered data */
    DIDEVICEOBJECTDATA_DX3 queue[QUEUE_MAX];
    int qhead, qcount, overflow;
    DWORD seq;
    padstate_t now;
} paddev;

static void *g_dev_vtbl[];

static int dev_slot_of(REFGUID guid)
{
    GUID g = PAD_INSTANCE;
    int s;
    for (s = 0; s < PAD_MAX; s++) {
        g.Data4[7] = (BYTE)s;
        if (IsEqualGUID(guid, &g)) {
            return s;
        }
    }
    return -1;
}

static DWORD obj_type(int o)
{
    switch (OBJS[o].kind) {
    case O_AXIS:
        return DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(OBJS[o].inst) | (o < 2 && pad_rumble_strength > 0 ? DIDFT_FFACTUATOR : 0);
    case O_BUTTON:
        return DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(OBJS[o].inst) | (pad_rumble_strength > 0 ? DIDFT_FFEFFECTTRIGGER : 0);
    default:
        return DIDFT_POV | DIDFT_MAKEINSTANCE(OBJS[o].inst);
    }
}

/* the default offset of a pad object (DIJOYSTATE layout), used before the game sets a data format */
static DWORD default_ofs(int o)
{
    switch (OBJS[o].kind) {
    case O_AXIS: return (DWORD)(OBJS[o].inst < 3 ? OBJS[o].inst * 4 : 12 + (OBJS[o].inst - 3) * 4);
    case O_BUTTON: return 0x30 + (DWORD)OBJS[o].inst;
    default: return 0x20;
    }
}

static DWORD obj_ofs(paddev *d, int o)
{
    return d->nfmt ? (d->obj_fmt[o] >= 0 ? d->fmt_ofs[d->obj_fmt[o]] : (DWORD)-1) : default_ofs(o);
}

static int kind_of_type(DWORD type)
{
    if (type & DIDFT_AXIS) return O_AXIS;
    if (type & DIDFT_BUTTON) return O_BUTTON;
    if (type & DIDFT_POV) return O_POV;
    return -1;
}

/* a pad object from a DIPH_BYOFFSET / DIPH_BYID reference */
static int find_obj(paddev *d, DWORD obj, DWORD how)
{
    int o;
    for (o = 0; o < NOBJS; o++) {
        if ((how == DIPH_BYOFFSET && obj_ofs(d, o) == obj)
            || (how == DIPH_BYID && (obj_type(o) & 0x00ffffff) == (obj & 0x00ffffff))) {
            return o;
        }
    }
    return -1;
}

/* the raw value of a pad object as the game's data format wants it */
static DWORD raw_value(paddev *d, int o, const padstate_t *s)
{
    if (OBJS[o].kind == O_AXIS) {
        int a = OBJS[o].inst;
        float v = s->axis[a], dz = d->dead[a] / 10000.0f, sat = d->sat[a] / 10000.0f, m = fabsf(v);
        if (m <= dz) {
            v = 0;
        } else if (sat > dz) {
            m = (m - dz) / (sat - dz);
            v = (v < 0 ? -1 : 1) * (m > 1 ? 1 : m);
        }
        return (DWORD)(LONG)(d->rmin[a] + (double)(v + 1) / 2 * ((double)d->rmax[a] - d->rmin[a]) + 0.5);
    }
    if (OBJS[o].kind == O_BUTTON) {
        return s->button[OBJS[o].inst];
    }
    return s->pov;
}

/* the format object's value when no pad object feeds it */
static DWORD empty_value(paddev *d, int f)
{
    (void)d;
    return kind_of_type(d->fmt_type[f]) == O_POV ? (DWORD)-1 : 0;
}

static void push_event(paddev *d, DWORD ofs, DWORD data)
{
    DIDEVICEOBJECTDATA_DX3 *ev;
    DWORD cap = d->bufsize < QUEUE_MAX ? d->bufsize : QUEUE_MAX;
    if (!cap) {
        return;
    }
    if ((DWORD)d->qcount >= cap) {
        d->qhead = (d->qhead + 1) % QUEUE_MAX;   /* the oldest is lost, like DirectInput */
        d->qcount--;
        d->overflow = 1;
    }
    ev = &d->queue[(d->qhead + d->qcount) % QUEUE_MAX];
    ev->dwOfs = ofs;
    ev->dwData = data;
    ev->dwTimeStamp = GetTickCount();
    ev->dwSequence = ++d->seq;
    d->qcount++;
}

/* reads the pad; queues the changes for GetDeviceData */
static void refresh(paddev *d)
{
    int f;
    pump();
    read_pad(d->slot, &d->now);
    if (!d->nfmt) {
        return;
    }
    for (f = 0; f < d->nfmt; f++) {
        DWORD v = d->fmt_obj[f] >= 0 ? raw_value(d, d->fmt_obj[f], &d->now) : empty_value(d, f);
        if (d->primed && v != d->last[f] && d->acquired) {
            push_event(d, d->fmt_ofs[f], v);
        }
        d->last[f] = v;
    }
    d->primed = 1;
}

int pad_trigger_down(void *device, DWORD ofs)
{
    paddev *d = device;
    int o = find_obj(d, ofs, DIPH_BYOFFSET);
    return o >= 0 && OBJS[o].kind == O_BUTTON && d->now.button[OBJS[o].inst];
}

#define DEV paddev *d = (paddev *)self

static HRESULT WINAPI dev_QueryInterface(void *self, REFIID iid, void **out)
{
    if (!out) {
        return E_POINTER;
    }
    if (IsEqualGUID(iid, &IID_DIDev7A) || IsEqualGUID(iid, &IID_DIDev2A) || IsEqualGUID(iid, &IID_DIDevA)
        || IsEqualGUID(iid, &IID_Unknown_)) {
        *out = self;
        ((paddev *)self)->ref++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dev_AddRef(void *self) { DEV; return (ULONG)++d->ref; }

static ULONG WINAPI dev_Release(void *self)
{
    DEV;
    LONG n = --d->ref;
    if (n > 0) {
        return (ULONG)n;
    }
    padfx_device_gone(d);
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}

static HRESULT WINAPI dev_GetCapabilities(void *self, LPDIDEVCAPS caps)
{
    DEV;
    DWORD size;
    if (!caps || (caps->dwSize != sizeof(DIDEVCAPS) && caps->dwSize != sizeof(DIDEVCAPS_DX3))) {
        return DIERR_INVALIDPARAM;
    }
    size = caps->dwSize;
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED | DIDC_EMULATED | (pad_rumble_strength > 0 ? DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE : 0);
    caps->dwDevType = DIDEVTYPE_JOYSTICK | g_subtype << 8 | DIDEVTYPE_HID;
    caps->dwAxes = NAXES;
    caps->dwButtons = NBUTTONS;
    caps->dwPOVs = 1;
    if (size == sizeof(DIDEVCAPS)) {
        caps->dwFFSamplePeriod = 10000;
        caps->dwFFMinTimeResolution = 1000;
        caps->dwFirmwareRevision = 1;
        caps->dwHardwareRevision = 1;
        caps->dwFFDriverVersion = 1;
    }
    (void)d;
    return DI_OK;
}

static void fill_object(paddev *d, int o, LPDIDEVICEOBJECTINSTANCEA oi)
{
    DWORD size = oi->dwSize;
    memset(oi, 0, size);
    oi->dwSize = size;
    oi->guidType = *OBJS[o].guid;
    oi->dwOfs = obj_ofs(d, o);
    oi->dwType = obj_type(o);
    oi->dwFlags = (oi->dwType & DIDFT_FFACTUATOR ? DIDOI_FFACTUATOR : 0) | (oi->dwType & DIDFT_FFEFFECTTRIGGER ? DIDOI_FFEFFECTTRIGGER : 0)
                  | (OBJS[o].kind == O_AXIS ? DIDOI_ASPECTPOSITION : 0);
    lstrcpynA(oi->tszName, OBJS[o].name, MAX_PATH);
    if (size == sizeof(DIDEVICEOBJECTINSTANCEA)) {
        oi->dwFFMaxForce = oi->dwFlags & DIDOI_FFACTUATOR ? 10000 : 0;
        oi->dwFFForceResolution = oi->dwFFMaxForce ? 1 : 0;
        oi->wUsagePage = OBJS[o].kind == O_BUTTON ? 9 : 1;
        oi->wUsage = OBJS[o].kind == O_BUTTON ? (WORD)(OBJS[o].inst + 1)
                     : OBJS[o].kind == O_POV ? 0x39 : (WORD)(0x30 + OBJS[o].inst);
    }
}

static HRESULT WINAPI dev_EnumObjects(void *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *ref, DWORD flags)
{
    DEV;
    int o;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    for (o = 0; o < NOBJS; o++) {
        DIDEVICEOBJECTINSTANCEA oi;
        DWORD t = obj_type(o);
        if (flags != DIDFT_ALL && !(t & flags & 0xff)) {
            continue;
        }
        if ((flags & DIDFT_FFACTUATOR) && !(t & DIDFT_FFACTUATOR)) continue;
        if ((flags & DIDFT_FFEFFECTTRIGGER) && !(t & DIDFT_FFEFFECTTRIGGER)) continue;
        oi.dwSize = sizeof oi;
        fill_object(d, o, &oi);
        if (cb(&oi, ref) == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

/* which axes a property header names (DIPH_DEVICE = all) */
static int prop_axes(paddev *d, LPCDIPROPHEADER h, int *first, int *last)
{
    int o;
    if (h->dwHow == DIPH_DEVICE) {
        *first = 0;
        *last = NAXES - 1;
        return 1;
    }
    o = find_obj(d, h->dwObj, h->dwHow);
    if (o < 0 || OBJS[o].kind != O_AXIS) {
        return 0;
    }
    *first = *last = OBJS[o].inst;
    return 1;
}

static HRESULT WINAPI dev_GetProperty(void *self, REFGUID prop, LPDIPROPHEADER h)
{
    DEV;
    uintptr_t id = (uintptr_t)prop;
    int a, b;
    if (!h) {
        return DIERR_INVALIDPARAM;
    }
    if (id == (uintptr_t)DIPROP_BUFFERSIZE) {
        ((LPDIPROPDWORD)h)->dwData = d->bufsize;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_FFGAIN || id == (uintptr_t)DIPROP_FFLOAD) {
        ((LPDIPROPDWORD)h)->dwData = id == (uintptr_t)DIPROP_FFGAIN ? 10000 : 0;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AUTOCENTER) {
        ((LPDIPROPDWORD)h)->dwData = DIPROPAUTOCENTER_OFF;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_GRANULARITY) {
        ((LPDIPROPDWORD)h)->dwData = 1;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AXISMODE) {
        ((LPDIPROPDWORD)h)->dwData = DIPROPAXISMODE_ABS;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_RANGE || id == (uintptr_t)DIPROP_DEADZONE || id == (uintptr_t)DIPROP_SATURATION) {
        if (h->dwHow == DIPH_DEVICE || !prop_axes(d, h, &a, &b)) {
            return DIERR_OBJECTNOTFOUND;
        }
        if (id == (uintptr_t)DIPROP_RANGE) {
            ((LPDIPROPRANGE)h)->lMin = d->rmin[a];
            ((LPDIPROPRANGE)h)->lMax = d->rmax[a];
        } else {
            ((LPDIPROPDWORD)h)->dwData = id == (uintptr_t)DIPROP_DEADZONE ? d->dead[a] : d->sat[a];
        }
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SetProperty(void *self, REFGUID prop, LPCDIPROPHEADER h)
{
    DEV;
    uintptr_t id = (uintptr_t)prop;
    int a, b, i;
    if (!h) {
        return DIERR_INVALIDPARAM;
    }
    if (id == (uintptr_t)DIPROP_BUFFERSIZE) {
        d->bufsize = ((LPCDIPROPDWORD)h)->dwData;
        d->qcount = 0;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_FFGAIN) {
        padfx_set_gain(d->slot, ((LPCDIPROPDWORD)h)->dwData);
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AUTOCENTER || id == (uintptr_t)DIPROP_AXISMODE || id == (uintptr_t)DIPROP_CALIBRATIONMODE) {
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_RANGE || id == (uintptr_t)DIPROP_DEADZONE || id == (uintptr_t)DIPROP_SATURATION) {
        if (!prop_axes(d, h, &a, &b)) {
            return DIERR_OBJECTNOTFOUND;
        }
        for (i = a; i <= b; i++) {
            if (id == (uintptr_t)DIPROP_RANGE) {
                if (((LPCDIPROPRANGE)h)->lMin >= ((LPCDIPROPRANGE)h)->lMax) {
                    return DIERR_INVALIDPARAM;
                }
                d->rmin[i] = ((LPCDIPROPRANGE)h)->lMin;
                d->rmax[i] = ((LPCDIPROPRANGE)h)->lMax;
            } else {
                DWORD v = ((LPCDIPROPDWORD)h)->dwData;
                v = v > 10000 ? 10000 : v;
                if (id == (uintptr_t)DIPROP_DEADZONE) {
                    d->dead[i] = v;
                } else {
                    d->sat[i] = v;
                }
            }
        }
        d->primed = 0;
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_Acquire(void *self)
{
    DEV;
    if (!d->nfmt) {
        return DIERR_INVALIDPARAM;
    }
    if (d->acquired) {
        return S_FALSE;
    }
    d->acquired = 1;
    d->primed = 0;
    d->qcount = 0;
    refresh(d);
    return DI_OK;
}

static HRESULT WINAPI dev_Unacquire(void *self)
{
    DEV;
    if (!d->acquired) {
        return DI_NOEFFECT;
    }
    d->acquired = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceState(void *self, DWORD size, void *data)
{
    DEV;
    int f;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (!data || size != d->datasize) {
        return DIERR_INVALIDPARAM;
    }
    refresh(d);
    memset(data, 0, size);
    for (f = 0; f < d->nfmt; f++) {
        DWORD v = d->last[f];
        BYTE *p = (BYTE *)data + d->fmt_ofs[f];
        if (kind_of_type(d->fmt_type[f]) == O_BUTTON) {
            if (d->fmt_ofs[f] < size) *p = (BYTE)v;
        } else if (d->fmt_ofs[f] + 4 <= size) {
            memcpy(p, &v, 4);
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceData(void *self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags)
{
    DEV;
    DWORD want, n = 0;
    HRESULT hr;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (!d->bufsize) {
        return DIERR_NOTBUFFERED;
    }
    if (!inout || (rgdod && cb < sizeof(DIDEVICEOBJECTDATA_DX3))) {
        return DIERR_INVALIDPARAM;
    }
    refresh(d);
    want = *inout;
    while (n < want && n < (DWORD)d->qcount) {
        if (rgdod) {
            BYTE *dst = (BYTE *)rgdod + n * cb;
            memset(dst, 0, cb);
            memcpy(dst, &d->queue[(d->qhead + n) % QUEUE_MAX], sizeof(DIDEVICEOBJECTDATA_DX3));
        }
        n++;
    }
    if (!(flags & DIGDD_PEEK)) {
        d->qhead = (d->qhead + n) % QUEUE_MAX;
        d->qcount -= n;
    }
    *inout = n;
    hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
        d->overflow = 0;
    }
    return hr;
}

static HRESULT WINAPI dev_SetDataFormat(void *self, LPCDIDATAFORMAT fmt)
{
    DEV;
    DWORD i;
    int used[NOBJS] = { 0 }, o;
    if (!fmt || fmt->dwSize != sizeof(DIDATAFORMAT) || fmt->dwObjSize != sizeof(DIOBJECTDATAFORMAT) || fmt->dwNumObjs > 64) {
        return DIERR_INVALIDPARAM;
    }
    if (d->acquired) {
        return DIERR_ACQUIRED;
    }
    d->nfmt = (int)fmt->dwNumObjs;
    d->datasize = fmt->dwDataSize;
    for (o = 0; o < NOBJS; o++) {
        d->obj_fmt[o] = -1;
    }
    for (i = 0; i < fmt->dwNumObjs; i++) {
        const DIOBJECTDATAFORMAT *f = &fmt->rgodf[i];
        int kind = kind_of_type(f->dwType);
        DWORD inst = DIDFT_GETINSTANCE(f->dwType);
        d->fmt_ofs[i] = f->dwOfs;
        d->fmt_type[i] = f->dwType;
        d->fmt_obj[i] = -1;
        for (o = 0; o < NOBJS; o++) {
            if (used[o] || OBJS[o].kind != kind) continue;
            if (f->pguid && !IsEqualGUID(f->pguid, OBJS[o].guid)) continue;
            if (inst != (DIDFT_ANYINSTANCE >> 8) && inst != (DWORD)OBJS[o].inst) continue;
            used[o] = 1;
            d->fmt_obj[i] = o;
            d->obj_fmt[o] = (int)i;
            break;
        }
    }
    d->primed = 0;
    d->qcount = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_SetEventNotification(void *self, HANDLE ev) { DEV; d->event = ev; return DI_OK; }
static HRESULT WINAPI dev_SetCooperativeLevel(void *self, HWND w, DWORD flags) { (void)self; (void)w; (void)flags; return DI_OK; }

static HRESULT WINAPI dev_GetObjectInfo(void *self, LPDIDEVICEOBJECTINSTANCEA oi, DWORD obj, DWORD how)
{
    DEV;
    int o;
    if (!oi || (oi->dwSize != sizeof(DIDEVICEOBJECTINSTANCEA) && oi->dwSize != sizeof(DIDEVICEOBJECTINSTANCE_DX3A))) {
        return DIERR_INVALIDPARAM;
    }
    o = find_obj(d, obj, how);
    if (o < 0) {
        return DIERR_OBJECTNOTFOUND;
    }
    fill_object(d, o, oi);
    return DI_OK;
}

static void fill_instance(int slot, LPDIDEVICEINSTANCEA di)
{
    DWORD size = di->dwSize;
    SDL_GameController *gc = pad_controller(slot);
    memset(di, 0, size);
    di->dwSize = size;
    di->guidInstance = PAD_INSTANCE;
    di->guidInstance.Data4[7] = (BYTE)slot;
    di->guidProduct = PAD_PRODUCT;
    di->guidProduct.Data4[7] = (BYTE)slot;
    di->dwDevType = DIDEVTYPE_JOYSTICK | g_subtype << 8 | DIDEVTYPE_HID;
    snprintf(di->tszInstanceName, MAX_PATH, "Gamepad %d", slot + 1);
    snprintf(di->tszProductName, MAX_PATH, "%s", gc && SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "Gamepad (SDL)");
    if (size == sizeof(DIDEVICEINSTANCEA)) {
        if (pad_rumble_strength > 0) {
            di->guidFFDriver = PAD_PRODUCT;
        }
        di->wUsagePage = 1;
        di->wUsage = 5;               /* game pad */
    }
}

static HRESULT WINAPI dev_GetDeviceInfo(void *self, LPDIDEVICEINSTANCEA di)
{
    DEV;
    if (!di || (di->dwSize != sizeof(DIDEVICEINSTANCEA) && di->dwSize != sizeof(DIDEVICEINSTANCE_DX3A))) {
        return DIERR_INVALIDPARAM;
    }
    fill_instance(d->slot, di);
    return DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(void *self, HWND w, DWORD flags) { (void)self; (void)w; (void)flags; return DI_OK; }
static HRESULT WINAPI dev_Initialize(void *self, HINSTANCE i, DWORD v, REFGUID g) { (void)self; (void)i; (void)v; (void)g; return DI_OK; }

static HRESULT WINAPI dev_CreateEffect(void *self, REFGUID guid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
    DEV;
    HRESULT hr;
    if (outer) {
        return DIERR_NOAGGREGATION;
    }
    if (pad_rumble_strength <= 0) {
        return DIERR_UNSUPPORTED;
    }
    hr = padfx_create(d, d->slot, guid, eff, out);
    if (SUCCEEDED(hr) && eff) {
        static int logged;
        if (logged++ < 8) {
            DIEFFECTINFOA info;
            info.dwSize = sizeof info;
            padfx_supported(guid, &info);
            mcm_log("gamepad: effect %s created on gamepad %d (duration %lu us, gain %lu)", info.tszName, d->slot + 1,
                    eff->dwDuration, eff->dwGain);
        }
    }
    return hr;
}

static HRESULT WINAPI dev_EnumEffects(void *self, LPDIENUMEFFECTSCALLBACKA cb, void *ref, DWORD type)
{
    (void)self;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    if (pad_rumble_strength > 0) {
        padfx_enum_effects(cb, ref, type);
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetEffectInfo(void *self, LPDIEFFECTINFOA info, REFGUID guid)
{
    (void)self;
    if (!info || info->dwSize != sizeof(DIEFFECTINFOA)) {
        return DIERR_INVALIDPARAM;
    }
    return padfx_supported(guid, info) ? DI_OK : DIERR_DEVICENOTREG;
}

static HRESULT WINAPI dev_GetForceFeedbackState(void *self, LPDWORD out)
{
    DEV;
    if (!out) {
        return DIERR_INVALIDPARAM;
    }
    *out = padfx_state(d, d->slot);
    return DI_OK;
}

static HRESULT WINAPI dev_SendForceFeedbackCommand(void *self, DWORD cmd)
{
    DEV;
    padfx_command(d, d->slot, cmd);
    return DI_OK;
}

static HRESULT WINAPI dev_EnumCreatedEffectObjects(void *self, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ref, DWORD flags)
{
    DEV;
    (void)flags;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    padfx_enum_created(d, cb, ref);
    return DI_OK;
}

static HRESULT WINAPI dev_Escape(void *self, LPDIEFFESCAPE esc) { (void)self; (void)esc; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(void *self)
{
    DEV;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    refresh(d);
    return DI_NOEFFECT;
}

static HRESULT WINAPI dev_SendDeviceData(void *self, DWORD cb, LPCDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags)
{
    (void)self; (void)cb; (void)data; (void)flags;
    if (inout) {
        *inout = 0;
    }
    return DI_OK;
}

static HRESULT WINAPI dev_EnumEffectsInFile(void *self, LPCSTR file, LPDIENUMEFFECTSINFILECALLBACK cb, void *ref, DWORD flags)
{
    (void)self; (void)file; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_WriteEffectToFile(void *self, LPCSTR file, DWORD n, LPDIFILEEFFECT fx, DWORD flags)
{
    (void)self; (void)file; (void)n; (void)fx; (void)flags;
    return DIERR_UNSUPPORTED;
}

static void *g_dev_vtbl[] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
    dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo, dev_RunControlPanel,
    dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo, dev_GetForceFeedbackState,
    dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll, dev_SendDeviceData,
    dev_EnumEffectsInFile, dev_WriteEffectToFile,
};

static HRESULT create_pad_device(int slot, void **out)
{
    paddev *d;
    int a;
    if (!out) {
        return E_POINTER;
    }
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) {
        return DIERR_OUTOFMEMORY;
    }
    d->vtbl = g_dev_vtbl;
    d->ref = 1;
    d->slot = slot;
    for (a = 0; a < NAXES; a++) {
        d->rmin[a] = 0;
        d->rmax[a] = 65535;           /* DirectInput's default range; MCM2 assumes it starts at 0 */
        d->sat[a] = 10000;
    }
    for (a = 0; a < NOBJS; a++) {
        d->obj_fmt[a] = -1;
    }
    *out = d;
    mcm_log("gamepad: gamepad %d opened by the game (%s)", slot + 1,
            pad_controller(slot) ? SDL_GameControllerName(pad_controller(slot)) : "not connected yet");
    return DI_OK;
}

/* ---------------------------------------------------------------- the IDirectInput7A wrapper */

typedef struct {
    void **vtbl;
    IDirectInput7A *real;
    LONG ref;
} diwrap;

static void *g_di_vtbl[];
#define DI diwrap *w = (diwrap *)self
#define REAL(m) w->real->lpVtbl->m

static HRESULT WINAPI di_QueryInterface(void *self, REFIID iid, void **out)
{
    DI;
    if (out && (IsEqualGUID(iid, &IID_DI7A) || IsEqualGUID(iid, &IID_DI2A) || IsEqualGUID(iid, &IID_DIA)
                || IsEqualGUID(iid, &IID_Unknown_))) {
        *out = self;
        w->ref++;
        REAL(AddRef)(w->real);
        return S_OK;
    }
    return REAL(QueryInterface)(w->real, iid, out);
}

static ULONG WINAPI di_AddRef(void *self) { DI; w->ref++; return REAL(AddRef)(w->real); }

static ULONG WINAPI di_Release(void *self)
{
    DI;
    ULONG n = REAL(Release)(w->real);
    if (--w->ref == 0) {
        HeapFree(GetProcessHeap(), 0, w);
    }
    return n;
}

static HRESULT WINAPI di_CreateDevice(void *self, REFGUID guid, LPDIRECTINPUTDEVICEA *out, LPUNKNOWN outer)
{
    DI;
    int slot = dev_slot_of(guid);
    if (slot >= 0) {
        return outer ? DIERR_NOAGGREGATION : create_pad_device(slot, (void **)out);
    }
    return REAL(CreateDevice)(w->real, guid, out, outer);
}

typedef struct { LPDIENUMDEVICESCALLBACKA cb; void *ref; int stopped, hidden; } enumctx;

/* a DirectInput device that is one of the SDL gamepads (its VID/PID from a HID product GUID, "PIDVID") */
static int is_twin(LPCDIDEVICEINSTANCEA di)
{
    static const BYTE pidvid[8] = { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' };
    int i, n;
    WORD vid, pid;
    if (!g_hide_twins || g_sdl != 1 || GET_DIDEVICE_TYPE(di->dwDevType) != DIDEVTYPE_JOYSTICK
        || memcmp(di->guidProduct.Data4, pidvid, 8)) {
        return 0;
    }
    vid = LOWORD(di->guidProduct.Data1);
    pid = HIWORD(di->guidProduct.Data1);
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        if (SDL_IsGameController(i) && SDL_JoystickGetDeviceVendor(i) == vid && SDL_JoystickGetDeviceProduct(i) == pid) {
            return 1;
        }
    }
    return 0;
}

static BOOL CALLBACK filter(LPCDIDEVICEINSTANCEA di, void *ref)
{
    enumctx *c = ref;
    if (is_twin(di)) {
        c->hidden++;
        mcm_log("gamepad: DirectInput device \"%s\" is an SDL gamepad, hidden", di->tszProductName);
        return DIENUM_CONTINUE;
    }
    if (c->cb(di, c->ref) == DIENUM_STOP) {
        c->stopped = 1;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

static HRESULT WINAPI di_EnumDevices(void *self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, void *ref, DWORD flags)
{
    DI;
    enumctx c = { cb, ref, 0, 0 };
    HRESULT hr;
    int s;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    if (type != 0 && type != DIDEVTYPE_JOYSTICK) {
        return REAL(EnumDevices)(w->real, type, cb, ref, flags);
    }
    sdl_init();
    hr = REAL(EnumDevices)(w->real, type, filter, &c, flags);
    if (FAILED(hr) || c.stopped || g_sdl != 1) {
        return hr;
    }
    if ((flags & DIEDFL_FORCEFEEDBACK) && pad_rumble_strength <= 0) {
        return hr;
    }
    for (s = 0; s < g_nslots; s++) {
        DIDEVICEINSTANCEA di;
        if (!pad_controller(s) && s >= g_min_slots) {
            /* no pad in this slot: offered only as one of the [Gamepad] Slots kept for pads plugged in later.
               The game picks a new joystick as the profile's controller, so an empty slot would take the
               keyboard away from a player without a gamepad */
            continue;
        }
        di.dwSize = sizeof di;
        fill_instance(s, &di);
        mcm_log("gamepad: offering gamepad %d (%s) to the game", s + 1, di.tszProductName);
        if (cb(&di, ref) == DIENUM_STOP) {
            break;
        }
    }
    return hr;
}

static HRESULT WINAPI di_GetDeviceStatus(void *self, REFGUID guid)
{
    DI;
    if (dev_slot_of(guid) >= 0) {
        return DI_OK;
    }
    return REAL(GetDeviceStatus)(w->real, guid);
}

static HRESULT WINAPI di_RunControlPanel(void *self, HWND hwnd, DWORD flags) { DI; return REAL(RunControlPanel)(w->real, hwnd, flags); }
static HRESULT WINAPI di_Initialize(void *self, HINSTANCE inst, DWORD ver) { DI; return REAL(Initialize)(w->real, inst, ver); }
static HRESULT WINAPI di_FindDevice(void *self, REFGUID cls, LPCSTR name, LPGUID out) { DI; return REAL(FindDevice)(w->real, cls, name, out); }

static HRESULT WINAPI di_CreateDeviceEx(void *self, REFGUID guid, REFIID iid, void **out, LPUNKNOWN outer)
{
    DI;
    int slot = dev_slot_of(guid);
    if (slot >= 0) {
        HRESULT hr;
        void *dev;
        if (outer) {
            return DIERR_NOAGGREGATION;
        }
        hr = create_pad_device(slot, &dev);
        if (FAILED(hr)) {
            return hr;
        }
        hr = dev_QueryInterface(dev, iid, out);
        dev_Release(dev);
        return hr;
    }
    return REAL(CreateDeviceEx)(w->real, guid, iid, out, outer);
}

static void *g_di_vtbl[] = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
    di_RunControlPanel, di_Initialize, di_FindDevice, di_CreateDeviceEx,
};

void *mcm_gamepad_wrap_dinput(void *direct_input)
{
    IDirectInput7A *real = direct_input;
    void *check = NULL;
    diwrap *w;
    if (!g_on || !real || *(void ***)real == g_di_vtbl) {
        return direct_input;          /* off, or already wrapped (proxy and import hook both see it) */
    }
    /* only the ANSI interface (IDirectInputA / 2A / 7A share one vtable): the game's IDirectInput7A */
    if (FAILED(real->lpVtbl->QueryInterface(real, &IID_DI7A, &check)) || !check) {
        return direct_input;
    }
    real->lpVtbl->Release(real);
    if (check != direct_input) {
        return direct_input;
    }
    w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    if (!w) {
        return direct_input;
    }
    w->vtbl = g_di_vtbl;
    w->real = real;
    w->ref = 1;
    mcm_log("gamepad: DirectInput wrapped, SDL gamepads are offered as joysticks");
    return w;
}

/* ---------------------------------------------------------------- install */

typedef HRESULT (WINAPI *DirectInputCreateEx_fn)(HINSTANCE, DWORD, REFIID, void **, void *);
static DirectInputCreateEx_fn real_DirectInputCreateEx;

static HRESULT WINAPI hk_DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr = real_DirectInputCreateEx(inst, ver, iid, out, outer);
    if (hr == DI_OK && out && *out && IsEqualGUID(iid, &IID_DI7A)) {
        *out = mcm_gamepad_wrap_dinput(*out);
    }
    return hr;
}

void mcm_gamepad_install(void)
{
    int strength;
    if (!mcm_ini_int("Gamepad", "UseSDL", 1)) {
        mcm_log("gamepad: UseSDL=0, gamepads only through DirectInput");
        return;
    }
    g_deadzone = mcm_ini_int("Gamepad", "Deadzone", 10);
    g_deadzone = g_deadzone < 0 ? 0 : g_deadzone > 90 ? 90 : g_deadzone;
    {
        /* 100 = linear (the stick as it is), 50 = squared, 0 = cubed */
        int sens = mcm_ini_int("Gamepad", "Sensitivity", 50);
        sens = sens < 0 ? 0 : sens > 100 ? 100 : sens;
        g_curve = 1.0f + (float)(100 - sens) / 50.0f;
    }
    g_trigger_buttons = mcm_ini_int("Gamepad", "TriggerButtons", 1);
    g_dpad_axes = mcm_ini_int("Gamepad", "DpadSteers", 1);
    g_hide_twins = mcm_ini_int("Gamepad", "HideDuplicates", 1);
    g_subtype = mcm_ini_int("Gamepad", "Subtype", DIDEVTYPEJOYSTICK_TRADITIONAL);
    g_min_slots = mcm_ini_int("Gamepad", "Slots", 0);
    g_min_slots = g_min_slots < 0 ? 0 : g_min_slots > PAD_MAX ? PAD_MAX : g_min_slots;
    g_nslots = g_min_slots;
    g_start_key = mcm_ini_int("Gamepad", "StartKey", 0x01);
    g_back_key = mcm_ini_int("Gamepad", "BackKey", 0x0e);
    strength = mcm_ini_int("Gamepad", "Rumble", 1) ? mcm_ini_int("Gamepad", "RumbleStrength", 100) : 0;
    pad_rumble_strength = strength < 0 ? 0 : strength > 100 ? 100 : strength;
    pad_rumble_sustain = mcm_ini_int("Gamepad", "RumbleSustain", 25);
    pad_rumble_sustain = pad_rumble_sustain < 0 ? 0 : pad_rumble_sustain > 100 ? 100 : pad_rumble_sustain;
    g_on = 1;
    /* ASI / Interposer builds: nobody wraps the object at creation, so the exe's import does */
    real_DirectInputCreateEx = mcm_hook_import("DINPUT.dll", "DirectInputCreateEx", hk_DirectInputCreateEx);
    mcm_log("gamepad: SDL gamepads on (dead zone %d%%, stick curve %.1f, rumble %d%%, %s)", g_deadzone, g_curve, pad_rumble_strength,
            real_DirectInputCreateEx ? "DirectInputCreateEx hooked" : "DirectInputCreateEx import not found");
}
