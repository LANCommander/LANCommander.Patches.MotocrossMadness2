/* The race HUD and the in-race dialogs at the size they have on a 640x480 screen ([Widescreen] ScaleHud): scaled by
 * height / 480 and kept in their corner, on any resolution and shape.
 *
 * How the game draws them in a race (the menus always run in 640x480, where nothing changes):
 *   - The overlays (Overlay, overlay.cpp; created by 0x4e4fa0): a textured quad plus what the class adds (needle,
 *     track lines, text quads), drawn with DrawPrimitive and pre-transformed vertices in their Render method
 *     (vtable +0x38). Overlay::Init (0x4ba690) takes a rectangle either in pixels or, flag +0xe4, in 640x480
 *     units that 0x4baad0 stretches by (w - 1) / 640 and (h - 1) / 480 (the speedometer: wider on a wide screen);
 *     the quad's corners end up at +0x44 / +0x48 and +0x84 / +0x88. Stats (position, laps, times) is 128 pixels
 *     on every screen, the radar 128 pixels up to 800 wide and 256 above (radar256.tga).
 *     Each Render is wrapped: its vertices are scaled to (128 x 128 units) * height / 480 towards the screen
 *     corner or edge the overlay sits at (a copy is drawn, the game's vertices stay as they are).
 *   - Text on the screen: TextQueueOverlay (the messages above the bottom-left corner, 0x520aa0) and
 *     DropTextOverlay (0x520740) draw GDI text into the back buffer (GetDC), with fonts of a fixed pixel height.
 *     DrawTextA / TextOutA on the back buffer's DC get the font scaled; the message queue's rectangle also moves
 *     out of its corner like the overlays (the drop text is placed in 640x480 units already).
 *   - Dialogs over a paused race (In-Game Options, ...): each frame the paused picture is blitted to the whole back
 *     buffer, then the dialog's pieces (Blt) and their GDI text, then the mouse pointer (a colour-keyed Blt). What
 *     was drawn over the picture is scaled about the screen centre when the frame is complete, and the pointer
 *     drawn after it where it points at the scaled dialog (so clicks still land on what is under it). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcm2patch.h"
#include "d3d.h"

static int g_on;

static float screen_scale(int h)
{
    return (float)h / 480.0f;
}

/* where a box sits along a screen side of the given size: 0 (near), size / 2 (middle) or size (far) */
static float anchor(float lo, float hi, float size)
{
    float c = (lo + hi) * 0.5f;
    return c < size / 3.0f ? 0.0f : c > size * 2.0f / 3.0f ? size : size * 0.5f;
}

/* ---------------------------------------------------------------- overlays (Direct3D) */

typedef struct { int on; float ax, ay, kx, ky; } xform_t;
static xform_t g_xf;                 /* the transform of the overlay being rendered */

static void *on_draw(void *dev, DWORD fvf, void *verts, DWORD n, DWORD stride)
{
    static BYTE *buf;
    static size_t cap;
    DWORD i;
    (void)dev;
    (void)fvf;
    if (!g_xf.on || !n || stride < 8) {
        return NULL;
    }
    if (n * stride > cap) {
        BYTE *b = realloc(buf, n * stride);
        if (!b) {
            return NULL;
        }
        buf = b;
        cap = n * stride;
    }
    memcpy(buf, verts, n * stride);
    for (i = 0; i < n; i++) {
        float *v = (float *)(buf + i * stride);
        v[0] = g_xf.ax + (v[0] - g_xf.ax) * g_xf.kx;
        v[1] = g_xf.ay + (v[1] - g_xf.ay) * g_xf.ky;
    }
    return buf;
}

