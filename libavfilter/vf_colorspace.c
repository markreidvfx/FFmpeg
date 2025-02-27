/*
 * Copyright (c) 2016 Ronald S. Bultje <rsbultje@gmail.com>
 *
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

/*
 * @file
 * Convert between colorspaces.
 */

#include "libavutil/avassert.h"
#include "libavutil/color_utils.h"
#include "libavutil/csp.h"
#include "libavutil/float2half.h"
#include "libavutil/half2float.h"
#include "libavutil/intfloat.h"
#include "libavutil/mem_internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

#include "avfilter.h"
#include "colorspacedsp.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "colorspace.h"

enum DitherMode {
    DITHER_NONE,
    DITHER_FSB,
    DITHER_NB,
};

enum Colorspace {
    CS_UNSPECIFIED,
    CS_BT470M,
    CS_BT470BG,
    CS_BT601_6_525,
    CS_BT601_6_625,
    CS_BT709,
    CS_SMPTE170M,
    CS_SMPTE240M,
    CS_BT2020,
    CS_NB,
};

enum WhitepointAdaptation {
    WP_ADAPT_BRADFORD,
    WP_ADAPT_VON_KRIES,
    NB_WP_ADAPT_NON_IDENTITY,
    WP_ADAPT_IDENTITY = NB_WP_ADAPT_NON_IDENTITY,
    NB_WP_ADAPT,
};

static const enum AVColorTransferCharacteristic default_trc[CS_NB + 1] = {
    [CS_UNSPECIFIED] = AVCOL_TRC_UNSPECIFIED,
    [CS_BT470M]      = AVCOL_TRC_GAMMA22,
    [CS_BT470BG]     = AVCOL_TRC_GAMMA28,
    [CS_BT601_6_525] = AVCOL_TRC_SMPTE170M,
    [CS_BT601_6_625] = AVCOL_TRC_SMPTE170M,
    [CS_BT709]       = AVCOL_TRC_BT709,
    [CS_SMPTE170M]   = AVCOL_TRC_SMPTE170M,
    [CS_SMPTE240M]   = AVCOL_TRC_SMPTE240M,
    [CS_BT2020]      = AVCOL_TRC_BT2020_10,
    [CS_NB]          = AVCOL_TRC_UNSPECIFIED,
};

static const enum AVColorPrimaries default_prm[CS_NB + 1] = {
    [CS_UNSPECIFIED] = AVCOL_PRI_UNSPECIFIED,
    [CS_BT470M]      = AVCOL_PRI_BT470M,
    [CS_BT470BG]     = AVCOL_PRI_BT470BG,
    [CS_BT601_6_525] = AVCOL_PRI_SMPTE170M,
    [CS_BT601_6_625] = AVCOL_PRI_BT470BG,
    [CS_BT709]       = AVCOL_PRI_BT709,
    [CS_SMPTE170M]   = AVCOL_PRI_SMPTE170M,
    [CS_SMPTE240M]   = AVCOL_PRI_SMPTE240M,
    [CS_BT2020]      = AVCOL_PRI_BT2020,
    [CS_NB]          = AVCOL_PRI_UNSPECIFIED,
};

static const enum AVColorSpace default_csp[CS_NB + 1] = {
    [CS_UNSPECIFIED] = AVCOL_SPC_UNSPECIFIED,
    [CS_BT470M]      = AVCOL_SPC_SMPTE170M,
    [CS_BT470BG]     = AVCOL_SPC_BT470BG,
    [CS_BT601_6_525] = AVCOL_SPC_SMPTE170M,
    [CS_BT601_6_625] = AVCOL_SPC_BT470BG,
    [CS_BT709]       = AVCOL_SPC_BT709,
    [CS_SMPTE170M]   = AVCOL_SPC_SMPTE170M,
    [CS_SMPTE240M]   = AVCOL_SPC_SMPTE240M,
    [CS_BT2020]      = AVCOL_SPC_BT2020_NCL,
    [CS_NB]          = AVCOL_SPC_UNSPECIFIED,
};

struct TransferCharacteristics {
    double alpha, beta, gamma, delta;
};

typedef struct ColorSpaceContext {
    const AVClass *class;

    ColorSpaceDSPContext dsp;

    enum Colorspace user_all, user_iall;
    enum AVColorSpace in_csp, out_csp, user_csp, user_icsp;
    enum AVColorRange in_rng, out_rng, user_rng, user_irng;
    enum AVColorTransferCharacteristic in_trc, out_trc, user_trc, user_itrc;
    enum AVColorPrimaries in_prm, out_prm, user_prm, user_iprm;
    enum AVPixelFormat in_format, user_format;
    int fast_mode;
    enum DitherMode dither;
    enum WhitepointAdaptation wp_adapt;

    uint8_t *rgb[3];
    ptrdiff_t rgb_stride;
    unsigned rgb_sz;
    int *dither_scratch[3][2], *dither_scratch_base[3][2];

    const AVColorPrimariesDesc *in_primaries, *out_primaries;
    int lrgb2lrgb_passthrough;
    DECLARE_ALIGNED(16, int16_t, lrgb2lrgb_coeffs)[3][3][8];
    DECLARE_ALIGNED(32, float,   lrgb2lrgb_coeffsf)[3][3][8];

    const struct TransferCharacteristics *in_txchr, *out_txchr;
    int rgb2rgb_passthrough;
    int16_t *lin_lut, *delin_lut;

    Float2HalfTables *f2h_tbl;
    Half2FloatTables *h2f_tbl;

    const AVLumaCoefficients *in_lumacoef, *out_lumacoef;
    int yuv2yuv_passthrough, yuv2yuv_fastmode;
    DECLARE_ALIGNED(16, int16_t, yuv2rgb_coeffs)[3][3][8];
    DECLARE_ALIGNED(16, int16_t, rgb2yuv_coeffs)[3][3][8];
    DECLARE_ALIGNED(16, int16_t, yuv2yuv_coeffs)[3][3][8];
    DECLARE_ALIGNED(16, int16_t, yuv_offset)[2 /* in, out */][8];

    avpriv_trc_function out_trc_fn;
    yuv2rgb_fn yuv2rgb;
    rgb2yuv_fn rgb2yuv;
    rgb2yuv_fsb_fn rgb2yuv_fsb;
    yuv2yuv_fn yuv2yuv;
    double yuv2rgb_dbl_coeffs[3][3], rgb2yuv_dbl_coeffs[3][3];
    int in_y_rng, in_uv_rng, out_y_rng, out_uv_rng;

    int did_warn_range;
    int is_float;
    int is_float16;
} ColorSpaceContext;

// FIXME deal with odd width/heights
// FIXME faster linearize/delinearize implementation (integer pow)
// FIXME bt2020cl support (linearization between yuv/rgb step instead of between rgb/xyz)
// FIXME test that the values in (de)lin_lut don't exceed their container storage
// type size (only useful if we keep the LUT and don't move to fast integer pow)
// FIXME dithering if bitdepth goes down?
// FIXME bitexact for fate integration?

// FIXME I'm pretty sure gamma22/28 also have a linear toe slope, but I can't
// find any actual tables that document their real values...
// See http://www.13thmonkey.org/~boris/gammacorrection/ first graph why it matters
static const struct TransferCharacteristics transfer_characteristics[AVCOL_TRC_NB] = {
    [AVCOL_TRC_BT709]     = { 1.099,  0.018,  0.45, 4.5 },
    [AVCOL_TRC_GAMMA22]   = { 1.0,    0.0,    1.0 / 2.2, 0.0 },
    [AVCOL_TRC_GAMMA28]   = { 1.0,    0.0,    1.0 / 2.8, 0.0 },
    [AVCOL_TRC_SMPTE170M] = { 1.099,  0.018,  0.45, 4.5 },
    [AVCOL_TRC_SMPTE240M] = { 1.1115, 0.0228, 0.45, 4.0 },
    [AVCOL_TRC_LINEAR]    = { 1.0,    0.0,    1.0,  0.0 },
    [AVCOL_TRC_IEC61966_2_1] = { 1.055, 0.0031308, 1.0 / 2.4, 12.92 },
    [AVCOL_TRC_IEC61966_2_4] = { 1.099, 0.018, 0.45, 4.5 },
    [AVCOL_TRC_BT2020_10] = { 1.099,  0.018,  0.45, 4.5 },
    [AVCOL_TRC_BT2020_12] = { 1.0993, 0.0181, 0.45, 4.5 },
};

