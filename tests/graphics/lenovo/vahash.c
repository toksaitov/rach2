/*
 * vahash.c: decode a clip's video through VA-API or in software with the
 * system's libavcodec, and print an MD5 for each decoded frame, so that the
 * GPU's decoding can be compared with software's and with Linux's.
 *
 *   vahash [-d NODE] [-l LOOPS] [-s STOP] [-t TYPE] va|sw CLIP
 *
 * `va` decodes through the hardware device of TYPE, vaapi by default, on the
 * DRM render node NODE (/dev/dri/renderD128), or another of libavutil's
 * types, such as videotoolbox; a frame that is not a hardware
 * surface means libavcodec fell back to software, which is an error. `sw`
 * decodes on the processor with one thread. The clip is decoded LOOPS times
 * (1), each time from its start, or with -s until the file STOP exists at
 * the end of a loop. Each frame prints
 *
 *   LOOP FRAME WIDTHxHEIGHT MD5
 *
 * FRAME counting from 0 in each loop, and each loop's end prints
 * `vahash: loop L: N frames, ended at T` on stderr, T in seconds since the
 * epoch.
 *
 * where the MD5 is over the frame's 8-bit 4:2:0 picture as three planes, Y,
 * then U, then V, each row as wide as the picture, whatever layout the
 * decoder gave it in, NV12 or planar. The end prints
 *
 *   vahash: result: MODE CLIP frames N decoder NAME format F
 *
 * NAME being libavcodec's decoder and F the layout of the last frame; the
 * status is 0 then. A clip that does not open, a decoder or device that does
 * not, a decode error, a software frame in `va`, or a picture not 8-bit 4:2:0
 * prints `vahash: CLIP: DETAIL` on stderr and exits 1; bad words exit 2. A
 * read error ends the clip, as in ffmpeg, and is printed on stderr.
 *
 * Built against Debian's FFmpeg 7.1 headers and linked with the system's
 * libavformat, libavcodec and libavutil, on Rach2's GNU root and on Debian
 * alike.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/md5.h>
#include <libavutil/pixdesc.h>

static const char *clip;             /* the clip's name, for messages */
static enum AVPixelFormat hw_format; /* the surfaces' format in `va` */
static long frames;                  /* frames decoded, over every loop */
static long frame;                   /* frames decoded in this loop */

/** Print `vahash: CLIP: MSG` and AVERROR `err`'s text, if any, and exit 1. */
static void die(const char *msg, int err)
{
    char text[AV_ERROR_MAX_STRING_SIZE];

    if (err < 0) {
        av_strerror(err, text, sizeof(text));
        fprintf(stderr, "vahash: %s: %s: %s\n", clip, msg, text);
    } else {
        fprintf(stderr, "vahash: %s: %s\n", clip, msg);
    }
    exit(1);
}

/**
 * libavcodec's get_format: the hardware surfaces' format when the decoder
 * offers it, or none, which fails the decoder's setup, so that `va` never
 * decodes in software.
 */
static enum AVPixelFormat pick_hw(AVCodecContext *ctx, const enum AVPixelFormat *offered)
{
    const enum AVPixelFormat *p;

    (void)ctx;
    for (p = offered; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hw_format)
            return *p;
    }
    fprintf(stderr, "vahash: %s: the decoder offers no %s surface\n", clip,
            av_get_pix_fmt_name(hw_format));
    return AV_PIX_FMT_NONE;
}

/** Add `rows` rows of `width` bytes from `p`, `stride` apart, to `md5`. */
static void hash_plane(struct AVMD5 *md5, const uint8_t *p, int stride, int width, int rows)
{
    int y;

    for (y = 0; y < rows; y++)
        av_md5_update(md5, p + (size_t)y * stride, (size_t)width);
}

/**
 * Print frame `f`'s line: its MD5 over the Y, U and V planes of its 8-bit
 * 4:2:0 picture. NV12's interleaved chroma is split into a row buffer first.
 */