typedef struct { uint32_t slot, orig; int radar; const char *name; } overlay_t;
static const overlay_t OVERLAYS[] = {
    { 0x55d914 + 0x38, 0x51dfc0, 0, "speedometer" },          /* InstrumentOverlay */
    { 0x55db54 + 0x38, 0x521370, 1, "radar" },                /* RadarOverlay */
    { 0x55da04 + 0x38, 0x51f000, 0, "stats" },                /* StatsOverlay */
    { 0x55dbd8 + 0x38, 0x5239e0, 0, "chat" },                 /* ChatOverlay */
};
#define NOVERLAYS ((int)(sizeof OVERLAYS / sizeof OVERLAYS[0]))

static void __fastcall overlay_render(char *ov, void *unused)
{
    uint32_t slot = *(uint32_t *)ov + 0x38;
    const char *view = *(const char **)(ov + 0x18);
    int i, w = *(const int *)(view + 0xc), h = *(const int *)(view + 0x10);
    float s = screen_scale(h);
    (void)unused;
    for (i = 0; i < NOVERLAYS - 1 && OVERLAYS[i].slot != slot; i++) {
    }
    if (i < NOVERLAYS - 1) {
        mcm_d3d.race_hud = 3;        /* a race on screen (d3d.c, [Video] ClearScreen); not the chat line */
    }
    if (g_on && s > 1.01f && w > 0) {
        float x0 = *(float *)(ov + 0x44), y0 = *(float *)(ov + 0x48), x1 = *(float *)(ov + 0x84), y1 = *(float *)(ov + 0x88);
        if (OVERLAYS[i].radar) {
            g_xf.kx = g_xf.ky = 128.0f * s / (x1 - x0 > 16 ? x1 - x0 : 128.0f);   /* 128 or 256 pixels */
        } else if (*(int *)(ov + 0xe4)) {
            g_xf.kx = s * 640.0f / (float)(w - 1);                                /* 640x480 units */
            g_xf.ky = s * 480.0f / (float)(h - 1);
        } else {
            g_xf.kx = g_xf.ky = s;                                                /* pixels */
        }
        g_xf.ax = anchor(x0, x1, (float)w);
        g_xf.ay = anchor(y0, y1, (float)h);
        g_xf.on = 1;
    }
    ((void (__fastcall *)(char *, void *))OVERLAYS[i].orig)(ov, NULL);
    g_xf.on = 0;
}

/* ---------------------------------------------------------------- dialogs over a paused race */

static int g_dialog;                 /* this frame restored a whole-screen picture */
static RECT g_ui;                    /* what was drawn over it */
static int g_dialog_shown;           /* the last frame shown had a dialog */
static struct {
    int on;
    void *src;
    RECT srcr, dst;
    DWORD flags;
    DDBLTFX fx;
    int has_fx;
} g_pointer;
static void *g_tmp;
static int g_tmp_w, g_tmp_h;

static void scale_rect(const RECT *in, RECT *out, float cx, float cy, float s)
{
    out->left = (LONG)floorf(cx + ((float)in->left - cx) * s);
    out->top = (LONG)floorf(cy + ((float)in->top - cy) * s);
    out->right = (LONG)ceilf(cx + ((float)in->right - cx) * s);
    out->bottom = (LONG)ceilf(cy + ((float)in->bottom - cy) * s);
}

static int on_backbuffer_blt(void *back, RECT *dst, void *src, RECT *srcr, DWORD flags, void *fx)
{
    int w = mcm_d3d.width, h = mcm_d3d.height;
    (void)back;
    if (!g_on || screen_scale(h) <= 1.01f || !src || (flags & DDBLT_COLORFILL)) {
        return 0;
    }
    if (!dst || (dst->left <= 0 && dst->top <= 0 && dst->right >= w && dst->bottom >= h)) {
        g_dialog = 1;                /* the paused race as the background */
        SetRectEmpty(&g_ui);
        return 0;
    }
    if (!g_dialog) {
        return 0;
    }
    if ((flags & DDBLT_KEYSRC) && srcr && srcr->right - srcr->left <= 48 && srcr->bottom - srcr->top <= 48
        && !g_pointer.on) {
        /* the mouse pointer: drawn once the dialog is scaled */
        g_pointer.on = 1;
        g_pointer.src = src;
        ((ULONG (WINAPI *)(void *))(*(void ***)src)[1])(src);   /* AddRef */
        g_pointer.srcr = *srcr;
        g_pointer.dst = *dst;
        g_pointer.flags = flags;
        g_pointer.has_fx = fx != NULL;
        if (fx) {
            g_pointer.fx = *(DDBLTFX *)fx;
        }
        return 1;
    }
    UnionRect(&g_ui, &g_ui, dst);
    return 0;
}

