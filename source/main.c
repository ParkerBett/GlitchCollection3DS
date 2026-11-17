// Glitch Collection - native 3DS app: show/episode menu + FFmpeg video & audio player
//
// Embedded RomFS layout:
//   romfs:/GlitchCollection/<folder>/main.jpg   show cover   (512x256 JPEG)
//   romfs:/GlitchCollection/<folder>/<n>.jpg    episode image (512x256 JPEG)
//   romfs:/GlitchCollection/<folder>/<n>.ogv    episode video (Theora + Vorbis)
//
// Menu:    D-Pad / Circle Pad = move, A = select, B = back, tap a row to select.
// Playing: A (or tap) = pause/resume, B = stop and go back.
// START exits from anywhere. There is NO automatic next-episode playback.

#include <3ds.h>
#include <citro2d.h>
#include <jpeglib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define ROOT "romfs:/GlitchCollection"
#define TOP_W 400.0f
#define BOT_W 320.0f

#define VIDEO_WIDTH    400
#define VIDEO_HEIGHT   224
#define TEXTURE_WIDTH  512
#define TEXTURE_HEIGHT 256
#define VIDEO_FPS      24

typedef struct { const char* name; const char* folder; int count; } Show;
static const Show SHOWS[] = {
    { "The Amazing Digital Circus", "amazing_digital_circus", 9 },
    { "Murder Drones",              "murder_drones",          8 },
};
#define NSHOWS ((int)(sizeof(SHOWS) / sizeof(SHOWS[0])))

typedef enum { ST_SHOWS, ST_EPISODES, ST_PLAY } State;

/* ================================================================== text */
static C2D_TextBuf g_textBuf;

static void draw_text(const char* s, float x, float y, float scale, u32 color) {
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw_wrapped(const char* s, float x, float y, float scale, u32 color, float width) {
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.5f, scale, scale, color, width);
}

/* Read video data from an embedded RomFS file for FFmpeg. */
static int romfs_read(void* opaque, uint8_t* buffer, int bufferSize) {
    FILE* file = (FILE*)opaque;
    size_t bytes = fread(buffer, 1, (size_t)bufferSize, file);

    if (bytes == 0) {
        if (feof(file))
            return AVERROR_EOF;
        return AVERROR(errno ? errno : EIO);
    }

    return (int)bytes;
}

/* Handle FFmpeg seek requests, including requests for the file size. */
static int64_t romfs_seek(void* opaque, int64_t offset, int whence) {
    FILE* file = (FILE*)opaque;

    if (whence == AVSEEK_SIZE) {
        off_t current = ftello(file);
        if (current < 0)
            return AVERROR(errno ? errno : EIO);

        if (fseeko(file, 0, SEEK_END) != 0)
            return AVERROR(errno ? errno : EIO);

        off_t size = ftello(file);
        if (size < 0)
            return AVERROR(errno ? errno : EIO);

        if (fseeko(file, current, SEEK_SET) != 0)
            return AVERROR(errno ? errno : EIO);

        return (int64_t)size;
    }

    whence &= ~AVSEEK_FORCE;

    if (fseeko(file, (off_t)offset, whence) != 0)
        return AVERROR(errno ? errno : EIO);

    off_t position = ftello(file);
    if (position < 0)
        return AVERROR(errno ? errno : EIO);

    return (int64_t)position;
}

/* ================================================================== covers
   Covers now use the SAME proven path as the video: decode to RGB565, tile it ourselves
   (Morton order) and upload with C3D_TexUpload. (The earlier GPU display-transfer path
   produced rotated/stretched images on the real console.) */
static unsigned int morton8(unsigned int x, unsigned int y) {
    return
        (x & 1)        |
        ((y & 1) << 1) |
        ((x & 2) << 1) |
        ((y & 2) << 2) |
        ((x & 4) << 2) |
        ((y & 4) << 3);
}

/* Linear RGB565 -> tiled (Morton) texture layout. srcW and srcH must be multiples of 8.
   Only the srcW x srcH area is written; the rest of destination is left as it was
   (allocate it with calloc so the padding stays black). */
static void swizzle_rgb565(const uint8_t* source, int sourceStride, int srcW, int srcH,
                           uint16_t* destination, int texW) {
    static unsigned char mt[8][8];
    static int mtReady = 0;

    if (!mtReady) {
        for (unsigned int py = 0; py < 8; py++)
            for (unsigned int px = 0; px < 8; px++)
                mt[py][px] = (unsigned char)morton8(px, py);
        mtReady = 1;
    }

    const int tilesX = texW / 8;
    const int wTiles = srcW / 8;
    const int hTiles = srcH / 8;

    for (int ty = 0; ty < hTiles; ty++) {
        for (int tx = 0; tx < wTiles; tx++) {
            uint16_t* tile = destination + ((size_t)ty * tilesX + tx) * 64;

            for (int py = 0; py < 8; py++) {
                const uint16_t* row =
                    (const uint16_t*)(source + (size_t)(ty * 8 + py) * sourceStride) + tx * 8;

                for (int px = 0; px < 8; px++)
                    tile[mt[py][px]] = row[px];
            }
        }
    }
}

struct my_err { struct jpeg_error_mgr pub; jmp_buf jb; };

static void my_error_exit(j_common_ptr c) {
    struct my_err* e = (struct my_err*)c->err;
    longjmp(e->jb, 1);
}

