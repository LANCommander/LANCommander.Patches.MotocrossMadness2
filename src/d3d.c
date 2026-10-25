/* DirectDraw 7 / Direct3D 7 hooks shared by the video modules (widescreen.c, hud.c, framerate.c), plus a trace of
 * what the game asks of them ([Debug] TraceD3D=1).
 *
 * The game creates DirectDraw through BLADE.DLL (Microsoft's Blade software rasteriser, which wraps
 * DirectDraw and Direct3D and hands the hardware devices to ddraw.dll: here dgVoodoo 2). The IDirectDraw7 it
 * gets back is hooked by patching the vtables of the objects themselves (every object of a class shares its
 * vtable); several classes implement each interface (Blade's and dgVoodoo's surfaces, for one), so the
 * original methods are kept per vtable and a hook always calls the one of its object's class:
 *   IDirectDraw7        SetDisplayMode, EnumDisplayModes, CreateSurface, QueryInterface (-> IDirect3D7)
 *   IDirect3D7          CreateDevice
 *   IDirect3DDevice7    Clear, SetTransform, SetViewport, BeginScene, EndScene, DrawPrimitive, DrawIndexedPrimitive
 *   IDirectDrawSurface7 Flip, Blt (the frame is shown; drawing into the back buffer), GetAttachedSurface (the
 *                       back buffer), GetDC / ReleaseDC (the back buffer's DC, for GDI text on the screen)
 * Modules register callbacks in mcm_d3d (d3d.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"
#include "d3d.h"

mcm_d3d_t mcm_d3d;

static int g_trace;
static int g_frames;

/* ---------------------------------------------------------------- vtable patching */

#define SLOTS 64

typedef struct {
    void **vt;
    void *orig[SLOTS];
} vtable_t;

static vtable_t g_vt[64];
static int g_nvt;
static CRITICAL_SECTION g_vt_cs;

static vtable_t *find_vt(void **vt)
{
    int i;
    for (i = 0; i < g_nvt; i++) {
        if (g_vt[i].vt == vt) {
            return &g_vt[i];
        }
    }
    return NULL;
}

/* the class's own method idx for obj, as it was before the hooks */
static void *orig_of(void *obj, int idx)
{
    void **vt = *(void ***)obj;
    vtable_t *v = find_vt(vt);
    return v ? v->orig[idx] : vt[idx];
}

#define ORIG(type, obj, idx) ((type)orig_of((obj), (idx)))

/* 1 the first time vt is seen: its slots are saved, then hooked by the caller */
static int first_time(void **vt)
{
    int r = 0;
    EnterCriticalSection(&g_vt_cs);
    if (!find_vt(vt) && g_nvt < 64) {
        memcpy(g_vt[g_nvt].orig, vt, sizeof g_vt[g_nvt].orig);
        g_vt[g_nvt].vt = vt;
        g_nvt++;
        r = 1;
    }
    LeaveCriticalSection(&g_vt_cs);
    return r;
}

static void vhook(void **vt, int idx, void *fn)
{
    if (vt[idx] != fn) {
        mcm_write((uint32_t)(uintptr_t)&vt[idx], &fn, 4);
    }
}

static const char *module_of(const void *p)
{
    static char name[MAX_PATH];
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, p, &m)
        && GetModuleFileNameA(m, name, sizeof name)) {
        char *s = strrchr(name, '\\');
        return s ? s + 1 : name;
    }
    return "?";
}

#define TRACE(...) do { if (g_trace) mcm_log(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- IDirect3DDevice7 */

typedef HRESULT (WINAPI *SetTransform_fn)(void *, D3DTRANSFORMSTATETYPE, D3DMATRIX *);
typedef HRESULT (WINAPI *SetViewport_fn)(void *, D3DVIEWPORT7 *);
typedef HRESULT (WINAPI *Scene_fn)(void *);
typedef HRESULT (WINAPI *DrawPrimitive_fn)(void *, D3DPRIMITIVETYPE, DWORD, void *, DWORD, DWORD);
typedef HRESULT (WINAPI *DrawIndexedPrimitive_fn)(void *, D3DPRIMITIVETYPE, DWORD, void *, DWORD, WORD *, DWORD, DWORD);
typedef HRESULT (WINAPI *QI_fn)(void *, REFIID, void **);

