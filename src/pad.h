/* Shared between gamepad.c (SDL, the DirectInput wrapper, the fake joysticks) and pad_rumble.c (force
 * feedback effects -> rumble). */
#ifndef PAD_H
#define PAD_H
#define SDL_MAIN_HANDLED
#include <SDL.h>
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <dinput.h>

#define PAD_MAX 4                     /* SDL gamepads offered to the game as joysticks */

/* gamepad.c */
SDL_GameController *pad_controller(int slot);
int pad_trigger_down(void *device, DWORD ofs);   /* the button at this data format offset is held */
extern int pad_rumble_strength;       /* percent, 0 = rumble off */
extern int pad_rumble_sustain;        /* percent of a steady (infinite) force that is felt */

/* pad_rumble.c: IDirectInputEffect objects of a fake joystick */
HRESULT padfx_create(void *device, int slot, REFGUID guid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out);
int padfx_supported(REFGUID guid, DIEFFECTINFOA *info);   /* fills info if non-NULL */
int padfx_enum_effects(LPDIENUMEFFECTSCALLBACKA cb, void *ref, DWORD type);
int padfx_enum_created(void *device, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ref);
void padfx_command(void *device, int slot, DWORD cmd);   /* SendForceFeedbackCommand */
DWORD padfx_state(void *device, int slot);               /* GetForceFeedbackState */
void padfx_set_gain(int slot, DWORD gain);               /* DIPROP_FFGAIN */
void padfx_device_gone(void *device);                    /* the fake joystick was released */
void padfx_frame(void);                                  /* drive the motors (game thread) */

#endif