/* Returns a malloc'd, already-tiled RGB565 buffer (w*h*2 bytes), or NULL. */
static uint16_t* load_jpeg_tiled565(const char* path, int* ow, int* oh, char* err, size_t errlen) {
    FILE* f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "missing: %s", path); return NULL; }

    struct jpeg_decompress_struct cinfo;
    struct my_err jerr;
    uint16_t* volatile lin = NULL;
    unsigned char* volatile row = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        if (lin) free(lin);
        if (row) free(row);
        snprintf(err, errlen, "jpeg decode failed: %s", path);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    bool pow2 = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    if (!pow2 || w < 8 || h < 8 || w > 1024 || h > 1024 || cinfo.output_components != 3) {
        snprintf(err, errlen, "bad image %dx%d (need power of 2)", w, h);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return NULL;
    }

    lin = (uint16_t*)malloc((size_t)w * h * 2);
    row = (unsigned char*)malloc((size_t)w * 3);
    if (!lin || !row) {
        snprintf(err, errlen, "out of memory (%dx%d)", w, h);
        if (lin) free(lin);
        if (row) free(row);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return NULL;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&cinfo, rp, 1);
        for (int x = 0; x < w; x++) {
            unsigned r = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
            lin[(size_t)y * w + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(row);

    uint16_t* tiled = (uint16_t*)calloc(1, (size_t)w * h * 2);
    if (!tiled) {
        free(lin);
        snprintf(err, errlen, "out of memory (tiling %dx%d)", w, h);
        return NULL;
    }

    swizzle_rgb565((const uint8_t*)lin, w * 2, w, h, tiled, w);
    free(lin);
    *ow = w;
    *oh = h;
    return tiled;
}

static C3D_Tex g_tex;
static bool g_hasTex = false;
static Tex3DS_SubTexture g_sub;
static char g_key[200] = "";
static char g_imgErr[200] = "";

static void unload_cover(void) {
    if (g_hasTex) {
        gspWaitForP3D();
        C3D_TexDelete(&g_tex);
        g_hasTex = false;
    }
    g_key[0] = 0;
}

/* Only one cover texture is ever alive at a time (memory safety on old 3DS/2DS). */
static void load_cover(const char* path) {
    if (strcmp(path, g_key) == 0) return;
    unload_cover();
    strncpy(g_key, path, sizeof g_key - 1);
    g_key[sizeof g_key - 1] = 0;
    g_imgErr[0] = 0;

    int w = 0, h = 0;
    uint16_t* px = load_jpeg_tiled565(path, &w, &h, g_imgErr, sizeof g_imgErr);
    if (!px) return;

    if (!C3D_TexInit(&g_tex, (u16)w, (u16)h, GPU_RGB565)) {
        snprintf(g_imgErr, sizeof g_imgErr, "C3D_TexInit failed (%dx%d)", w, h);
        free(px);
        return;
    }

    C3D_TexUpload(&g_tex, px);
    C3D_TexFlush(&g_tex);
    free(px);
    C3D_TexSetFilter(&g_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* Same orientation convention as the video texture: top = 1.0 is image row 0. */
    g_sub.width = (u16)w;
    g_sub.height = (u16)h;
    g_sub.left = 0.0f;
    g_sub.right = 1.0f;
    g_sub.top = 1.0f;
    g_sub.bottom = 0.0f;
    g_hasTex = true;
}

/* ================================================================== diagnostics */
static int decodeErrorCode = 0;
static char lastLog[200] = "";        /* last FFmpeg warning/error line */
static char decodeInfo[160] = "";     /* details of a failing decode call */

/* Timing (system ticks) so slow stages can be found. */
static u64 tDecode = 0, tConvert = 0, tSwizzle = 0, tUpload = 0, tAudio = 0;
static int timedFrames = 0, droppedFrames = 0;
#define TICKS_TO_MS(t) ((double)(t) * 1000.0 / (double)SYSCLOCK_ARM11)

static void ffmpeg_log(void* ptr, int level, const char* fmt, va_list vl) {
    (void)ptr;
    if (level > AV_LOG_WARNING)
        return;

    char line[256];
    vsnprintf(line, sizeof(line), fmt, vl);

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;

    if (n > 0)
        snprintf(lastLog, sizeof(lastLog), "%s", line);
}

/* ================================================================== audio
   Vorbis packet -> FFmpeg decoder -> swresample (s16 stereo) -> software FIFO
   -> ndsp wave buffers. The DSP, FIFO and wave memory are set up once for the
   whole app; the decoder is opened per episode. */
#define AUDIO_CHANNEL 0
#define WB_COUNT 8
#define WB_FRAMES 2048
#define FIFO_FRAMES 44100
#define AUDIO_TARGET_FRAMES 22050   /* keep about 0.5 s decoded ahead */
#define PREROLL_FRAMES 16384        /* fill every ndsp buffer before starting */

static int g_dspReady = 0;
static char g_dspMsg[120] = "audio not initialised";
static s16* g_fifo = NULL;
static s16* g_wbData = NULL;
static ndspWaveBuf g_wb[WB_COUNT];

static int g_audioStream = -1;
static AVCodecContext* g_adec = NULL;
static SwrContext* g_swr = NULL;
static AVFrame* g_aframe = NULL;
static uint8_t* g_conv = NULL;
static int g_convCap = 0;                       /* in frames */
static int g_fifoRead = 0, g_fifoCount = 0;     /* in frames (1 frame = L+R s16) */
static int g_nextWb = 0;
static int g_audioDropped = 0, g_underruns = 0, g_inUnderrun = 0, g_audioStarted = 0;
static char g_audioStatus[140] = "audio: off";

static void audio_global_init(void) {
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        snprintf(g_dspMsg, sizeof g_dspMsg, "ndspInit failed 0x%08lX (dsp firmware?)", (unsigned long)rc);
        return;
    }

    g_fifo = (s16*)malloc((size_t)FIFO_FRAMES * 4);
    g_wbData = (s16*)linearAlloc((size_t)WB_COUNT * WB_FRAMES * 4);

    if (!g_fifo || !g_wbData) {
        free(g_fifo);
        g_fifo = NULL;
        if (g_wbData) { linearFree(g_wbData); g_wbData = NULL; }
        ndspExit();
        snprintf(g_dspMsg, sizeof g_dspMsg, "audio: out of memory");
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    g_dspReady = 1;
}

static void audio_global_exit(void) {
    if (!g_dspReady)
        return;

    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspExit();
    free(g_fifo);
    g_fifo = NULL;
    linearFree(g_wbData);
    g_wbData = NULL;
    g_dspReady = 0;
}

/* Per-episode cleanup. */
static void audio_close(void) {
    if (g_dspReady)
        ndspChnWaveBufClear(AUDIO_CHANNEL);

    swr_free(&g_swr);

    if (g_aframe)
        av_frame_free(&g_aframe);

    if (g_adec)
        avcodec_free_context(&g_adec);

    free(g_conv);
    g_conv = NULL;
    g_convCap = 0;

    g_fifoRead = g_fifoCount = 0;
    g_nextWb = 0;
    g_audioStream = -1;
    memset(g_wb, 0, sizeof(g_wb));
}

static void audio_fail(const char* message) {
    audio_close();
    snprintf(g_audioStatus, sizeof(g_audioStatus), "audio: %s", message);
}

static void fifo_push(const s16* src, int frames) {
    int space = FIFO_FRAMES - g_fifoCount;

    if (frames > space) {
        g_audioDropped += frames - space;
        frames = space;
    }

    if (frames <= 0)
        return;

    int write = (g_fifoRead + g_fifoCount) % FIFO_FRAMES;
    int first = FIFO_FRAMES - write;

    if (first > frames)
        first = frames;

    memcpy(g_fifo + write * 2, src, (size_t)first * 4);
    memcpy(g_fifo, src + first * 2, (size_t)(frames - first) * 4);
    g_fifoCount += frames;
}

static void fifo_pop(s16* dst, int frames) {
    int first = FIFO_FRAMES - g_fifoRead;

    if (first > frames)
        first = frames;

    memcpy(dst, g_fifo + g_fifoRead * 2, (size_t)first * 4);
    memcpy(dst + first * 2, g_fifo, (size_t)(frames - first) * 4);
    g_fifoRead = (g_fifoRead + frames) % FIFO_FRAMES;
    g_fifoCount -= frames;
}

static void audio_open(AVFormatContext* fmt) {
    g_audioStream = -1;
    g_audioDropped = g_underruns = g_inUnderrun = g_audioStarted = 0;
    snprintf(g_audioStatus, sizeof(g_audioStatus), "audio: off");

    if (!g_dspReady) {
        snprintf(g_audioStatus, sizeof(g_audioStatus), "audio: %s", g_dspMsg);
        return;
    }

    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            g_audioStream = (int)i;
            break;
        }
    }

    if (g_audioStream < 0) {
        audio_fail("no audio stream");
        return;
    }

    AVCodecParameters* par = fmt->streams[g_audioStream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);

    if (!codec) {
        audio_fail("decoder not found");
        return;
    }

    g_adec = avcodec_alloc_context3(codec);

    if (!g_adec ||
        avcodec_parameters_to_context(g_adec, par) < 0 ||
        avcodec_open2(g_adec, codec, NULL) < 0) {
        audio_fail("could not open decoder");
        return;
    }

#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;

    if (swr_alloc_set_opts2(&g_swr,
                            &outLayout, AV_SAMPLE_FMT_S16, g_adec->sample_rate,
                            &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate,
                            0, NULL) < 0) {
        audio_fail("resampler setup failed");
        return;
    }
#else
    int64_t inLayout = g_adec->channel_layout
        ? (int64_t)g_adec->channel_layout
        : av_get_default_channel_layout(g_adec->channels);

    g_swr = swr_alloc_set_opts(NULL,
                               AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, g_adec->sample_rate,
                               inLayout, g_adec->sample_fmt, g_adec->sample_rate,
                               0, NULL);
#endif

    if (!g_swr || swr_init(g_swr) < 0) {
        audio_fail("resampler init failed");
        return;
    }

    g_aframe = av_frame_alloc();

    if (!g_aframe) {
        audio_fail("out of memory");
        return;
    }

    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, (float)g_adec->sample_rate);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(AUDIO_CHANNEL, mix);

    memset(g_wb, 0, sizeof(g_wb));
    g_nextWb = 0;
    g_fifoRead = g_fifoCount = 0;

    snprintf(g_audioStatus, sizeof(g_audioStatus), "audio: %d Hz, %s",
             g_adec->sample_rate, codec->name);
}