static int g_trace_draws;

static HRESULT WINAPI d_SetTransform(void *dev, D3DTRANSFORMSTATETYPE type, D3DMATRIX *m)
{
    D3DMATRIX copy;
    if (m && mcm_d3d.on_transform) {
        copy = *m;
        mcm_d3d.on_transform(dev, type, &copy);
        m = &copy;
    }
    static D3DMATRIX last;
    if (g_trace && g_trace_draws > 0 && m && type != D3DTRANSFORMSTATE_PROJECTION) {
        TRACE("d3d: transform %d pos %.1f %.1f %.1f x %.3f %.3f %.3f z %.3f %.3f %.3f", type, m->_41, m->_42, m->_43,
              m->_11, m->_12, m->_13, m->_31, m->_32, m->_33);
    }
    if (g_trace && m && type == D3DTRANSFORMSTATE_PROJECTION && memcmp(&last, m, sizeof last)) {
        last = *m;
        TRACE("d3d: projection %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f",
              m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33, m->_34,
              m->_41, m->_42, m->_43, m->_44);
    }
    return ORIG(SetTransform_fn, dev, 11)(dev, type, m);
}

static HRESULT WINAPI d_SetViewport(void *dev, D3DVIEWPORT7 *vp)
{
    D3DVIEWPORT7 copy;
    if (vp && mcm_d3d.on_viewport) {
        copy = *vp;
        mcm_d3d.on_viewport(dev, &copy);
        vp = &copy;
    }
    static D3DVIEWPORT7 lastvp;
    if (vp && g_trace && memcmp(&lastvp, vp, sizeof lastvp)) {
        lastvp = *vp;
        TRACE("d3d: viewport %lu,%lu %lux%lu z %.2f..%.2f", vp->dwX, vp->dwY, vp->dwWidth, vp->dwHeight, vp->dvMinZ, vp->dvMaxZ);
    }
    return ORIG(SetViewport_fn, dev, 13)(dev, vp);
}

typedef HRESULT (WINAPI *SetRenderState_fn)(void *, D3DRENDERSTATETYPE, DWORD);

static HRESULT WINAPI d_SetRenderState(void *dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if (g_trace && g_trace_draws > 0) {
        TRACE("d3d: renderstate %d = %lu", state, value);
    }
    return ORIG(SetRenderState_fn, dev, 20)(dev, state, value);
}

typedef HRESULT (WINAPI *Clear_fn)(void *, DWORD, D3DRECT *, DWORD, D3DCOLOR, D3DVALUE, DWORD);

/* A race frame starts with a whole-screen Clear of the depth buffer only (the colour it passes is the fog's): the sky
   and the track are trusted to cover every pixel. What they leave uncovered shows an older frame, flickering with the
   buffers of the swap chain. [Video] ClearScreen clears the picture too, to that colour, on the first whole-screen
   clear of a race frame, before anything is drawn (a later one keeps what was drawn). Only in races (hud.c sees the
   race HUD drawn): the menus redraw just what changes and keep the rest of the back buffer */
static int g_clear_screen;
static int g_frame_cleared;

static HRESULT WINAPI d_Clear(void *dev, DWORD n, D3DRECT *rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil)
{
    if (n == 1 && rects && rects->x1 <= 0 && rects->y1 <= 0 && rects->x2 >= mcm_d3d.width && rects->y2 >= mcm_d3d.height
        && mcm_d3d.width > 0) {
        if (g_clear_screen && !g_frame_cleared && (flags & D3DCLEAR_ZBUFFER) && mcm_d3d.race_hud > 0) {
            static int logged;
            if (!logged++) {
                mcm_log("d3d: race frames are cleared to the fog colour %06lx", color & 0xffffff);
            }
            flags |= D3DCLEAR_TARGET;
        }
        g_frame_cleared = 1;
    }
    if (g_trace && g_trace_draws > 0) {
        g_trace_draws--;
        mcm_log("d3d: clear %lu rects (first %ld,%ld..%ld,%ld) flags %lx color %08lx z %.2f", n,
                n && rects ? rects->x1 : -1, n && rects ? rects->y1 : -1, n && rects ? rects->x2 : -1,
                n && rects ? rects->y2 : -1, flags, color, z);
    }
    return ORIG(Clear_fn, dev, 10)(dev, n, rects, flags, color, z, stencil);
}