static void on_frame_done(void *back)
{
    int w = mcm_d3d.width, h = mcm_d3d.height;
    float s = screen_scale(h), cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    if (g_dialog && !IsRectEmpty(&g_ui) && s > 1.01f) {
        RECT src = g_ui, dst;
        IntersectRect(&src, &src, &(RECT){ 0, 0, w, h });
        scale_rect(&src, &dst, cx, cy, s);
        /* the part of the scaled dialog that fits on the screen */
        if (dst.left < 0) { src.left += (LONG)ceilf((float)-dst.left / s); dst.left = (LONG)floorf(cx + ((float)src.left - cx) * s); }
        if (dst.top < 0) { src.top += (LONG)ceilf((float)-dst.top / s); dst.top = (LONG)floorf(cy + ((float)src.top - cy) * s); }
        if (dst.right > w) { src.right -= (LONG)ceilf((float)(dst.right - w) / s); dst.right = (LONG)ceilf(cx + ((float)src.right - cx) * s); }
        if (dst.bottom > h) { src.bottom -= (LONG)ceilf((float)(dst.bottom - h) / s); dst.bottom = (LONG)ceilf(cy + ((float)src.bottom - cy) * s); }
        if (!g_tmp || g_tmp_w != w || g_tmp_h != h) {
            if (g_tmp) {
                ((ULONG (WINAPI *)(void *))(*(void ***)g_tmp)[2])(g_tmp);
            }
            g_tmp = mcm_d3d_offscreen(w, h);
            g_tmp_w = w;
            g_tmp_h = h;
            if (!g_tmp) {
                mcm_log("hud: no offscreen surface for the dialogs, they stay at their pixel size");
            }
        }
        if (g_tmp && !IsRectEmpty(&src) && mcm_d3d_blt(g_tmp, &src, back, &src, DDBLT_WAIT, NULL) == 0) {
            mcm_d3d_blt(back, &dst, g_tmp, &src, DDBLT_WAIT, NULL);
        }
    }
    if (g_pointer.on) {
        RECT dst;
        float px = (float)g_pointer.dst.left, py = (float)g_pointer.dst.top;
        dst.left = (LONG)(cx + (px - cx) * s);
        dst.top = (LONG)(cy + (py - cy) * s);
        dst.right = dst.left + (LONG)((float)(g_pointer.dst.right - g_pointer.dst.left) * s + 0.5f);
        dst.bottom = dst.top + (LONG)((float)(g_pointer.dst.bottom - g_pointer.dst.top) * s + 0.5f);
        if (dst.left < w && dst.top < h && dst.right > 0 && dst.bottom > 0) {
            RECT srcr = g_pointer.srcr;
            /* clipped by hand: a Blt that does not fit the surface fails */
            if (dst.right > w) { srcr.right -= (LONG)((float)(dst.right - w) / s); dst.right = w; }
            if (dst.bottom > h) { srcr.bottom -= (LONG)((float)(dst.bottom - h) / s); dst.bottom = h; }
            if (dst.left < 0) { srcr.left += (LONG)((float)-dst.left / s); dst.left = 0; }
            if (dst.top < 0) { srcr.top += (LONG)((float)-dst.top / s); dst.top = 0; }
            if (!IsRectEmpty(&srcr) && !IsRectEmpty(&dst)) {
                HRESULT hr = mcm_d3d_blt(back, &dst, g_pointer.src, &srcr, g_pointer.flags,
                                         g_pointer.has_fx ? &g_pointer.fx : NULL);
                static int logged;
                if (hr && !logged++) {
                    mcm_log("hud: the pointer over a dialog cannot be drawn (%08lx)", hr);
                }
            }
        }
        ((ULONG (WINAPI *)(void *))(*(void ***)g_pointer.src)[2])(g_pointer.src);   /* Release */
        g_pointer.on = 0;
    }
    g_dialog_shown = g_dialog;
    g_dialog = 0;
    SetRectEmpty(&g_ui);
}

