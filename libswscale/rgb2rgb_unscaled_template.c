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

#define SCALE 1.0f

#if SRC_FLOAT
    #if defined(SRC_F16)
        #define SRC_NAME f16
        #define SRC_TYPE uint16_t
        #define SRC_FLOAT_SIZE 2
        #define SRC_BSWAP av_bswap16
        #define INPUT_FLOAT(x) av_int2float(half2float(x, h2f_tbl))
        #define INPUT_UINT(x) lrintf(av_clipf(scale * av_int2float(half2float(x, h2f_tbl)), 0.0f, (float)((1 << dst_bpp) - 1)))
    #else
        #define SRC_NAME f32
        #define SRC_TYPE uint32_t
        #define SRC_FLOAT_SIZE 4
        #define SRC_BSWAP av_bswap32
        #define INPUT_FLOAT(x) av_int2float(x)
        #define INPUT_UINT(x) lrintf(av_clipf(scale * av_int2float(x), 0.0f, (float)((1 << dst_bpp) - 1)))
    #endif
#else
    #define SRC_NAME 16
    #define SRC_TYPE uint16_t
    #define SRC_FLOAT_SIZE 0
    #define SRC_BSWAP av_bswap16

    #undef SCALE
    #define SCALE 1.0f / (float)((1 << src_bpp) - 1)

    #define INPUT_FLOAT(x) (float)x
    // re-quantization using bit replication
    #define INPUT_UINT(x) (x << scale_high | x >> scale_low) >> shift
#endif

#if DST_FLOAT
    #if defined(DST_F16)
        #define DST_NAME f16
        #define DST_TYPE uint16_t
        #define DST_FLOAT_SIZE 2
        #define DST_BSWAP av_bswap16
        #define ALPHA_VALUE 0x3c00
        #if defined(SRC_F16)
            #define OUTPUT(x) x
        #else
            #define OUTPUT(x) float2half(av_float2int(INPUT_FLOAT(x) * scale), f2h_tbl)
        #endif
    #else
        #define DST_NAME f32
        #define DST_TYPE uint32_t
        #define DST_FLOAT_SIZE 4
        #define DST_BSWAP av_bswap32
        #define ALPHA_VALUE 0x3f800000
        #if SRC_FLOAT && !defined(SRC_F16)
            #define OUTPUT(x) x
        #else
            #define OUTPUT(x) av_float2int(INPUT_FLOAT(x) * scale)
        #endif
    #endif
#else
    #define DST_NAME 16
    #define DST_TYPE uint16_t
    #define DST_FLOAT_SIZE 0
    #define DST_BSWAP av_bswap16

    #undef SCALE
    #define SCALE (float)((1 << dst_bpp) - 1)

    #define ALPHA_VALUE 0xffff >> (16 - dst_bpp)
    #define OUTPUT(x) INPUT_UINT(x)
#endif