static HRESULT WINAPI d_BeginScene(void *dev)
{
    if (mcm_d3d.on_begin_scene) {
        mcm_d3d.on_begin_scene(dev);
    }
    return ORIG(Scene_fn, dev, 5)(dev);
}

static HRESULT WINAPI d_EndScene(void *dev)
{
    if (mcm_d3d.on_end_scene) {
        mcm_d3d.on_end_scene(dev);
    }
    return ORIG(Scene_fn, dev, 6)(dev);
}

static DWORD vertex_size(DWORD fvf)
{
    DWORD s = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: s = 12; break;
    case D3DFVF_XYZRHW: s = 16; break;
    case D3DFVF_XYZB1: s = 16; break;
    case D3DFVF_XYZB2: s = 20; break;
    case D3DFVF_XYZB3: s = 24; break;
    case D3DFVF_XYZB4: s = 28; break;
    case D3DFVF_XYZB5: s = 32; break;
    }
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_RESERVED1) s += 4;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    s += ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) * 8;
    return s;
}

static void *g_texture;          /* stage 0 texture (trace) */
static int g_trace_2d_only;

void mcm_d3d_trace_draws(int n, int only_2d)
{
    g_trace_draws = n;
    g_trace_2d_only = only_2d;
    g_trace = 1;
}

