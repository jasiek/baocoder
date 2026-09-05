/*
 * ambe_blend.c - the two-frame spectral envelope interpolation.
 *
 * Vocoder_ResampleSpectralEnvelope 0x00026A84 and its helper
 * Vocoder_ComputeHarmonicResampleRatio 0x000269B0, transcribed from a run of
 * the firmware under the p-code emulator rather than from the decompiler: a
 * break at 0x00026BB4 - after the mix loop, where the output and both scratch
 * arrays are simultaneously live - gives the arguments and the answer for
 * every call, and tools/fw_oracle/resample_probe.py re-derives every stage
 * from them.  the .fwblend fixtures carry 232 of those triples and
 * tests/test_blend.c checks this file against them.
 *
 * What it is for: Vocoder_ConfigureFrame 0x00016CDC synthesises each 160-sample
 * AMBE frame as two 80-sample halves, and hands the first one THIS - the
 * current and previous frames' envelopes resampled onto a common pitch and
 * averaged - and the second the current frame's parameters alone.  Measured on
 * 115 live calls to Vocoder_SynthesizeFrame 0x00019DB8, whose r0 alternates
 * between this block and PARAMS+0x000 into destinations 0xA0 apart.
 *
 * Two details are easy to get wrong and are load-bearing:
 *
 *   The resampling ratio is f0_out/f0_src, a FREQUENCY ratio, not the
 *   prevL/curL index ratio the envelope predictor in ambe_params.c uses.
 *
 *   The interpolation weight is 0x7FFF, not 0x8000, so even an exact copy
 *   loses one LSB per entry.  That is not a transcription slip; it is what the
 *   firmware does, and the fixture would reject 0x8000.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_basop.h"

/*
 * Math_SDivHalf 0x00018C9C.  The "Half" is literal and load-bearing: the
 * quotient is shifted right by one before the sign is reapplied
 * (`iVar1 = dwDividend / uVar2 >> 1`).  Missing it makes every ratio exactly
 * twice too large, which the fixture catches immediately.
 */
static int sdiv_h(int a, int b)
{
    int sign = ((a < 0) != (b < 0)) ? -1 : 1;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (b == 0) return 0;
    if (a < b) return 0;
    return sign * ((a / b) >> 1);
}