static void hash_frame(int loop, const AVFrame *f)
{
    struct AVMD5 *md5 = av_md5_alloc();
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2, x, y, c;
    uint8_t digest[16], *row;

    if (!md5)
        die("no memory", 0);
    av_md5_init(md5);
    hash_plane(md5, f->data[0], f->linesize[0], f->width, f->height);
    switch (f->format) {
    case AV_PIX_FMT_NV12:
        row = malloc((size_t)cw);
        if (!row)
            die("no memory", 0);
        for (c = 0; c < 2; c++) {
            for (y = 0; y < ch; y++) {
                const uint8_t *uv = f->data[1] + (size_t)y * f->linesize[1];
                for (x = 0; x < cw; x++)
                    row[x] = uv[2 * x + c];
                av_md5_update(md5, row, (size_t)cw);
            }
        }
        free(row);
        break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        hash_plane(md5, f->data[1], f->linesize[1], cw, ch);
        hash_plane(md5, f->data[2], f->linesize[2], cw, ch);
        break;
    default:
        fprintf(stderr, "vahash: %s: frame %ld is %s, not 8-bit 4:2:0\n", clip, frame,
                av_get_pix_fmt_name((enum AVPixelFormat)f->format));
        exit(1);
    }
    av_md5_final(md5, digest);
    av_free(md5);
    printf("%d %ld %dx%d ", loop, frame, f->width, f->height);
    for (x = 0; x < 16; x++)
        printf("%02x", digest[x]);
    printf("\n");
}

/**
 * Take every frame the decoder `ctx` has ready, copying each hardware
 * surface to memory first, and print each; `last` gets the last frame's
 * layout. Returns 0, or the decoder's error other than EAGAIN and EOF.
 */
static int drain(AVCodecContext *ctx, int va, int loop, AVFrame *f, AVFrame *sw,
                 enum AVPixelFormat *last)
{
    const AVFrame *out;
    int err;

    for (;;) {
        err = avcodec_receive_frame(ctx, f);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return 0;
        if (err < 0)
            return err;
        if (va) {
            if (f->format != hw_format) {
                fprintf(stderr, "vahash: %s: frame %ld came from software, as %s\n", clip, frame,
                        av_get_pix_fmt_name((enum AVPixelFormat)f->format));
                exit(1);
            }
            av_frame_unref(sw);
            err = av_hwframe_transfer_data(sw, f, 0);
            if (err < 0)
                die("copying a surface to memory", err);
            sw->width = f->width;
            sw->height = f->height;
            out = sw;
        } else {
            out = f;
        }
        hash_frame(loop, out);
        *last = (enum AVPixelFormat)out->format;
        frames++;
        frame++;
        av_frame_unref(f);
    }
}

/** The hardware device and the buffers every loop shares. */
static AVBufferRef *device;
static enum AVHWDeviceType type;
static const char *type_name = "vaapi";
static int va;
static AVFrame *frm, *swf;
static AVPacket *pkt;
static enum AVPixelFormat last = AV_PIX_FMT_NONE;
static const AVCodec *codec;

/**
 * Have `ctx`, a decoder of `codec`, decode into the device's surfaces:
 * the format of codec's hardware configuration for the device's type, and
 * pick_hw to insist on it. Exits 1 when the codec has none.
 */