static void trace_draw(const char *what, D3DPRIMITIVETYPE type, DWORD fvf, void *verts, DWORD n)
{
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    DWORD i, size = vertex_size(fvf);
    if (!g_trace || g_trace_draws <= 0 || (g_trace_2d_only && (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)) {
        return;
    }
    g_trace_draws--;
    for (i = 0; i < n && size; i++) {
        float *v = (float *)((char *)verts + i * size);
        x0 = v[0] < x0 ? v[0] : x0;
        x1 = v[0] > x1 ? v[0] : x1;
        y0 = v[1] < y0 ? v[1] : y0;
        y1 = v[1] > y1 ? v[1] : y1;
    }
    mcm_log("d3d: %s type %d fvf %03lx n %lu x %.1f..%.1f y %.1f..%.1f tex %p", what, type, fvf, n, x0, x1, y0, y1, g_texture);
}

typedef HRESULT (WINAPI *SetTexture_fn)(void *, DWORD, void *);

static HRESULT WINAPI d_SetTexture(void *dev, DWORD stage, void *tex)
{
    if (stage == 0) {
        g_texture = tex;
    }
    return ORIG(SetTexture_fn, dev, 35)(dev, stage, tex);
}

static HRESULT WINAPI d_DrawPrimitive(void *dev, D3DPRIMITIVETYPE type, DWORD fvf, void *verts, DWORD n, DWORD flags)
{
    if (mcm_d3d.on_draw && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        void *changed = mcm_d3d.on_draw(dev, fvf, verts, n, vertex_size(fvf));
        verts = changed ? changed : verts;
    }
    g_frame_cleared = 1;              /* drawn into: a clear from now on keeps the picture */
    trace_draw("draw", type, fvf, verts, n);
    return ORIG(DrawPrimitive_fn, dev, 25)(dev, type, fvf, verts, n, flags);
}

static HRESULT WINAPI d_DrawIndexedPrimitive(void *dev, D3DPRIMITIVETYPE type, DWORD fvf, void *verts, DWORD n,
                                             WORD *idx, DWORD ni, DWORD flags)
{
    if (mcm_d3d.on_draw && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        void *changed = mcm_d3d.on_draw(dev, fvf, verts, n, vertex_size(fvf));
        verts = changed ? changed : verts;
    }
    g_frame_cleared = 1;
    trace_draw("drawidx", type, fvf, verts, n);
    return ORIG(DrawIndexedPrimitive_fn, dev, 26)(dev, type, fvf, verts, n, idx, ni, flags);
}

static HRESULT WINAPI d_QueryInterface(void *dev, REFIID iid, void **out)
{
    HRESULT hr = ORIG(QI_fn, dev, 0)(dev, iid, out);
    static int n;
    if (g_trace && iid && n++ < 40) {
        mcm_log("d3d: device QI {%08lx-%04x} -> %08lx", iid->Data1, iid->Data2, hr);
    }
    return hr;
}

/* trace: every slot of a device vtable counts its calls (inc [count]; jmp [orig]) */
#define COUNT_SLOTS 49
static LONG g_counts[COUNT_SLOTS];
static void *g_count_orig[COUNT_SLOTS];

static void count_slots(void **vt)
{
    uint8_t *code = VirtualAlloc(NULL, COUNT_SLOTS * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int i;
    if (!code) {
        return;
    }
    for (i = 3; i < COUNT_SLOTS; i++) {
        uint8_t *t = code + i * 16;
        uint32_t cnt = (uint32_t)(uintptr_t)&g_counts[i], org = (uint32_t)(uintptr_t)&g_count_orig[i];
        void *fn = t;
        g_count_orig[i] = vt[i];
        t[0] = 0xff; t[1] = 0x05; memcpy(t + 2, &cnt, 4);     /* inc dword [cnt] */
        t[6] = 0xff; t[7] = 0x25; memcpy(t + 8, &org, 4);     /* jmp dword [org] */
        mcm_write((uint32_t)(uintptr_t)&vt[i], &fn, 4);
    }
}

static void dump_counts(void)
{
    char line[1024];
    int i, n = 0;
    line[0] = '\0';
    for (i = 3; i < COUNT_SLOTS; i++) {
        if (g_counts[i]) {
            n += snprintf(line + n, sizeof line - n, " %d:%ld", i, g_counts[i]);
        }
    }
    if (n) {
        mcm_log("d3d: device calls%s", line);
    }
}

static void hook_device(void *dev)
{
    void **vt = *(void ***)dev;
    if (!first_time(vt)) {
        return;
    }
    TRACE("d3d: device %p, vtable in %s", dev, module_of(vt[0]));
    if (g_trace && mcm_ini_int("Debug", "CountDeviceCalls", 0)) {
        count_slots(vt);                /* the saved originals stay the real methods: the counters are not hooks */
    }
    if (g_trace) {
        vhook(vt, 0, d_QueryInterface);
    }
    vhook(vt, 5, d_BeginScene);
    vhook(vt, 6, d_EndScene);
    vhook(vt, 11, d_SetTransform);
    vhook(vt, 13, d_SetViewport);
    vhook(vt, 25, d_DrawPrimitive);
    vhook(vt, 26, d_DrawIndexedPrimitive);
    vhook(vt, 10, d_Clear);
    if (g_trace || getenv("MCM_TEST")) {
        vhook(vt, 35, d_SetTexture);
        vhook(vt, 20, d_SetRenderState);
    }
    if (mcm_d3d.on_device) {
        mcm_d3d.on_device(dev);
    }
}

/* ---------------------------------------------------------------- IDirect3D7 */

typedef HRESULT (WINAPI *CreateDevice_fn)(void *, REFCLSID, void *, void **);

static HRESULT WINAPI d3_CreateDevice(void *d3d, REFCLSID cls, void *surface, void **out)
{
    HRESULT hr = ORIG(CreateDevice_fn, d3d, 4)(d3d, cls, surface, out);
    TRACE("d3d: CreateDevice {%08lx} -> %08lx", cls ? cls->Data1 : 0, hr);
    if (hr == 0 && out && *out) {
        hook_device(*out);
    }
    return hr;
}

static void hook_d3d(void *d3d)
{
    void **vt = *(void ***)d3d;
    if (first_time(vt)) {
        vhook(vt, 4, d3_CreateDevice);
    }
}

/* ---------------------------------------------------------------- IDirectDrawSurface7 */

typedef HRESULT (WINAPI *Flip_fn)(void *, void *, DWORD);
typedef HRESULT (WINAPI *Blt_fn)(void *, RECT *, void *, RECT *, DWORD, void *);
typedef HRESULT (WINAPI *GetAttached_fn)(void *, DDSCAPS2 *, void **);
typedef HRESULT (WINAPI *GetCaps_fn)(void *, DDSCAPS2 *);
static void hook_surface(void *surface);

/* ---------------------------------------------------------------- frame capture (test harness) */

typedef HRESULT (WINAPI *Lock_fn)(void *, RECT *, DDSURFACEDESC2 *, DWORD, HANDLE);
typedef HRESULT (WINAPI *Unlock_fn)(void *, RECT *);

static volatile LONG g_capture_wanted;
static HANDLE g_capture_done;
static BYTE *g_capture_rgb;
static int g_capture_w, g_capture_h;

/* the frame about to be shown, as 24-bit RGB (bottom-up rows, like a DIB) */
static void capture(void *surface)
{
    DDSURFACEDESC2 d;
    int x, y;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (ORIG(Lock_fn, surface, 25)(surface, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != 0) {
        return;
    }
    g_capture_w = (int)d.dwWidth;
    g_capture_h = (int)d.dwHeight;
    g_capture_rgb = realloc(g_capture_rgb, (size_t)g_capture_w * g_capture_h * 3);
    for (y = 0; y < g_capture_h; y++) {
        const BYTE *row = (const BYTE *)d.lpSurface + (size_t)y * d.lPitch;
        BYTE *out = g_capture_rgb + (size_t)(g_capture_h - 1 - y) * g_capture_w * 3;
        for (x = 0; x < g_capture_w; x++, out += 3) {
            if (d.ddpfPixelFormat.dwRGBBitCount == 16) {
                WORD p = ((const WORD *)row)[x];
                if (d.ddpfPixelFormat.dwGBitMask == 0x7e0) {
                    out[2] = (BYTE)((p >> 11) << 3); out[1] = (BYTE)(((p >> 5) & 63) << 2); out[0] = (BYTE)((p & 31) << 3);
                } else {
                    out[2] = (BYTE)(((p >> 10) & 31) << 3); out[1] = (BYTE)(((p >> 5) & 31) << 3); out[0] = (BYTE)((p & 31) << 3);
                }
            } else {
                DWORD p = ((const DWORD *)row)[x];
                out[2] = (BYTE)(p >> 16); out[1] = (BYTE)(p >> 8); out[0] = (BYTE)p;
            }
        }
    }
    ORIG(Unlock_fn, surface, 32)(surface, NULL);
}

int mcm_d3d_capture(BYTE **rgb, int *w, int *h, DWORD timeout)
{
    if (!g_capture_done) {
        g_capture_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    ResetEvent(g_capture_done);
    InterlockedExchange(&g_capture_wanted, 1);
    if (WaitForSingleObject(g_capture_done, timeout) != WAIT_OBJECT_0 || !g_capture_rgb) {
        InterlockedExchange(&g_capture_wanted, 0);
        return 0;
    }
    *rgb = g_capture_rgb;
    *w = g_capture_w;
    *h = g_capture_h;
    return 1;
}

static void frame_shown(void *surface)
{
    static LARGE_INTEGER freq, last, first;
    static double min_ms = 1e9, max_ms;
    LARGE_INTEGER now;
    g_frames++;
    g_frame_cleared = 0;
    if (mcm_d3d.race_hud > 0) {
        mcm_d3d.race_hud--;
    }
    if (g_trace) {
        double ms;
        if (!freq.QuadPart) {
            QueryPerformanceFrequency(&freq);
        }
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            min_ms = ms < min_ms ? ms : min_ms;
            max_ms = ms > max_ms ? ms : max_ms;
        } else {
            first = now;
        }
        last = now;
        if (g_frames % 300 == 0) {
            double total = (double)(now.QuadPart - first.QuadPart) / (double)freq.QuadPart;
            mcm_log("d3d: %d frames, %.1f fps, frame %.1f..%.1f ms", g_frames, total > 0 ? 299.0 / total : 0, min_ms, max_ms);
            first = now;
            min_ms = 1e9;
            max_ms = 0;
            dump_counts();
        }
    }
    if (mcm_d3d.on_present) {
        mcm_d3d.on_present(surface);
    }
}

/* 1 if the surface is what the game draws a frame into: the back buffer, or (windowed) its 3-D render target */
static int is_back(void *surface)
{
    DDSCAPS2 caps;
    memset(&caps, 0, sizeof caps);
    if (ORIG(GetCaps_fn, surface, 14)(surface, &caps) != 0) {
        return 0;
    }
    return (caps.dwCaps & DDSCAPS_BACKBUFFER)
        || ((caps.dwCaps & DDSCAPS_3DDEVICE) && !(caps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE)));
}

static void frame_done(void *back)
{
    if (mcm_d3d.on_frame_done) {
        mcm_d3d.on_frame_done(back);
    }
    if (g_capture_wanted) {
        capture(back);
        InterlockedExchange(&g_capture_wanted, 0);
        SetEvent(g_capture_done);
    }
}

static HRESULT WINAPI s_Flip(void *surface, void *target, DWORD flags)
{
    DDSCAPS2 caps;
    void *back = NULL;
    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (ORIG(GetAttached_fn, surface, 12)(surface, &caps, &back) == 0 && back) {
        frame_done(back);
        ((ULONG (WINAPI *)(void *))(*(void ***)back)[2])(back);
    }
    frame_shown(surface);
    return ORIG(Flip_fn, surface, 11)(surface, target, flags);
}

HRESULT mcm_d3d_blt(void *dst, RECT *dstr, void *src, RECT *srcr, DWORD flags, void *fx)
{
    return ORIG(Blt_fn, dst, 5)(dst, dstr, src, srcr, flags, fx);
}

static void trace_blt(const char *what, void *surface, int x, int y, RECT *dst, RECT *srcr, DWORD flags)
{
    if (!g_trace || g_trace_draws <= 0) {
        return;
    }
    g_trace_draws--;
    mcm_log("d3d: %s %p dst %ld,%ld..%ld,%ld (at %d,%d) src %ld,%ld..%ld,%ld flags %08lx", what, surface,
            dst ? dst->left : -1, dst ? dst->top : -1, dst ? dst->right : -1, dst ? dst->bottom : -1, x, y,
            srcr ? srcr->left : -1, srcr ? srcr->top : -1, srcr ? srcr->right : -1, srcr ? srcr->bottom : -1, flags);
}

typedef HRESULT (WINAPI *BltFast_fn)(void *, DWORD, DWORD, void *, RECT *, DWORD);
typedef HRESULT (WINAPI *GetDC_fn)(void *, HDC *);
typedef HRESULT (WINAPI *ReleaseDC_fn)(void *, HDC);

static HRESULT WINAPI s_BltFast(void *surface, DWORD x, DWORD y, void *src, RECT *srcr, DWORD flags)
{
    trace_blt("bltfast", surface, (int)x, (int)y, NULL, srcr, flags);
    return ORIG(BltFast_fn, surface, 7)(surface, x, y, src, srcr, flags);
}

static HRESULT WINAPI s_GetDC(void *surface, HDC *dc)
{
    HRESULT hr = ORIG(GetDC_fn, surface, 17)(surface, dc);
    trace_blt("getdc", surface, 0, 0, NULL, NULL, 0);
    if (hr == 0 && dc && is_back(surface)) {
        mcm_d3d.backbuffer_dc = *dc;
        g_frame_cleared = 1;
    }
    return hr;
}

static HRESULT WINAPI s_ReleaseDC(void *surface, HDC dc)
{
    if (dc && dc == mcm_d3d.backbuffer_dc) {
        mcm_d3d.backbuffer_dc = NULL;
    }
    return ORIG(ReleaseDC_fn, surface, 26)(surface, dc);
}

static HRESULT WINAPI s_Blt(void *surface, RECT *dst, void *src, RECT *srcr, DWORD flags, void *fx)
{
    DDSCAPS2 caps;
    trace_blt("blt", surface, 0, 0, dst, srcr, flags);
    memset(&caps, 0, sizeof caps);
    if (src && ORIG(GetCaps_fn, surface, 14)(surface, &caps) == 0 && (caps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        frame_done(src);          /* windowed: the back buffer is blitted to the primary */
        frame_shown(surface);
    } else if (is_back(surface)) {
        g_frame_cleared = 1;
        if (mcm_d3d.on_backbuffer_blt && mcm_d3d.on_backbuffer_blt(surface, dst, src, srcr, flags, fx)) {
            return DD_OK;
        }
    }
    return ORIG(Blt_fn, surface, 5)(surface, dst, src, srcr, flags, fx);
}

static HRESULT WINAPI s_GetAttached(void *surface, DDSCAPS2 *caps, void **out)
{
    HRESULT hr = ORIG(GetAttached_fn, surface, 12)(surface, caps, out);
    if (hr == 0 && out && *out) {
        hook_surface(*out);
    }
    return hr;
}

static void hook_surface(void *surface)
{
    void **vt = *(void ***)surface;
    if (first_time(vt)) {
        TRACE("d3d: surface vtable %p in %s", (void *)vt, module_of(vt[0]));
        vhook(vt, 5, s_Blt);
        vhook(vt, 11, s_Flip);
        vhook(vt, 12, s_GetAttached);
        vhook(vt, 17, s_GetDC);
        vhook(vt, 26, s_ReleaseDC);
        if (g_trace || getenv("MCM_TEST")) {
            vhook(vt, 7, s_BltFast);
        }
    }
}
/* ---------------------------------------------------------------- IDirectDraw7 */

static const GUID IID_D3D7 = { 0xf5049e77, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };

typedef HRESULT (WINAPI *CreateSurface_fn)(void *, DDSURFACEDESC2 *, void **, void *);
typedef HRESULT (WINAPI *EnumDisplayModes_fn)(void *, DWORD, DDSURFACEDESC2 *, void *, LPDDENUMMODESCALLBACK2);
typedef HRESULT (WINAPI *SetCooperativeLevel_fn)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *SetDisplayMode_fn)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);

static void *g_dd;                 /* the IDirectDraw7 the display mode was set on */

static HRESULT WINAPI dd_QueryInterface(void *dd, REFIID iid, void **out)
{
    HRESULT hr = ORIG(QI_fn, dd, 0)(dd, iid, out);
    if (hr == 0 && out && *out && iid && !memcmp(iid, &IID_D3D7, sizeof(GUID))) {
        hook_d3d(*out);
    }
    return hr;
}

static HRESULT WINAPI dd_CreateSurface(void *dd, DDSURFACEDESC2 *desc, void **out, void *outer)
{
    HRESULT hr;
    if (desc && mcm_d3d.on_create_surface) {
        mcm_d3d.on_create_surface(desc);
    }
    hr = ORIG(CreateSurface_fn, dd, 6)(dd, desc, out, outer);
    if (desc && (desc->ddsCaps.dwCaps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE))) {
        TRACE("d3d: CreateSurface caps %08lx %lux%lu flags %08lx backbuffers %lu -> %08lx", desc->ddsCaps.dwCaps,
              desc->dwWidth, desc->dwHeight, desc->dwFlags, desc->dwBackBufferCount, hr);
        if (hr == 0 && out && *out) {
            hook_surface(*out);
        }
    }
    return hr;
}