static inline void RENAME(rgba, _to_planar_gbrap)(SwsContext *c, const uint8_t *src, int srcStride,
                                                  uint8_t *_dst[], int dstStride[], int srcSliceH,
                                                  int src_alpha, int swap, int src_bpp, int dst_bpp, int width)
{
    int x, h, i;
    int dst_alpha = _dst[3] != NULL;
#if (SRC_FLOAT || DST_FLOAT) && (SRC_FLOAT_SIZE != DST_FLOAT_SIZE)
    float scale = SCALE;
#endif

#if defined(SRC_F16) && !defined(DST_F16)
    const Half2FloatTables *h2f_tbl = c->h2f_tables;
#endif

#if defined(DST_F16) && !defined(SRC_F16)
    const Float2HalfTables *f2h_tbl = c->f2h_tables;
#endif

#if !SRC_FLOAT && !DST_FLOAT
    int scale_high = 16 - src_bpp, scale_low = (src_bpp - 8) * 2;
    int shift = 16 - dst_bpp;
#endif

    DST_TYPE alpha_value = swap >= 2 ? DST_BSWAP(ALPHA_VALUE) : ALPHA_VALUE;
    SRC_TYPE component;

    DST_TYPE **dst = (DST_TYPE**)_dst;

    for (h = 0; h < srcSliceH; h++) {
        SRC_TYPE *src_line = (SRC_TYPE *)(src + srcStride * h);
        switch (swap) {
        case 3:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[3][x] = DST_BSWAP(OUTPUT(component));
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    dst[3][x] = alpha_value;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    src_line++;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                }
            }
            break;
        case 2:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[3][x] = DST_BSWAP(OUTPUT(component));
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    dst[3][x] = alpha_value;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                    src_line++;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[1][x] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dst[2][x] = DST_BSWAP(OUTPUT(component));
                }
            }
            break;
        case 1:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[3][x] = OUTPUT(component);
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = OUTPUT(component);
                    dst[3][x] = alpha_value;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = OUTPUT(component);
                    src_line++;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dst[0][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[1][x] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dst[2][x] = OUTPUT(component);
                }
            }
            break;
        default:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[1][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[2][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[3][x] = OUTPUT(component);
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[1][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[2][x] = OUTPUT(component);
                    dst[3][x] = alpha_value;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[1][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[2][x] = OUTPUT(component);
                    src_line++;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dst[0][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[1][x] = OUTPUT(component);
                    component = *src_line++;
                    dst[2][x] = OUTPUT(component);
                }
            }
        }
        for (i = 0; i < 4; i++)
            dst[i] += dstStride[i] / sizeof(DST_TYPE);
    }
}


static inline void RENAME(gbrap, _to_packed_rgba)(SwsContext *c, const uint8_t *_src[], int srcStride[],
                                                  uint8_t *dst, int dstStride, int srcSliceH,
                                                  int alpha, int swap, int src_bpp, int dst_bpp, int width)
{
    int x, h, i;
    int src_alpha = _src[3] != NULL;

#if (SRC_FLOAT || DST_FLOAT) && (SRC_FLOAT_SIZE != DST_FLOAT_SIZE)
    float scale = SCALE;
#endif

#if defined(SRC_F16) && !defined(DST_F16)
    Half2FloatTables *h2f_tbl = c->h2f_tables;
#endif

#if defined(DST_F16) && !defined(SRC_F16)
    Float2HalfTables *f2h_tbl = c->f2h_tables;
#endif

#if !SRC_FLOAT && !DST_FLOAT
    int scale_high = 16 - src_bpp, scale_low = (src_bpp - 8) * 2;
    int shift = 16 - dst_bpp;
#endif

    DST_TYPE alpha_value = swap >= 2 ? DST_BSWAP(ALPHA_VALUE) : ALPHA_VALUE;
    SRC_TYPE component;

    const SRC_TYPE **src = (const SRC_TYPE **)(_src);

    for (h = 0; h < srcSliceH; h++) {
        DST_TYPE *dest = (DST_TYPE *)(dst + dstStride * h);

        switch(swap) {
        case 3:
            if (alpha && !src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    *dest++ = alpha_value;
                }
            } else if (alpha && src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[3][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = DST_BSWAP(OUTPUT(component));
                }
            }
            break;
        case 2:
            if (alpha && !src_alpha) {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[1][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[2][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    *dest++ = alpha_value;
                }
            } else if (alpha && src_alpha) {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[1][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[2][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[3][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[1][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                    component = src[2][x];
                    *dest++ = DST_BSWAP(OUTPUT(component));
                }
            }
            break;
        case 1:
            if (alpha && !src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = OUTPUT(component);
                    *dest++ = alpha_value;
                }
            } else if (alpha && src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[3][x]);
                    *dest++ = OUTPUT(component);
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[0][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[1][x]);
                    *dest++ = OUTPUT(component);
                    component = SRC_BSWAP(src[2][x]);
                    *dest++ = OUTPUT(component);
                }
            }
            break;
        default:
            if (alpha && !src_alpha) {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = OUTPUT(component);
                    component = src[1][x];
                    *dest++ = OUTPUT(component);
                    component = src[2][x];
                    *dest++ = OUTPUT(component);
                    *dest++ = alpha_value;
                }
            } else if (alpha && src_alpha) {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = OUTPUT(component);
                    component = src[1][x];
                    *dest++ = OUTPUT(component);
                    component = src[2][x];
                    *dest++ = OUTPUT(component);
                    component = src[3][x];
                    *dest++ = OUTPUT(component);
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = src[0][x];
                    *dest++ = OUTPUT(component);
                    component = src[1][x];
                    *dest++ = OUTPUT(component);
                    component = src[2][x];
                    *dest++ = OUTPUT(component);
                }
            }
        }
        for (i = 0; i < 3 + src_alpha; i++)
            src[i] += srcStride[i] / sizeof(SRC_TYPE);
    }
}