static void audio_feed(AVPacket* pkt) {
    if (!g_adec || pkt->size == 0)
        return;

    u64 a0 = svcGetSystemTick();

    if (avcodec_send_packet(g_adec, pkt) >= 0) {
        while (avcodec_receive_frame(g_adec, g_aframe) == 0) {
            int outCap = swr_get_out_samples(g_swr, g_aframe->nb_samples);

            if (outCap < g_aframe->nb_samples)
                outCap = g_aframe->nb_samples + 256;

            if (outCap > g_convCap) {
                free(g_conv);
                g_conv = (uint8_t*)malloc((size_t)outCap * 4);
                g_convCap = g_conv ? outCap : 0;
            }

            if (g_conv) {
                int n = swr_convert(g_swr, &g_conv, g_convCap,
                                    (const uint8_t**)g_aframe->extended_data,
                                    g_aframe->nb_samples);

                if (n > 0)
                    fifo_push((const s16*)g_conv, n);
            }

            av_frame_unref(g_aframe);
        }
    }

    tAudio += svcGetSystemTick() - a0;
}

static int ndsp_queued(void) {
    int queued = 0;
    for (int i = 0; i < WB_COUNT; i++) {
        if (g_wb[i].status == NDSP_WBUF_QUEUED || g_wb[i].status == NDSP_WBUF_PLAYING)
            queued++;
    }
    return queued;
}

