/* Indeo Video 5 (IV50) decoder as a Video for Windows codec, for the game's movies (README.md).
 *
 * A VfW DriverProc around FFmpeg's indeo5 decoder. The patch (src/movie.c) loads this DLL and installs
 * DriverProc with ICInstall(ICTYPE_VIDEO, 'IV50', ..., ICINSTALL_FUNCTION): it is a codec of the game's
 * process only, found by ICLocate / ICOpen like an installed one (DirectShow's AVI Decompressor, AVIFile).
 *
 * Input: IV50 / iv50. Output: uncompressed RGB, 16 (BI_RGB = 555, BI_BITFIELDS 555 or 565), 24 or 32 bits,
 * bottom-up (positive biHeight) or top-down. The decoder gives YUV 4:1:0 (AV_PIX_FMT_YUV410P), converted
 * here with BT.601 studio range. A frame that decodes to nothing (an empty chunk, an Indeo null frame)
 * repeats the last picture: the caller's output buffer may be a different one each time. */
#include <windows.h>
#include <vfw.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#define FOURCC_IV50 mmioFOURCC('I', 'V', '5', '0')

typedef struct {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;        /* the last decoded picture */
    int have;              /* frame holds a picture */
    uint8_t *buf;          /* padded copy of the input chunk */
    int bufsize;
} inst_t;

/* ---------------------------------------------------------------- formats */

static DWORD upper_fourcc(DWORD f)
{
    DWORD r = 0;
    int i;
    for (i = 0; i < 4; i++) {
        BYTE c = (BYTE)(f >> (8 * i));
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        r |= (DWORD)c << (8 * i);
    }
    return r;
}

static int input_ok(const BITMAPINFOHEADER *in)
{
    return in && upper_fourcc(in->biCompression) == FOURCC_IV50 && in->biWidth > 0 && in->biHeight != 0;
}

enum { OUT_BAD, OUT_555, OUT_565, OUT_24, OUT_32 };

static int output_kind(const BITMAPINFOHEADER *out)
{
    if (out->biCompression == BI_RGB) {
        switch (out->biBitCount) {
        case 16: return OUT_555;
        case 24: return OUT_24;
        case 32: return OUT_32;
        }
    } else if (out->biCompression == BI_BITFIELDS) {
        const DWORD *mask = (const DWORD *)((const BYTE *)out + out->biSize);
        if (out->biSize >= sizeof(BITMAPV4HEADER)) {
            mask = &((const BITMAPV4HEADER *)out)->bV4RedMask;
        }
        if (out->biBitCount == 16 && mask[0] == 0xf800 && mask[1] == 0x07e0 && mask[2] == 0x001f) {
            return OUT_565;
        }
        if (out->biBitCount == 16 && mask[0] == 0x7c00 && mask[1] == 0x03e0 && mask[2] == 0x001f) {
            return OUT_555;
        }
        if (out->biBitCount == 32 && mask[0] == 0xff0000 && mask[1] == 0xff00 && mask[2] == 0xff) {
            return OUT_32;
        }
    }
    return OUT_BAD;
}

static LRESULT query(const BITMAPINFOHEADER *in, const BITMAPINFOHEADER *out)
{
    if (!input_ok(in)) {
        return ICERR_BADFORMAT;
    }
    if (!out) {
        return ICERR_OK;
    }
    if (output_kind(out) == OUT_BAD || out->biWidth != in->biWidth || abs(out->biHeight) != abs(in->biHeight)
        || out->biPlanes > 1) {
        return ICERR_BADFORMAT;
    }
    return ICERR_OK;
}

static LRESULT get_format(const BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
    if (!input_ok(in)) {
        return ICERR_BADFORMAT;
    }
    if (!out) {
        return sizeof(BITMAPINFOHEADER);
    }
    memset(out, 0, sizeof *out);
    out->biSize = sizeof *out;
    out->biWidth = in->biWidth;
    out->biHeight = abs(in->biHeight);
    out->biPlanes = 1;
    out->biBitCount = 24;
    out->biCompression = BI_RGB;
    out->biSizeImage = ((out->biWidth * 24 + 31) / 32) * 4 * out->biHeight;
    return ICERR_OK;
}

/* ---------------------------------------------------------------- YUV -> RGB */

static int g_ty[256], g_trv[256], g_tgu[256], g_tgv[256], g_tbu[256];
static int g_tables;

