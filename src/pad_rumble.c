/* Force feedback effects -> gamepad rumble.
 *
 * MCM2's force feedback (KrustyBike.cpp, JoystickDevice in PCInputDeviceType.cpp) only runs on a joystick whose
 * GetCapabilities has DIDC_FORCEFEEDBACK. It uses two effect slots:
 *   slot 0: GUID_ConstantForce, 70 ms (0x11170 us), gain from Options - Controls "Feedback" (app+0xffc),
 *           started once per impact (the bike's +0x108 flag, 0x497445) -> a short jolt
 *   slot 1: GUID_ConstantForce, infinite, gain 10000; every frame (0x4973a0) its magnitude is set from the bike's
 *           suspension/terrain force (clamped to +-10000, SetParameters DIEP_TYPESPECIFICPARAMS) and it is started
 *           or stopped (Start(1, 0) / Stop) depending on whether the bike is on the ground
 * plus the JoystickDevice helpers for GUID_Square (periodic) effects and SendForceFeedbackCommand (reset 1,
 * pause 4, continue 8). The fake joysticks implement IDirectInputEffect here and turn every playing effect into
 * a motor level: finite constant / ramp forces are jolts on the big (low frequency) motor; an infinite force
 * is mostly felt through its changes (bumps), plus [Gamepad] RumbleSustain percent of its steady level;
 * periodic effects are a buzz on the small motor (short periods) or pulses (long periods). Condition effects
 * (spring, damper, ...) have no rumble equivalent and stay silent. */
#include <math.h>
#include <string.h>
#include "pad.h"
#include "mcm2patch.h"

static const GUID IID_Unknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_Effect = { 0xe7e1f7c0, 0x88d2, 0x11d0, { 0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35 } };
#define FX_GUID(n) { 0x13541c20 + (n), 0x8e33, 0x11d0, { 0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35 } }

enum { K_CONSTANT, K_RAMP, K_PERIODIC, K_CONDITION, K_CUSTOM };

static const struct { GUID guid; int kind; DWORD efftype; const char *name; } FX[] = {
    { FX_GUID(0), K_CONSTANT, DIEFT_CONSTANTFORCE, "Constant Force" },
    { FX_GUID(1), K_RAMP, DIEFT_RAMPFORCE, "Ramp Force" },
    { FX_GUID(2), K_PERIODIC, DIEFT_PERIODIC, "Square" },
    { FX_GUID(3), K_PERIODIC, DIEFT_PERIODIC, "Sine" },
    { FX_GUID(4), K_PERIODIC, DIEFT_PERIODIC, "Triangle" },
    { FX_GUID(5), K_PERIODIC, DIEFT_PERIODIC, "Sawtooth Up" },
    { FX_GUID(6), K_PERIODIC, DIEFT_PERIODIC, "Sawtooth Down" },
    { FX_GUID(7), K_CONDITION, DIEFT_CONDITION, "Spring" },
    { FX_GUID(8), K_CONDITION, DIEFT_CONDITION, "Damper" },
    { FX_GUID(9), K_CONDITION, DIEFT_CONDITION, "Inertia" },
    { FX_GUID(10), K_CONDITION, DIEFT_CONDITION, "Friction" },
    { FX_GUID(11), K_CUSTOM, DIEFT_CUSTOMFORCE, "Custom Force" },
};
#define NFX ((int)(sizeof FX / sizeof FX[0]))

#define MIN_PULSE_MS 120              /* a shorter jolt does not get the motor going */
#define KICK_MS      150              /* how long a change of an infinite force is felt */
#define STALE_MS     250              /* an infinite force the game stopped updating: the race is paused */

