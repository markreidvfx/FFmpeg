/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <float.h>

#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lfg.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/half2float.h"
#include "libavutil/float2half.h"

#include "libswscale/swscale.h"

#define DEFAULT_W 96
#define DEFAULT_H 96

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV444P16BE,
    AV_PIX_FMT_YUV444P16LE,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV444P9BE, AV_PIX_FMT_YUV444P10BE,
    AV_PIX_FMT_YUV444P9LE, AV_PIX_FMT_YUV444P10LE,
    AV_PIX_FMT_YUV444P12BE, AV_PIX_FMT_YUV444P14BE,
    AV_PIX_FMT_YUV444P12LE, AV_PIX_FMT_YUV444P14LE,
    AV_PIX_FMT_RGB24,  AV_PIX_FMT_BGR24,
    AV_PIX_FMT_RGBA,   AV_PIX_FMT_BGRA,
    AV_PIX_FMT_ARGB,   AV_PIX_FMT_ABGR,
    AV_PIX_FMT_0RGB,   AV_PIX_FMT_0BGR,
    AV_PIX_FMT_RGB0,   AV_PIX_FMT_BGR0,
    AV_PIX_FMT_GRAY9LE, AV_PIX_FMT_GRAY9BE,
    AV_PIX_FMT_GRAY10LE, AV_PIX_FMT_GRAY10BE,
    AV_PIX_FMT_GRAY12LE, AV_PIX_FMT_GRAY12BE,
    AV_PIX_FMT_GRAY14LE, AV_PIX_FMT_GRAY14BE,
    AV_PIX_FMT_GRAY16LE, AV_PIX_FMT_GRAY16BE,
    AV_PIX_FMT_GRAYF16LE, AV_PIX_FMT_GRAYF16BE,
    AV_PIX_FMT_GRAYF32LE, AV_PIX_FMT_GRAYF32BE,
    AV_PIX_FMT_RGB48BE,  AV_PIX_FMT_BGR48BE,
    AV_PIX_FMT_RGB48LE,  AV_PIX_FMT_BGR48LE,
    AV_PIX_FMT_RGBA64BE, AV_PIX_FMT_BGRA64BE,
    AV_PIX_FMT_RGBA64LE, AV_PIX_FMT_BGRA64LE,
    AV_PIX_FMT_GBRP,   AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_GBRP9LE,
    AV_PIX_FMT_GBRP10BE, AV_PIX_FMT_GBRAP10BE,
    AV_PIX_FMT_GBRP10LE, AV_PIX_FMT_GBRAP10LE,
    AV_PIX_FMT_GBRP12BE, AV_PIX_FMT_GBRAP12BE,
    AV_PIX_FMT_GBRP12LE, AV_PIX_FMT_GBRAP12LE,
    AV_PIX_FMT_GBRP14BE,
    AV_PIX_FMT_GBRP14LE,
    AV_PIX_FMT_GBRP16BE, AV_PIX_FMT_GBRAP16BE,
    AV_PIX_FMT_GBRP16LE, AV_PIX_FMT_GBRAP16LE,
    AV_PIX_FMT_GBRPF16BE, AV_PIX_FMT_GBRAPF16BE,
    AV_PIX_FMT_GBRPF16LE, AV_PIX_FMT_GBRAPF16LE,
    AV_PIX_FMT_GBRPF32BE, AV_PIX_FMT_GBRAPF32BE,
    AV_PIX_FMT_GBRPF32LE, AV_PIX_FMT_GBRAPF32LE,
    AV_PIX_FMT_RGBF16LE, AV_PIX_FMT_RGBAF16LE,
    AV_PIX_FMT_RGBF16BE, AV_PIX_FMT_RGBAF16BE,
    AV_PIX_FMT_RGBF32LE, AV_PIX_FMT_RGBAF32LE,
    AV_PIX_FMT_RGBF32BE, AV_PIX_FMT_RGBAF32BE
};

static Half2FloatTables h2f_table;
static Float2HalfTables f2h_table;

const char *usage =  "floatimg_cmp -pixel_format <pix_fmt> -size <image_size> -ref <testfile>\n";