static void make_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        g_ty[i] = (i - 16) * 76309;          /* 1.164 << 16 */
        g_trv[i] = (i - 128) * 104597;       /* 1.596 */
        g_tgu[i] = (i - 128) * 25675;        /* 0.391 */
        g_tgv[i] = (i - 128) * 53279;        /* 0.813 */
        g_tbu[i] = (i - 128) * 132201;       /* 2.018 */
    }
    g_tables = 1;
}

static inline int clamp8(int v)
{
    v >>= 16;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void convert(const AVFrame *f, const BITMAPINFOHEADER *out, BYTE *dst, int xdst, int ydst)
{
    int kind = output_kind(out);
    int bpp = out->biBitCount / 8;
    int w = f->width, h = f->height;
    int ow = out->biWidth, oh = abs(out->biHeight);
    int stride = ((ow * out->biBitCount + 31) / 32) * 4;
    int x, y;
    if (!g_tables) {
        make_tables();
    }
    if (w > ow - xdst) w = ow - xdst;
    if (h > oh - ydst) h = oh - ydst;
    for (y = 0; y < h; y++) {
        const uint8_t *py = f->data[0] + y * f->linesize[0];
        const uint8_t *pu = f->data[1] + (y >> 2) * f->linesize[1];
        const uint8_t *pv = f->data[2] + (y >> 2) * f->linesize[2];
        int row = ydst + y;
        BYTE *d = dst + (size_t)(out->biHeight > 0 ? oh - 1 - row : row) * stride + (size_t)xdst * bpp;
        for (x = 0; x < w; x++) {
            int yy = g_ty[py[x]], u = pu[x >> 2], v = pv[x >> 2];
            int r = clamp8(yy + g_trv[v]), g = clamp8(yy - g_tgu[u] - g_tgv[v]), b = clamp8(yy + g_tbu[u]);
            switch (kind) {
            case OUT_32:
                d[0] = (BYTE)b; d[1] = (BYTE)g; d[2] = (BYTE)r; d[3] = 0xff;
                d += 4;
                break;
            case OUT_24:
                d[0] = (BYTE)b; d[1] = (BYTE)g; d[2] = (BYTE)r;
                d += 3;
                break;
            case OUT_565:
                *(WORD *)d = (WORD)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                d += 2;
                break;
            default:
                *(WORD *)d = (WORD)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
                d += 2;
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- decoding */

static void end(inst_t *in)
{
    avcodec_free_context(&in->ctx);
    av_packet_free(&in->pkt);
    av_frame_free(&in->frame);
    av_freep(&in->buf);
    in->bufsize = 0;
    in->have = 0;
}

static LRESULT begin(inst_t *in, const BITMAPINFOHEADER *bin, const BITMAPINFOHEADER *bout)
{
    const AVCodec *codec;
    if (query(bin, bout) != ICERR_OK) {
        return ICERR_BADFORMAT;
    }
    end(in);
    codec = avcodec_find_decoder(AV_CODEC_ID_INDEO5);
    if (!codec || !(in->ctx = avcodec_alloc_context3(codec))) {
        return ICERR_MEMORY;
    }
    in->ctx->width = bin->biWidth;
    in->ctx->height = abs(bin->biHeight);
    in->ctx->codec_tag = bin->biCompression;
    in->pkt = av_packet_alloc();
    in->frame = av_frame_alloc();
    if (!in->pkt || !in->frame || avcodec_open2(in->ctx, codec, NULL) < 0) {
        end(in);
        return ICERR_ERROR;
    }
    return ICERR_OK;
}

static LRESULT decode(inst_t *in, const BITMAPINFOHEADER *bin, const void *src, const BITMAPINFOHEADER *bout,
                      void *dst, int xdst, int ydst)
{
    DWORD size = bin->biSizeImage;
    if (!in->ctx) {
        LRESULT r = begin(in, bin, bout);
        if (r != ICERR_OK) {
            return r;
        }
    }
    if (size && src) {
        if ((int)size + AV_INPUT_BUFFER_PADDING_SIZE > in->bufsize) {
            av_freep(&in->buf);
            in->bufsize = (int)size + AV_INPUT_BUFFER_PADDING_SIZE;
            in->buf = av_malloc(in->bufsize);
            if (!in->buf) {
                in->bufsize = 0;
                return ICERR_MEMORY;
            }
        }
        memcpy(in->buf, src, size);
        memset(in->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        in->pkt->data = in->buf;
        in->pkt->size = (int)size;
        if (avcodec_send_packet(in->ctx, in->pkt) >= 0) {
            AVFrame *f = av_frame_alloc();
            while (f && avcodec_receive_frame(in->ctx, f) >= 0) {
                av_frame_unref(in->frame);
                av_frame_move_ref(in->frame, f);
                in->have = 1;
            }
            av_frame_free(&f);
        }
    }
    if (in->have && dst) {
        convert(in->frame, bout, dst, xdst, ydst);
    }
    return ICERR_OK;
}

/* ---------------------------------------------------------------- the driver */

static LRESULT get_info(ICINFO *info, DWORD size)
{
    if (!info || size < sizeof *info) {
        return 0;
    }
    memset(info, 0, sizeof *info);
    info->dwSize = sizeof *info;
    info->fccType = ICTYPE_VIDEO;
    info->fccHandler = FOURCC_IV50;
    info->dwVersion = 1;
    info->dwVersionICM = ICVERSION;
    lstrcpyW(info->szName, L"IV50");
    lstrcpyW(info->szDescription, L"Indeo Video 5 (FFmpeg, mcm2patch)");
    return sizeof *info;
}

static LRESULT driver(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2);

/* FFmpeg is built without threading (its one-time initialisation is not locked): one call at a time */
static SRWLOCK g_lock = SRWLOCK_INIT;

__declspec(dllexport) LRESULT CALLBACK DriverProc(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2)
{
    LRESULT r;
    AcquireSRWLockExclusive(&g_lock);
    r = driver(id, drv, msg, p1, p2);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static LRESULT driver(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2)
{
    inst_t *in = (inst_t *)id;
    (void)drv;
    switch (msg) {
    case DRV_LOAD:
    case DRV_FREE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_OPEN: {
        ICOPEN *open = (ICOPEN *)p2;
        if (open && open->fccType != ICTYPE_VIDEO && open->fccType != 0) {
            return 0;
        }
        in = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *in);
        if (open) {
            open->dwError = in ? ICERR_OK : ICERR_MEMORY;
        }
        return (LRESULT)in;
    }
    case DRV_CLOSE:
        if (in) {
            end(in);
            HeapFree(GetProcessHeap(), 0, in);
        }
        return 1;
    case DRV_QUERYCONFIGURE:
        return 0;
    case ICM_GETINFO:
        return get_info((ICINFO *)p1, (DWORD)p2);
    case ICM_GETSTATE:
    case ICM_SETSTATE:
        return 0;
    case ICM_DECOMPRESS_QUERY:
        return query((BITMAPINFOHEADER *)p1, (BITMAPINFOHEADER *)p2);
    case ICM_DECOMPRESS_GET_FORMAT:
        return get_format((BITMAPINFOHEADER *)p1, (BITMAPINFOHEADER *)p2);
    case ICM_DECOMPRESS_GET_PALETTE:
        return ICERR_BADFORMAT;
    case ICM_DECOMPRESS_BEGIN:
        return in ? begin(in, (BITMAPINFOHEADER *)p1, (BITMAPINFOHEADER *)p2) : ICERR_ERROR;
    case ICM_DECOMPRESS: {
        ICDECOMPRESS *d = (ICDECOMPRESS *)p1;
        if (!in || !d) {
            return ICERR_ERROR;
        }
        return decode(in, d->lpbiInput, d->lpInput, d->lpbiOutput, d->lpOutput, 0, 0);
    }
    case ICM_DECOMPRESS_END:
        if (in) {
            end(in);
        }
        return ICERR_OK;
    case ICM_DECOMPRESSEX_QUERY: {
        ICDECOMPRESSEX *d = (ICDECOMPRESSEX *)p1;
        return d ? query(d->lpbiSrc, d->lpbiDst) : ICERR_BADFORMAT;
    }
    case ICM_DECOMPRESSEX_BEGIN: {
        ICDECOMPRESSEX *d = (ICDECOMPRESSEX *)p1;
        return in && d ? begin(in, d->lpbiSrc, d->lpbiDst) : ICERR_ERROR;
    }
    case ICM_DECOMPRESSEX: {
        ICDECOMPRESSEX *d = (ICDECOMPRESSEX *)p1;
        if (!in || !d) {
            return ICERR_ERROR;
        }
        return decode(in, d->lpbiSrc, d->lpSrc, d->lpbiDst, d->lpDst, d->xDst, d->yDst);
    }
    case ICM_DECOMPRESSEX_END:
        if (in) {
            end(in);
        }
        return ICERR_OK;
    }
    if (msg < DRV_USER) {
        return 0;
    }
    return ICERR_UNSUPPORTED;
}