typedef struct padfx {
    void **vtbl;
    LONG ref;
    void *device;                     /* the fake joystick (NULL once it was released) */
    int slot;
    int type;                         /* index into FX */
    /* parameters */
    DWORD duration_us, gain, trigger, start_delay_us;
    int has_env;
    DIENVELOPE env;
    LONG mag, ramp_start, ramp_end;   /* constant / ramp */
    DWORD pmag, period_us;            /* periodic */
    LONG poffset;
    /* playback */
    int playing;
    DWORD start_ms, iterations;
    float kick;                       /* infinite constant forces: the last change, fading */
    DWORD kick_ms;
    DWORD touched_ms;                 /* the game's last SetParameters / Start */
    struct padfx *next;
} padfx;

static padfx *g_fx;
static struct {
    int paused, off;
    DWORD gain;                       /* DIPROP_FFGAIN, 0..10000 */
    int low, high;
    DWORD sent;
    int logged;
} g_slot[PAD_MAX];
static int g_init;

static void init_slots(void)
{
    int i;
    if (!g_init) {
        for (i = 0; i < PAD_MAX; i++) {
            g_slot[i].gain = 10000;
        }
        g_init = 1;
    }
}

static int fx_index(REFGUID guid)
{
    int i;
    for (i = 0; i < NFX; i++) {
        if (IsEqualGUID(guid, &FX[i].guid)) {
            return i;
        }
    }
    return -1;
}

static void fill_info(int i, DIEFFECTINFOA *info)
{
    DWORD size = info->dwSize;
    memset(info, 0, size);
    info->dwSize = size;
    info->guid = FX[i].guid;
    info->dwEffType = FX[i].efftype | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY;
    info->dwStaticParams = DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE
                           | DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    info->dwDynamicParams = info->dwStaticParams;
    lstrcpynA(info->tszName, FX[i].name, MAX_PATH);
}

int padfx_supported(REFGUID guid, DIEFFECTINFOA *info)
{
    int i = fx_index(guid);
    if (i >= 0 && info) {
        fill_info(i, info);
    }
    return i >= 0;
}