static const struct TransferCharacteristics *
    get_transfer_characteristics(enum AVColorTransferCharacteristic trc)
{
    const struct TransferCharacteristics *coeffs;

    if (trc >= AVCOL_TRC_NB)
        return NULL;
    coeffs = &transfer_characteristics[trc];
    if (!coeffs->alpha)
        return NULL;

    return coeffs;
}

static int fill_gamma_table(ColorSpaceContext *s)
{
    int n;
    double in_alpha = s->in_txchr->alpha, in_beta = s->in_txchr->beta;
    double in_gamma = s->in_txchr->gamma, in_delta = s->in_txchr->delta;
    double in_ialpha = 1.0 / in_alpha, in_igamma = 1.0 / in_gamma, in_idelta = 1.0 / in_delta;
    double out_alpha = 0.0, out_beta = 0.0;
    double out_gamma = 0.0, out_delta = 0.0;

    if (!s->out_trc_fn) {
        out_alpha = s->out_txchr->alpha, out_beta = s->out_txchr->beta;
        out_gamma = s->out_txchr->gamma, out_delta = s->out_txchr->delta;
    }

    s->lin_lut = av_malloc(sizeof(*s->lin_lut) * 32768 * 2);
    if (!s->lin_lut)
        return AVERROR(ENOMEM);
    s->delin_lut = &s->lin_lut[32768];
    for (n = 0; n < 32768; n++) {
        double v = (n - 2048.0) / 28672.0, d, l;

        // delinearize
        if (s->out_trc_fn) {
            d = s->out_trc_fn(v);
        } else {
            if (v <= -out_beta) {
                d = -out_alpha * pow(-v, out_gamma) + (out_alpha - 1.0);
            } else if (v < out_beta) {
                d = out_delta * v;
            } else {
                d = out_alpha * pow(v, out_gamma) - (out_alpha - 1.0);
            }
        }
        s->delin_lut[n] = av_clip_int16(lrint(d * 28672.0));

        // linearize
        if (v <= -in_beta * in_delta) {
            l = -pow((1.0 - in_alpha - v) * in_ialpha, in_igamma);
        } else if (v < in_beta * in_delta) {
            l = v * in_idelta;
        } else {
            l = pow((v + in_alpha - 1.0) * in_ialpha, in_igamma);
        }
        s->lin_lut[n] = av_clip_int16(lrint(l * 28672.0));
    }

    return 0;
}

static int fill_gamma_table_f16(ColorSpaceContext *s)
{
    int n;
    uint16_t *lin_lut;
    uint16_t *delin_lut;
    double in_alpha = s->in_txchr->alpha, in_beta = s->in_txchr->beta;
    double in_gamma = s->in_txchr->gamma, in_delta = s->in_txchr->delta;
    double in_ialpha = 1.0 / in_alpha, in_igamma = 1.0 / in_gamma, in_idelta = 1.0 / in_delta;
    double out_alpha = 0.0, out_beta = 0.0;
    double out_gamma = 0.0, out_delta = 0.0;

    if (!s->out_trc_fn) {
        out_alpha = s->out_txchr->alpha, out_beta = s->out_txchr->beta;
        out_gamma = s->out_txchr->gamma, out_delta = s->out_txchr->delta;
    }

    s->lin_lut = av_malloc(sizeof(*s->lin_lut) * 65536 * 2);
    if (!s->lin_lut)
        return AVERROR(ENOMEM);
    s->delin_lut = &s->lin_lut[65536];

    lin_lut   = (uint16_t *)s->lin_lut;
    delin_lut = (uint16_t *)s->delin_lut ;

    for (n = 0; n < 65536; n++) {
        double d, l;
        double v = (double)av_int2float(half2float(n, s->h2f_tbl));

        // delinearize
        if (s->out_trc_fn) {
            d = s->out_trc_fn(v);
        } else {
            if (v <= -out_beta) {
                d = -out_alpha * pow(-v, out_gamma) + (out_alpha - 1.0);
            } else if (v < out_beta) {
                d = out_delta * v;
            } else {
                d = out_alpha * pow(v, out_gamma) - (out_alpha - 1.0);
            }
        }

        delin_lut[n] = float2half(av_float2int((float)d), s->f2h_tbl);

        // linearize
        if (v <= -in_beta * in_delta) {
            l = -pow((1.0 - in_alpha - v) * in_ialpha, in_igamma);
        } else if (v < in_beta * in_delta) {
            l = v * in_idelta;
        } else {
            l = pow((v + in_alpha - 1.0) * in_ialpha, in_igamma);
        }
        lin_lut[n] = float2half(av_float2int((float)l), s->f2h_tbl);

    }

    return 0;
}

/*
 * See http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
 * This function uses the Bradford mechanism.
 */
static void fill_whitepoint_conv_table(double out[3][3], enum WhitepointAdaptation wp_adapt,
                                       const AVWhitepointCoefficients *wp_src,
                                       const AVWhitepointCoefficients *wp_dst)
{
    static const double ma_tbl[NB_WP_ADAPT_NON_IDENTITY][3][3] = {
        [WP_ADAPT_BRADFORD] = {
            {  0.8951,  0.2664, -0.1614 },
            { -0.7502,  1.7135,  0.0367 },
            {  0.0389, -0.0685,  1.0296 },
        }, [WP_ADAPT_VON_KRIES] = {
            {  0.40024,  0.70760, -0.08081 },
            { -0.22630,  1.16532,  0.04570 },
            {  0.00000,  0.00000,  0.91822 },
        },
    };
    const double (*ma)[3] = ma_tbl[wp_adapt];
    double xw_src = av_q2d(wp_src->x), yw_src = av_q2d(wp_src->y);
    double xw_dst = av_q2d(wp_dst->x), yw_dst = av_q2d(wp_dst->y);
    double zw_src = 1.0 - xw_src - yw_src;
    double zw_dst = 1.0 - xw_dst - yw_dst;
    double mai[3][3], fac[3][3], tmp[3][3];
    double rs, gs, bs, rd, gd, bd;

    ff_matrix_invert_3x3(ma, mai);
    rs = ma[0][0] * xw_src + ma[0][1] * yw_src + ma[0][2] * zw_src;
    gs = ma[1][0] * xw_src + ma[1][1] * yw_src + ma[1][2] * zw_src;
    bs = ma[2][0] * xw_src + ma[2][1] * yw_src + ma[2][2] * zw_src;
    rd = ma[0][0] * xw_dst + ma[0][1] * yw_dst + ma[0][2] * zw_dst;
    gd = ma[1][0] * xw_dst + ma[1][1] * yw_dst + ma[1][2] * zw_dst;
    bd = ma[2][0] * xw_dst + ma[2][1] * yw_dst + ma[2][2] * zw_dst;
    fac[0][0] = rd / rs;
    fac[1][1] = gd / gs;
    fac[2][2] = bd / bs;
    fac[0][1] = fac[0][2] = fac[1][0] = fac[1][2] = fac[2][0] = fac[2][1] = 0.0;
    ff_matrix_mul_3x3(tmp, ma, fac);
    ff_matrix_mul_3x3(out, tmp, mai);
}

static void apply_lut(int16_t *buf[3], ptrdiff_t stride,
                      int w, int h, const int16_t *lut)
{
    int y, x, n;

    for (n = 0; n < 3; n++) {
        int16_t *data = buf[n];

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++)
                data[x] = lut[av_clip_uintp2(2048 + data[x], 15)];

            data += stride;
        }
    }
}

typedef struct ThreadData {
    AVFrame *in, *out;
    ptrdiff_t in_linesize[4], out_linesize[4];
    int in_ss_h, out_ss_h;
} ThreadData;

