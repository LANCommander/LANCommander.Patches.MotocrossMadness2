/* Test harness: drives the game from a script, for automated checks of the patches. Does nothing unless
 * the environment variable MCM_TEST names a script file.
 *
 * The game reads the keyboard and mouse through DirectInput in foreground cooperative mode, so while it
 * is not the foreground window (tests run next to other windows) it gets no input at all. With the harness
 * on, the real DirectInput device methods are wrapped (the vtable dinput.dll shares between its devices):
 * Acquire always succeeds, and the keyboard and mouse report only what the script injects.
 *
 * Script, one command per line (# comments):
 *   wait <ms>                 sleep
 *   key <name|0xDIK> [ms]     press and release a key (enter esc up down left right space tab a..z 0..9 f1..f12)
 *   down <key> / up <key>     hold / release a key
 *   move <dx> <dy>            relative mouse motion
 *   click [right]             mouse button press + release
 *   shot <file.png>           screenshot of the game window
 *   log <text>                line in mcm2patch.log
 *   quit                      end the game (ExitProcess) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>
#include <SDL.h>
#include "mcm2patch.h"
#include "d3d.h"

#define GAME_HWND (*(HWND *)(MCM_APP + 0x31c))

#define QUEUE_MAX 16                   /* the game's DIPROP_BUFFERSIZE */

static CRITICAL_SECTION g_cs;
static BYTE g_keys[256];
static struct { DWORD ofs, data; } g_queue_k[256], g_queue_m[256];
static int g_nk, g_nm;
static LONG g_mx, g_my;
static BYTE g_mbuttons[4];
static DWORD g_seq;

typedef HRESULT (WINAPI *acquire_fn)(IDirectInputDevice7A *);
typedef HRESULT (WINAPI *state_fn)(IDirectInputDevice7A *, DWORD, void *);
typedef HRESULT (WINAPI *data_fn)(IDirectInputDevice7A *, DWORD, DIDEVICEOBJECTDATA *, DWORD *, DWORD);
typedef HRESULT (WINAPI *info_fn)(IDirectInputDevice7A *, DIDEVICEINSTANCEA *);
static acquire_fn real_acquire;
static state_fn real_state;
static data_fn real_data;
static info_fn real_info;

static struct { void *dev; int type; } g_devs[32];

/* 2 = mouse, 3 = keyboard (DIDEVTYPE_*), other = leave alone */
static int dev_type(IDirectInputDevice7A *dev)
{
    int i;
    DIDEVICEINSTANCEA di;
    for (i = 0; i < 32 && g_devs[i].dev; i++) {
        if (g_devs[i].dev == dev) {
            return g_devs[i].type;
        }
    }
    memset(&di, 0, sizeof di);
    di.dwSize = sizeof di;
    if (i < 32 && real_info(dev, &di) == DI_OK) {
        g_devs[i].dev = dev;
        g_devs[i].type = di.dwDevType & 0xff;
        return g_devs[i].type;
    }
    return 0;
}

static HRESULT WINAPI hk_acquire(IDirectInputDevice7A *dev)
{
    int t = dev_type(dev);
    HRESULT hr = real_acquire(dev);
    return t == DIDEVTYPE_MOUSE || t == DIDEVTYPE_KEYBOARD ? DI_OK : hr;
}

static HRESULT WINAPI hk_state(IDirectInputDevice7A *dev, DWORD size, void *out)
{
    int t = dev_type(dev);
    if (t == DIDEVTYPE_KEYBOARD) {
        EnterCriticalSection(&g_cs);
        memcpy(out, g_keys, size < 256 ? size : 256);
        LeaveCriticalSection(&g_cs);
        return DI_OK;
    }
    if (t == DIDEVTYPE_MOUSE) {
        DIMOUSESTATE ms;
        EnterCriticalSection(&g_cs);
        memset(&ms, 0, sizeof ms);
        ms.lX = g_mx;
        ms.lY = g_my;
        g_mx = g_my = 0;
        memcpy(ms.rgbButtons, g_mbuttons, 4);
        LeaveCriticalSection(&g_cs);
        memset(out, 0, size);
        memcpy(out, &ms, size < sizeof ms ? size : sizeof ms);
        return DI_OK;
    }
    return real_state(dev, size, out);
}

