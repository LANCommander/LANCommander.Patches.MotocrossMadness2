/* The movies (ui\intro.avi, ui\events.avi behind the main menu, ui\credits.avi).
 *
 * They are Indeo Video 5 (IV50), a codec Windows no longer ships. The game plays them with DirectShow's
 * IAMMultiMediaStream into a DirectDraw surface (CMovie, vtable 0x55a1f8; Open 0x4a3ad0). Without the codec
 * OpenFile fails, Open frees the movie widget and returns 0, and the main menu (0x44c462 and others) calls
 * through the null pointer the game kept: "Access Violation at 0x44c468" on every start.
 *
 * Two fixes:
 *   - The Indeo 5 decoder (mcm2patch\ir50.dll, FFmpeg's, LGPL) is installed for this process only with
 *     ICInstall(ICINSTALL_FUNCTION), so DirectShow's AVI decompressor finds it like a system codec. No
 *     registry, no system install. [Movies] Decoder=0 turns it off.
 *   - If a movie still cannot be opened, Open succeeds with an empty movie instead: the COM objects it made
 *     are released and a do-nothing IAMMultiMediaStream takes their place, so the widget stays valid and
 *     simply never has a frame (CMovie's update checks for a sample, 0x4a3d70). The intro then ends at once. */
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"
#include <vfw.h>

#define OPEN_FAIL    0x4a3d1fu   /* every failure in CMovie::Open: mov edx,[esi]; mov ecx,esi; call [edx+8] */
#define OPEN_OK      0x4a3d35u   /* the success exit: copies the size and returns esi */

/* CMovie fields (this = esi in Open) */
#define M_SURFACE    0x2c        /* IDirectDrawSurface the frames go to */
#define M_WIDTH      0x30
#define M_HEIGHT     0x34
#define M_SURFACE2   0x38        /* the same surface as IDirectDrawSurface3 (QueryInterface 0x55b380) */
#define M_STREAM     0x50        /* IAMMultiMediaStream */
#define M_DDRAW      0x54        /* IDirectDraw (QueryInterface 0x55b310 of the game's DirectDraw) */
#define M_MEDIA      0x5c        /* IMediaStream (primary video) */
#define M_DDSTREAM   0x60        /* IDirectDrawMediaStream */
#define M_SAMPLE     0x64        /* IDirectDrawStreamSample */

/* ---------------------------------------------------------------- the empty movie */

typedef struct { void **vtbl; } com_t;

static HRESULT WINAPI ns_query(com_t *self, REFIID iid, void **out) { (void)self; (void)iid; if (out) *out = NULL; return E_NOINTERFACE; }
static ULONG WINAPI ns_addref(com_t *self) { (void)self; return 1; }
static ULONG WINAPI ns_release(com_t *self) { (void)self; return 1; }
static HRESULT WINAPI ns_fail2(com_t *self, void *a) { (void)self; (void)a; return E_FAIL; }
static HRESULT WINAPI ns_fail3(com_t *self, void *a, void *b) { (void)self; (void)a; (void)b; return E_FAIL; }
static HRESULT WINAPI ns_time(com_t *self, LONGLONG *t) { (void)self; if (t) *t = 0; return S_OK; }
static HRESULT WINAPI ns_seek(com_t *self, LONGLONG t) { (void)self; (void)t; return S_OK; }
static HRESULT WINAPI ns_fail4(com_t *self, void *a, void *b, void *c) { (void)self; (void)a; (void)b; (void)c; return E_FAIL; }
static HRESULT WINAPI ns_fail5(com_t *self, void *a, void *b, void *c, void *d) { (void)self; (void)a; (void)b; (void)c; (void)d; return E_FAIL; }

/* IAMMultiMediaStream: IUnknown, IMultiMediaStream (GetInformation .. GetEndOfStreamEventHandle), then
   Initialize, GetFilterGraph, GetFilter, AddMediaStream, OpenFile, OpenMoniker, Render */
