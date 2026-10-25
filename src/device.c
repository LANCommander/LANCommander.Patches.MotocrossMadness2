/* No "Please choose a 3-D card" dialog.
 *
 * At start-up (0x4c475e) the game calls SelectVideoCard (0x4d14b0, cdecl: force dialog, use last card, out
 * flag) over its DirectDraw/Direct3D device table (0x690894[], count at 0x6908a4, at most 4). It skips the
 * dialog only when "UseLastVideoCard" is set and "UseVideoCardIdx" names a device, or when exactly one hardware
 * device (flag 1 at +0x1b8) is not disabled in its DriverInfo key. dgVoodoo 2 offers two hardware devices
 * next to the software rasteriser, so the dialog came up on every start.
 *
 * With [Video] SkipDeviceDialog=1 the call goes here: if the stored choice is not a hardware device, the first
 * hardware device is stored as the choice, and the original runs as if "use last card" were set.
 * [Video] Device=<n> forces a device index instead. */
#include "mcm2patch.h"

#define SELECT_CARD   0x4d14b0u
#define SELECT_SITE   0x4c475eu
#define DEVICES       ((char **)0x690894)
#define DEVICE_COUNT  (*(int *)0x6908a4)
#define DEV_FLAGS     0x1b8
#define DEV_GUID      0x4bc

typedef void *(__cdecl *select_fn)(int force, int use_last, int *out);

static int usable(int i)
{
    return i >= 0 && i < DEVICE_COUNT && i < 4 && DEVICES[i] && (DEVICES[i][DEV_FLAGS] & 1);
}

static void *__cdecl select_card(int force, int use_last, int *out)
{
    int forced = mcm_ini_int("Video", "Device", -1);
    int idx = forced >= 0 ? forced : mcm_reg_get_dword("", "UseVideoCardIdx", -1);
    int i;
    if (!usable(idx)) {
        for (idx = -1, i = 0; i < DEVICE_COUNT && i < 4 && idx < 0; i++) {
            if (usable(i)) {
                idx = i;
            }
        }
    }
    if (idx < 0 || force) {
        mcm_log("device: %s, the game asks", force ? "dialog requested" : "no hardware 3-D device");
        return ((select_fn)SELECT_CARD)(force, use_last, out);
    }
    mcm_reg_set_dword("", "UseVideoCardIdx", (DWORD)idx);
    mcm_log("device: using 3-D device %d of %d (%s)", idx, DEVICE_COUNT, DEVICES[idx] + DEV_GUID);
    return ((select_fn)SELECT_CARD)(force, 1, out);
}

void mcm_device_install(void)
{
    if (!mcm_ini_int("Video", "SkipDeviceDialog", 1)) {
        return;
    }
    if (mcm_redirect_call(SELECT_SITE, SELECT_CARD, select_card)) {
        mcm_log("device: 3-D card dialog skipped");
    }
}