/* GameCursor::Render (0x43fce0; GUICursor's 0x486100 calls it) draws the pointer at the mouse device's position, the
   floats at +0x24 of the X and Y axis objects (cursor+0x3c, +0x40), clamped to the screen. Over a scaled dialog only
   the middle 1 / s of the screen is shown, so the position is kept in there: the pointer stays visible. */
#define CURSOR_RENDER 0x43fce0

static int __fastcall cursor_render(char *cur, void *unused)
{
    int w = mcm_d3d.width, h = mcm_d3d.height;
    float s = screen_scale(h);
    char *ax = *(char **)(cur + 0x3c), *ay = *(char **)(cur + 0x40);
    (void)unused;
    if (g_on && g_dialog_shown && s > 1.01f && ax && ay) {
        float *x = (float *)(ax + 0x24), *y = (float *)(ay + 0x24);
        float cx = (float)w * 0.5f, cy = (float)h * 0.5f, rx = cx / s, ry = cy / s;
        *x = *x < cx - rx ? cx - rx : *x > cx + rx - 2.0f ? cx + rx - 2.0f : *x;
        *y = *y < cy - ry ? cy - ry : *y > cy + ry - 2.0f ? cy + ry - 2.0f : *y;
    }
    return ((int (__fastcall *)(char *, void *))CURSOR_RENDER)(cur, NULL);
}

/* ---------------------------------------------------------------- GDI text on the screen */

static int (WINAPI *real_DrawTextA)(HDC, LPCSTR, int, LPRECT, UINT);
static BOOL (WINAPI *real_TextOutA)(HDC, int, int, LPCSTR, int);

#define FONTS 16
static struct { LOGFONTA orig; int pct; HFONT scaled; } g_fonts[FONTS];
static int g_next_font;

/* the DC's font, s times as tall */
static HFONT scaled_font(HDC dc, float s)
{
    HFONT f = GetCurrentObject(dc, OBJ_FONT);
    LOGFONTA lf;
    int i, pct = (int)(s * 100.0f + 0.5f);
    if (!f || GetObjectA(f, sizeof lf, &lf) != sizeof lf) {
        return NULL;
    }
    for (i = 0; i < FONTS; i++) {
        if (g_fonts[i].scaled && g_fonts[i].pct == pct && !memcmp(&g_fonts[i].orig, &lf, sizeof lf)) {
            return g_fonts[i].scaled;
        }
    }
    i = g_next_font;
    g_next_font = (g_next_font + 1) % FONTS;
    if (g_fonts[i].scaled) {
        DeleteObject(g_fonts[i].scaled);
    }
    g_fonts[i].orig = lf;
    g_fonts[i].pct = pct;
    lf.lfHeight = (LONG)lroundf((float)(lf.lfHeight ? lf.lfHeight : -12) * s);
    lf.lfWidth = (LONG)lroundf((float)lf.lfWidth * s);
    g_fonts[i].scaled = CreateFontIndirectA(&lf);
    return g_fonts[i].scaled;
}

/* the scale for text the game draws with this DC: 0 = leave it */
static float text_scale(HDC dc)
{
    float s = screen_scale(mcm_d3d.height);
    if (!g_on || !dc || dc != mcm_d3d.backbuffer_dc || s <= 1.01f || g_dialog) {
        return 0;                    /* not the screen (overlay textures), menus, or a dialog (scaled whole) */
    }
    return s;
}