static int convert(AVFilterContext *ctx, void *data, int job_nr, int n_jobs)
{
    const ThreadData *td = data;
    ColorSpaceContext *s = ctx->priv;
    uint8_t *in_data[3], *out_data[3];
    int16_t *rgb[3];
    int h_in = (td->in->height + 1) >> 1;
    int h1 = 2 * (job_nr * h_in / n_jobs), h2 = 2 * ((job_nr + 1) * h_in / n_jobs);
    int w = td->in->width, h = h2 - h1;
    int rgb_stride = s->rgb_stride / 2;

    in_data[0]  = td->in->data[0]  + td->in_linesize[0]  *  h1;
    in_data[1]  = td->in->data[1]  + td->in_linesize[1]  * (h1 >> td->in_ss_h);
    in_data[2]  = td->in->data[2]  + td->in_linesize[2]  * (h1 >> td->in_ss_h);
    out_data[0] = td->out->data[0] + td->out_linesize[0] *  h1;
    out_data[1] = td->out->data[1] + td->out_linesize[1] * (h1 >> td->out_ss_h);
    out_data[2] = td->out->data[2] + td->out_linesize[2] * (h1 >> td->out_ss_h);
    rgb[0]      = (uint16_t*)(s->rgb[0] + s->rgb_stride  *  h1);
    rgb[1]      = (uint16_t*)(s->rgb[1] + s->rgb_stride  *  h1);
    rgb[2]      = (uint16_t*)(s->rgb[2] + s->rgb_stride  *  h1);

    // FIXME for simd, also make sure we do pictures with negative stride
    // top-down so we don't overwrite lines with padding of data before it
    // in the same buffer (same as swscale)

    if (s->yuv2yuv_fastmode) {
        // FIXME possibly use a fast mode in case only the y range changes?
        // since in that case, only the diagonal entries in yuv2yuv_coeffs[]
        // are non-zero
        s->yuv2yuv(out_data, td->out_linesize, in_data, td->in_linesize, w, h,
                   s->yuv2yuv_coeffs, s->yuv_offset);
    } else {
        // FIXME maybe (for caching efficiency) do pipeline per-line instead of
        // full buffer per function? (Or, since yuv2rgb requires 2 lines: per
        // 2 lines, for yuv420.)
        /*
         * General design:
         * - yuv2rgb converts from whatever range the input was ([16-235/240] or
         *   [0,255] or the 10/12bpp equivalents thereof) to an integer version
         *   of RGB in psuedo-restricted 15+sign bits. That means that the float
         *   range [0.0,1.0] is in [0,28762], and the remainder of the int16_t
         *   range is used for overflow/underflow outside the representable
         *   range of this RGB type. rgb2yuv is the exact opposite.
         * - gamma correction is done using a LUT since that appears to work
         *   fairly fast.
         * - If the input is chroma-subsampled (420/422), the yuv2rgb conversion
         *   (or rgb2yuv conversion) uses nearest-neighbour sampling to read
         *   read chroma pixels at luma resolution. If you want some more fancy
         *   filter, you can use swscale to convert to yuv444p.
         * - all coefficients are 14bit (so in the [-2.0,2.0] range).
         */
        s->yuv2rgb(rgb, rgb_stride, in_data, td->in_linesize, w, h,
                   s->yuv2rgb_coeffs, s->yuv_offset[0]);
        if (!s->rgb2rgb_passthrough) {
            apply_lut(rgb, rgb_stride, w, h, s->lin_lut);
            if (!s->lrgb2lrgb_passthrough)
                s->dsp.multiply3x3(rgb, rgb_stride, w, h, s->lrgb2lrgb_coeffs);
            apply_lut(rgb, rgb_stride, w, h, s->delin_lut);
        }
        if (s->dither == DITHER_FSB) {
            s->rgb2yuv_fsb(out_data, td->out_linesize, rgb, rgb_stride, w, h,
                           s->rgb2yuv_coeffs, s->yuv_offset[1], s->dither_scratch);
        } else {
            s->rgb2yuv(out_data, td->out_linesize, rgb, rgb_stride, w, h,
                       s->rgb2yuv_coeffs, s->yuv_offset[1]);
        }
    }

    return 0;
}

static void apply_lut_f16(uint16_t *out[3], const ptrdiff_t out_linesize[3],
                          uint16_t *in[3], const ptrdiff_t in_linesize[3], int w, int h,
                          const uint16_t *lut)
{
    int y, x, n;
    for (n = 0; n < 3; n++) {
        const uint16_t *src = in[n];
        uint16_t *dst = out[n];

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++)
                dst[x] = lut[src[x]];

            src += in_linesize[n]  / 2;
            dst += out_linesize[n] / 2;
        }
    }
}

static void multiply3x3_f16(ColorSpaceContext *s, uint16_t *buf[3], ptrdiff_t stride,
                            int w, int h, const float m[3][3][8])
{
    int y, x;
    uint16_t *buf0 = buf[0], *buf1 = buf[1], *buf2 = buf[2];

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v0 = av_int2float(half2float(buf0[x], s->h2f_tbl));
            float v1 = av_int2float(half2float(buf1[x], s->h2f_tbl));
            float v2 = av_int2float(half2float(buf2[x], s->h2f_tbl));
            buf0[x] = float2half(av_float2int(m[0][0][0] * v0 + m[0][1][0] * v1 + m[0][2][0] * v2), s->f2h_tbl);
            buf1[x] = float2half(av_float2int(m[1][0][0] * v0 + m[1][1][0] * v1 + m[1][2][0] * v2), s->f2h_tbl);
            buf2[x] = float2half(av_float2int(m[2][0][0] * v0 + m[2][1][0] * v1 + m[2][2][0] * v2), s->f2h_tbl);
        }

        buf0 += stride;
        buf1 += stride;
        buf2 += stride;
    }
}

static int convert_f16(AVFilterContext *ctx, void *data, int job_nr, int n_jobs)
{
    const ThreadData *td = data;
    ColorSpaceContext *s = ctx->priv;
    uint16_t *in_data[4], *out_data[4];
    uint16_t *rgb[3];
    int h_in = (td->in->height + 1) >> 1;
    int h1 = 2 * (job_nr * h_in / n_jobs), h2 = 2 * ((job_nr + 1) * h_in / n_jobs);
    int w = td->in->width, h = h2 - h1;
    ptrdiff_t rgb_stride[3] = { s->rgb_stride, s->rgb_stride, s->rgb_stride };

    // GBR order
    in_data[0]  = (uint16_t*)(td->in->data[2]  + td->in_linesize[2]  * h1);
    in_data[1]  = (uint16_t*)(td->in->data[0]  + td->in_linesize[0]  * h1);
    in_data[2]  = (uint16_t*)(td->in->data[1]  + td->in_linesize[1]  * h1);
    in_data[3]  = (uint16_t*)(td->in->data[3]  + td->in_linesize[3]  * h1);
    out_data[0] = (uint16_t*)(td->out->data[2] + td->out_linesize[2] * h1);
    out_data[1] = (uint16_t*)(td->out->data[0] + td->out_linesize[0] * h1);
    out_data[2] = (uint16_t*)(td->out->data[1] + td->out_linesize[1] * h1);
    out_data[3] = (uint16_t*)(td->out->data[3] + td->out_linesize[3] * h1);
    rgb[0]      = (uint16_t*)(s->rgb[0]        + s->rgb_stride       * h1);
    rgb[1]      = (uint16_t*)(s->rgb[1]        + s->rgb_stride       * h1);
    rgb[2]      = (uint16_t*)(s->rgb[2]        + s->rgb_stride       * h1);

    if (s->in_trc == AVCOL_TRC_LINEAR && s->lrgb2lrgb_passthrough) {
        apply_lut_f16(out_data, td->out_linesize,
                      in_data, td->in_linesize, w, h, s->delin_lut);
    } else {
        apply_lut_f16(rgb, rgb_stride, in_data, td->in_linesize, w, h, (uint16_t*)s->lin_lut);
        if (!s->lrgb2lrgb_passthrough)
            multiply3x3_f16(s, rgb, s->rgb_stride/2, w, h, s->lrgb2lrgb_coeffsf);
        apply_lut_f16(out_data, td->out_linesize, rgb, rgb_stride, w, h, (uint16_t*)s->delin_lut);
    }

    // // copy alpha
    if(in_data[3]) {
        uint8_t *src = (uint8_t*)in_data[3];
        uint8_t *dst = (uint8_t*)out_data[3];
        for (int y = 0; y < h; y++) {
            memcpy(dst, src, td->in_linesize[3]);
            src += td->in_linesize[3];
            dst += td->out_linesize[3];
        }
    }

    return 0;
}