typedef struct {
    LPDDENUMMODESCALLBACK2 cb;
    void *ctx;
} enum_ctx;

static HRESULT WINAPI enum_mode(DDSURFACEDESC2 *desc, void *ctx)
{
    enum_ctx *e = ctx;
    TRACE("d3d: mode %lux%lux%lu %luHz", desc->dwWidth, desc->dwHeight, desc->ddpfPixelFormat.dwRGBBitCount,
          desc->dwRefreshRate);
    if (mcm_d3d.on_enum_mode && !mcm_d3d.on_enum_mode(desc)) {
        return DDENUMRET_OK;
    }
    return e->cb(desc, e->ctx);
}

static HRESULT WINAPI dd_EnumDisplayModes(void *dd, DWORD flags, DDSURFACEDESC2 *desc, void *ctx, LPDDENUMMODESCALLBACK2 cb)
{
    enum_ctx e = { cb, ctx };
    HRESULT hr = ORIG(EnumDisplayModes_fn, dd, 8)(dd, flags, desc, &e, enum_mode);
    if (mcm_d3d.on_enum_done) {
        mcm_d3d.on_enum_done(cb, ctx);
    }
    return hr;
}

static HRESULT WINAPI dd_SetCooperativeLevel(void *dd, HWND hwnd, DWORD flags)
{
    TRACE("d3d: SetCooperativeLevel %p %08lx", (void *)hwnd, flags);
    return ORIG(SetCooperativeLevel_fn, dd, 20)(dd, hwnd, flags);
}