static inline void RENAME(rgba, _to_packed_rgba)(SwsContext *c, const uint8_t *src, int srcStride,
                                                 uint8_t *dst, int dstStride, int srcSliceH,
                                                 int src_alpha, int dst_alpha, int swap, int order, int src_bpp, int dst_bpp, int width)
{
    int x, h;
#if (SRC_FLOAT || DST_FLOAT) && (SRC_FLOAT_SIZE != DST_FLOAT_SIZE)
    float scale = SCALE;
#endif

#if defined(SRC_F16) && !defined(DST_F16)
    const Half2FloatTables *h2f_tbl = c->h2f_tables;
#endif

#if defined(DST_F16) && !defined(SRC_F16)
    const Float2HalfTables *f2h_tbl = c->f2h_tables;
#endif

#if !SRC_FLOAT && !DST_FLOAT
    int scale_high = 16 - src_bpp, scale_low = (src_bpp - 8) * 2;
    int shift = 16 - dst_bpp;
#endif

    DST_TYPE alpha_value = swap >= 2 ? DST_BSWAP(ALPHA_VALUE) : ALPHA_VALUE;
    SRC_TYPE component;

    int rdx = 0;
    int gdx = 1;
    int bdx = 2;
    int adx = 3;

    if (order == 1) {
        rdx = 2;
        gdx = 1;
        bdx = 0;
        adx = 3;
    }

    for (h = 0; h < srcSliceH; h++) {
        SRC_TYPE *src_line = (SRC_TYPE *)(src + srcStride * h);
        DST_TYPE *dest = (DST_TYPE *)(dst + dstStride * h);

        switch (swap) {
        case 3:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[adx] = DST_BSWAP(OUTPUT(component));
                    dest += 4;
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    dest[adx] = alpha_value;
                    dest += 4;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    src_line++;
                    dest += 3;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    dest += 3;
                }
            }
            break;
        case 2:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[adx] = DST_BSWAP(OUTPUT(component));
                    dest += 4;
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    dest[adx] = alpha_value;
                    dest += 4;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    dest += 3;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[gdx] = DST_BSWAP(OUTPUT(component));
                    component = *src_line++;
                    dest[bdx] = DST_BSWAP(OUTPUT(component));
                    dest += 3;
                }
            }
            break;
        case 1:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[adx] = OUTPUT(component);
                    dest += 4;
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = OUTPUT(component);
                    dest[adx] = alpha_value;
                    dest += 4;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = OUTPUT(component);
                    src_line++;
                    dest += 3;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(*src_line++);
                    dest[rdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[gdx] = OUTPUT(component);
                    component = SRC_BSWAP(*src_line++);
                    dest[bdx] = OUTPUT(component);
                    dest += 3;
                }
            }
            break;
        default:
            if (src_alpha && dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[gdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[bdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[adx] = OUTPUT(component);
                    dest += 4;
                }
            } else if (dst_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[gdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[bdx] = OUTPUT(component);
                    dest[adx] = alpha_value;
                    dest += 4;
                }
            } else if (src_alpha) {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[gdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[bdx] = OUTPUT(component);
                    src_line++;
                    dest += 3;
                }
            } else {
                for (x = 0; x < width; x++) {
                    component = *src_line++;
                    dest[rdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[gdx] = OUTPUT(component);
                    component = *src_line++;
                    dest[bdx] = OUTPUT(component);
                    dest += 3;
                }
            }
        }
    }
}