static void use_device(AVCodecContext *ctx)
{
    const AVCodecHWConfig *hw;
    int i;

    for (i = 0;; i++) {
        hw = avcodec_get_hw_config(codec, i);
        if (!hw) {
            fprintf(stderr, "vahash: %s: %s has no %s decoding\n", clip, codec->name, type_name);
            exit(1);
        }
        if ((hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && hw->device_type == type)
            break;
    }
    hw_format = hw->pix_fmt;
    ctx->get_format = pick_hw;
    ctx->hw_device_ctx = av_buffer_ref(device);
    if (!ctx->hw_device_ctx)
        die("no memory", 0);
}

/**
 * Open a decoder of the best video stream of `fmt`, which `*stream` gets,
 * on one thread, and on the device in `va`. Exits 1 on any failure.
 */
static AVCodecContext *open_decoder(AVFormatContext *fmt, int *stream)
{
    AVCodecContext *ctx;
    int err;

    *stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (*stream < 0)
        die("finding its video", *stream);
    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        die("no memory", 0);
    err = avcodec_parameters_to_context(ctx, fmt->streams[*stream]->codecpar);
    if (err < 0)
        die("setting up the decoder", err);
    ctx->thread_count = 1;
    if (va)
        use_device(ctx);
    err = avcodec_open2(ctx, codec, NULL);
    if (err < 0)
        die("opening the decoder", err);
    return ctx;
}

/** Decode the clip once from its start as loop `loop`, printing each frame. */
static void decode_once(int loop)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx;
    int err, stream;

    frame = 0;
    err = avformat_open_input(&fmt, clip, NULL, NULL);
    if (err < 0)
        die("opening", err);
    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0)
        die("reading its streams", err);
    ctx = open_decoder(fmt, &stream);
    while ((err = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == stream) {
            err = avcodec_send_packet(ctx, pkt);
            if (err < 0 || (err = drain(ctx, va, loop, frm, swf, &last)) < 0)
                die("decoding", err);
        }
        av_packet_unref(pkt);
    }
    if (err != AVERROR_EOF) {
        /* As ffmpeg does, a read error ends the input: the .rcv demuxer
         * gives one after a clip's last frame. */
        av_strerror(err, text, sizeof(text));
        fprintf(stderr, "vahash: %s: reading ended: %s\n", clip, text);
    }
    err = avcodec_send_packet(ctx, NULL);
    if (err < 0 || (err = drain(ctx, va, loop, frm, swf, &last)) < 0)
        die("flushing the decoder", err);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    fprintf(stderr, "vahash: loop %d: %ld frames, ended at %ld\n", loop, frame, (long)time(NULL));
}

int main(int argc, char **argv)
{
    const char *node = "/dev/dri/renderD128", *stop = NULL;
    int loops = 1, loop, opt, err;

    while ((opt = getopt(argc, argv, "d:l:s:t:")) != -1) {
        if (opt == 'd')
            node = optarg;
        else if (opt == 'l')
            loops = atoi(optarg);
        else if (opt == 's')
            stop = optarg;
        else if (opt == 't')
            type_name = optarg;
        else
            goto usage;
    }
    if (argc - optind != 2 || loops < 1 ||
        (strcmp(argv[optind], "va") != 0 && strcmp(argv[optind], "sw") != 0))
        goto usage;
    va = strcmp(argv[optind], "va") == 0;
    clip = argv[optind + 1];
    av_log_set_level(AV_LOG_ERROR);

    if (va) {
        type = av_hwdevice_find_type_by_name(type_name);
        if (type == AV_HWDEVICE_TYPE_NONE)
            die("no such hardware device type", 0);
        /* Only VA-API takes a device's name. */
        err = av_hwdevice_ctx_create(&device, type, type == AV_HWDEVICE_TYPE_VAAPI ? node : NULL,
                                     NULL, 0);
        if (err < 0)
            die("opening the hardware device", err);
    }
    frm = av_frame_alloc();
    swf = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frm || !swf || !pkt)
        die("no memory", 0);
    for (loop = 0; stop ? loop == 0 || access(stop, F_OK) != 0 : loop < loops; loop++)
        decode_once(loop);
    printf("vahash: result: %s %s frames %ld decoder %s format %s\n", va ? "va" : "sw", clip,
           frames, codec->name, last == AV_PIX_FMT_NONE ? "none" : av_get_pix_fmt_name(last));
    av_frame_free(&frm);
    av_frame_free(&swf);
    av_packet_free(&pkt);
    av_buffer_unref(&device);
    return 0;

usage:
    fprintf(stderr, "usage: vahash [-d NODE] [-l LOOPS] [-s STOP] [-t TYPE] va|sw CLIP\n");
    return 2;
}