static void *g_null_vtbl[] = {
    ns_query, ns_addref, ns_release,
    ns_fail3,               /* GetInformation(flags*, type*) */
    ns_fail3,               /* GetMediaStream(purpose, stream**) */
    ns_fail3,               /* EnumMediaStreams(index, stream**) */
    ns_fail2,               /* GetState(state*) */
    ns_fail2,               /* SetState(state): CMovie::Start (0x4a3ef0) then leaves the missing sample alone */
    ns_time,                /* GetTime(time*) */
    ns_time,                /* GetDuration(time*) */
    ns_seek,                /* Seek(time) */
    ns_fail2,               /* GetEndOfStreamEventHandle(handle*) */
    ns_fail4,               /* Initialize(type, flags, graph) */
    ns_fail2,               /* GetFilterGraph(graph**) */
    ns_fail2,               /* GetFilter(filter**) */
    ns_fail5,               /* AddMediaStream(unknown, purpose, flags, stream**) */
    ns_fail3,               /* OpenFile(name, flags) */
    ns_fail4,               /* OpenMoniker(ctx, moniker, flags) */
    ns_fail2,               /* Render(flags) */
};
static com_t g_null_stream = { g_null_vtbl };
static int g_failures;

static void release(char *movie, int offset)
{
    com_t *p = *(com_t **)(movie + offset);
    if (p && p != &g_null_stream) {
        ((ULONG (WINAPI *)(com_t *))p->vtbl[2])(p);
    }
    *(com_t **)(movie + offset) = NULL;
}

/* CMovie::Open failed: make it an empty movie */
static void __cdecl open_failed(char *movie)
{
    static const int owned[] = { M_SAMPLE, M_SURFACE2, M_SURFACE, M_DDSTREAM, M_MEDIA, M_DDRAW, M_STREAM };
    size_t i;
    for (i = 0; i < sizeof owned / sizeof owned[0]; i++) {
        release(movie, owned[i]);
    }
    *(com_t **)(movie + M_STREAM) = &g_null_stream;
    if (!*(int *)(movie + M_WIDTH) || !*(int *)(movie + M_HEIGHT)) {
        *(int *)(movie + M_WIDTH) = 16;
        *(int *)(movie + M_HEIGHT) = 16;
    }
    if (g_failures++ < 8) {
        mcm_log("movie: a movie could not be opened (no Indeo 5 decoder?); it is skipped");
    }
}

/* jumped to from 0x4a3d1f with esi = the movie; leaves through Open's success exit with ebx = 0 (the rect
   fields it stores) */
__attribute__((naked)) static void open_fail_stub(void)
{
    __asm__ volatile(
        "push %%esi\n\t"
        "call %P0\n\t"
        "add $4, %%esp\n\t"
        "xor %%ebx, %%ebx\n\t"
        "push $0x4a3d35\n\t"
        "ret\n\t"
        :: "i"(open_failed));
}

/* ---------------------------------------------------------------- the Indeo 5 decoder */

typedef LRESULT (CALLBACK *driverproc_fn)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);

static void install_decoder(void)
{
    char path[MAX_PATH];
    HMODULE dll;
    driverproc_fn proc;
    BOOL (VFWAPI *ic_install)(DWORD, DWORD, LPARAM, LPSTR, UINT);
    HMODULE vfw = LoadLibraryA("msvfw32.dll");
    snprintf(path, sizeof path, "%smcm2patch\\ir50.dll", mcm_game_dir());
    dll = LoadLibraryA(path);
    if (!dll) {
        mcm_log("movie: no %s (%lu), the movies need an installed Indeo 5 codec", path, GetLastError());
        return;
    }
    proc = (driverproc_fn)(void (*)(void))GetProcAddress(dll, "DriverProc");
    ic_install = vfw ? (BOOL (VFWAPI *)(DWORD, DWORD, LPARAM, LPSTR, UINT))(void (*)(void))GetProcAddress(vfw, "ICInstall") : NULL;
    if (!proc || !ic_install) {
        mcm_log("movie: %s has no DriverProc, or msvfw32 has no ICInstall", path);
        return;
    }
    if (ic_install(ICTYPE_VIDEO, mmioFOURCC('I', 'V', '5', '0'), (LPARAM)proc, NULL, ICINSTALL_FUNCTION)) {
        mcm_log("movie: Indeo 5 decoder installed for this process (%s)", path);
    } else {
        mcm_log("movie: ICInstall of the Indeo 5 decoder failed");
    }
}

void mcm_movie_install(void)
{
    static const uint8_t fail[] = { 0x8b, 0x16, 0x8b, 0xce, 0xff, 0x52, 0x08 };
    if (mcm_ini_int("Movies", "Decoder", 1)) {
        install_decoder();
    }
    if (mcm_expect(OPEN_FAIL, fail, sizeof fail) && mcm_jmp(OPEN_FAIL, open_fail_stub)) {
        mcm_log("movie: a movie that cannot be opened is skipped instead of crashing the menu");
    } else {
        mcm_log("movie: CMovie::Open does not match, left alone");
    }
}
