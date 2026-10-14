/*
 * bss2ppm - decode the pre-rendered room backgrounds of RE2 (PS1).
 *
 * COMMON/BSS/ROOMsrr.BSS holds one 64 KB slot per camera. Each slot starts
 * with a PlayStation MDEC "BS" frame (version 3, 320x240). FFmpeg's mdec
 * decoder handles that format, so this tool feeds each slot to libavcodec
 * and writes the result as a binary PPM (RGB, 320x240).
 *
 * Build:
 *   clang -O2 -o bss2ppm bss2ppm.c -I/opt/homebrew/include -L/opt/homebrew/lib \
 *         -lavcodec -lavutil -lswscale
 * Usage:
 *   bss2ppm ROOM100.BSS outdir/room100
 *   -> outdir/room100_cam00.ppm, outdir/room100_cam01.ppm, ...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define SLOT_SIZE 0x10000
#define BG_WIDTH 320
#define BG_HEIGHT 240

static int is_bs_frame(const uint8_t *p)
{
    /* u16 rlc_words, u16 0x3800, u16 qscale, u16 version */
    return p[2] == 0x00 && p[3] == 0x38 && (p[6] == 2 || p[6] == 3) && p[7] == 0;
}

static int write_ppm(const char *path, const uint8_t *rgb, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", BG_WIDTH, BG_HEIGHT);
    for (int y = 0; y < BG_HEIGHT; y++)
        fwrite(rgb + y * stride, 3, BG_WIDTH, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s ROOMxxx.BSS out_prefix\n", argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (fread(data, 1, size, in) != (size_t)size) {
        fprintf(stderr, "read error\n");
        return 1;
    }
    fclose(in);
    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MDEC);
    if (!codec) {
        fprintf(stderr, "libavcodec has no MDEC decoder\n");
        return 1;
    }

    uint8_t *rgb = NULL;
    int rgb_linesize[4];
    av_image_alloc(&rgb, rgb_linesize, BG_WIDTH, BG_HEIGHT, AV_PIX_FMT_RGB24, 16);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int written = 0;

    for (long off = 0; off + 8 <= size; off += SLOT_SIZE) {
        int cam = (int)(off / SLOT_SIZE);
        if (!is_bs_frame(data + off)) {
            fprintf(stderr, "slot %d: no BS header, skipped\n", cam);
            continue;
        }

        /* A fresh context per frame keeps the DC predictors independent. */
        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        ctx->width = BG_WIDTH;
        ctx->height = BG_HEIGHT;
        if (avcodec_open2(ctx, codec, NULL) < 0) {
            fprintf(stderr, "slot %d: cannot open decoder\n", cam);
            return 1;
        }

        long len = size - off < SLOT_SIZE ? size - off : SLOT_SIZE;
        pkt->data = data + off;
        pkt->size = (int)len;
        if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0) {
            fprintf(stderr, "slot %d: decode failed\n", cam);
            avcodec_free_context(&ctx);
            continue;
        }

        struct SwsContext *sws = sws_getContext(frame->width, frame->height, frame->format,
                                                BG_WIDTH, BG_HEIGHT, AV_PIX_FMT_RGB24,
                                                SWS_POINT, NULL, NULL, NULL);
        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                  &rgb, rgb_linesize);
        sws_freeContext(sws);

        char path[1024];
        snprintf(path, sizeof(path), "%s_cam%02d.ppm", argv[2], cam);
        if (write_ppm(path, rgb, rgb_linesize[0]) == 0) {
            printf("%s (qscale %d)\n", path, data[off + 4] | (data[off + 5] << 8));
            written++;
        }
        av_frame_unref(frame);
        avcodec_free_context(&ctx);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_freep(&rgb);
    free(data);
    return written > 0 ? 0 : 1;
}