static HRESULT WINAPI dd_SetDisplayMode(void *dd, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD flags)
{
    HRESULT hr;
    if (mcm_d3d.on_display_mode) {
        mcm_d3d.on_display_mode(&w, &h, &bpp, &rate);
    }
    hr = ORIG(SetDisplayMode_fn, dd, 21)(dd, w, h, bpp, rate, flags);
    mcm_log("d3d: display mode %lux%lux%lu %luHz -> %08lx", w, h, bpp, rate, hr);
    if (hr == 0) {
        mcm_d3d.width = (int)w;
        mcm_d3d.height = (int)h;
        g_dd = dd;
    }
    return hr;
}

void *mcm_d3d_offscreen(int w, int h)
{
    DDSURFACEDESC2 d;
    void *s = NULL;
    if (!g_dd) {
        return NULL;
    }
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = (DWORD)w;
    d.dwHeight = (DWORD)h;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    if (ORIG(CreateSurface_fn, g_dd, 6)(g_dd, &d, &s, NULL) != 0) {
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        if (ORIG(CreateSurface_fn, g_dd, 6)(g_dd, &d, &s, NULL) != 0) {
            return NULL;
        }
    }
    return s;
}

static void hook_ddraw(void *dd)
{
    void **vt = *(void ***)dd;
    if (!first_time(vt)) {
        return;
    }
    TRACE("d3d: IDirectDraw7 %p, vtable in %s", dd, module_of(vt[0]));
    vhook(vt, 0, dd_QueryInterface);
    vhook(vt, 6, dd_CreateSurface);
    vhook(vt, 8, dd_EnumDisplayModes);
    vhook(vt, 20, dd_SetCooperativeLevel);
    vhook(vt, 21, dd_SetDisplayMode);
}