/* True when everything decoded has also been played. */
static int audio_drained(void) {
    if (g_audioStream < 0 || !g_dspReady)
        return 1;
    return ndsp_queued() == 0 && g_fifoCount < WB_FRAMES;
}

/* Hands queued audio to whichever ndsp buffer is next. Buffers are used in strict
   ring order so playback order stays correct. */
static void audio_pump(void) {
    if (!g_dspReady || g_audioStream < 0)
        return;

    int queued = ndsp_queued();

    if (g_audioStarted && queued == 0 && !g_inUnderrun) {
        g_underruns++;
        g_inUnderrun = 1;
    }
    if (queued > 0)
        g_inUnderrun = 0;

    while (g_fifoCount >= WB_FRAMES) {
        ndspWaveBuf* wb = &g_wb[g_nextWb];

        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE)
            break;

        s16* data = g_wbData + (size_t)g_nextWb * WB_FRAMES * 2;
        fifo_pop(data, WB_FRAMES);

        wb->data_vaddr = data;
        wb->nsamples = WB_FRAMES;
        DSP_FlushDataCache(data, WB_FRAMES * 4);
        ndspChnWaveBufAdd(AUDIO_CHANNEL, wb);
        g_audioStarted = 1;

        g_nextWb = (g_nextWb + 1) % WB_COUNT;
    }
}

/* Throw away all queued/decoded audio (used when seeking). */
static void audio_seek_reset(void) {
    if (g_audioStream < 0)
        return;

    if (g_dspReady)
        ndspChnWaveBufClear(AUDIO_CHANNEL);

    if (g_adec)
        avcodec_flush_buffers(g_adec);

    g_fifoRead = g_fifoCount = 0;
    g_nextWb = 0;
    g_audioStarted = 0;
    g_inUnderrun = 0;
    memset(g_wb, 0, sizeof(g_wb));
}

/* ================================================================== read-ahead
   Video packets are queued so the file can be read AHEAD of the video decoder.
   That lets audio stay decoded well in advance and survive slow video frames. */
#define VQ_CAP 64

static AVPacket* vq[VQ_CAP];
static int vqHead = 0, vqCount = 0;
static int demuxEnded = 0;

static void demux_reset(void) {
    while (vqCount > 0) {
        av_packet_free(&vq[vqHead]);
        vqHead = (vqHead + 1) % VQ_CAP;
        vqCount--;
    }
    vqHead = 0;
    demuxEnded = 0;
}

/* Read one packet from the file and route it: video -> queue, audio -> decoder. */
static int demux_one(AVFormatContext* fmt, AVPacket* tmp, int videoStream) {
    if (av_read_frame(fmt, tmp) < 0) {
        demuxEnded = 1;
        return 0;
    }

    if (tmp->stream_index == videoStream) {
        AVPacket* copy = (vqCount < VQ_CAP) ? av_packet_alloc() : NULL;

        if (copy) {
            av_packet_move_ref(copy, tmp);
            vq[(vqHead + vqCount) % VQ_CAP] = copy;
            vqCount++;
        } else {
            av_packet_unref(tmp);
        }
    } else {
        if (tmp->stream_index == g_audioStream)
            audio_feed(tmp);

        av_packet_unref(tmp);
    }

    return 1;
}

/* Keep about half a second of audio decoded ahead (bounded work per call). */
static void demux_ahead(AVFormatContext* fmt, AVPacket* tmp, int videoStream) {
    int budget = 32;

    while (budget-- > 0 && !demuxEnded && vqCount < VQ_CAP - 1) {
        int need = (g_audioStream >= 0)
            ? (g_fifoCount < AUDIO_TARGET_FRAMES)
            : (vqCount < 8);

        if (!need)
            break;

        demux_one(fmt, tmp, videoStream);
    }
}

/* ================================================================== video */
/* Convert linear RGB565 video pixels into the tiled layout required by the 3DS GPU. */
static void swizzle_frame(const uint8_t* source, int sourceStride, uint16_t* destination) {
    swizzle_rgb565(source, sourceStride, VIDEO_WIDTH, VIDEO_HEIGHT, destination, TEXTURE_WIDTH);
}

typedef struct {
    int active;                 /* a video is open and playing/paused */
    int failed;                 /* open or decode failed: show message */
    char message[200];
    AVFormatContext* format;
    FILE* romfsFile;
    AVIOContext* io;
    AVCodecContext* decoder;
    AVPacket* packet;
    AVFrame* frame;
    struct SwsContext* scaler;
    uint8_t* rgbData[4];
    int rgbStride[4];
    uint16_t* tiled;
    C3D_Tex tex;
    int texReady;
    Tex3DS_SubTexture sub;
    C2D_Image image;
    int videoStream;
    int inputEnded, flushSent;
    int frames;                 /* frames presented (including repeats) */
    int hasFrame;
    int eof;
    u64 eofMs;
    int paused;
    u64 startMs, pauseMs;
} Player;

static Player P;