static void apply_linearize_f32(ColorSpaceContext *s,
                                float *out[3], const ptrdiff_t out_linesize[3],
                                float *in[3], const ptrdiff_t in_linesize[3], int w, int h)
{
    int y, x, n;
    float v;

    double in_alpha = s->in_txchr->alpha, in_beta = s->in_txchr->beta;
    double in_gamma = s->in_txchr->gamma, in_delta = s->in_txchr->delta;
    double in_ialpha = 1.0 / in_alpha, in_igamma = 1.0 / in_gamma, in_idelta = 1.0 / in_delta;

    for (n = 0; n < 3; n++) {
        const float *src = in[n];
        float *dst = out[n];

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                v =  src[x];
                if (v <= -in_beta * in_delta) {
                    v = -powf((1.0 - in_alpha - v) * in_ialpha, in_igamma);
                } else if (v < in_beta * in_delta) {
                    v = v * in_idelta;
                } else {
                    v = powf((v + in_alpha - 1.0) * in_ialpha, in_igamma);
                }

                dst[x] = v;
            }

            src += in_linesize[n] / 4;
            dst += out_linesize[n] / 4;
        }
    }
}

static void apply_delinearize_f32(ColorSpaceContext *s,
                                  float *out[3], const ptrdiff_t out_linesize[3],
                                  float *in[3], const ptrdiff_t in_linesize[3], int w, int h)
{
    int y, x, n;
    float v;

    double out_alpha = 0.0, out_beta = 0.0;
    double out_gamma = 0.0, out_delta = 0.0;

    if (!s->out_trc_fn) {
        out_alpha = s->out_txchr->alpha, out_beta = s->out_txchr->beta;
        out_gamma = s->out_txchr->gamma, out_delta = s->out_txchr->delta;
    }

    for (n = 0; n < 3; n++) {
        const float *src = in[n];
        float *dst = out[n];

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                v =  src[x];
                if (s->out_trc_fn) {
                    v = s->out_trc_fn(v);
                } else {
                    if (v <= -out_beta) {
                        v = -out_alpha * powf(-v, out_gamma) + (out_alpha - 1.0);
                    } else if (v < out_beta) {
                        v = out_delta * v;
                    } else {
                        v = out_alpha * powf(v, out_gamma) - (out_alpha - 1.0);
                    }
                }
                dst[x] = v;
            }

            src += in_linesize[n] / 4;
            dst += out_linesize[n] / 4;
        }
    }
}

static int convert_f32(AVFilterContext *ctx, void *data, int job_nr, int n_jobs)
{
    const ThreadData *td = data;
    ColorSpaceContext *s = ctx->priv;
    float *in_data[4], *out_data[4];
    float *rgb[3];
    int h_in = (td->in->height + 1) >> 1;
    int h1 = 2 * (job_nr * h_in / n_jobs), h2 = 2 * ((job_nr + 1) * h_in / n_jobs);
    int w = td->in->width, h = h2 - h1;
    ptrdiff_t rgb_stride[3] = { s->rgb_stride, s->rgb_stride, s->rgb_stride };

    // GBR order
    in_data[0]  = (float*)(td->in->data[2]  + td->in_linesize[2]  * h1);
    in_data[1]  = (float*)(td->in->data[0]  + td->in_linesize[0]  * h1);
    in_data[2]  = (float*)(td->in->data[1]  + td->in_linesize[1]  * h1);
    in_data[3]  = (float*)(td->in->data[3]  + td->in_linesize[3]  * h1);
    out_data[0] = (float*)(td->out->data[2] + td->out_linesize[2] * h1);
    out_data[1] = (float*)(td->out->data[0] + td->out_linesize[0] * h1);
    out_data[2] = (float*)(td->out->data[1] + td->out_linesize[1] * h1);
    out_data[3] = (float*)(td->out->data[3] + td->out_linesize[3] * h1);
    rgb[0]      = (float*)(s->rgb[0]        + s->rgb_stride       * h1);
    rgb[1]      = (float*)(s->rgb[1]        + s->rgb_stride       * h1);
    rgb[2]      = (float*)(s->rgb[2]        + s->rgb_stride       * h1);

    if (s->in_trc == AVCOL_TRC_LINEAR && s->lrgb2lrgb_passthrough) {
        apply_delinearize_f32(s, out_data, td->out_linesize,
                              in_data, td->in_linesize, w, h);
    } else {
        apply_linearize_f32(s, rgb, rgb_stride,in_data, td->in_linesize, w, h);
        if (!s->lrgb2lrgb_passthrough)
            s->dsp.multiply3x3_f32(rgb, s->rgb_stride/4, w, h, s->lrgb2lrgb_coeffsf);
        apply_delinearize_f32(s, out_data, td->out_linesize, rgb, rgb_stride, w, h);
    }

    // copy alpha
    if(in_data[3]) {
        uint8_t *src = (uint8_t*)in_data[3];
        uint8_t *dst = (uint8_t*)out_data[3];
        for (int y = 0; y < h; y++) {
            memcpy(dst, src, td->in_linesize[3]);
            src += td->in_linesize[3];
            dst += td->out_linesize[3];
        }
    }

    return 0;
}