static HRESULT (WINAPI *real_DirectDrawCreateEx)(GUID *, void **, REFIID, IUnknown *);

static HRESULT WINAPI hk_DirectDrawCreateEx(GUID *guid, void **out, REFIID iid, IUnknown *outer)
{
    HRESULT hr = real_DirectDrawCreateEx(guid, out, iid, outer);
    TRACE("d3d: DirectDrawCreateEx(%s) -> %08lx", guid ? "device" : "primary", hr);
    if (hr == 0 && out && *out) {
        hook_ddraw(*out);
    }
    return hr;
}

void mcm_d3d_install(void)
{
    InitializeCriticalSection(&g_vt_cs);
    if (!mcm_ini_int("Debug", "D3DHooks", 1)) {
        mcm_log("d3d: D3DHooks=0, the video patches are off");
        return;
    }
    g_trace = mcm_ini_int("Debug", "TraceD3D", 0);
    g_trace_draws = g_trace ? mcm_ini_int("Debug", "TraceDraws", 300) : 0;
    g_clear_screen = mcm_ini_int("Video", "ClearScreen", 1);
    real_DirectDrawCreateEx = mcm_hook_import("blade.dll", "DirectDrawCreateEx", hk_DirectDrawCreateEx);
    if (!real_DirectDrawCreateEx) {
        mcm_log("d3d: DirectDrawCreateEx import missing, the video patches are off");
    }
}