static inline void RENAME(gbrap, _to_planar_gbrap)(SwsContext *c, const uint8_t *_src[], int srcStride[],
                                                   uint8_t *_dst[], int dstStride[], int srcSliceH,
                                                   int swap, int src_bpp, int dst_bpp, int width)
{
    int x, h, i;
    int src_alpha = _src[3] != NULL;
    int dst_alpha = _dst[3] != NULL;
    int channels = dst_alpha && src_alpha ? 4 : 3;

#if (SRC_FLOAT || DST_FLOAT) && (SRC_FLOAT_SIZE != DST_FLOAT_SIZE)
    float scale = SCALE;
#endif

#if defined(SRC_F16) && !defined(DST_F16)
    Half2FloatTables *h2f_tbl = c->h2f_tables;
#endif

#if defined(DST_F16) && !defined(SRC_F16)
    Float2HalfTables *f2h_tbl = c->f2h_tables;
#endif

#if !SRC_FLOAT && !DST_FLOAT
    int scale_high = 16 - src_bpp, scale_low = (src_bpp - 8) * 2;
    int shift = 16 - dst_bpp;
#endif

    DST_TYPE alpha_value = swap >= 2 ? DST_BSWAP(ALPHA_VALUE) : ALPHA_VALUE;
    SRC_TYPE component;

    const SRC_TYPE **src = (const SRC_TYPE **)(_src);
    DST_TYPE **dst = (DST_TYPE **)(_dst);

    for (i = 0; i < channels; i++) {
        if (!_dst[i])
            continue;

        for (h = 0; h < srcSliceH; h++) {
            switch(swap) {
            case 3:
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[i][x]);
                    dst[i][x] = DST_BSWAP(OUTPUT(component));
                }
                break;
            case 2:
                for (x = 0; x < width; x++) {
                    component = src[i][x];
                    dst[i][x] = DST_BSWAP(OUTPUT(component));
                }
                break;
            case 1:
                for (x = 0; x < width; x++) {
                    component = SRC_BSWAP(src[i][x]);
                    dst[i][x] = OUTPUT(component);
                }
                break;
            default:
                for (x = 0; x < width; x++) {
                    component = src[i][x];
                    dst[i][x] = OUTPUT(component);
                }
            }
            src[i] += srcStride[i] / sizeof(SRC_TYPE);
            dst[i] += dstStride[i] / sizeof(DST_TYPE);
        }
    }

    if (dst_alpha && !src_alpha) {
        for (h = 0; h < srcSliceH; h++) {
            for (x = 0; x < width; x++) {
                dst[3][x] = alpha_value;
            }
            dst[3] += dstStride[3] / sizeof(DST_TYPE);
        }
    }
}

#undef SRC_FLOAT
#undef DST_FLOAT
#undef SCALE

#undef SRC_TYPE
#undef SRC_BSWAP
#undef INPUT_FLOAT
#undef INPUT_UINT

#undef DST_TYPE
#undef DST_BSWAP
#undef ALPHA_VALUE
#undef OUTPUT

#undef SRC_NAME
#undef DST_NAME

#undef SRC_FLOAT_SIZE
#undef DST_FLOAT_SIZE

#undef RENAME

#if defined(SRC_F16)
#undef SRC_F16
#endif

#if defined(DST_F16)
#undef DST_F16
#endif