/* Releases every resource. Does not touch message/failed so an error stays visible. */
static void player_release(void) {
    gspWaitForP3D();

    if (P.scaler) { sws_freeContext(P.scaler); P.scaler = NULL; }
    if (P.rgbData[0]) av_freep(&P.rgbData[0]);
    free(P.tiled);
    P.tiled = NULL;
    if (P.texReady) { C3D_TexDelete(&P.tex); P.texReady = 0; }
    if (P.frame) av_frame_free(&P.frame);
    if (P.packet) av_packet_free(&P.packet);
    if (P.decoder) avcodec_free_context(&P.decoder);

    demux_reset();
    audio_close();

    if (P.format)
        avformat_close_input(&P.format);

    if (P.io) {
        av_freep(&P.io->buffer);
        avio_context_free(&P.io);
    }

    if (P.romfsFile) {
        fclose(P.romfsFile);
        P.romfsFile = NULL;
    }

    P.active = 0;
}

/* Fully leave playback (user pressed B / video ended). */
static void player_stop(void) {
    player_release();
    P.failed = 0;
    P.message[0] = 0;
    P.paused = 0;
    P.hasFrame = 0;
}

static int player_fail(const char* msg) {
    snprintf(P.message, sizeof(P.message), "%s", msg);
    player_release();
    P.failed = 1;
    return 0;
}

/* After a seek the decoder may reject a few packets until the next keyframe; ignore them. */
static int seekGrace = 0;

/* Return 1 for a decoded frame, 2 for "repeat the previous frame",
   0 at end of file, or -1 on error. */
static int decode_next_frame(void) {
    while (1) {
        int result = avcodec_receive_frame(P.decoder, P.frame);

        if (result == 0)
            return 1;

        if (result == AVERROR_EOF)
            return 0;

        if (result != AVERROR(EAGAIN)) {
            if (seekGrace > 0) { seekGrace--; continue; }
            decodeErrorCode = result;
            snprintf(decodeInfo, sizeof(decodeInfo), "failed in: receive_frame");
            return -1;
        }

        if (P.inputEnded) {
            if (!P.flushSent) {
                result = avcodec_send_packet(P.decoder, NULL);

                if (result < 0 && result != AVERROR(EAGAIN)) {
                    decodeErrorCode = result;
                    snprintf(decodeInfo, sizeof(decodeInfo), "failed in: flush send_packet");
                    return -1;
                }

                P.flushSent = 1;
                continue;
            }

            return 0;
        }

        /* Take the next queued video packet (reading more from the file if needed). */
        while (vqCount == 0 && !demuxEnded)
            demux_one(P.format, P.packet, P.videoStream);

        if (vqCount == 0) {
            P.inputEnded = 1;
            continue;
        }

        av_packet_move_ref(P.packet, vq[vqHead]);
        av_packet_free(&vq[vqHead]);
        vqHead = (vqHead + 1) % VQ_CAP;
        vqCount--;

        if (P.packet->size == 0) {
            /* Theora uses zero-length packets for "repeat the previous frame".
               FFmpeg rejects them (EINVAL), so we handle them ourselves. */
            av_packet_unref(P.packet);
            return 2;
        }

        int packetSize = P.packet->size;

        result = avcodec_send_packet(P.decoder, P.packet);
        av_packet_unref(P.packet);

        if (result < 0 && result != AVERROR(EAGAIN)) {
            if (seekGrace > 0) { seekGrace--; continue; }
            decodeErrorCode = result;
            snprintf(decodeInfo, sizeof(decodeInfo),
                     "send_packet: size=%d codec=%s threads=%d",
                     packetSize,
                     P.decoder->codec ? P.decoder->codec->name : "?",
                     P.decoder->thread_count);
            return -1;
        }
    }
}

/* path is an embedded RomFS path; returns 1 on success. On failure P.failed + P.message are set. */
static int player_open(const char* path) {
    memset(&P, 0, sizeof(P));
    P.videoStream = -1;

    lastLog[0] = 0;
    decodeInfo[0] = 0;
    decodeErrorCode = 0;
    seekGrace = 0;
    tDecode = tConvert = tSwizzle = tUpload = tAudio = 0;
    timedFrames = droppedFrames = 0;
    demux_reset();

    P.romfsFile = fopen(path, "rb");
    if (!P.romfsFile)
        return player_fail("could not open embedded video");

    const int ioBufferSize = 32 * 1024;
    uint8_t* ioBuffer = (uint8_t*)av_malloc(ioBufferSize);
    if (!ioBuffer)
        return player_fail("could not allocate video buffer");

    P.io = avio_alloc_context(
        ioBuffer,
        ioBufferSize,
        0,
        P.romfsFile,
        romfs_read,
        NULL,
        romfs_seek
    );

    if (!P.io) {
        av_free(ioBuffer);
        return player_fail("could not create video reader");
    }

    P.format = avformat_alloc_context();
    if (!P.format)
        return player_fail("could not allocate video context");

    P.format->pb = P.io;
    P.format->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&P.format, NULL, NULL, NULL) < 0)
        return player_fail("could not open embedded video");

    if (avformat_find_stream_info(P.format, NULL) < 0)
        return player_fail("could not read stream info");

    for (unsigned int i = 0; i < P.format->nb_streams; i++) {
        if (P.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            P.videoStream = (int)i;
            break;
        }
    }

    if (P.videoStream < 0)
        return player_fail("no video stream found");

    const AVCodec* codec = avcodec_find_decoder(P.format->streams[P.videoStream]->codecpar->codec_id);

    if (!codec)
        return player_fail("video decoder not found");

    P.decoder = avcodec_alloc_context3(codec);

    if (!P.decoder)
        return player_fail("decoder allocation failed");

    if (avcodec_parameters_to_context(P.decoder, P.format->streams[P.videoStream]->codecpar) < 0 ||
        avcodec_open2(P.decoder, codec, NULL) < 0)
        return player_fail("could not initialise decoder");

    P.packet = av_packet_alloc();
    P.frame = av_frame_alloc();

    if (!P.packet || !P.frame)
        return player_fail("frame allocation failed");

    if (!C3D_TexInit(&P.tex, TEXTURE_WIDTH, TEXTURE_HEIGHT, GPU_RGB565))
        return player_fail("texture allocation failed");

    P.texReady = 1;
    C3D_TexSetFilter(&P.tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&P.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    P.sub.width = VIDEO_WIDTH;
    P.sub.height = VIDEO_HEIGHT;
    P.sub.left = 0.0f;
    P.sub.top = 1.0f;
    P.sub.right = (float)VIDEO_WIDTH / TEXTURE_WIDTH;
    P.sub.bottom = 1.0f - (float)VIDEO_HEIGHT / TEXTURE_HEIGHT;
    P.image.tex = &P.tex;
    P.image.subtex = &P.sub;

    if (av_image_alloc(P.rgbData, P.rgbStride, VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGB565LE, 1) < 0)
        return player_fail("video buffer allocation failed");

    P.tiled = (uint16_t*)calloc(1, TEXTURE_WIDTH * TEXTURE_HEIGHT * sizeof(uint16_t));

    if (!P.tiled)
        return player_fail("texture buffer allocation failed");

    audio_open(P.format);   /* non-fatal: video still plays if audio setup fails */

    /* Prime: read ahead until the sound buffers can all be filled (or the file ends),
       then start audio and the video clock together so they begin in step. */
    while (!demuxEnded && vqCount < VQ_CAP - 1 &&
           (g_audioStream >= 0 ? g_fifoCount < PREROLL_FRAMES : vqCount < 8))
        demux_one(P.format, P.packet, P.videoStream);

    audio_pump();
    P.startMs = osGetTime();
    P.active = 1;
    return 1;
}