static void compare_images(uint8_t *rgbIn[4], uint8_t *rgbOut[4], int rgbStride[4], const AVPixFmtDescriptor *desc, int alpha,
                    int width, int height,
                    float *avg_error, float *min_error, float *max_error)
{
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    float diff;
    double count = 0;
    double sum = 0.0;
    union av_intfloat32 v0, v1;
    int channels = alpha ? 4 : 3;
    int depth = desc->comp[0].depth;
    int is_be = desc->flags & AV_PIX_FMT_FLAG_BE;
    int is_float = desc->flags & AV_PIX_FMT_FLAG_FLOAT;
    float scale = 1.0f / (float)((1 << depth) - 1);

    for (int p = 0; p < channels; p++) {
        if (!rgbStride[p])
            continue;

        for (int y = 0; y < height; y++) {
            uint8_t *in = rgbIn[p] + y * rgbStride[p];
            uint8_t *out = rgbOut[p] + y * rgbStride[p];
            for (int x = 0; x < width; x++) {
                if (depth <= 16)
                    if (is_float) {
                        if (is_be) {
                            v0.i = half2float(AV_RB16(in), &h2f_table);
                            v1.i = half2float(AV_RB16(out), &h2f_table);
                        } else {
                            v0.i = half2float(AV_RL16(in), &h2f_table);
                            v1.i = half2float(AV_RL16(out),&h2f_table);
                        }
                    } else {
                        if (is_be) {
                            v0.f = (float)AV_RB16(in) * scale;
                            v1.f = (float)AV_RB16(out) * scale;
                        } else {
                            v0.f = (float)AV_RL16(in) * scale;
                            v1.f = (float)AV_RL16(out) * scale;
                        }
                    }
                else {
                    if (is_be) {
                        v0.i = AV_RB32(in);
                        v1.i = AV_RB32(out);
                    } else {
                        v0.i = AV_RL32(in);
                        v1.i = AV_RL32(out);
                    }
                }

                diff = fabsf(v0.f - v1.f);
                sum += diff;
                minimum = FFMIN(minimum, diff);
                maximum = FFMAX(maximum, diff);

                count++;
                if (depth <= 16) {
                    in += 2;
                    out += 2;
                } else {
                    in += 4;
                    out += 4;
                }
            }
        }
    }

    *min_error = minimum;
    *max_error = maximum;
    *avg_error = sum / count;
}
int main(int argc, char **argv)
{
    enum AVPixelFormat inFormat = AV_PIX_FMT_NONE;
    enum AVPixelFormat dstFormat = AV_PIX_FMT_NONE;
    enum AVPixelFormat yuvFormat = AV_PIX_FMT_YUVA444P16LE;
    const AVPixFmtDescriptor *inDesc;
    const AVPixFmtDescriptor *outDesc;
    uint8_t *ptr;

    uint8_t *rgbIn[4]  = {NULL, NULL, NULL, NULL};
    uint8_t *rgbOut[4] = {NULL, NULL, NULL, NULL};
    int rgbStride[4];

    uint8_t *yuva[4] = {NULL, NULL, NULL, NULL};
    int yuvaStride[4];

    uint8_t *dst[4] = {NULL, NULL, NULL, NULL};
    int dstStride[4];

    int i, x, y, p, in_depth;
    int res = -1;
    int w = -1;
    int h = -1;
    int is_float = 0;
    int alpha = 0;
    union av_intfloat32 v0;
    float min_error, max_error, avg_error;

    struct SwsContext *sws = NULL;
    AVLFG rand;
    FILE *fp = NULL;

    for (i = 1; i < argc; i += 2) {
        if (argv[i][0] != '-' || i + 1 == argc)
            goto bad_option;
        if (!strcmp(argv[i], "-ref")) {
            fp = fopen(argv[i + 1], "rb");
            if (!fp) {
                fprintf(stderr, "could not open '%s'\n", argv[i + 1]);
                goto end;
            }
        } else if (!strcmp(argv[i], "-size")) {
            res = av_parse_video_size(&w, &h, argv[i + 1]);
            if (res < 0) {
                fprintf(stderr, "invalid video size %s\n",  argv[i + 1]);
                goto end;
            }
        } else if (!strcmp(argv[i], "-pixel_format")) {
            inFormat = av_get_pix_fmt(argv[i + 1]);
            if (inFormat == AV_PIX_FMT_NONE) {
                fprintf(stderr, "invalid pixel format %s\n", argv[i + 1]);
                goto end;
            }
        } else {
bad_option:
            fprintf(stderr, "%s", usage);
            fprintf(stderr, "bad option or argument missing (%s)\n", argv[i]);
            goto end;
        };
    }

    if (!fp) {
        if(inFormat == AV_PIX_FMT_NONE)
            inFormat = AV_PIX_FMT_GBRPF32LE;
        w = DEFAULT_W;
        h = DEFAULT_H;
    }

    if (w <= 0 || h <= 0) {
        fprintf(stderr, "%s", usage);
        fprintf(stderr, "invalid -video_size\n");
        goto end;
    }

    if (inFormat == AV_PIX_FMT_NONE) {
        fprintf(stderr, "%s", usage);
        fprintf(stderr, "invalid input pixel format\n");
        goto end;
    }

    inDesc = av_pix_fmt_desc_get(inFormat);
    in_depth = inDesc->comp[0].depth;
    if (in_depth <= 8) {
        fprintf(stderr, "input pixel depth %d not supported.\n", in_depth);
        goto end;
    }

    is_float = inDesc->flags & AV_PIX_FMT_FLAG_FLOAT;
    if (in_depth <= 16) {
        if (is_float) {
            ff_init_half2float_tables(&h2f_table);
            ff_init_float2half_tables(&f2h_table);
        }
    }

    res = av_image_fill_linesizes(rgbStride, inFormat, w);
    if (res < 0) {
        fprintf(stderr, "av_image_fill_linesizes failed\n");
        goto end;
    }
    for (p = 0; p < 4; p++) {
        rgbStride[p] = FFALIGN(rgbStride[p], 16);
        if (rgbStride[p]) {
            rgbIn[p] = av_mallocz(rgbStride[p] * h + 16);
            rgbOut[p] = av_mallocz(rgbStride[p] * h + 16);
        }
        if (rgbStride[p] && (!rgbIn[p] || !rgbOut[p])) {
            goto end;
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts); i++) {
        dstFormat = pix_fmts[i];
        outDesc = av_pix_fmt_desc_get(dstFormat);
        alpha = (inDesc->flags & AV_PIX_FMT_FLAG_ALPHA) && (outDesc->flags & AV_PIX_FMT_FLAG_ALPHA);
        if (fp) {
            fseek(fp, 0, SEEK_SET);
            for (p = 0; p < 4; p++) {
                if (!rgbStride[p])
                    continue;

                ptr = rgbIn[p];
                for (y = 0; y < h; y++) {
                    size_t size = fread(ptr, 1, w*4, fp);
                    if (size != w*4) {
                        fprintf(stderr, "read error: %zd\n", size);
                        goto end;
                    }
                    ptr += rgbStride[p];
                }
            }
        } else {
            // fill src with random values between 0.0 - 1.0
            av_lfg_init(&rand, 1);
            for (p = 0; p < 4; p++) {
                if (!rgbStride[p])
                    continue;
                for (y = 0; y < h; y++) {
                    if (in_depth <= 16) {
                        uint16_t *in = (uint16_t*)(rgbIn[p] + y * rgbStride[p]);
                        for (x = 0; x < w; x++) {
                            uint16_t v;
                            if (is_float) {
                                v0.f = (float)av_lfg_get(&rand)/(float)(UINT32_MAX);
                                v = float2half(v0.i, &f2h_table);
                            } else {
                                v = (uint16_t)(av_lfg_get(&rand) & (0xFFFF >> (16 - in_depth)));
                            }
                            if (inDesc->flags & AV_PIX_FMT_FLAG_BE)
                                *in++ = AV_RB16(&v);
                            else
                                *in++ = AV_RL16(&v);
                        }
                    } else {
                        uint32_t *in = (uint32_t*)(rgbIn[p] + y * rgbStride[p]);
                        for (x = 0; x < w; x++) {
                            v0.f = (float)av_lfg_get(&rand)/(float)(UINT32_MAX);
                            if (inDesc->flags & AV_PIX_FMT_FLAG_BE)
                                *in++ = AV_RB32(&v0.i);
                            else
                                *in++ = AV_RL32(&v0.i);
                        }
                    }
                }
            }
        }

        // setup intermediate image
        for (p = 0; p < 4; p++) {
            av_freep(&dst[p]);
        }

        res = av_image_fill_linesizes(dstStride, dstFormat, w);
        if (res < 0) {
            fprintf(stderr, "av_image_fill_linesizes failed\n");
            goto end;
        }
        for (p = 0; p < 4; p++) {
            dstStride[p] = FFALIGN(dstStride[p], 16);
            if (dstStride[p]) {
                dst[p] = av_mallocz(dstStride[p] * h + 16);
            }
            if (dstStride[p] && !dst[p]) {
                goto end;
            }
        }

        // srcFormat -> dstFormat
        sws = sws_getContext(w, h, inFormat, w, h,
                             dstFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if (!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(inFormat), av_get_pix_fmt_name(dstFormat) );
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)rgbIn, rgbStride, 0, h, dst, dstStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);

        // dstFormat -> srcFormat
        sws = sws_getContext(w, h, dstFormat, w, h,
                            inFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if(!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(dstFormat), av_get_pix_fmt_name(inFormat) );
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)dst, dstStride, 0, h, rgbOut, rgbStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);
        sws = NULL;

        min_error = FLT_MAX;
        max_error = -FLT_MAX;
        avg_error = FLT_MAX;
        compare_images(rgbIn, rgbOut, rgbStride, inDesc, alpha, w, h,
                       &avg_error, &min_error, &max_error);

        fprintf(stdout, "%s -> %s -> %s\n", av_get_pix_fmt_name(inFormat), av_get_pix_fmt_name(dstFormat), av_get_pix_fmt_name(inFormat) );
        fprintf(stdout, "avg diff: %f\nmin diff: %f\nmax diff: %f\n", avg_error, min_error, max_error);

        // setup intermediate image
        for (p = 0; p < 4; p++) {
            av_freep(&dst[p]);
        }

        res = av_image_fill_linesizes(dstStride, dstFormat, w);
        if (res < 0) {
            fprintf(stderr, "av_image_fill_linesizes failed\n");
            goto end;
        }
        for (p = 0; p < 4; p++) {
            dstStride[p] = FFALIGN(dstStride[p], 16);
            if (dstStride[p]) {
                dst[p] = av_mallocz(dstStride[p] * h + 16);
            }
            if (dstStride[p] && !dst[p]) {
                goto end;
            }
        }

        // setup intermediate yuva image
        for (p = 0; p < 4; p++) {
            av_freep(&yuva[p]);
        }

        res = av_image_fill_linesizes(yuvaStride, yuvFormat, w);
        if (res < 0) {
            fprintf(stderr, "av_image_fill_linesizes failed\n");
            goto end;
        }
        for (p = 0; p < 4; p++) {
            yuvaStride[p] = FFALIGN(yuvaStride[p], 16);
            if (yuvaStride[p]) {
                yuva[p] = av_mallocz(yuvaStride[p] * h + 16);
            }
            if (yuvaStride[p] && !yuva[p]) {
                goto end;
            }
        }

        // srcFormat -> yuvFormat
        sws = sws_getContext(w, h, inFormat, w, h,
                             yuvFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if (!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(inFormat), av_get_pix_fmt_name(yuvFormat));
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)rgbIn, rgbStride, 0, h, yuva, yuvaStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);
        sws = NULL;

        // yuvFormat -> dstFormat
        sws = sws_getContext(w, h, yuvFormat, w, h,
                             dstFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if (!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(yuvFormat), av_get_pix_fmt_name(dstFormat));
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)yuva, yuvaStride, 0, h, dst, dstStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);
        sws = NULL;

        // dstFormat -> yuvFormat
        sws = sws_getContext(w, h, dstFormat, w, h,
                             yuvFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if (!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(dstFormat), av_get_pix_fmt_name(yuvFormat));
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)dst, dstStride, 0, h, yuva, yuvaStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);
        sws = NULL;

        // yuvFormat -> srcFormat
        sws = sws_getContext(w, h, yuvFormat, w, h,
                             inFormat, SWS_BILINEAR | SWS_BITEXACT, NULL, NULL, NULL);
        if (!sws) {
            fprintf(stderr, "Failed to get %s -> %s\n", av_get_pix_fmt_name(yuvFormat), av_get_pix_fmt_name(inFormat));
            goto end;
        }

        res = sws_scale(sws, (const uint8_t *const *)yuva, yuvaStride, 0, h, rgbOut, rgbStride);
        if (res < 0 || res != h) {
            fprintf(stderr, "sws_scale failed\n");
            res = -1;
            goto end;
        }
        sws_freeContext(sws);
        sws = NULL;

        min_error = FLT_MAX;
        max_error = -FLT_MAX;
        avg_error = FLT_MAX;
        compare_images(rgbIn, rgbOut, rgbStride, inDesc, alpha, w, h,
                       &avg_error, &min_error, &max_error);

        fprintf(stdout, "%s -> %s -> %s -> %s -> %s\n", av_get_pix_fmt_name(inFormat), av_get_pix_fmt_name(yuvFormat),
                                                        av_get_pix_fmt_name(dstFormat), av_get_pix_fmt_name(yuvFormat),
                                                        av_get_pix_fmt_name(inFormat) );
        fprintf(stdout, "avg diff: %f\nmin diff: %f\nmax diff: %f\n", avg_error, min_error, max_error);
        res = 0;
    }

end:
    sws_freeContext(sws);
    for (p = 0; p < 4; p++) {
        av_freep(&rgbIn[p]);
        av_freep(&rgbOut[p]);
        av_freep(&dst[p]);
    }
    if (fp)
        fclose(fp);

    return res;
}
