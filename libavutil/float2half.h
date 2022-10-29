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

#ifndef AVUTIL_FLOAT2HALF_H
#define AVUTIL_FLOAT2HALF_H

#include <stdint.h>
#include "intfloat.h"

#include "config.h"

typedef struct Float2HalfTables {
#if HAVE_FAST_FLOAT16
    uint8_t dummy;
#else
    uint16_t basetable[512];
    uint8_t shifttable[512]; // first bit used to indicate rounding is needed
#endif
} Float2HalfTables;

void ff_init_float2half_tables(Float2HalfTables *t);

static inline uint16_t float2half(uint32_t f, const Float2HalfTables *t)
{
#if HAVE_FAST_FLOAT16
    union {
        _Float16 f;
        uint16_t i;
    } u;
    u.f = av_int2float(f);
    return u.i;
#else
    uint16_t h;
    int i = (f >> 23) & 0x01FF;
    int shift = t->shifttable[i] >> 1;
    uint16_t round = t->shifttable[i] & 1;

    // bit shifting can lose nan, ensures nan stays nan
    uint16_t keep_nan = ((f & 0x7FFFFFFF) > 0x7F800000) << 9;
    // guard bit (most significant discarded bit)
    uint16_t g = ((f | 0x00800000) >> (shift - 1)) & round;
    // sticky bit (all but the most significant discarded bits)
    uint16_t s = (f & ((1 << (shift - 1)) - 1)) != 0;

    h = t->basetable[i] + ((f & 0x007FFFFF) >> shift);

    // round to nearest, ties to even
    h += (g & (s | h));

    // or 0x0200 if nan
    h |= keep_nan;
    return h;
#endif
}

#endif /* AVUTIL_FLOAT2HALF_H */