static void player_toggle_pause(void) {
    if (!P.active)
        return;

    u64 now = osGetTime();

    if (!P.paused) {
        P.paused = 1;
        P.pauseMs = now;
        if (g_dspReady && g_audioStream >= 0)
            ndspChnSetPaused(AUDIO_CHANNEL, true);
    } else {
        P.paused = 0;
        P.startMs += now - P.pauseMs;
        if (g_dspReady && g_audioStream >= 0)
            ndspChnSetPaused(AUDIO_CHANNEL, false);
    }
}

static unsigned player_elapsed_s(void);
static unsigned player_total_s(void);

/* Jump to (about) sec seconds. Lands on the keyframe at or before that point, then the
   clock is set to where we really landed so picture and sound stay together. */
static void player_seek_to(double sec) {
    if (!P.active || P.eof || P.failed)
        return;

    unsigned total = player_total_s();
    if (total > 0 && sec > (double)total - 2.0)
        sec = (double)total - 2.0;
    if (sec < 0.0)
        sec = 0.0;

    if (av_seek_frame(P.format, -1, (int64_t)(sec * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0)
        return;   /* seek not possible: keep playing as before */

    demux_reset();
    audio_seek_reset();
    avcodec_flush_buffers(P.decoder);
    av_frame_unref(P.frame);
    P.inputEnded = 0;
    P.flushSent = 0;
    seekGrace = 30;

    if (P.paused) {   /* seeking resumes playback so the new picture appears */
        P.paused = 0;
        if (g_dspReady && g_audioStream >= 0)
            ndspChnSetPaused(AUDIO_CHANNEL, false);
    }

    while (!demuxEnded && vqCount < VQ_CAP - 1 &&
           (g_audioStream >= 0 ? g_fifoCount < PREROLL_FRAMES : vqCount < 8))
        demux_one(P.format, P.packet, P.videoStream);

    audio_pump();

    double landing = sec;
    if (vqCount > 0 && vq[vqHead]->pts != AV_NOPTS_VALUE) {
        double t = (double)vq[vqHead]->pts * av_q2d(P.format->streams[P.videoStream]->time_base);
        if (t >= 0.0 && t <= sec + 1.0 && t >= sec - 15.0)
            landing = t;
    }

    P.frames = (int)(landing * VIDEO_FPS + 0.5);
    P.startMs = osGetTime() - (u64)(landing * 1000.0);
}

static unsigned player_elapsed_s(void) {
    u64 now = P.paused ? P.pauseMs : osGetTime();
    return (unsigned)((now - P.startMs) / 1000ULL);
}

/* Total length in whole seconds, or 0 when the container does not report one. */
static unsigned player_total_s(void) {
    if (P.format && P.format->duration > 0)
        return (unsigned)(P.format->duration / AV_TIME_BASE);
    return 0;
}

/* Decodes one frame. skip = decode only, no convert/upload (used to catch up).
   Returns 1 when playback ended or failed (stop looping), otherwise 0. */
static int player_decode_one(int skip, u64 now) {
    u64 t0 = svcGetSystemTick();
    int r = decode_next_frame();
    tDecode += svcGetSystemTick() - t0;

    if (r == 1) {
        if (!P.scaler) {
            P.scaler = sws_getContext(P.frame->width, P.frame->height,
                                      (enum AVPixelFormat)P.frame->format,
                                      VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGB565LE,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);

            if (!P.scaler) {
                player_fail("video conversion failed");
                return 1;
            }
        }

        if (!skip) {
            u64 c0 = svcGetSystemTick();
            sws_scale(P.scaler, (const uint8_t* const*)P.frame->data, P.frame->linesize,
                      0, P.frame->height, P.rgbData, P.rgbStride);
            u64 c1 = svcGetSystemTick();
            swizzle_frame(P.rgbData[0], P.rgbStride[0], P.tiled);
            u64 c2 = svcGetSystemTick();
            C3D_TexUpload(&P.tex, P.tiled);
            C3D_TexFlush(&P.tex);
            u64 c3 = svcGetSystemTick();

            tConvert += c1 - c0;
            tSwizzle += c2 - c1;
            tUpload += c3 - c2;
            timedFrames++;
            P.hasFrame = 1;
        } else {
            droppedFrames++;
        }

        av_frame_unref(P.frame);
        P.frames++;
        return 0;
    }

    if (r == 2) {
        P.frames++;   /* repeat the previous picture */
        return 0;
    }

    if (r == 0) {
        P.eof = 1;
        P.eofMs = now;
        return 1;
    }

    char error[AV_ERROR_MAX_STRING_SIZE];
    char msg[200];
    av_strerror(decodeErrorCode, error, sizeof(error));
    snprintf(msg, sizeof(msg), "Decode (frame %d): %.100s", P.frames, error);
    player_fail(msg);
    return 1;
}

/* Call once per loop pass while playing. Returns 1 when playback has finished. */
static int player_update(void) {
    if (!P.active || P.paused)
        return 0;

    u64 now = osGetTime();

    demux_ahead(P.format, P.packet, P.videoStream);
    audio_pump();

    if (P.eof)
        return (audio_drained() || now - P.eofMs > 1500) ? 1 : 0;

    /* Catch-up loop: while the picture is more than one frame behind the clock, decode
       WITHOUT drawing (Theora needs every frame) so it rejoins the audio. At most one
       frame is drawn per pass, and at most 6 are decoded, so the buttons stay responsive. */
    for (int n = 0; n < 6; n++) {
        now = osGetTime();
        u64 elapsed = now - P.startMs;

        if (elapsed < (u64)P.frames * 1000ULL / VIDEO_FPS)
            break;   /* next frame is not due yet */

        int skip = elapsed > (u64)(P.frames + 1) * 1000ULL / VIDEO_FPS;

        if (player_decode_one(skip, now))
            break;   /* ended or failed */

        if (!skip)
            break;   /* a frame was drawn: wait for the next vblank */

        audio_pump();   /* keep the sound fed during a long catch-up */
    }

    return 0;
}

/* ================================================================== main */
int main(void) {
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_textBuf = C2D_TextBufNew(4096);

    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ffmpeg_log);
    audio_global_init();

    const u32 cBlack = C2D_Color32(0, 0, 0, 255);
    const u32 cBg    = C2D_Color32(20, 20, 26, 255);
    const u32 cHead  = C2D_Color32(64, 16, 32, 255);
    const u32 cSel   = C2D_Color32(230, 77, 115, 255);
    const u32 cWhite = C2D_Color32(255, 255, 255, 255);
    const u32 cGrey  = C2D_Color32(180, 180, 190, 255);
    const u32 cRed   = C2D_Color32(255, 110, 110, 255);
    const u32 cShade = C2D_Color32(0, 0, 0, 170);

    State state = ST_SHOWS;
    int showIdx = 0, epIdx = 0;
    const int ROWS = 7, ROW_H = 28, LIST_Y = 32;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        /* ---------------------------------------------------- input / logic */
        if (state == ST_PLAY) {
            bool stop = (kDown & KEY_B) != 0;

            if (kDown & KEY_A)
                player_toggle_pause();

            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);

                /* touching the progress bar jumps there; anywhere else pauses */
                if (P.active && tp.py >= 96 && tp.py <= 134 && player_total_s() > 0) {
                    float f = ((float)tp.px - 12.0f) / 296.0f;
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    player_seek_to(f * (float)player_total_s());
                } else {
                    player_toggle_pause();
                }
            }

            if (kDown & KEY_DLEFT)  player_seek_to((double)player_elapsed_s() - 10.0);
            if (kDown & KEY_DRIGHT) player_seek_to((double)player_elapsed_s() + 10.0);

            if (!stop && player_update())
                stop = true;   /* finished: back to the episode list (no auto-next) */

            if (stop) {
                player_stop();
                g_key[0] = 0;
                state = ST_EPISODES;
            }
        } else {
            int n = (state == ST_SHOWS) ? NSHOWS : SHOWS[showIdx].count;
            int* sel = (state == ST_SHOWS) ? &showIdx : &epIdx;

            if (kDown & (KEY_DUP | KEY_CPAD_UP))     *sel = (*sel + n - 1) % n;
            if (kDown & (KEY_DDOWN | KEY_CPAD_DOWN)) *sel = (*sel + 1) % n;

            int first = *sel - 3;
            if (first > n - ROWS) first = n - ROWS;
            if (first < 0) first = 0;

            bool activate = (kDown & KEY_A) != 0;
            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                if (tp.py >= LIST_Y) {
                    int row = (tp.py - LIST_Y) / ROW_H;
                    int idx = first + row;
                    if (row < ROWS && idx < n) { *sel = idx; activate = true; }
                }
            }

            if (activate) {
                if (state == ST_SHOWS) {
                    state = ST_EPISODES;
                    epIdx = 0;
                } else {
                    /* Start playback of the selected episode. */
                    unload_cover();   /* free the cover texture while the video runs */
                    char vp[256];
                    snprintf(vp, sizeof vp, ROOT "/%s/%d.ogv", SHOWS[showIdx].folder, epIdx + 1);
                    player_open(vp);
                    state = ST_PLAY;
                }
            } else if (kDown & KEY_B) {
                if (state == ST_EPISODES) state = ST_SHOWS;
            }

            /* The state may have just changed. n / sel / first above were computed for the
               OLD state, and the draw code reads SHOWS[idx] for idx < n, so recompute. */
            n = (state == ST_SHOWS) ? NSHOWS : SHOWS[showIdx].count;
            sel = (state == ST_SHOWS) ? &showIdx : &epIdx;
            first = *sel - 3;
            if (first > n - ROWS) first = n - ROWS;
            if (first < 0) first = 0;

            if (state != ST_PLAY) {
                char path[200];
                if (state == ST_SHOWS) snprintf(path, sizeof path, ROOT "/%s/main.jpg", SHOWS[showIdx].folder);
                else snprintf(path, sizeof path, ROOT "/%s/%d.jpg", SHOWS[showIdx].folder, epIdx + 1);
                load_cover(path);
            }
        }

        /* ---------------------------------------------------- draw */
        char buf[320];

        /* list geometry for drawing (menu states) */
        int dn = (state == ST_SHOWS) ? NSHOWS : SHOWS[showIdx].count;
        int dsel = (state == ST_SHOWS) ? showIdx : epIdx;
        int dfirst = dsel - 3;
        if (dfirst > dn - ROWS) dfirst = dn - ROWS;
        if (dfirst < 0) dfirst = 0;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(g_textBuf);

        /* ---- top screen */
        C2D_TargetClear(top, cBlack);
        C2D_SceneBegin(top);

        if (state == ST_PLAY) {
            if (P.active && P.hasFrame)
                C2D_DrawImageAt(P.image, 0, 8, 0, NULL, 1.0f, 1.0f);
            else if (P.failed)
                draw_wrapped(P.message, 10, 20, 0.5f, cRed, 380.0f);
        } else {
            if (g_hasTex) {
                C2D_Image img = { &g_tex, &g_sub };
                C2D_DrawImageAt(img, 0.0f, 7.5f, 0.0f, NULL, TOP_W / g_sub.width, 225.0f / g_sub.height);
            } else {
                draw_text(g_imgErr[0] ? g_imgErr : "no image", 8, 100, 0.45f, cGrey);
            }
            C2D_DrawRectSolid(0, 190, 0.4f, TOP_W, 50, cShade);
            draw_text(SHOWS[showIdx].name, 10, 192, 0.65f, cWhite);
            if (state == ST_SHOWS) snprintf(buf, sizeof buf, "%d episodes", SHOWS[showIdx].count);
            else snprintf(buf, sizeof buf, "Episode %d of %d", epIdx + 1, SHOWS[showIdx].count);
            draw_text(buf, 10, 216, 0.5f, cGrey);
        }

        /* ---- bottom screen */
        C2D_TargetClear(bot, cBg);
        C2D_SceneBegin(bot);
        C2D_DrawRectSolid(0, 0, 0.0f, BOT_W, 28, cHead);

        if (state == ST_PLAY) {
            draw_text(SHOWS[showIdx].name, 8, 2, 0.55f, cWhite);

            snprintf(buf, sizeof buf, "Episode %d", epIdx + 1);
            draw_text(buf, 12, 40, 0.8f, cWhite);

            if (P.failed) {
                draw_text("Error", 12, 80, 0.55f, cRed);
            } else if (P.eof) {
                draw_text("Finished", 12, 80, 0.55f, cGrey);
            } else if (P.active) {
                draw_text(P.paused ? "Paused" : "Playing", 12, 80, 0.55f, cGrey);
            }

            if (P.active) {
                unsigned t = player_elapsed_s();
                unsigned total = player_total_s();
                if (total > 0 && t > total) t = total;

                if (total > 0)
                    snprintf(buf, sizeof buf, "%u:%02u / %u:%02u", t / 60, t % 60, total / 60, total % 60);
                else
                    snprintf(buf, sizeof buf, "%u:%02u", t / 60, t % 60);
                draw_text(buf, 170, 80, 0.55f, cWhite);

                /* progress bar */
                float frac = total > 0 ? (float)t / (float)total : 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                C2D_DrawRectSolid(12, 108, 0.1f, 296, 10, cHead);
                C2D_DrawRectSolid(12, 108, 0.2f, 296.0f * frac, 10, cSel);
            }

            if (P.failed)
                draw_wrapped(P.message, 12, 104, 0.4f, cRed, 296.0f);
            if (decodeInfo[0] && P.failed)
                draw_wrapped(decodeInfo, 12, 134, 0.35f, cGrey, 296.0f);
            if (lastLog[0] && P.failed)
                draw_wrapped(lastLog, 12, 160, 0.35f, cRed, 296.0f);

            /* Audio problems (e.g. missing DSP firmware) are still worth showing. */
            if (!P.failed && !g_dspReady)
                draw_wrapped(g_audioStatus, 12, 150, 0.35f, cGrey, 296.0f);

            draw_text("A: pause   Pad L/R: seek 10s   B: stop", 8, 222, 0.4f, cGrey);
        } else {
            if (state == ST_SHOWS) draw_text("Choose a show", 8, 2, 0.6f, cWhite);
            else draw_text(SHOWS[showIdx].name, 8, 2, 0.55f, cWhite);

            for (int r = 0; r < ROWS; r++) {
                int idx = dfirst + r;
                if (idx >= dn) break;
                float y = (float)(LIST_Y + r * ROW_H);
                if (idx == dsel) C2D_DrawRectSolid(6, y, 0.1f, BOT_W - 12, ROW_H - 2, cSel);
                if (state == ST_SHOWS) snprintf(buf, sizeof buf, "%s", SHOWS[idx].name);
                else snprintf(buf, sizeof buf, "Episode %d", idx + 1);
                draw_text(buf, 14, y + 1, 0.55f, idx == dsel ? cWhite : cGrey);
            }
            draw_text("A: select   B: back   START: exit", 8, 222, 0.4f, cGrey);
        }

        C3D_FrameEnd(0);
    }

    /* ---------------------------------------------------- shutdown */
    player_stop();
    unload_cover();
    audio_global_exit();
    romfsExit();

    gspWaitForP3D();
    gspWaitForPPF();

    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}