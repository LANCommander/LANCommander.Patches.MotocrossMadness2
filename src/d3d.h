#ifndef MCM_D3D_H
#define MCM_D3D_H
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <ddraw.h>
#include <d3d.h>

/* d3d.c: callbacks into the video modules, all optional, called on the game thread */
typedef struct {
    void (*on_display_mode)(DWORD *w, DWORD *h, DWORD *bpp, DWORD *rate);   /* before SetDisplayMode */
    int (*on_enum_mode)(DDSURFACEDESC2 *desc);         /* 0 = hide this mode from the game */
    void (*on_enum_done)(LPDDENUMMODESCALLBACK2 cb, void *ctx);   /* after the modes: may report more */
    void (*on_create_surface)(DDSURFACEDESC2 *desc);   /* before CreateSurface */
    void (*on_device)(void *device);                   /* a Direct3D device was created */
    void (*on_transform)(void *device, D3DTRANSFORMSTATETYPE type, D3DMATRIX *m);   /* may change m */
    void (*on_viewport)(void *device, D3DVIEWPORT7 *vp);                            /* may change vp */
    /* pre-transformed (XYZRHW) vertices about to be drawn: may return a changed copy to draw instead */
    void *(*on_draw)(void *device, DWORD fvf, void *verts, DWORD n, DWORD stride);
    void (*on_begin_scene)(void *device);
    void (*on_end_scene)(void *device);
    /* a Blt to the back buffer: 1 = handled, the original is not called */
    int (*on_backbuffer_blt)(void *back, RECT *dst, void *src, RECT *srcr, DWORD flags, void *fx);
    void (*on_frame_done)(void *back);                 /* the back buffer is complete, about to be shown */
    void (*on_present)(void *surface);                 /* a frame is shown (Flip, or Blt to the primary) */
    int width, height;                                 /* the display mode */
    HDC backbuffer_dc;                                 /* the back buffer's GetDC, while the game holds it */
    int race_hud;                                      /* hud.c: > 0 while the race HUD is being drawn (frames) */
} mcm_d3d_t;

extern mcm_d3d_t mcm_d3d;
void mcm_d3d_install(void);
/* the original IDirectDrawSurface7::Blt / GetCaps, and an offscreen surface of the given size (NULL on failure) */
HRESULT mcm_d3d_blt(void *dst, RECT *dstr, void *src, RECT *srcr, DWORD flags, void *fx);
void *mcm_d3d_offscreen(int w, int h);
/* the next frame shown, as 24-bit bottom-up RGB (valid until the next capture); 0 on timeout */
int mcm_d3d_capture(BYTE **rgb, int *w, int *h, DWORD timeout);
void mcm_d3d_trace_draws(int n, int only_2d);   /* log the next n draws (test harness) */
#endif