int padfx_enum_effects(LPDIENUMEFFECTSCALLBACKA cb, void *ref, DWORD type)
{
    int i;
    DWORD want = DIEFT_GETTYPE(type);
    for (i = 0; i < NFX; i++) {
        DIEFFECTINFOA info;
        if (want != DIEFT_ALL && want != DIEFT_GETTYPE(FX[i].efftype)) {
            continue;
        }
        info.dwSize = sizeof info;
        fill_info(i, &info);
        if (cb(&info, ref) == DIENUM_STOP) {
            break;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- parameters */

static HRESULT set_params(padfx *e, LPCDIEFFECT eff, DWORD flags)
{
    if (!eff || (eff->dwSize != sizeof(DIEFFECT) && eff->dwSize != sizeof(DIEFFECT_DX5))) {
        return DIERR_INVALIDPARAM;
    }
    e->touched_ms = timeGetTime();
    if (flags & DIEP_DURATION) {
        e->duration_us = eff->dwDuration;
    }
    if (flags & DIEP_GAIN) {
        e->gain = eff->dwGain > 10000 ? 10000 : eff->dwGain;
    }
    if (flags & DIEP_TRIGGERBUTTON) {
        e->trigger = eff->dwTriggerButton;
    }
    if ((flags & DIEP_STARTDELAY) && eff->dwSize == sizeof(DIEFFECT)) {
        e->start_delay_us = eff->dwStartDelay;
    }
    if (flags & DIEP_ENVELOPE) {
        e->has_env = eff->lpEnvelope != NULL;
        if (e->has_env) {
            e->env = *eff->lpEnvelope;
        }
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) && eff->lpvTypeSpecificParams) {
        switch (FX[e->type].kind) {
        case K_CONSTANT:
            if (eff->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
                LONG m = ((const DICONSTANTFORCE *)eff->lpvTypeSpecificParams)->lMagnitude;
                DWORD now = timeGetTime();
                /* a change of a force that plays on and on is what the rider feels as a bump */
                float delta = fabsf((float)(m - e->mag)) / 10000.0f * 2.0f;
                float left = e->kick * (1.0f - (float)(now - e->kick_ms) / KICK_MS);
                if (left < 0) {
                    left = 0;
                }
                if (delta > left) {
                    e->kick = delta > 1 ? 1 : delta;
                    e->kick_ms = now;
                }
                e->mag = m;
            }
            break;
        case K_RAMP:
            if (eff->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
                e->ramp_start = ((const DIRAMPFORCE *)eff->lpvTypeSpecificParams)->lStart;
                e->ramp_end = ((const DIRAMPFORCE *)eff->lpvTypeSpecificParams)->lEnd;
            }
            break;
        case K_PERIODIC:
            if (eff->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
                const DIPERIODIC *p = eff->lpvTypeSpecificParams;
                e->pmag = p->dwMagnitude;
                e->poffset = p->lOffset;
                e->period_us = p->dwPeriod;
            }
            break;
        }
    }
    return DI_OK;
}

static void start(padfx *e, DWORD iterations)
{
    e->playing = 1;
    e->touched_ms = timeGetTime();
    e->start_ms = e->touched_ms + e->start_delay_us / 1000;
    e->iterations = iterations ? iterations : 1;
}

/* the level of one effect right now (0..1 per motor) */
static void level(padfx *e, DWORD now, float *lo, float *hi)
{
    float m = 0, t, dur;
    int infinite = e->duration_us == INFINITE;
    int active = e->playing;
    if (!active && e->trigger != DIEB_NOTRIGGER && e->device && pad_trigger_down(e->device, e->trigger)) {
        active = 1;                   /* a triggered effect plays while its button is held */
        if (!e->playing) {
            start(e, 1);
            e->playing = 2;           /* started by the trigger: stops with it */
        }
    } else if (e->playing == 2 && !(e->device && pad_trigger_down(e->device, e->trigger))) {
        e->playing = active = 0;
    }
    if (!active || (int)(now - e->start_ms) < 0) {
        return;
    }
    if (infinite && (FX[e->type].kind == K_CONSTANT || FX[e->type].kind == K_RAMP) && now - e->touched_ms > STALE_MS) {
        /* the game sets the road force every frame of a race (0x4973a0) and leaves it playing when the race is
           paused (the in-game menu): silent until it is set again */
        return;
    }
    t = (float)(now - e->start_ms);
    dur = infinite ? 0 : (float)e->duration_us / 1000.0f;
    if (!infinite) {
        float total = e->iterations == INFINITE ? 1e30f : dur * (float)e->iterations;
        if (total < MIN_PULSE_MS && e->iterations != INFINITE) {
            total = MIN_PULSE_MS;
        }
        if (t >= total) {
            if (e->playing == 1) {
                e->playing = 0;
            }
            return;
        }
        if (dur > 0 && t > dur && e->iterations > 1) {
            t = fmodf(t, dur);
        }
    }
    switch (FX[e->type].kind) {
    case K_CONSTANT:
        m = fabsf((float)e->mag) / 10000.0f;
        break;
    case K_RAMP:
        m = dur > 0 ? (float)e->ramp_start + (float)(e->ramp_end - e->ramp_start) * (t < dur ? t : dur) / dur : (float)e->ramp_start;
        m = fabsf(m) / 10000.0f;
        break;
    case K_PERIODIC:
        m = ((float)e->pmag + fabsf((float)e->poffset)) / 10000.0f;
        break;
    default:
        return;
    }
    if (m > 1) {
        m = 1;
    }
    if (e->has_env) {
        float attack = (float)e->env.dwAttackTime / 1000.0f, fade = (float)e->env.dwFadeTime / 1000.0f;
        if (attack > 0 && t < attack) {
            float a = (float)e->env.dwAttackLevel / 10000.0f;
            m = a + (m - a) * t / attack;
        }
        if (!infinite && fade > 0 && t > dur - fade && dur > 0) {
            float f = (float)e->env.dwFadeLevel / 10000.0f;
            m = m + (f - m) * (t - (dur - fade)) / fade;
        }
    }
    m *= (float)e->gain / 10000.0f;
    switch (FX[e->type].kind) {
    case K_CONSTANT:
    case K_RAMP:
        if (infinite) {
            float kick = e->kick * (1.0f - (float)(now - e->kick_ms) / KICK_MS);
            float steady = m * (float)pad_rumble_sustain / 100.0f;
            m = steady + (kick > 0 ? kick * (float)e->gain / 10000.0f : 0);
            *lo += m;
            *hi += 0.25f * m;
        } else {
            *lo += m;
            *hi += 0.35f * m;
        }
        break;
    case K_PERIODIC: {
        float period = (float)e->period_us / 1000.0f;
        if (period <= 0 || period <= 60) {
            *hi += m;                 /* a fast vibration */
            *lo += 0.4f * m;
        } else if (fmodf(t, period) < period / 2) {
            *lo += m;                 /* slow pulses */
            *hi += 0.5f * m;
        }
        break;
    }
    }
}

void padfx_frame(void)
{
    DWORD now = timeGetTime();
    int s;
    padfx *e;
    float lo[PAD_MAX] = { 0 }, hi[PAD_MAX] = { 0 };
    init_slots();
    if (pad_rumble_strength <= 0) {
        return;
    }
    for (e = g_fx; e; e = e->next) {
        if (e->slot >= 0 && e->slot < PAD_MAX) {
            level(e, now, &lo[e->slot], &hi[e->slot]);
        }
    }
    for (s = 0; s < PAD_MAX; s++) {
        SDL_GameController *gc = pad_controller(s);
        float k = (float)g_slot[s].gain / 10000.0f * (float)pad_rumble_strength / 100.0f;
        int low, high;
        if (!gc) {
            g_slot[s].low = g_slot[s].high = 0;
            continue;
        }
        if (g_slot[s].paused || g_slot[s].off) {
            k = 0;
        }
        low = (int)((lo[s] > 1 ? 1 : lo[s]) * k * 65535.0f);
        high = (int)((hi[s] > 1 ? 1 : hi[s]) * k * 65535.0f);
        low = low > 65535 ? 65535 : low;
        high = high > 65535 ? 65535 : high;
        /* SDL keeps a rumble going for the duration given: refresh it while it is on */
        if (low != g_slot[s].low || high != g_slot[s].high || ((low || high) && now - g_slot[s].sent >= 100)) {
            int ok = SDL_GameControllerRumble(gc, (Uint16)low, (Uint16)high, 250) == 0 ? 1 : -1;
            if ((low || high) && g_slot[s].logged != ok) {
                if (ok > 0) {
                    mcm_log("gamepad: rumble works on %s", SDL_GameControllerName(gc));
                } else {
                    mcm_log("gamepad: rumble failed on %s: %s", SDL_GameControllerName(gc), SDL_GetError());
                }
                g_slot[s].logged = ok;
            }
            g_slot[s].low = low;
            g_slot[s].high = high;
            g_slot[s].sent = now;
        }
    }
}

/* ---------------------------------------------------------------- IDirectInputEffect */

#define FX_SELF padfx *e = (padfx *)self

static HRESULT WINAPI fx_QueryInterface(void *self, REFIID iid, void **out)
{
    if (!out) {
        return E_POINTER;
    }
    if (IsEqualGUID(iid, &IID_Effect) || IsEqualGUID(iid, &IID_Unknown_)) {
        *out = self;
        ((padfx *)self)->ref++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fx_AddRef(void *self) { FX_SELF; return (ULONG)++e->ref; }

static ULONG WINAPI fx_Release(void *self)
{
    FX_SELF;
    padfx **p;
    LONG n = --e->ref;
    if (n > 0) {
        return (ULONG)n;
    }
    for (p = &g_fx; *p; p = &(*p)->next) {
        if (*p == e) {
            *p = e->next;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, e);
    padfx_frame();
    return 0;
}

static HRESULT WINAPI fx_Initialize(void *self, HINSTANCE inst, DWORD ver, REFGUID guid)
{
    (void)self; (void)inst; (void)ver; (void)guid;
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectGuid(void *self, LPGUID guid)
{
    FX_SELF;
    if (!guid) {
        return E_POINTER;
    }
    *guid = FX[e->type].guid;
    return DI_OK;
}

static HRESULT WINAPI fx_GetParameters(void *self, LPDIEFFECT eff, DWORD flags)
{
    FX_SELF;
    if (!eff) {
        return E_POINTER;
    }
    if (flags & DIEP_DURATION) eff->dwDuration = e->duration_us;
    if (flags & DIEP_SAMPLEPERIOD) eff->dwSamplePeriod = 0;
    if (flags & DIEP_GAIN) eff->dwGain = e->gain;
    if (flags & DIEP_TRIGGERBUTTON) eff->dwTriggerButton = e->trigger;
    if (flags & DIEP_TRIGGERREPEATINTERVAL) eff->dwTriggerRepeatInterval = 0;
    if ((flags & DIEP_STARTDELAY) && eff->dwSize == sizeof(DIEFFECT)) eff->dwStartDelay = e->start_delay_us;
    if ((flags & DIEP_ENVELOPE) && eff->lpEnvelope) {
        if (e->has_env) {
            *eff->lpEnvelope = e->env;
        } else {
            eff->lpEnvelope = NULL;
        }
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) && eff->lpvTypeSpecificParams) {
        switch (FX[e->type].kind) {
        case K_CONSTANT:
            if (eff->cbTypeSpecificParams < sizeof(DICONSTANTFORCE)) return DIERR_MOREDATA;
            ((DICONSTANTFORCE *)eff->lpvTypeSpecificParams)->lMagnitude = e->mag;
            eff->cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
            break;
        case K_RAMP:
            if (eff->cbTypeSpecificParams < sizeof(DIRAMPFORCE)) return DIERR_MOREDATA;
            ((DIRAMPFORCE *)eff->lpvTypeSpecificParams)->lStart = e->ramp_start;
            ((DIRAMPFORCE *)eff->lpvTypeSpecificParams)->lEnd = e->ramp_end;
            eff->cbTypeSpecificParams = sizeof(DIRAMPFORCE);
            break;
        case K_PERIODIC: {
            DIPERIODIC *p = eff->lpvTypeSpecificParams;
            if (eff->cbTypeSpecificParams < sizeof(DIPERIODIC)) return DIERR_MOREDATA;
            p->dwMagnitude = e->pmag;
            p->lOffset = e->poffset;
            p->dwPhase = 0;
            p->dwPeriod = e->period_us;
            eff->cbTypeSpecificParams = sizeof(DIPERIODIC);
            break;
        }
        default:
            eff->cbTypeSpecificParams = 0;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI fx_SetParameters(void *self, LPCDIEFFECT eff, DWORD flags)
{
    FX_SELF;
    HRESULT hr = set_params(e, eff, flags);
    if (FAILED(hr)) {
        return hr;
    }
    if (flags & DIEP_START) {
        start(e, 1);
    }
    padfx_frame();
    return DI_OK;
}

static HRESULT WINAPI fx_Start(void *self, DWORD iterations, DWORD flags)
{
    FX_SELF;
    if (flags & DIES_SOLO) {
        padfx *o;
        for (o = g_fx; o; o = o->next) {
            if (o != e && o->device == e->device) {
                o->playing = 0;
            }
        }
    }
    start(e, iterations);
    padfx_frame();
    return DI_OK;
}

static HRESULT WINAPI fx_Stop(void *self)
{
    FX_SELF;
    e->playing = 0;
    padfx_frame();
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectStatus(void *self, LPDWORD status)
{
    FX_SELF;
    float lo = 0, hi = 0;
    if (!status) {
        return E_POINTER;
    }
    level(e, timeGetTime(), &lo, &hi);   /* updates "playing" for effects that ran out */
    *status = e->playing ? DIEGES_PLAYING : 0;
    return DI_OK;
}

static HRESULT WINAPI fx_Download(void *self) { (void)self; return DI_OK; }
static HRESULT WINAPI fx_Unload(void *self) { FX_SELF; e->playing = 0; padfx_frame(); return DI_OK; }
static HRESULT WINAPI fx_Escape(void *self, LPDIEFFESCAPE esc) { (void)self; (void)esc; return DIERR_UNSUPPORTED; }

static void *g_fx_vtbl[] = {
    fx_QueryInterface, fx_AddRef, fx_Release, fx_Initialize, fx_GetEffectGuid, fx_GetParameters,
    fx_SetParameters, fx_Start, fx_Stop, fx_GetEffectStatus, fx_Download, fx_Unload, fx_Escape,
};

HRESULT padfx_create(void *device, int slot, REFGUID guid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out)
{
    int i = fx_index(guid);
    padfx *e;
    init_slots();
    if (!out) {
        return E_POINTER;
    }
    *out = NULL;
    if (i < 0) {
        return DIERR_DEVICENOTREG;
    }
    e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    if (!e) {
        return DIERR_OUTOFMEMORY;
    }
    e->vtbl = g_fx_vtbl;
    e->ref = 1;
    e->device = device;
    e->slot = slot;
    e->type = i;
    e->duration_us = INFINITE;
    e->gain = 10000;
    e->trigger = DIEB_NOTRIGGER;
    if (eff && FAILED(set_params(e, eff, DIEP_ALLPARAMS))) {
        HeapFree(GetProcessHeap(), 0, e);
        return DIERR_INVALIDPARAM;
    }
    e->kick = 0;                      /* the first magnitude is not a change */
    e->next = g_fx;
    g_fx = e;
    *out = (LPDIRECTINPUTEFFECT)e;
    return DI_OK;
}

int padfx_enum_created(void *device, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ref)
{
    padfx *e, *next;
    for (e = g_fx; e; e = next) {
        next = e->next;
        if (e->device == device && cb((LPDIRECTINPUTEFFECT)e, ref) == DIENUM_STOP) {
            break;
        }
    }
    return 1;
}

void padfx_command(void *device, int slot, DWORD cmd)
{
    padfx *e;
    init_slots();
    if (slot < 0 || slot >= PAD_MAX) {
        return;
    }
    if (cmd & (DISFFC_RESET | DISFFC_STOPALL)) {
        for (e = g_fx; e; e = e->next) {
            if (e->device == device) {
                e->playing = 0;
            }
        }
        g_slot[slot].paused = 0;
    }
    if (cmd & DISFFC_PAUSE) g_slot[slot].paused = 1;
    if (cmd & DISFFC_CONTINUE) g_slot[slot].paused = 0;
    if (cmd & DISFFC_SETACTUATORSOFF) g_slot[slot].off = 1;
    if (cmd & DISFFC_SETACTUATORSON) g_slot[slot].off = 0;
    padfx_frame();
}

DWORD padfx_state(void *device, int slot)
{
    padfx *e;
    DWORD st = DIGFFS_POWERON, playing = 0, any = 0;
    init_slots();
    for (e = g_fx; e; e = e->next) {
        if (e->device == device) {
            any = 1;
            playing |= e->playing != 0;
        }
    }
    if (!any) st |= DIGFFS_EMPTY;
    st |= playing ? 0 : DIGFFS_STOPPED;
    if (slot >= 0 && slot < PAD_MAX) {
        st |= g_slot[slot].paused ? DIGFFS_PAUSED : 0;
        st |= g_slot[slot].off ? DIGFFS_ACTUATORSOFF : DIGFFS_ACTUATORSON;
    }
    return st;
}

void padfx_set_gain(int slot, DWORD gain)
{
    init_slots();
    if (slot >= 0 && slot < PAD_MAX) {
        g_slot[slot].gain = gain > 10000 ? 10000 : gain;
    }
}

void padfx_device_gone(void *device)
{
    padfx *e;
    for (e = g_fx; e; e = e->next) {
        if (e->device == device) {
            e->device = NULL;
            e->playing = 0;
        }
    }
}