static HRESULT WINAPI hk_data(IDirectInputDevice7A *dev, DWORD size, DIDEVICEOBJECTDATA *out, DWORD *count, DWORD flags)
{
    int t = dev_type(dev);
    if (t == DIDEVTYPE_KEYBOARD || t == DIDEVTYPE_MOUSE) {
        /* never more than the game's DIPROP_BUFFERSIZE (16, 0x4c8160): it asks for INFINITE into a 16-entry
           stack buffer, which real DirectInput can never overfill */
        DWORD n = 0, max = count ? (*count > QUEUE_MAX ? QUEUE_MAX : *count) : 0, i;
        int *nq = t == DIDEVTYPE_KEYBOARD ? &g_nk : &g_nm;
        void *q = t == DIDEVTYPE_KEYBOARD ? (void *)g_queue_k : (void *)g_queue_m;
        struct { DWORD ofs, data; } *queue = q;
        EnterCriticalSection(&g_cs);
        if (out) {
            for (; n < max && n < (DWORD)*nq; n++) {
                DIDEVICEOBJECTDATA *d = (DIDEVICEOBJECTDATA *)((char *)out + n * size);
                memset(d, 0, size);
                d->dwOfs = queue[n].ofs;
                d->dwData = queue[n].data;
                d->dwTimeStamp = GetTickCount();
                d->dwSequence = ++g_seq;
            }
            if (!(flags & DIGDD_PEEK)) {
                for (i = n; i < (DWORD)*nq; i++) {
                    queue[i - n] = queue[i];
                }
                *nq -= n;
            }
        } else {
            n = (DWORD)*nq;
            if (!(flags & DIGDD_PEEK) && max == INFINITE) {
                *nq = 0;
            }
        }
        LeaveCriticalSection(&g_cs);
        if (count) {
            *count = n;
        }
        return DI_OK;
    }
    return real_data(dev, size, out, count, flags);
}

/* a full queue drops the oldest event (DirectInput's buffer overflow) */
static void push(int keyboard, DWORD ofs, DWORD data)
{
    int *n = keyboard ? &g_nk : &g_nm;
    struct { DWORD ofs, data; } *q = keyboard ? (void *)g_queue_k : (void *)g_queue_m;
    EnterCriticalSection(&g_cs);
    if (*n == QUEUE_MAX) {
        memmove(q, q + 1, (QUEUE_MAX - 1) * sizeof *q);
        (*n)--;
    }
    q[*n].ofs = ofs;
    q[*n].data = data;
    (*n)++;
    LeaveCriticalSection(&g_cs);
}

/* relative motion, as immediate state only: the game reads the axes from GetDeviceState (and the buttons
   from the buffer), and motion left in the buffer while nothing reads it would arrive later as a jump */
static void mouse_move(int dx, int dy)
{
    EnterCriticalSection(&g_cs);
    g_mx += dx;
    g_my += dy;
    LeaveCriticalSection(&g_cs);
}

static void set_key(int dik, int down)
{
    EnterCriticalSection(&g_cs);
    g_keys[dik & 0xff] = down ? 0x80 : 0;
    LeaveCriticalSection(&g_cs);
    push(1, (DWORD)dik, down ? 0x80 : 0);
}