static int clz32(uint32_t v)
{
    int n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

/*
 * Math_DivideNormalized 0x0002692C: num/den in Q16, normalising the divisor
 * first so the 16-bit divide keeps its precision, then undoing the shift.
 * Equal arguments short-circuit to exactly 1.0, which is what makes an
 * unchanged pitch a pure copy.
 */
int32_t ambe_divide_normalized(int16_t num, int16_t den)
{
    uint32_t u3;
    int iv1 = 0, iv4 = num, lz;
    int sv5;

    if (num == den)
        return 0x10000;

    u3 = (uint32_t)((int32_t)den << 16);
    if (u3 == 0) {
        sv5 = -0xB - 4;
        iv4 = sdiv_h(iv4 << 16, 0);
    } else {
        uint32_t u2 = ((int32_t)u3 < 0) ? ~u3 : u3;
        lz = clz32(u2);
        sv5 = lz - 0xC;
        iv1 = (int32_t)(u3 << (lz - 1)) >> 16;
        if (iv4 < iv1) {
            sv5 -= 4;
            iv4 = sdiv_h(iv4 << 16, iv1);
        } else {
            sv5 -= 3;
            iv4 = sdiv_h((iv4 << 16) >> 1, iv1);
        }
    }
    if (sv5 < 0)
        return (int32_t)((uint32_t)iv4 << 16) >> (-sv5 & 31);
    return (int32_t)((uint32_t)iv4 << 16 << (sv5 & 31));
}

/*
 * Vocoder_ComputeHarmonicResampleRatio 0x000269B0.
 *
 * The 60-entry window is a 1-based indexing shim: buf[0] = src[0],
 * buf[k] = src[k-1] for k = 1..56 and three edge-holds above, so index k IS
 * harmonic k and the inner loop needs no bounds test.
 */
void ambe_resample_envelope(int16_t *out, int32_t f0_out, int32_t f0_src,
                            const int16_t *src, int n)
{
    int16_t buf[60];
    int32_t ratio, acc;
    int k;

    buf[0] = src[0];
    for (k = 1; k <= AMBE_MAX_HARMONICS; k++)
        buf[k] = src[k - 1];
    buf[57] = buf[58] = buf[59] = src[AMBE_MAX_HARMONICS - 1];

    ratio = ambe_divide_normalized((int16_t)f0_out, (int16_t)f0_src);
    acc = ratio;
    for (k = 0; k < n; k++) {
        int idx  = (int16_t)((uint32_t)acc >> 16);
        int frac = (int)((((uint32_t)acc - (uint32_t)(idx * 0x10000)) & 0x1FFFFu) >> 1);
        int idx2 = (int16_t)(idx + 1);
        int32_t a = (idx >= 60 || idx < 0) ? buf[59] : buf[idx];
        int32_t b = (idx >= 60 || idx < 0 || idx2 >= 60) ? buf[59] : buf[idx2];
        int64_t v = (int64_t)a * (int16_t)(0x7FFF - frac) * 2
                  + (int64_t)frac * b * 2;
        v >>= 16;
        if (v > 0x7FFF) v = 0x7FFF;
        if (v < -0x8000) v = -0x8000;
        out[k] = (int16_t)v;
        acc += ratio;
    }
}

/*
 * The pitch the interpolated frame is built on.  The masks 0x55555555 and
 * 0xAAAAAAAA are the literals at 0x00026C64 and 0x00026C68: alternate bit
 * planes of the packed voicing word.  `vuv_out` is the word left in the output
 * block by the PREVIOUS call - the firmware reads its own output's voicing
 * before writing it, so a reimplementation has to be handed the same value.
 */
int32_t ambe_blend_pitch(int32_t f0_a, uint32_t vuv_a,
                         int32_t f0_b, uint32_t vuv_b, uint32_t vuv_out)
{
    if ((vuv_b & 0x55555555u) == 0 || (vuv_out & 0xAAAAAAAAu) != 0)
        return f0_a;
    if ((vuv_a & 0x55555555u) == 0)
        return f0_b;
    return ambe_geometric_pitch(f0_a, f0_b);
}

/*
 * Vocoder_ResampleSpectralEnvelope 0x00026A84's interpolating path: pick a
 * pitch, take its harmonic count, resample both envelopes onto that grid, and
 * average with the firmware's sign-safe add-and-halve and its clamps.
 * Returns the harmonic count; `f0_out` receives the pitch.
 */
int ambe_blend_envelope(int16_t *out, int32_t *f0_out,
                        int32_t f0_a, uint32_t vuv_a, const int16_t *env_a,
                        int32_t f0_b, uint32_t vuv_b, const int16_t *env_b,
                        uint32_t vuv_prev_out)
{
    int16_t ra[AMBE_MAX_HARMONICS], rb[AMBE_MAX_HARMONICS];
    int32_t f0 = ambe_blend_pitch(f0_a, vuv_a, f0_b, vuv_b, vuv_prev_out);
    int L = ambe_harmonic_count(f0);
    int k;

    ambe_resample_envelope(ra, f0, f0_a, env_a, L);
    ambe_resample_envelope(rb, f0, f0_b, env_b, L);
    for (k = 0; k < L; k++) {
        /*
         * The firmware sign-extends each operand into a 64-bit pair (asri for
         * the sign words) and adds with paired addc, then takes bits [32:17].
         * Doing this in 32 bits drops the carry out of bit 31 whenever both
         * operands are negative, which shows up as a spurious +0x77FF clamp.
         */
        int64_t sum = ((int64_t)ra[k] << 16) + ((int64_t)rb[k] << 16);
        int32_t v = (int32_t)(int16_t)(uint16_t)((uint64_t)sum >> 17);
        if (v < -0x7800)      out[k] = (int16_t)0x8801;
        else if (v >= 0x7800) out[k] = (int16_t)0x77FF;
        else                  out[k] = (int16_t)v;
    }
    *f0_out = f0;
    return L;
}

/*
 * Math_SqrtScaled 0x000193E0, at the one call site this file needs:
 * sqrt(2 * f0_a * f0_b) with nExp -8 and nQFormat -4, which is the geometric
 * mean of the two pitches back in Q19.  The core is ambe_sqrt - the same
 * odd/even-exponent Horner evaluation over the SRAM table at 0x18001618 - and
 * what this adds is the rebase to a caller-chosen exponent.
 */
int32_t ambe_geometric_pitch(int32_t f0_a, int32_t f0_b)
{
    int32_t mant = (int32_t)((int64_t)f0_a * f0_b * 2);
    short e = -8;
    uint32_t u2 = 0;
    int sh;

    if (mant != 0)
        u2 = (((uint32_t)ambe_sqrt(mant, &e)) << 16);
    else
        e = -8;
    u2 = (u2 + 0x8000u) & 0xFFFF0000u;
    sh = (int)(short)(e - (-4));
    if (sh >= 0)
        return (int32_t)((((uint32_t)u2 << (sh & 0x3F)) + 0x8000u) >> 16) & 0xFFFF;
    return (int32_t)(((uint32_t)(((int32_t)u2 >> (-sh & 0x3F)) + 0x8000)) >> 16) & 0xFFFF;
}
