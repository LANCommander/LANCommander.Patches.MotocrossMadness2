/* Widescreen.
 *
 * What the game does on a wide display mode (measured at 1280x720 and 1920x1080):
 *   - Menus, the front end and its dialogs always run in 640x480 (the game switches to it, 0x45f7c9), whatever
 *     race resolution is chosen; dgVoodoo 2 scales that to the screen (pillarboxed: dgvoodoo.c).
 *   - Races run in the resolution chosen in Options - Graphics (an index into the device's mode table, kept in
 *     the profile: app+0xfc4, set at 0x454906 / 0x49ac27).
 *   - The HUD: the speedometer is stretched by width / 640 and height / 480, the radar and the position panel are
 *     drawn at their pixel size, and so are the in-race dialogs (hud.c scales them all with the height).
 *   - The 3-D view was vert-: the projection keeps the horizontal view (_11) and derives the vertical one from
 *     the viewport (_22 = _11 * w / h), so a wide screen cut the top and bottom off a 4:3 picture.
 * This module makes the 3-D view Hor+ ([Widescreen] HorPlus): the vertical view a 4:3 screen has, and as much
 * of the sides as the screen's shape adds.
 *
 * The wide modes themselves come from the video driver. dgVoodoo 2 refuses a mode it did not list
 * (DDERR_UNSUPPORTEDMODE, after which the game crashes at 0x4c975e), so dgvoodoo.c adds them to dgVoodoo.conf.
 * The patch logs when the driver offers no wide mode. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"
#include "d3d.h"

/* ---------------------------------------------------------------- display modes */

static int g_wide_modes, g_modes_seen;

static int on_enum_mode(DDSURFACEDESC2 *d)
{
    g_modes_seen++;
    if (d->dwWidth * 3 > d->dwHeight * 4 + 8 && d->ddpfPixelFormat.dwRGBBitCount >= 16) {
        g_wide_modes++;
    }
    return 1;
}

static void on_enum_done(LPDDENUMMODESCALLBACK2 cb, void *ctx)
{
    static int logged;
    (void)cb;
    (void)ctx;
    if (!logged && g_modes_seen) {
        logged = 1;
        if (g_wide_modes) {
            mcm_log("widescreen: %d wide display modes offered", g_wide_modes);
        } else {
            mcm_log("widescreen: the video driver offers no wide display modes; with dgVoodoo 2 add them in dgVoodoo.conf, "
                    "[DirectXExt] ExtraEnumeratedResolutions = max, max_16_9, 1920x1080");
        }
    }
    g_wide_modes = g_modes_seen = 0;
}

/* ---------------------------------------------------------------- 3-D */

/* Camera::UpdateMatrices (0x42fd50, vtable slot 0x70 of every camera class) builds, from the horizontal field of
   view in degrees (camera+0x16c) and the viewport size (+0x1a8, +0x1ac): the projection (+0x6c, by 0x4a29d0:
   _11 = cot(fov / 2), _22 = _11 * w / h), view * projection (+0x12c), which the game culls the track and objects
   with, and the focal length in pixels (+0x198, w / 2 / tan(fov / 2)). Hor+ runs it with the horizontal view
   widened to what keeps a 4:3 screen's vertical view, so everything drawn, culled and measured agrees (widening only
   the matrix Direct3D gets left the track at the sides culled away). The field of view goes back afterwards: the
   camera code animates it. Square render-to-texture views are not wider than 4:3 and are left alone. */
#define CAM_UPDATE 0x42fd50
static const uint32_t CAM_VTABLE_SLOTS[] = { 0x555ce8, 0x5560a0, 0x557954, 0x55987c, 0x55b0a4, 0x55c830, 0x55e024 };

static void __fastcall cam_update(char *cam, void *unused)
{
    float *fov = (float *)(cam + 0x16c), keep = *fov;
    int w = *(int *)(cam + 0x1a8), h = *(int *)(cam + 0x1ac);
    (void)unused;
    if (h > 0 && w * 3 > h * 4 + 4 && keep > 1.0f && keep < 170.0f) {
        double half = keep * 0.5 * 3.14159265358979 / 180.0;
        *fov = (float)(2.0 * atan(tan(half) * (double)w * 3.0 / ((double)h * 4.0)) * 180.0 / 3.14159265358979);
    }
    ((void (__fastcall *)(char *, void *))CAM_UPDATE)(cam, NULL);
    *fov = keep;
}