static int key_code(const char *name)
{
    static const struct { const char *name; int dik; } names[] = {
        {"enter", DIK_RETURN}, {"esc", DIK_ESCAPE}, {"up", DIK_UP}, {"down", DIK_DOWN}, {"left", DIK_LEFT},
        {"right", DIK_RIGHT}, {"space", DIK_SPACE}, {"tab", DIK_TAB}, {"backspace", DIK_BACK},
        {"lshift", DIK_LSHIFT}, {"lctrl", DIK_LCONTROL}, {"f1", DIK_F1}, {"f2", DIK_F2}, {"f3", DIK_F3},
        {"f4", DIK_F4}, {"f5", DIK_F5}, {"f6", DIK_F6}, {"f7", DIK_F7}, {"f8", DIK_F8}, {"f9", DIK_F9},
        {"f10", DIK_F10}, {"f11", DIK_F11}, {"f12", DIK_F12}, {"pgup", DIK_PRIOR}, {"pgdn", DIK_NEXT},
        {"home", DIK_HOME}, {"end", DIK_END},
    };
    static const char letters[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
    size_t i;
    if (!_strnicmp(name, "0x", 2)) {
        return (int)strtol(name, NULL, 16);
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (!_stricmp(name, names[i].name)) {
            return names[i].dik;
        }
    }
    if (name[0] && !name[1]) {
        if (name[0] >= '1' && name[0] <= '9') {
            return DIK_1 + name[0] - '1';
        }
        if (name[0] == '0') {
            return DIK_0;
        }
        for (i = 0; i < sizeof letters; i++) {
            if (letters[i] && letters[i] == (name[0] | 0x20)) {
                return (int)(DIK_Q + i);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- a virtual SDL gamepad (gamepad.c) */

static SDL_Joystick *g_vpad;

static int vpad_rumble(void *user, Uint16 low, Uint16 high)
{
    static Uint16 last_low, last_high;
    (void)user;
    if (low != last_low || high != last_high) {
        mcm_log("test: rumble low %u high %u", low, high);
        last_low = low;
        last_high = high;
    }
    return 0;
}

static void vpad(const char *what, const char *name, const char *value)
{
    if (!strcmp(what, "on") && !g_vpad) {
        SDL_VirtualJoystickDesc desc;
        int index;
        memset(&desc, 0, sizeof desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = "Test Gamepad";
        desc.Rumble = vpad_rumble;
        index = SDL_JoystickAttachVirtualEx(&desc);
        g_vpad = index >= 0 ? SDL_JoystickOpen(index) : NULL;
        if (g_vpad) {
            SDL_JoystickSetVirtualAxis(g_vpad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            SDL_JoystickSetVirtualAxis(g_vpad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        }
        mcm_log("test: virtual gamepad %s", g_vpad ? "plugged in" : SDL_GetError());
    } else if (g_vpad && !strcmp(what, "axis")) {
        SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(name);
        double v = atof(value);
        int trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        int raw = trigger ? (int)(-32768 + v * 65535) : (int)(v * 32767);
        SDL_JoystickSetVirtualAxis(g_vpad, axis, (Sint16)(raw < -32768 ? -32768 : raw > 32767 ? 32767 : raw));
    } else if (g_vpad && !strcmp(what, "button")) {
        SDL_JoystickSetVirtualButton(g_vpad, SDL_GameControllerGetButtonFromString(name), atoi(value) != 0);
    }
}

/* ---------------------------------------------------------------- screenshots (GDI+ flat API) */

typedef struct { UINT32 version; void *callback; BOOL no_thread; BOOL no_codecs; } gdip_input;
static int g_gdip;
static ULONG_PTR g_gdip_token;
static int (WINAPI *p_GdiplusStartup)(ULONG_PTR *, const gdip_input *, void *);
static int (WINAPI *p_GdipCreateBitmapFromHBITMAP)(HBITMAP, HPALETTE, void **);
static int (WINAPI *p_GdipSaveImageToFile)(void *, const WCHAR *, const CLSID *, void *);
static int (WINAPI *p_GdipDisposeImage)(void *);

static void shot(const char *file)
{
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    HWND hwnd = MCM_APP ? GAME_HWND : NULL;
    RECT r;
    HDC screen, mem;
    HBITMAP bmp;
    void *img = NULL;
    WCHAR wfile[MAX_PATH];
    if (!g_gdip) {
        HMODULE g = LoadLibraryA("gdiplus.dll");
        gdip_input in = { 1, NULL, FALSE, FALSE };
        p_GdiplusStartup = (void *)GetProcAddress(g, "GdiplusStartup");
        p_GdipCreateBitmapFromHBITMAP = (void *)GetProcAddress(g, "GdipCreateBitmapFromHBITMAP");
        p_GdipSaveImageToFile = (void *)GetProcAddress(g, "GdipSaveImageToFile");
        p_GdipDisposeImage = (void *)GetProcAddress(g, "GdipDisposeImage");
        g_gdip = p_GdiplusStartup && p_GdiplusStartup(&g_gdip_token, &in, NULL) == 0 ? 1 : -1;
    }
    if (g_gdip < 0) {
        return;
    }
    MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH);
    {
        /* the game's own frame (d3d.c), whatever covers its window */
        BYTE *rgb;
        int w, h;
        if (mcm_d3d_capture(&rgb, &w, &h, 1500)) {
            BITMAPINFO bi;
            void *bits;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (bmp) {
                int y;
                for (y = 0; y < h; y++) {      /* DIB rows are DWORD aligned */
                    memcpy((BYTE *)bits + (size_t)y * ((w * 3 + 3) & ~3), rgb + (size_t)y * w * 3, (size_t)w * 3);
                }
                if (p_GdipCreateBitmapFromHBITMAP(bmp, NULL, &img) == 0) {
                    p_GdipSaveImageToFile(img, wfile, &png, NULL);
                    p_GdipDisposeImage(img);
                }
                DeleteObject(bmp);
                mcm_log("test: shot %s (frame %dx%d)", file, w, h);
                return;
            }
        }
    }
    if (!hwnd || !GetWindowRect(hwnd, &r) || r.right - r.left < 320) {
        r.left = r.top = 0;
        r.right = GetSystemMetrics(SM_CXSCREEN);
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, r.right - r.left, r.bottom - r.top);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, r.right - r.left, r.bottom - r.top, screen, r.left, r.top, SRCCOPY);
    if (p_GdipCreateBitmapFromHBITMAP(bmp, NULL, &img) == 0) {
        p_GdipSaveImageToFile(img, wfile, &png, NULL);
        p_GdipDisposeImage(img);
    }
    DeleteDC(mem);
    DeleteObject(bmp);
    ReleaseDC(NULL, screen);
    mcm_log("test: shot %s (%ldx%ld)", file, r.right - r.left, r.bottom - r.top);
}

/* ---------------------------------------------------------------- script */

static int hook_dinput(void)
{
    char path[MAX_PATH];
    HMODULE real;
    HRESULT (WINAPI *create)(HINSTANCE, DWORD, REFIID, void **, IUnknown *);
    IDirectInput7A *di = NULL;
    IDirectInputDevice7A *kb = NULL;
    void **vt;
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    static const GUID iid_di7 = { 0x9a4cb684, 0x236d, 0x11d3, { 0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae } };
    static const GUID iid_dev7 = { 0x57d7c6bc, 0x2356, 0x11d3, { 0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae } };
    static const GUID sys_kb = { 0x6f1d2b61, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
    real = LoadLibraryA(path);
    create = real ? (void *)GetProcAddress(real, "DirectInputCreateEx") : NULL;
    if (!create || create(GetModuleHandleA(NULL), 0x700, &iid_di7, (void **)&di, NULL) != DI_OK) {
        return 0;
    }
    if (IDirectInput7_CreateDeviceEx(di, &sys_kb, &iid_dev7, (void **)&kb, NULL) != DI_OK) {
        IDirectInput7_Release(di);
        return 0;
    }
    vt = *(void ***)kb;
    real_info = (info_fn)vt[15];
    real_acquire = (acquire_fn)vt[7];
    real_state = (state_fn)vt[9];
    real_data = (data_fn)vt[10];
    {
        void *mine[3] = { (void *)hk_acquire, NULL, NULL };
        mcm_write((uint32_t)(uintptr_t)&vt[7], &mine[0], 4);
        mine[0] = (void *)hk_state;
        mcm_write((uint32_t)(uintptr_t)&vt[9], &mine[0], 4);
        mine[0] = (void *)hk_data;
        mcm_write((uint32_t)(uintptr_t)&vt[10], &mine[0], 4);
    }
    /* kb and di stay alive: the vtable lives in dinput.dll, which must stay loaded */
    return 1;
}

/* skydiag: logs the sky cube's tile masks for a few frames (SkyCube vtable 0x55ce28 slot 14 = render 0x43f460) */
static int g_skydiag;

static void __fastcall sky_render_diag(char *sky, void *unused)
{
    (void)unused;
    if (g_skydiag > 0) {
        char *cube = *(char **)(sky + 0x2c), *cam = *(char **)(*(char **)(sky + 0x18) + 8);
        int f;
        g_skydiag--;
        mcm_log("test: sky %p cube %p fov %.1f fwd %.2f %.2f %.2f", (void *)sky, (void *)cube, *(float *)(cam + 0x16c),
                *(float *)(cam + 0xb4), *(float *)(cam + 0xc4), *(float *)(cam + 0xd4));
        for (f = 0; cube && f < 6; f++) {
            char *face = cube + f * 0xf8;
            void **tiles = (void **)(face + 0x40);
            mcm_log("test:   face %d loaded %04x visible %04x tile0 %p", f, *(uint16_t *)(face + 0x3c),
                    *(uint16_t *)(face + 0x120), tiles[0]);
        }
    }
    ((void (__fastcall *)(char *, void *))0x43f460)(sky, NULL);
}

/* aidiag: per computer rider (the call at 0x497bd7 to BikeAI::Drive 0x414000, or framerate.c's AiRate wrapper), every
   second of race time: distance covered (position +0x10c, resets skipped), mean speed (+0x1b4), and the heading of
   the velocity (+0x64, +0x6c): mean turn rate (weaving) and how often the turn changes direction */
#define AI_STEP_CALL 0x497bd7
static struct ai_diag { char *bike; float t, dist, speed, turn, last[3], heading, dh; int flips, frames; } g_ai[12];
static int g_aidiag;
static int (__fastcall *g_ai_drive)(char *, void *, float);

static int __fastcall ai_step_diag(char *bike, void *unused, float dt)
{
    int r = g_ai_drive(bike, unused, dt), i;
    float *p = (float *)(bike + 0x10c), *v = (float *)(bike + 0x64), step, h, dh;
    struct ai_diag *d = NULL;
    if (!g_aidiag) {
        return r;
    }
    for (i = 0; i < 12 && g_ai[i].bike; i++) {
        if (g_ai[i].bike == bike) {
            break;
        }
    }
    if (i == 12) {
        return r;
    }
    d = &g_ai[i];
    h = atan2f(v[0], v[2]);
    if (!d->bike) {
        d->bike = bike;
        memcpy(d->last, p, sizeof d->last);
        d->heading = h;
        return r;
    }
    step = sqrtf((p[0] - d->last[0]) * (p[0] - d->last[0]) + (p[1] - d->last[1]) * (p[1] - d->last[1])
                 + (p[2] - d->last[2]) * (p[2] - d->last[2]));
    if (step < 10.0f) {
        d->dist += step;
    }
    memcpy(d->last, p, sizeof d->last);
    dh = h - d->heading;
    dh = dh > 3.14159f ? dh - 6.28318f : dh < -3.14159f ? dh + 6.28318f : dh;
    d->heading = h;
    if (v[0] * v[0] + v[2] * v[2] > 25.0f && dh != 0) {
        d->turn += fabsf(dh);
        d->flips += (dh > 0) != (d->dh > 0);
        d->dh = dh;
    }
    d->speed += *(float *)(bike + 0x1b4) * dt;
    d->frames++;
    d->t += dt;
    if (d->t >= 1.0f) {
        mcm_log("test: ai %d dist/s %.1f speed %.1f turn %.0f deg/s flips/s %.1f frames %d", i, d->dist / d->t,
                d->speed / d->t, d->turn / d->t * 57.3f, d->flips / d->t, d->frames);
        d->t = d->dist = d->speed = d->turn = 0;
        d->flips = d->frames = 0;
        g_aidiag--;
    }
    return r;
}

static DWORD WINAPI script_thread(void *arg)
{
    FILE *f = fopen((const char *)arg, "r");
    char line[512];
    if (!f) {
        mcm_log("test: cannot open script %s", (const char *)arg);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char cmd[32] = "", a[MAX_PATH] = "", b[64] = "";
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (!n || line[0] == '#') {
            continue;
        }
        sscanf(line, "%31s %259s %63s", cmd, a, b);
        if (!strcmp(cmd, "wait")) {
            Sleep((DWORD)atoi(a));
        } else if (!strcmp(cmd, "key")) {
            int k = key_code(a);
            set_key(k, 1);
            Sleep(b[0] ? (DWORD)atoi(b) : 80);
            set_key(k, 0);
        } else if (!strcmp(cmd, "down") || !strcmp(cmd, "up")) {
            set_key(key_code(a), cmd[0] == 'd');
        } else if (!strcmp(cmd, "move")) {
            mouse_move(atoi(a), atoi(b));
        } else if (!strcmp(cmd, "moveto")) {
            /* menu cursor to (x, y) in 640x480 menu pixels: into the corner, then the game's mouse scale
               (1.71 x, 1.29 y per mickey at the default sensitivity) */
            int dx = (int)(atoi(a) / 1.71 + 0.5), dy = (int)(atoi(b) / 1.29 + 0.5);
            mouse_move(-3000, -3000);
            Sleep(300);
            while (dx > 0 || dy > 0) {          /* the game takes small steps reliably, one big one not */
                int sx = dx > 5 ? 5 : dx, sy = dy > 5 ? 5 : dy;
                mouse_move(sx, sy);
                dx -= sx;
                dy -= sy;
                Sleep(40);
            }
        } else if (!strcmp(cmd, "click")) {
            int btn = !strcmp(a, "right") ? 1 : 0;
            g_mbuttons[btn] = 0x80;
            push(0, DIMOFS_BUTTON0 + btn, 0x80);
            Sleep(80);
            g_mbuttons[btn] = 0;
            push(0, DIMOFS_BUTTON0 + btn, 0);
        } else if (!strcmp(cmd, "setres")) {
            /* the race resolution, as Options - Graphics stores it: the index into the device's mode table
               (app+0xc -> +0x10 entries of 0x24 bytes: w, h, bpp; count at +8) in 0x68e918 and app+0xfc4 */
            int w = atoi(a), h = atoi(b), bpp = 16, i, count;
            char *dev = *(char **)(MCM_APP + 0xc);
            int *modes = *(int **)(dev + 0x10);
            sscanf(line, "%*s %*s %*s %d", &bpp);
            count = *(int *)(dev + 8);
            for (i = 0; i < count; i++) {
                int *m = (int *)((char *)modes + i * 0x24);
                if (m[0] == w && m[1] == h && m[2] == bpp) {
                    *(int *)0x68e918 = i;
                    *(int *)(MCM_APP + 0xfc4) = i;
                    mcm_log("test: race resolution %dx%dx%d = mode %d of %d", w, h, bpp, i, count);
                    break;
                }
            }
            if (i == count) {
                mcm_log("test: no mode %dx%dx%d among %d", w, h, bpp, count);
            }
        } else if (!strcmp(cmd, "vpad")) {
            char v[32] = "";
            sscanf(line, "%*s %*s %*s %31s", v);
            vpad(a, b, v);
        } else if (!strcmp(cmd, "tracedraws")) {
            mcm_d3d_trace_draws(atoi(a), !strcmp(b, "2d"));
        } else if (!strcmp(cmd, "logres")) {
            char *dev = *(char **)(MCM_APP + 0xc);
            int want = *(int *)(MCM_APP + 0xfc4), cur = *(int *)(dev + 0xc), count = *(int *)(dev + 8);
            int *modes = *(int **)(dev + 0x10), i, list16 = 0, list32 = 0;
            for (i = 0; i < count; i++) {   /* the flags Options - Graphics lists a mode by (+0x14, +0x18) */
                list16 += modes[i * 9 + 5] == 1;
                list32 += modes[i * 9 + 6] == 1;
            }
            mcm_log("test: settings mode %d (%dx%dx%d), device mode %d, %d modes, global %d", want,
                    want >= 0 && want < count ? modes[want * 9] : 0, want >= 0 && want < count ? modes[want * 9 + 1] : 0,
                    want >= 0 && want < count ? modes[want * 9 + 2] : 0, cur, count, *(int *)0x68e918);
            mcm_log("test: %d / %d modes listed, video memory %d MB", list16, list32, *(int *)(dev + 0x54) / (1 << 20));
        } else if (!strcmp(cmd, "shot")) {
            shot(line + 5);          /* the rest of the line: the path may have spaces */
        } else if (!strcmp(cmd, "appset")) {
            /* a dword of the application object (hex offset), e.g. appset fd4 1 = Options - Graphics Sky on */
            int off = (int)strtol(a, NULL, 16);
            mcm_log("test: app+%x = %d (was %d)", off, atoi(b), *(int *)(MCM_APP + off));
            *(int *)(MCM_APP + off) = atoi(b);
        } else if (!strcmp(cmd, "skydiag")) {
            uint32_t orig = 0x43f460, hook = (uint32_t)(uintptr_t)sky_render_diag;
            g_skydiag = atoi(a);
            mcm_patch(0x55ce28 + 14 * 4, &orig, &hook, 4);
        } else if (!strcmp(cmd, "aidiag")) {
            uint32_t target = AI_STEP_CALL + 5 + *(uint32_t *)MCM_ADDR(AI_STEP_CALL + 1);
            g_aidiag = atoi(a);
            g_ai_drive = (int (__fastcall *)(char *, void *, float))(uintptr_t)target;
            mcm_redirect_call(AI_STEP_CALL, target, ai_step_diag);
        } else if (!strcmp(cmd, "appget")) {
            int off = (int)strtol(a, NULL, 16);
            mcm_log("test: app+%x = %d", off, *(int *)(MCM_APP + off));
        } else if (!strcmp(cmd, "log")) {
            mcm_log("test: %s", line + 4);
        } else if (!strcmp(cmd, "quit")) {
            mcm_log("test: quit");
            fclose(f);
            ExitProcess(0);
        }
    }
    fclose(f);
    mcm_log("test: script done");
    return 0;
}

static DWORD WINAPI start_thread(void *arg)
{
    /* after DllMain (no DirectInput under the loader lock) */
    Sleep(200);
    if (!hook_dinput()) {
        mcm_log("test: cannot wrap DirectInput, no input injection");
    }
    return script_thread(arg);
}

void mcm_test_install(void)
{
    static char script[MAX_PATH];
    if (!GetEnvironmentVariableA("MCM_TEST", script, sizeof script) || !script[0]) {
        return;
    }
    InitializeCriticalSection(&g_cs);
    mcm_log("test: harness on, script %s", script);
    CloseHandle(CreateThread(NULL, 0, start_thread, script, 0, NULL));
}
