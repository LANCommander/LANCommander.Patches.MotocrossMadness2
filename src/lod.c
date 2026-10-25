/* Level of detail: the distances at which the scenery switches to its detailed models.
 *
 * Trees and bushes (the ecosystem, EcoSystem.cpp) are flat billboards in the distance and 3-D models near the
 * camera. Which one an instance gets is decided per frame by 0x457650 from its distance on the ground to the camera,
 * against a row of a static table picked by the Options - Graphics "Ecosystem" slider (app+0x1ed8, 0..9, kept in
 * the ecosystem at +0x5c0). The table is 0x5707e8 with a hardware 3-D card, 0x5706a8 without (app+0x2d0 set,
 * 0x45a2c3); a row is 8 ints:
 *     +0x00  3-D distance: beyond it the billboard only            (hardware table: 20 .. 150)
 *     +0x04  fade start: from here to the 3-D distance the model fades into the billboard (with BlendLODs)
 *     +0x08  billboard texture format, +0x0c billboard draw distance (256 .. 768), +0x10..+0x1c render switches
 * Even on the top setting the models stop at 150 units, a few bike lengths, which is the pop-in. [Detail]
 * TreeDistance scales the 3-D and fade distances of every row; the billboard distance is left alone. The game
 * reads the table every frame, so it is patched once at start.
 *
 * Every other model with levels of detail (soultree "AutoLOD#i", D3DIMSoulTree.CPP: riders, bikes, buildings,
 * props) picks one in 0x444e9f by its size on screen: with s = the camera's focal length in pixels / distance,
 * level i+1 is used where s * AutoLOD#i * the object's LOD scale (+0x284) < 1.0. [Detail] ModelDistance points that
 * 1.0 (fcomp [0x555748], shared with a lot of other code) at our own 100 / percent, so each switch happens that
 * many times further away. */
#include <string.h>
#include "mcm2patch.h"

#define ECO_TABLE_SOFT 0x5706a8u
#define ECO_TABLE_HARD 0x5707e8u
#define ECO_ROWS       10
#define ECO_ROW_INTS   8
#define MAX_PERCENT    1000

#define LOD_COMPARE    0x444ec2u   /* fcomp dword ptr [0x555748] in the AutoLOD loop */

static float g_lod_one = 1.0f;

static int scale_trees(uint32_t table, int percent)
{
    static const int32_t top[2] = { 150, 75 };   /* the top row of both tables, as a check */
    int32_t *row = (int32_t *)MCM_ADDR(table), v[2];
    int i;
    if (!mcm_expect(table + 9 * ECO_ROW_INTS * 4, top, sizeof top)) {
        mcm_log("lod: ecosystem table %08x differs, left alone", table);
        return 0;
    }
    for (i = 0; i < ECO_ROWS; i++, row += ECO_ROW_INTS) {
        v[0] = (int32_t)((int64_t)row[0] * percent / 100);
        v[1] = (int32_t)((int64_t)row[1] * percent / 100);
        if (!mcm_write((uint32_t)(uintptr_t)row, v, sizeof v)) {
            return 0;
        }
    }
    return 1;
}

static void install_trees(int percent)
{
    if (percent == 100) {
        return;
    }
    if (scale_trees(ECO_TABLE_HARD, percent) & scale_trees(ECO_TABLE_SOFT, percent)) {
        mcm_log("lod: trees and bushes in 3-D up to %d%% of the original distance (%d units on the top setting)",
                percent, 150 * percent / 100);
    }
}

static void install_models(int percent)
{
    static const uint8_t orig[6] = { 0xd8, 0x1d, 0x48, 0x57, 0x55, 0x00 };
    uint8_t code[6] = { 0xd8, 0x1d };
    float *one = &g_lod_one;
    if (percent == 100) {
        return;
    }
    g_lod_one = 100.0f / (float)percent;
    memcpy(code + 2, &one, 4);
    if (mcm_patch(LOD_COMPARE, orig, code, sizeof code)) {
        mcm_log("lod: model detail levels switch at %d%% of the original distance", percent);
    }
}

static int read_percent(const char *key, int def)
{
    int p = mcm_ini_int("Detail", key, def);
    if (p < 10 || p > MAX_PERCENT) {
        mcm_log("lod: [Detail] %s=%d out of range (10..%d), using 100", key, p, MAX_PERCENT);
        p = 100;
    }
    return p;
}

void mcm_lod_install(void)
{
    install_trees(read_percent("TreeDistance", 300));
    install_models(read_percent("ModelDistance", 200));
}