/* The sky cube (cubedraw.cpp: 6 faces of 4 x 4 tiles, drawn by 0x43f460) picks its tiles with two tests of its own
   that do not go through the matrices, so the sky lost tiles at the top and the sides of a Hor+ view:
     - 0x5347b0, per face: visible if dot(view direction, face normal) <= (fov - 10) * 0.0046154, from the field of
       view at camera+0x16c, which is the 4:3 one again once the matrices are built;
     - 0x535130 (from 0x534b40), per tile: corners in view space, inside if |x| <= z and |y| <= z * h / w, a 90 degree
       horizontal view (it gives up and keeps everything above 90) with the vertical one from the screen's shape.
       Hor+ keeps the taller 4:3 vertical view, so a 16:9 screen's top and bottom rows of sky were dropped.
   Both get the view actually drawn: the face test the widened field of view, the tile test its corners scaled so
   that its fixed 90 degrees x h / w covers the real view (never narrower than the original test). */
#define SKY_FACE_TEST 0x5347b0
#define SKY_FACE_CALL 0x43f361
#define SKY_TILE_TEST 0x535130
#define SKY_TILE_CALL 0x534bcf

/* the horizontal field of view drawn (degrees) and the tangents of the half view across and up */
static int drawn_view(char *cam, float *fov, double *tan_x, double *tan_y)
{
    int w = *(int *)(cam + 0x1a8), h = *(int *)(cam + 0x1ac);
    double f = *(float *)(cam + 0x16c), half;
    if (h <= 0 || w <= 0 || f <= 1.0 || f >= 170.0) {
        return 0;
    }
    half = f * 0.5 * 3.14159265358979 / 180.0;
    if (w * 3 > h * 4 + 4) {    /* as cam_update: the vertical view of 4:3 */
        *tan_y = tan(half) * 3.0 / 4.0;
        *tan_x = *tan_y * w / h;
    } else {
        *tan_x = tan(half);
        *tan_y = *tan_x * h / w;
    }
    *fov = (float)(2.0 * atan(*tan_x) * 180.0 / 3.14159265358979);
    return 1;
}

static int __stdcall sky_face_test(char *cam, const float *dir, const float *normal)
{
    float fov = *(float *)(cam + 0x16c), dot = dir[0] * normal[0] + dir[1] * normal[1] + dir[2] * normal[2];
    double tx, ty;
    drawn_view(cam, &fov, &tx, &ty);
    return !((fov - 10.0f) * 0.004615380f < dot);
}

static int __stdcall sky_tile_test(char *cam, float *pts, int n)
{
    typedef int (__stdcall *test_fn)(char *, float *, int);
    float fov, copy[16 * 4];
    double tx, ty, aspect, sx, sy;
    int i, r;
    if (n <= 0 || n > 16 || !drawn_view(cam, &fov, &tx, &ty)) {
        return ((test_fn)SKY_TILE_TEST)(cam, pts, n);
    }
    aspect = (double)*(int *)(cam + 0x1ac) / *(int *)(cam + 0x1a8);
    sx = tx > 1.0 ? tx : 1.0;
    sy = ty > aspect ? ty / aspect : 1.0;
    for (i = 0; i < n; i++) {
        copy[i * 4] = (float)(pts[i * 4] / sx);
        copy[i * 4 + 1] = (float)(pts[i * 4 + 1] / sy);
        copy[i * 4 + 2] = pts[i * 4 + 2];
        copy[i * 4 + 3] = pts[i * 4 + 3];
    }
    r = ((test_fn)SKY_TILE_TEST)(cam, copy, n);
    for (i = 0; i < n; i++) {
        pts[i * 4 + 3] = copy[i * 4 + 3];   /* the outcodes it writes */
    }
    return r;
}

static void install_horplus(void)
{
    uint32_t orig = CAM_UPDATE, hook = (uint32_t)(uintptr_t)cam_update;
    int i, n = 0;
    for (i = 0; i < (int)(sizeof CAM_VTABLE_SLOTS / sizeof CAM_VTABLE_SLOTS[0]); i++) {
        n += mcm_patch(CAM_VTABLE_SLOTS[i], &orig, &hook, 4);
    }
    mcm_log("widescreen: Hor+ 3-D view on wide screens (%d of %d camera classes)", n,
            (int)(sizeof CAM_VTABLE_SLOTS / sizeof CAM_VTABLE_SLOTS[0]));
    if (mcm_redirect_call(SKY_FACE_CALL, SKY_FACE_TEST, sky_face_test)
        & mcm_redirect_call(SKY_TILE_CALL, SKY_TILE_TEST, sky_tile_test)) {
        mcm_log("widescreen: the sky is culled with the view drawn");
    }
}

void mcm_widescreen_install(void)
{
    mcm_d3d.on_enum_mode = on_enum_mode;
    mcm_d3d.on_enum_done = on_enum_done;
    if (mcm_ini_int("Widescreen", "HorPlus", 1)) {
        install_horplus();
    }
}