static int get_range_off(AVFilterContext *ctx, int *off,
                         int *y_rng, int *uv_rng,
                         enum AVColorRange rng, int depth)
{
    switch (rng) {
    case AVCOL_RANGE_UNSPECIFIED: {
        ColorSpaceContext *s = ctx->priv;

        if (!s->did_warn_range) {
            av_log(ctx, AV_LOG_WARNING, "Input range not set, assuming tv/mpeg\n");
            s->did_warn_range = 1;
        }
    }
        // fall-through
    case AVCOL_RANGE_MPEG:
        *off = 16 << (depth - 8);
        *y_rng = 219 << (depth - 8);
        *uv_rng = 224 << (depth - 8);
        break;
    case AVCOL_RANGE_JPEG:
        *off = 0;
        *y_rng = *uv_rng = (256 << (depth - 8)) - 1;
        break;
    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int create_filtergraph(AVFilterContext *ctx,
                              const AVFrame *in, const AVFrame *out)
{
    ColorSpaceContext *s = ctx->priv;
    const AVPixFmtDescriptor *in_desc  = av_pix_fmt_desc_get(in->format);
    const AVPixFmtDescriptor *out_desc = av_pix_fmt_desc_get(out->format);
    int emms = 0, m, n, o, res, fmt_identical, redo_yuv2rgb = 0, redo_rgb2yuv = 0;

#define supported_depth(d) ((d) == 8 || (d) == 10 || (d) == 12 || (d) == 16 || (d) == 32)
#define supported_subsampling(lcw, lch) \
    (((lcw) == 0 && (lch) == 0) || ((lcw) == 1 && (lch) == 0) || ((lcw) == 1 && (lch) == 1))
#define supported_format(d) \
    ((d) != NULL && (d)->nb_components >= 3 && \
     supported_depth((d)->comp[0].depth) && \
     supported_subsampling((d)->log2_chroma_w, (d)->log2_chroma_h))

    if ((in_desc->flags & AV_PIX_FMT_FLAG_RGB) != (out_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        av_log(ctx, AV_LOG_ERROR,
               "Unsupported format conversion %d (%s) to %d (%s)\n",
               in->format, av_get_pix_fmt_name(in->format),
               out->format, av_get_pix_fmt_name(out->format));
        return AVERROR(EINVAL);
    }

    if (!supported_format(in_desc)) {
        av_log(ctx, AV_LOG_ERROR,
               "Unsupported input format %d (%s) or bitdepth (%d)\n",
               in->format, av_get_pix_fmt_name(in->format),
               in_desc ? in_desc->comp[0].depth : -1);
        return AVERROR(EINVAL);
    }
    if (!supported_format(out_desc)) {
        av_log(ctx, AV_LOG_ERROR,
               "Unsupported output format %d (%s) or bitdepth (%d)\n",
               out->format, av_get_pix_fmt_name(out->format),
               out_desc ? out_desc->comp[0].depth : -1);
        return AVERROR(EINVAL);
    }

    s->is_float = in_desc->flags & AV_PIX_FMT_FLAG_FLOAT;

    if (s->is_float && in_desc->comp[0].depth == 16) {
        s->is_float16 = 1;
        if (!s->f2h_tbl) {
            s->f2h_tbl = av_malloc(sizeof(*s->f2h_tbl));
            if (!s->f2h_tbl)
                return AVERROR(ENOMEM);
            ff_init_float2half_tables(s->f2h_tbl);
        }

        if (!s->h2f_tbl) {
            s->h2f_tbl = av_malloc(sizeof(*s->h2f_tbl));
            if (!s->h2f_tbl)
                return AVERROR(ENOMEM);
            ff_init_half2float_tables(s->h2f_tbl);
        }
    }

    if (in->color_primaries  != s->in_prm)  s->in_primaries  = NULL;
    if (out->color_primaries != s->out_prm) s->out_primaries = NULL;
    if (in->color_trc        != s->in_trc)  s->in_txchr      = NULL;
    if (out->color_trc       != s->out_trc) s->out_txchr     = NULL;
    if (in->colorspace       != s->in_csp ||
        in->color_range      != s->in_rng)  s->in_lumacoef   = NULL;
    if (out->colorspace      != s->out_csp ||
        out->color_range     != s->out_rng) s->out_lumacoef  = NULL;

    if (!s->out_primaries || !s->in_primaries) {
        s->in_prm = in->color_primaries;
        if (s->user_iall != CS_UNSPECIFIED)
            s->in_prm = default_prm[FFMIN(s->user_iall, CS_NB)];
        if (s->user_iprm != AVCOL_PRI_UNSPECIFIED)
            s->in_prm = s->user_iprm;
        s->in_primaries = av_csp_primaries_desc_from_id(s->in_prm);
        if (!s->in_primaries) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unsupported input primaries %d (%s)\n",
                   s->in_prm, av_color_primaries_name(s->in_prm));
            return AVERROR(EINVAL);
        }
        s->out_prm = out->color_primaries;
        s->out_primaries = av_csp_primaries_desc_from_id(s->out_prm);
        if (!s->out_primaries) {
            if (s->out_prm == AVCOL_PRI_UNSPECIFIED) {
                if (s->user_all == CS_UNSPECIFIED) {
                    av_log(ctx, AV_LOG_ERROR, "Please specify output primaries\n");
                } else {
                    av_log(ctx, AV_LOG_ERROR,
                           "Unsupported output color property %d\n", s->user_all);
                }
            } else {
                av_log(ctx, AV_LOG_ERROR,
                       "Unsupported output primaries %d (%s)\n",
                       s->out_prm, av_color_primaries_name(s->out_prm));
            }
            return AVERROR(EINVAL);
        }
        s->lrgb2lrgb_passthrough = !memcmp(s->in_primaries, s->out_primaries,
                                           sizeof(*s->in_primaries));
        if (!s->lrgb2lrgb_passthrough) {
            double rgb2xyz[3][3], xyz2rgb[3][3], rgb2rgb[3][3];
            const AVWhitepointCoefficients *wp_out, *wp_in;

            wp_out = &s->out_primaries->wp;
            wp_in = &s->in_primaries->wp;
            ff_fill_rgb2xyz_table(&s->out_primaries->prim, wp_out, rgb2xyz);
            ff_matrix_invert_3x3(rgb2xyz, xyz2rgb);
            ff_fill_rgb2xyz_table(&s->in_primaries->prim, wp_in, rgb2xyz);
            if (memcmp(wp_in, wp_out, sizeof(*wp_in)) != 0 &&
                s->wp_adapt != WP_ADAPT_IDENTITY) {
                double wpconv[3][3], tmp[3][3];

                fill_whitepoint_conv_table(wpconv, s->wp_adapt, &s->in_primaries->wp,
                                           &s->out_primaries->wp);
                ff_matrix_mul_3x3(tmp, rgb2xyz, wpconv);
                ff_matrix_mul_3x3(rgb2rgb, tmp, xyz2rgb);
            } else {
                ff_matrix_mul_3x3(rgb2rgb, rgb2xyz, xyz2rgb);
            }
            for (m = 0; m < 3; m++)
                for (n = 0; n < 3; n++) {
                    s->lrgb2lrgb_coeffs[m][n][0] = lrint(16384.0 * rgb2rgb[m][n]);
                    s->lrgb2lrgb_coeffsf[m][n][0] = rgb2rgb[m][n];
                    for (o = 1; o < 8; o++) {
                        s->lrgb2lrgb_coeffs[m][n][o] = s->lrgb2lrgb_coeffs[m][n][0];
                        s->lrgb2lrgb_coeffsf[m][n][o] = s->lrgb2lrgb_coeffsf[m][n][0];
                    }
                }

            emms = 1;
        }
    }

    if (!s->in_txchr) {
        av_freep(&s->lin_lut);
        s->in_trc = in->color_trc;
        if (s->user_iall != CS_UNSPECIFIED)
            s->in_trc = default_trc[FFMIN(s->user_iall, CS_NB)];
        if (s->user_itrc != AVCOL_TRC_UNSPECIFIED)
            s->in_trc = s->user_itrc;
        s->in_txchr = get_transfer_characteristics(s->in_trc);
        if (!s->in_txchr) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unsupported input transfer characteristics %d (%s)\n",
                   s->in_trc, av_color_transfer_name(s->in_trc));
            return AVERROR(EINVAL);
        }
    }

    if (!s->out_txchr) {
        av_freep(&s->lin_lut);
        s->out_trc = out->color_trc;
        s->out_txchr = get_transfer_characteristics(s->out_trc);
        if (!s->out_txchr) {
            s->out_trc_fn = avpriv_get_trc_function_from_trc(s->out_trc);
            if (!s->out_trc_fn) {
                if (s->out_trc == AVCOL_TRC_UNSPECIFIED) {
                    if (s->user_all == CS_UNSPECIFIED) {
                        av_log(ctx, AV_LOG_ERROR,
                            "Please specify output transfer characteristics\n");
                    } else {
                        av_log(ctx, AV_LOG_ERROR,
                            "Unsupported output color property %d\n", s->user_all);
                    }
                } else {
                    av_log(ctx, AV_LOG_ERROR,
                        "Unsupported output transfer characteristics %d (%s)\n",
                        s->out_trc, av_color_transfer_name(s->out_trc));
                }
                return AVERROR(EINVAL);
            }
        }
    }

    s->rgb2rgb_passthrough = s->fast_mode || (s->lrgb2lrgb_passthrough && !s->out_trc_fn &&
                             !memcmp(s->in_txchr, s->out_txchr, sizeof(*s->in_txchr)));
    if (!s->rgb2rgb_passthrough && !s->lin_lut) {
        if (!s->is_float) {
            res = fill_gamma_table(s);
            if (res < 0)
                return res;
            emms = 1;
        } else if (s->is_float16) {
            res = fill_gamma_table_f16(s);
            if (res < 0)
                return res;
            emms = 1;
        }
    }

    if (!s->in_lumacoef) {
        s->in_csp = in->colorspace;
        if (s->user_iall != CS_UNSPECIFIED)
            s->in_csp = default_csp[FFMIN(s->user_iall, CS_NB)];
        if (s->user_icsp != AVCOL_SPC_UNSPECIFIED)
            s->in_csp = s->user_icsp;
        s->in_rng = in->color_range;
        if (s->user_irng != AVCOL_RANGE_UNSPECIFIED)
            s->in_rng = s->user_irng;
        s->in_lumacoef = av_csp_luma_coeffs_from_avcsp(s->in_csp);
        if (!s->in_lumacoef) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unsupported input colorspace %d (%s)\n",
                   s->in_csp, av_color_space_name(s->in_csp));
            return AVERROR(EINVAL);
        }
        redo_yuv2rgb = 1;
    }

    if (!s->out_lumacoef) {
        s->out_csp = out->colorspace;
        s->out_rng = out->color_range;
        s->out_lumacoef = av_csp_luma_coeffs_from_avcsp(s->out_csp);
        if (!s->out_lumacoef) {
            if (s->out_csp == AVCOL_SPC_UNSPECIFIED) {
                if (s->user_all == CS_UNSPECIFIED) {
                    av_log(ctx, AV_LOG_ERROR,
                           "Please specify output transfer characteristics\n");
                } else {
                    av_log(ctx, AV_LOG_ERROR,
                           "Unsupported output color property %d\n", s->user_all);
                }
            } else {
                av_log(ctx, AV_LOG_ERROR,
                       "Unsupported output transfer characteristics %d (%s)\n",
                       s->out_csp, av_color_space_name(s->out_csp));
            }
            return AVERROR(EINVAL);
        }
        redo_rgb2yuv = 1;
    }

    fmt_identical = in_desc->log2_chroma_h == out_desc->log2_chroma_h &&
                    in_desc->log2_chroma_w == out_desc->log2_chroma_w;
    s->yuv2yuv_fastmode = s->rgb2rgb_passthrough && fmt_identical;
    s->yuv2yuv_passthrough = s->yuv2yuv_fastmode && s->in_rng == s->out_rng &&
                             !memcmp(s->in_lumacoef, s->out_lumacoef,
                                     sizeof(*s->in_lumacoef)) &&
                             in_desc->comp[0].depth == out_desc->comp[0].depth;
    if (!s->yuv2yuv_passthrough && !(in_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        if (redo_yuv2rgb) {
            double rgb2yuv[3][3], (*yuv2rgb)[3] = s->yuv2rgb_dbl_coeffs;
            int off, bits, in_rng;

            res = get_range_off(ctx, &off, &s->in_y_rng, &s->in_uv_rng,
                                s->in_rng, in_desc->comp[0].depth);
            if (res < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "Unsupported input color range %d (%s)\n",
                       s->in_rng, av_color_range_name(s->in_rng));
                return res;
            }
            for (n = 0; n < 8; n++)
                s->yuv_offset[0][n] = off;
            ff_fill_rgb2yuv_table(s->in_lumacoef, rgb2yuv);
            ff_matrix_invert_3x3(rgb2yuv, yuv2rgb);
            bits = 1 << (in_desc->comp[0].depth - 1);
            for (n = 0; n < 3; n++) {
                for (in_rng = s->in_y_rng, m = 0; m < 3; m++, in_rng = s->in_uv_rng) {
                    s->yuv2rgb_coeffs[n][m][0] = lrint(28672 * bits * yuv2rgb[n][m] / in_rng);
                    for (o = 1; o < 8; o++)
                        s->yuv2rgb_coeffs[n][m][o] = s->yuv2rgb_coeffs[n][m][0];
                }
            }
            av_assert2(s->yuv2rgb_coeffs[0][1][0] == 0);
            av_assert2(s->yuv2rgb_coeffs[2][2][0] == 0);
            av_assert2(s->yuv2rgb_coeffs[0][0][0] == s->yuv2rgb_coeffs[1][0][0]);
            av_assert2(s->yuv2rgb_coeffs[0][0][0] == s->yuv2rgb_coeffs[2][0][0]);
            s->yuv2rgb = s->dsp.yuv2rgb[(in_desc->comp[0].depth - 8) >> 1]
                                       [in_desc->log2_chroma_h + in_desc->log2_chroma_w];
            emms = 1;
        }

        if (redo_rgb2yuv && !(out_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
            double (*rgb2yuv)[3] = s->rgb2yuv_dbl_coeffs;
            int off, out_rng, bits;

            res = get_range_off(ctx, &off, &s->out_y_rng, &s->out_uv_rng,
                                s->out_rng, out_desc->comp[0].depth);
            if (res < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "Unsupported output color range %d (%s)\n",
                       s->out_rng, av_color_range_name(s->out_rng));
                return res;
            }
            for (n = 0; n < 8; n++)
                s->yuv_offset[1][n] = off;
            ff_fill_rgb2yuv_table(s->out_lumacoef, rgb2yuv);
            bits = 1 << (29 - out_desc->comp[0].depth);
            for (out_rng = s->out_y_rng, n = 0; n < 3; n++, out_rng = s->out_uv_rng) {
                for (m = 0; m < 3; m++) {
                    s->rgb2yuv_coeffs[n][m][0] = lrint(bits * out_rng * rgb2yuv[n][m] / 28672);
                    for (o = 1; o < 8; o++)
                        s->rgb2yuv_coeffs[n][m][o] = s->rgb2yuv_coeffs[n][m][0];
                }
            }
            av_assert2(s->rgb2yuv_coeffs[1][2][0] == s->rgb2yuv_coeffs[2][0][0]);
            s->rgb2yuv = s->dsp.rgb2yuv[(out_desc->comp[0].depth - 8) >> 1]
                                       [out_desc->log2_chroma_h + out_desc->log2_chroma_w];
            s->rgb2yuv_fsb = s->dsp.rgb2yuv_fsb[(out_desc->comp[0].depth - 8) >> 1]
                                       [out_desc->log2_chroma_h + out_desc->log2_chroma_w];
            emms = 1;
        }

        if (s->yuv2yuv_fastmode && (redo_yuv2rgb || redo_rgb2yuv) && !(in_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
            int idepth = in_desc->comp[0].depth, odepth = out_desc->comp[0].depth;
            double (*rgb2yuv)[3] = s->rgb2yuv_dbl_coeffs;
            double (*yuv2rgb)[3] = s->yuv2rgb_dbl_coeffs;
            double yuv2yuv[3][3];
            int in_rng, out_rng;

            ff_matrix_mul_3x3(yuv2yuv, yuv2rgb, rgb2yuv);
            for (out_rng = s->out_y_rng, m = 0; m < 3; m++, out_rng = s->out_uv_rng) {
                for (in_rng = s->in_y_rng, n = 0; n < 3; n++, in_rng = s->in_uv_rng) {
                    s->yuv2yuv_coeffs[m][n][0] =
                        lrint(16384 * yuv2yuv[m][n] * out_rng * (1 << idepth) /
                              (in_rng * (1 << odepth)));
                    for (o = 1; o < 8; o++)
                        s->yuv2yuv_coeffs[m][n][o] = s->yuv2yuv_coeffs[m][n][0];
                }
            }
            av_assert2(s->yuv2yuv_coeffs[1][0][0] == 0);
            av_assert2(s->yuv2yuv_coeffs[2][0][0] == 0);
            s->yuv2yuv = s->dsp.yuv2yuv[(idepth - 8) >> 1][(odepth - 8) >> 1]
                                       [in_desc->log2_chroma_h + in_desc->log2_chroma_w];
        }
    }

    if (emms)
        emms_c();

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    ColorSpaceContext *s = ctx->priv;

    ff_colorspacedsp_init(&s->dsp);

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    ColorSpaceContext *s = ctx->priv;

    av_freep(&s->rgb[0]);
    av_freep(&s->rgb[1]);
    av_freep(&s->rgb[2]);
    s->rgb_sz = 0;
    av_freep(&s->dither_scratch_base[0][0]);
    av_freep(&s->dither_scratch_base[0][1]);
    av_freep(&s->dither_scratch_base[1][0]);
    av_freep(&s->dither_scratch_base[1][1]);
    av_freep(&s->dither_scratch_base[2][0]);
    av_freep(&s->dither_scratch_base[2][1]);

    av_freep(&s->lin_lut);

    av_freep(&s->f2h_tbl);
    av_freep(&s->h2f_tbl);
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    ColorSpaceContext *s = ctx->priv;
    // FIXME if yuv2yuv_passthrough, don't get a new buffer but use the
    // input one if it is writable *OR* the actual literal values of in_*
    // and out_* are identical (not just their respective properties)
    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    int res;

    int pixel_size = (in->format == AV_PIX_FMT_GBRPF32 ||
                      in->format == AV_PIX_FMT_GBRAPF32) ?
                      sizeof(float) : sizeof(uint16_t);
    ptrdiff_t rgb_stride = FFALIGN(in->width * pixel_size, 32);
    unsigned rgb_sz = rgb_stride * in->height;
    ThreadData td;

    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    res = av_frame_copy_props(out, in);
    if (res < 0) {
        av_frame_free(&in);
        av_frame_free(&out);
        return res;
    }

    out->color_primaries = s->user_prm == AVCOL_PRI_UNSPECIFIED ?
                           default_prm[FFMIN(s->user_all, CS_NB)] : s->user_prm;
    if (s->user_trc == AVCOL_TRC_UNSPECIFIED) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(out->format);

        out->color_trc   = default_trc[FFMIN(s->user_all, CS_NB)];
        if (out->color_trc == AVCOL_TRC_BT2020_10 && desc && desc->comp[0].depth >= 12)
            out->color_trc = AVCOL_TRC_BT2020_12;
    } else {
        out->color_trc   = s->user_trc;
    }
    out->colorspace      = s->user_csp == AVCOL_SPC_UNSPECIFIED ?
                           default_csp[FFMIN(s->user_all, CS_NB)] : s->user_csp;
    out->color_range     = s->user_rng == AVCOL_RANGE_UNSPECIFIED ?
                           in->color_range : s->user_rng;
    if (rgb_sz != s->rgb_sz) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(out->format);
        int uvw = in->width >> desc->log2_chroma_w;

        av_freep(&s->rgb[0]);
        av_freep(&s->rgb[1]);
        av_freep(&s->rgb[2]);
        s->rgb_sz = 0;
        av_freep(&s->dither_scratch_base[0][0]);
        av_freep(&s->dither_scratch_base[0][1]);
        av_freep(&s->dither_scratch_base[1][0]);
        av_freep(&s->dither_scratch_base[1][1]);
        av_freep(&s->dither_scratch_base[2][0]);
        av_freep(&s->dither_scratch_base[2][1]);

        s->rgb[0] = av_malloc(rgb_sz);
        s->rgb[1] = av_malloc(rgb_sz);
        s->rgb[2] = av_malloc(rgb_sz);
        s->dither_scratch_base[0][0] =
            av_malloc(sizeof(*s->dither_scratch_base[0][0]) * (in->width + 4));
        s->dither_scratch_base[0][1] =
            av_malloc(sizeof(*s->dither_scratch_base[0][1]) * (in->width + 4));
        s->dither_scratch_base[1][0] =
            av_malloc(sizeof(*s->dither_scratch_base[1][0]) * (uvw + 4));
        s->dither_scratch_base[1][1] =
            av_malloc(sizeof(*s->dither_scratch_base[1][1]) * (uvw + 4));
        s->dither_scratch_base[2][0] =
            av_malloc(sizeof(*s->dither_scratch_base[2][0]) * (uvw + 4));
        s->dither_scratch_base[2][1] =
            av_malloc(sizeof(*s->dither_scratch_base[2][1]) * (uvw + 4));
        s->dither_scratch[0][0] = &s->dither_scratch_base[0][0][1];
        s->dither_scratch[0][1] = &s->dither_scratch_base[0][1][1];
        s->dither_scratch[1][0] = &s->dither_scratch_base[1][0][1];
        s->dither_scratch[1][1] = &s->dither_scratch_base[1][1][1];
        s->dither_scratch[2][0] = &s->dither_scratch_base[2][0][1];
        s->dither_scratch[2][1] = &s->dither_scratch_base[2][1][1];
        if (!s->rgb[0] || !s->rgb[1] || !s->rgb[2] ||
            !s->dither_scratch_base[0][0] || !s->dither_scratch_base[0][1] ||
            !s->dither_scratch_base[1][0] || !s->dither_scratch_base[1][1] ||
            !s->dither_scratch_base[2][0] || !s->dither_scratch_base[2][1]) {
            uninit(ctx);
            av_frame_free(&in);
            av_frame_free(&out);
            return AVERROR(ENOMEM);
        }
        s->rgb_sz = rgb_sz;
    }
    res = create_filtergraph(ctx, in, out);
    if (res < 0) {
        av_frame_free(&in);
        av_frame_free(&out);
        return res;
    }
    s->rgb_stride = rgb_stride;
    td.in = in;
    td.out = out;
    td.in_linesize[0] = in->linesize[0];
    td.in_linesize[1] = in->linesize[1];
    td.in_linesize[2] = in->linesize[2];
    td.in_linesize[3] = in->linesize[3];
    td.out_linesize[0] = out->linesize[0];
    td.out_linesize[1] = out->linesize[1];
    td.out_linesize[2] = out->linesize[2];
    td.out_linesize[3] = out->linesize[3];
    td.in_ss_h = av_pix_fmt_desc_get(in->format)->log2_chroma_h;
    td.out_ss_h = av_pix_fmt_desc_get(out->format)->log2_chroma_h;
    if (s->yuv2yuv_passthrough) {
        res = av_frame_copy(out, in);
        if (res < 0) {
            av_frame_free(&in);
            av_frame_free(&out);
            return res;
        }
    } else {
        ff_filter_execute(ctx, s->is_float? s->is_float16? convert_f16 : convert_f32 : convert,
                          &td, NULL, FFMIN((in->height + 1) >> 1, ff_filter_get_nb_threads(ctx)));
    }
    av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,   AV_PIX_FMT_YUV422P,   AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_YUVJ420P,  AV_PIX_FMT_YUVJ422P,  AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_GBRPF16, AV_PIX_FMT_GBRAPF16,
        AV_PIX_FMT_GBRPF32, AV_PIX_FMT_GBRAPF32,
        AV_PIX_FMT_NONE
    };
    int res;
    ColorSpaceContext *s = ctx->priv;
    AVFilterFormats *formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);
    if (s->user_format == AV_PIX_FMT_NONE)
        return ff_set_common_formats(ctx, formats);
    res = ff_formats_ref(formats, &ctx->inputs[0]->outcfg.formats);
    if (res < 0)
        return res;
    formats = NULL;
    res = ff_add_format(&formats, s->user_format);
    if (res < 0)
        return res;

    return ff_formats_ref(formats, &ctx->outputs[0]->incfg.formats);
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->dst;
    AVFilterLink *inlink = outlink->src->inputs[0];

    if (inlink->w % 2 || inlink->h % 2) {
        av_log(ctx, AV_LOG_ERROR, "Invalid odd size (%dx%d)\n",
               inlink->w, inlink->h);
        return AVERROR_PATCHWELCOME;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base = inlink->time_base;

    return 0;
}