static int WINAPI hk_DrawTextA(HDC dc, LPCSTR text, int n, LPRECT r, UINT fmt)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    float s = text_scale(dc), w = (float)mcm_d3d.width, h = (float)mcm_d3d.height, ax, ay;
    RECT t;
    HFONT f, old;
    int ret;
    if (!s || !r || (fmt & DT_CALCRECT) || !(f = scaled_font(dc, s))) {
        return real_DrawTextA(dc, text, n, r, fmt);
    }
    if (caller >= 0x520aa0 && caller < 0x520b90) {
        /* the message queue, laid out in pixels from the bottom-left corner */
        ax = anchor((float)r->left, (float)r->right, w);
        ay = anchor((float)r->top, (float)r->bottom, h);
    } else {
        /* placed for the screen already: the text grows from the point it is aligned to */
        ax = fmt & DT_CENTER ? (float)(r->left + r->right) * 0.5f : fmt & DT_RIGHT ? (float)r->right : (float)r->left;
        ay = fmt & DT_VCENTER ? (float)(r->top + r->bottom) * 0.5f : fmt & DT_BOTTOM ? (float)r->bottom : (float)r->top;
    }
    t.left = (LONG)(ax + ((float)r->left - ax) * s);
    t.right = (LONG)(ax + ((float)r->right - ax) * s);
    t.top = (LONG)(ay + ((float)r->top - ay) * s);
    t.bottom = (LONG)(ay + ((float)r->bottom - ay) * s);
    old = SelectObject(dc, f);
    ret = real_DrawTextA(dc, text, n, &t, fmt);
    SelectObject(dc, old);
    return ret;
}

static BOOL WINAPI hk_TextOutA(HDC dc, int x, int y, LPCSTR text, int n)
{
    float s = text_scale(dc);
    HFONT f, old;
    BOOL ret;
    if (!s || !(f = scaled_font(dc, s))) {
        return real_TextOutA(dc, x, y, text, n);
    }
    old = SelectObject(dc, f);
    ret = real_TextOutA(dc, x, y, text, n);
    SelectObject(dc, old);
    return ret;
}

/* ---------------------------------------------------------------- */

void mcm_hud_install(void)
{
    int i, n = 0;
    /* hooked either way: the overlays also tell d3d.c that a race is on screen */
    for (i = 0; i < NOVERLAYS; i++) {
        uint32_t hook = (uint32_t)(uintptr_t)overlay_render;
        n += mcm_patch(OVERLAYS[i].slot, &OVERLAYS[i].orig, &hook, 4);
    }
    if (!mcm_ini_int("Widescreen", "ScaleHud", 1)) {
        return;
    }
    {
        uint32_t orig = CURSOR_RENDER, hook = (uint32_t)(uintptr_t)cursor_render;
        mcm_patch(0x556400, &orig, &hook, 4);                   /* GameCursor vtable */
        mcm_redirect_call(0x486114, CURSOR_RENDER, cursor_render);   /* GUICursor::Render */
    }
    real_DrawTextA = mcm_hook_import("USER32.dll", "DrawTextA", hk_DrawTextA);
    real_TextOutA = mcm_hook_import("GDI32.dll", "TextOutA", hk_TextOutA);
    if (!real_DrawTextA || !real_TextOutA) {
        mcm_log("hud: text drawing imports missing, the HUD is not scaled");
        return;
    }
    mcm_d3d.on_draw = on_draw;
    mcm_d3d.on_backbuffer_blt = on_backbuffer_blt;
    mcm_d3d.on_frame_done = on_frame_done;
    g_on = 1;
    mcm_log("hud: HUD and in-race dialogs scaled with the screen height (%d of %d overlays)", n, NOVERLAYS);
}