#define OFFSET(x) offsetof(ColorSpaceContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
#define ENUM(x, y, z) { x, "", 0, AV_OPT_TYPE_CONST, { .i64 = y }, INT_MIN, INT_MAX, FLAGS, z }

static const AVOption colorspace_options[] = {
    { "all",        "Set all color properties together",
      OFFSET(user_all),   AV_OPT_TYPE_INT, { .i64 = CS_UNSPECIFIED },
      CS_UNSPECIFIED, CS_NB - 1, FLAGS, "all" },
    ENUM("bt470m",      CS_BT470M,             "all"),
    ENUM("bt470bg",     CS_BT470BG,            "all"),
    ENUM("bt601-6-525", CS_BT601_6_525,        "all"),
    ENUM("bt601-6-625", CS_BT601_6_625,        "all"),
    ENUM("bt709",       CS_BT709,              "all"),
    ENUM("smpte170m",   CS_SMPTE170M,          "all"),
    ENUM("smpte240m",   CS_SMPTE240M,          "all"),
    ENUM("bt2020",      CS_BT2020,             "all"),

    { "space",      "Output colorspace",
      OFFSET(user_csp),   AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED },
      AVCOL_SPC_RGB, AVCOL_SPC_NB - 1, FLAGS,  "csp"},
    ENUM("bt709",       AVCOL_SPC_BT709,       "csp"),
    ENUM("fcc",         AVCOL_SPC_FCC,         "csp"),
    ENUM("bt470bg",     AVCOL_SPC_BT470BG,     "csp"),
    ENUM("smpte170m",   AVCOL_SPC_SMPTE170M,   "csp"),
    ENUM("smpte240m",   AVCOL_SPC_SMPTE240M,   "csp"),
    ENUM("ycgco",       AVCOL_SPC_YCGCO,       "csp"),
    ENUM("gbr",         AVCOL_SPC_RGB,         "csp"),
    ENUM("bt2020nc",    AVCOL_SPC_BT2020_NCL,  "csp"),
    ENUM("bt2020ncl",   AVCOL_SPC_BT2020_NCL,  "csp"),

    { "range",      "Output color range",
      OFFSET(user_rng),   AV_OPT_TYPE_INT, { .i64 = AVCOL_RANGE_UNSPECIFIED },
      AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_NB - 1, FLAGS, "rng" },
    ENUM("tv",          AVCOL_RANGE_MPEG,      "rng"),
    ENUM("mpeg",        AVCOL_RANGE_MPEG,      "rng"),
    ENUM("pc",          AVCOL_RANGE_JPEG,      "rng"),
    ENUM("jpeg",        AVCOL_RANGE_JPEG,      "rng"),

    { "primaries",  "Output color primaries",
      OFFSET(user_prm),   AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_UNSPECIFIED },
      AVCOL_PRI_RESERVED0, AVCOL_PRI_NB - 1, FLAGS, "prm" },
    ENUM("bt709",        AVCOL_PRI_BT709,      "prm"),
    ENUM("bt470m",       AVCOL_PRI_BT470M,     "prm"),
    ENUM("bt470bg",      AVCOL_PRI_BT470BG,    "prm"),
    ENUM("smpte170m",    AVCOL_PRI_SMPTE170M,  "prm"),
    ENUM("smpte240m",    AVCOL_PRI_SMPTE240M,  "prm"),
    ENUM("smpte428",     AVCOL_PRI_SMPTE428,   "prm"),
    ENUM("film",         AVCOL_PRI_FILM,       "prm"),
    ENUM("smpte431",     AVCOL_PRI_SMPTE431,   "prm"),
    ENUM("smpte432",     AVCOL_PRI_SMPTE432,   "prm"),
    ENUM("bt2020",       AVCOL_PRI_BT2020,     "prm"),
    ENUM("jedec-p22",    AVCOL_PRI_JEDEC_P22,  "prm"),
    ENUM("ebu3213",      AVCOL_PRI_EBU3213,    "prm"),

    { "trc",        "Output transfer characteristics",
      OFFSET(user_trc),   AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_UNSPECIFIED },
      AVCOL_TRC_RESERVED0, AVCOL_TRC_NB - 1, FLAGS, "trc" },
    ENUM("bt709",        AVCOL_TRC_BT709,        "trc"),
    ENUM("bt470m",       AVCOL_TRC_GAMMA22,      "trc"),
    ENUM("gamma22",      AVCOL_TRC_GAMMA22,      "trc"),
    ENUM("bt470bg",      AVCOL_TRC_GAMMA28,      "trc"),
    ENUM("gamma28",      AVCOL_TRC_GAMMA28,      "trc"),
    ENUM("smpte170m",    AVCOL_TRC_SMPTE170M,    "trc"),
    ENUM("smpte240m",    AVCOL_TRC_SMPTE240M,    "trc"),
    ENUM("linear",       AVCOL_TRC_LINEAR,       "trc"),
    ENUM("srgb",         AVCOL_TRC_IEC61966_2_1, "trc"),
    ENUM("iec61966-2-1", AVCOL_TRC_IEC61966_2_1, "trc"),
    ENUM("xvycc",        AVCOL_TRC_IEC61966_2_4, "trc"),
    ENUM("iec61966-2-4", AVCOL_TRC_IEC61966_2_4, "trc"),
    ENUM("bt2020-10",    AVCOL_TRC_BT2020_10,    "trc"),
    ENUM("bt2020-12",    AVCOL_TRC_BT2020_12,    "trc"),
    // output trc only
    ENUM("log",          AVCOL_TRC_LOG,          "trc"),
    ENUM("log_sqrt",     AVCOL_TRC_LOG_SQRT,     "trc"),
    ENUM("bt1361",       AVCOL_TRC_BT1361_ECG,   "trc"),
    ENUM("smpte2084",    AVCOL_TRC_SMPTEST2084,  "trc"),
    ENUM("smpte428-1",   AVCOL_TRC_SMPTEST428_1, "trc"),

    { "format",   "Output pixel format",
      OFFSET(user_format), AV_OPT_TYPE_INT,  { .i64 = AV_PIX_FMT_NONE },
      AV_PIX_FMT_NONE, AV_PIX_FMT_GBRAP12LE, FLAGS, "fmt" },
    ENUM("yuv420p",   AV_PIX_FMT_YUV420P,   "fmt"),
    ENUM("yuv420p10", AV_PIX_FMT_YUV420P10, "fmt"),
    ENUM("yuv420p12", AV_PIX_FMT_YUV420P12, "fmt"),
    ENUM("yuv422p",   AV_PIX_FMT_YUV422P,   "fmt"),
    ENUM("yuv422p10", AV_PIX_FMT_YUV422P10, "fmt"),
    ENUM("yuv422p12", AV_PIX_FMT_YUV422P12, "fmt"),
    ENUM("yuv444p",   AV_PIX_FMT_YUV444P,   "fmt"),
    ENUM("yuv444p10", AV_PIX_FMT_YUV444P10, "fmt"),
    ENUM("yuv444p12", AV_PIX_FMT_YUV444P12, "fmt"),

    { "fast",     "Ignore primary chromaticity and gamma correction",
      OFFSET(fast_mode), AV_OPT_TYPE_BOOL,  { .i64 = 0    },
      0, 1, FLAGS },

    { "dither",   "Dithering mode",
      OFFSET(dither), AV_OPT_TYPE_INT, { .i64 = DITHER_NONE },
      DITHER_NONE, DITHER_NB - 1, FLAGS, "dither" },
    ENUM("none", DITHER_NONE, "dither"),
    ENUM("fsb",  DITHER_FSB,  "dither"),

    { "wpadapt", "Whitepoint adaptation method",
      OFFSET(wp_adapt), AV_OPT_TYPE_INT, { .i64 = WP_ADAPT_BRADFORD },
      WP_ADAPT_BRADFORD, NB_WP_ADAPT - 1, FLAGS, "wpadapt" },
    ENUM("bradford", WP_ADAPT_BRADFORD, "wpadapt"),
    ENUM("vonkries", WP_ADAPT_VON_KRIES, "wpadapt"),
    ENUM("identity", WP_ADAPT_IDENTITY, "wpadapt"),

    { "iall",       "Set all input color properties together",
      OFFSET(user_iall),   AV_OPT_TYPE_INT, { .i64 = CS_UNSPECIFIED },
      CS_UNSPECIFIED, CS_NB - 1, FLAGS, "all" },
    { "ispace",     "Input colorspace",
      OFFSET(user_icsp),  AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED },
      AVCOL_PRI_RESERVED0, AVCOL_PRI_NB - 1, FLAGS, "csp" },
    { "irange",     "Input color range",
      OFFSET(user_irng),  AV_OPT_TYPE_INT, { .i64 = AVCOL_RANGE_UNSPECIFIED },
      AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_NB - 1, FLAGS, "rng" },
    { "iprimaries", "Input color primaries",
      OFFSET(user_iprm),  AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_UNSPECIFIED },
      AVCOL_PRI_RESERVED0, AVCOL_PRI_NB - 1, FLAGS, "prm" },
    { "itrc",       "Input transfer characteristics",
      OFFSET(user_itrc),  AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_UNSPECIFIED },
      AVCOL_TRC_RESERVED0, AVCOL_TRC_NB - 1, FLAGS, "trc" },

    { NULL }
};

AVFILTER_DEFINE_CLASS(colorspace);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
};

const AVFilter ff_vf_colorspace = {
    .name            = "colorspace",
    .description     = NULL_IF_CONFIG_SMALL("Convert between colorspaces."),
    .init            = init,
    .uninit          = uninit,
    .priv_size       = sizeof(ColorSpaceContext),
    .priv_class      = &colorspace_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
};
