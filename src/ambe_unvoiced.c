/*
 * ambe_unvoiced.c - Vocoder_SynthesizeUnvoiced 0x0001AFE0, whole.
 *
 * The noise half of the excitation.  The function builds a 2n-sample windowed
 * noise segment, transforms it, scales each harmonic's band of bins to the
 * frame's amplitude for that harmonic - or empties the band, if the voiced
 * synthesiser owns it - transforms back, and overlap-adds the segment into the
 * accumulator under the same square-root window.  It is bit-exact against 103
 * firmware calls at every one of those stages: tests/test_unvoiced.c.
 *
 * The generator is a 16-bit LCG, x = 173x + 13849 (0xAD and 0x3619), which is
 * the same one mbelib uses - so the two decoders' noise is the same sequence
 * given the same seed, which is not true of anything that calls rand().
 *
 * The state is pChannelState+0x648:
 *
 *   [0]           the LCG's carry - seeded from here and written back
 *   [1..0x54]     the noise history, 84 entries
 *   [0x55..0xa8]  the previous segment's overlap-add tail
 *   [0xa9]        that tail's block-float exponent
 *
 * Per frame the history is shifted down by nCount and the vacated tail is
 * refilled from the generator.  With nCount 0x50, which is what this radio
 * always uses, that leaves four entries to carry over and eighty to generate -
 * and makes the whole Math_FloatDivExponent branch that resamples the window
 * for other lengths unreachable, so it is not transcribed.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"

/*
 * The unvoiced synthesis window, 81 entries at SRAM 0x18003584 (file 0x66c44).
 * It is 32767*sqrt(k/80) to within one LSB, but the exact int16s are what the
 * firmware multiplies by, so those are what ship.
 */
const int16_t ambe_uv_window_q15[81] = {
         0,   3664,   5181,   6345,   7327,   8192,   8974,   9693,  10362,  10991,
     11585,  12151,  12691,  13209,  13708,  14189,  14654,  15105,  15543,  15969,
     16384,  16789,  17184,  17570,  17948,  18318,  18681,  19037,  19386,  19729,
     20066,  20398,  20724,  21046,  21362,  21674,  21981,  22285,  22584,  22879,
     23170,  23458,  23743,  24024,  24301,  24576,  24848,  25116,  25382,  25645,
     25905,  26163,  26418,  26671,  26922,  27170,  27416,  27659,  27901,  28140,
     28378,  28613,  28847,  29079,  29309,  29537,  29763,  29988,  30211,  30432,
     30652,  30870,  31086,  31302,  31515,  31727,  31938,  32148,  32356,  32563,
     32767,
};

#define LCG_MUL  0xAD
#define LCG_ADD  0x3619
#define HIST_END 0x54
#define TAIL     0x55   /* the previous segment's overlap-add tail */
#define TAIL_EXP 0xa9   /* and its block-float exponent */

void ambe_unvoiced_reset(ambe_unvoiced_state *u)
{
    int i;
    for (i = 0; i < AMBE_UNVOICED_STATE; i++)
        u->s[i] = 0;
}

void ambe_unvoiced_advance_noise(ambe_unvoiced_state *u, int n)
{
    int carry = HIST_END - n;
    int i;
    int16_t x;

    /* what survives of the previous frame's history */
    for (i = 0; i < carry; i++)
        u->s[1 + i] = u->s[n + i + 1];

    /* and the rest generated afresh, continuing the same sequence */
    x = u->s[0];
    for (i = carry; i < HIST_END; i++) {
        x = (int16_t)(x * LCG_MUL + LCG_ADD);
        u->s[i + 1] = x;
    }
    u->s[0] = x;
}

/*
 * The 2n-sample windowed noise segment, zero-padded to the transform length.
 *
 * The rising half is windowed against the history BEFORE the generator is
 * advanced and the falling half against it after, with the window index
 * reversed - so one square-root window is used in both directions and the two
 * halves overlap-add cleanly across the frame boundary.  The order matters:
 * calling this after advancing the noise gives a buffer that is wrong in its
 * first half only, which is easy to miss.
 *
 * `buf` receives `1 << size_bits` shorts; the tail beyond 2n is zeroed.
 */
void ambe_unvoiced_window(int16_t *buf, ambe_unvoiced_state *u, int n,
                          int size_bits)
{
    int len = 1 << size_bits;
    int i;

    for (i = 0; i < n; i++)
        buf[i] = (int16_t)(((int32_t)ambe_uv_window_q15[i] * u->s[i + 1]) >> 15);

    ambe_unvoiced_advance_noise(u, n);

    for (i = 0; i < n; i++)
        buf[n + i] = (int16_t)(((int32_t)ambe_uv_window_q15[n - i] * u->s[i + 1]) >> 15);

    for (i = 2 * n; i < len; i++)
        buf[i] = 0;
}

/* ------------------------------------------------------------------------
 * The spectral shaping between the two transforms.
 *
 * The rest of Vocoder_SynthesizeUnvoiced 0x0001AFE0: the windowed noise is
 * transformed, each harmonic's band of bins is scaled so the band carries the
 * frame's amplitude for that harmonic (or zeroed, if the band is voiced - the
 * voiced synthesiser will fill it), the result is transformed back, and the
 * segment is overlap-added into the accumulator under the same square-root
 * window.
 *
 * Everything here is block floating point across three exponents that have to
 * stay in step: the forward transform's, the amplitude array's
 * (pFrameParams[0x42], written by Vocoder_NormalizeSpectralBlock just before
 * the call), and the per-band one the energy normalisation produces.  The
 * inverse transform is handed their sum, so an error in any of them is a
 * clean power of two on the output and nothing else.
 * ---------------------------------------------------------------------- */
#include "ambe_basop.h"
#include "ambe_fft.h"
#include "ambe_unvoiced_int.h"

/*
 * FUN_0001abdc: twice the dot product of two int16 arrays, renormalised.
 *
 * The accumulation is 64-bit, the doubling is the Q15 fractional convention,
 * and the result comes back as a 32-bit mantissa with the shift that produced
 * it: value = mant * 2^exp.  Its one caller passes the same array twice, so
 * in practice this is a band's energy - but it is transcribed as the dot
 * product it is.
 *
 * The normalisation is the stock code's one's-complement trick: a negative
 * accumulator is complemented rather than negated before the leading-zero
 * count, which is off by one LSB and is what the radio does.
 */
static int32_t uv_dot_norm(int16_t *exp_out, const int16_t *a, const int16_t *b,
                           int n)
{
    int64_t acc = 0;
    uint32_t lo, hi, mlo, mhi;
    int i, lzc, shift;

    for (i = 0; i < n; i++)
        acc += (int64_t)a[i] * (int64_t)b[i];
    lo = (uint32_t)acc;
    hi = (uint32_t)((uint64_t)acc >> 32);
    hi = (hi << 1) | (lo >> 31);            /* the 64-bit doubling, as the */
    lo <<= 1;                               /* stock code's add-with-carry */

    shift = 0;
    if (hi != 0 || lo != 0) {
        mhi = hi; mlo = lo;
        if ((int32_t)hi < 0) { mhi = ~hi; mlo = ~lo; }
        lzc = (int)ambe_lzcount32(mlo);
        if (mhi != 0)
            lzc = (int)ambe_lzcount32(mhi) - 32;
        shift = lzc - 1;
    }
    *exp_out = (int16_t)(-shift);
    if (shift >= 0)
        return (int32_t)(lo << (shift & 31));
    return (int32_t)(uint32_t)((((uint64_t)hi << 32) | lo) >> (-shift));
}

/*
 * Vocoder_BuildFrameResetPattern 0x00022CD0, reached through
 * Vocoder_ResetFrameBuffer 0x00019D0C: the frame's 32-bit voicing word
 * expanded to one flag per harmonic.
 *
 * The word is sixteen 2-bit crumbs, least significant first, one per 250 Hz
 * band - `f0_q19 * 4 >> 16` is the harmonic's band index, since f0 is a
 * fraction of the sample rate at Q19.  A harmonic's flag is the OR of its own
 * band's crumb and the next one up, so a band boundary that falls inside a
 * harmonic counts as voiced on both sides; a resulting 3 is folded to 1.
 *
 * Note this is a different mapping from the eight 500 Hz bands
 * ambe_params.c's decoder uses for ambe_parms.Vl, and it is not the same
 * quantity: the value here is the crumb, not its low bit, and the unvoiced
 * synthesiser only asks whether it is zero.
 */
void ambe_unvoiced_voicing(uint16_t *out, uint32_t vuv, int16_t f0_q19, int L)
{
    uint16_t d[16];
    int k, l, step, acc;

    for (k = 0; k < 16; k++) { d[k] = (uint16_t)(vuv & 3); vuv >>= 2; }
    step = ((int)f0_q19 << 6) >> 4;
    acc  = step;
    for (l = 0; l < L; l++) {
        int16_t i = (int16_t)((uint32_t)acc >> 16);
        uint16_t a, b;
        if (i < 0x10) {
            a = d[i];
            b = ((int16_t)(i + 1) != 0x10) ? d[(int16_t)(i + 1)] : d[15];
        } else {
            a = d[15];
            b = d[15];
        }
        b |= a;
        if ((int16_t)b > 2)
            b = 1;
        out[l] = b;
        acc += step;
    }
    for (; l < AMBE_UNVOICED_BANDS; l++)
        out[l] = 0;
}

/* 2*a*b saturated into 32 bits, which is what the overlap-add multiplies by. */
static int32_t uv_mul2_sat(int16_t a, int16_t b)
{
    int64_t v = 2 * (int64_t)a * (int64_t)b;

    if (v >= 0x80000000LL)  return (int32_t)0x7fffffff;
    if (v < -0x80000000LL)  return (int32_t)0x80000000;
    return (int32_t)v;
}

/* v * 2^e, saturating on overflow; a negative e is an arithmetic right shift. */
static int32_t uv_shift_sat(int32_t v, int e)
{
    uint32_t m;

    if (v == 0)
        return 0;
    if (e < 0)
        return v >> ((-e) & 0x3f);
    m = (uint32_t)v;
    if (v < 0)
        m = ~m;
    if ((int)(int16_t)((int16_t)ambe_lzcount32(m) - 1) < e)
        return (v < 0) ? (int32_t)0x80000000 : (int32_t)0x7fffffff;
    return ambe_shl32(v, e & 0x3f);
}

/*
 * The shaping itself, between the two transforms: `fft` holds the forward
 * transform of the windowed noise as FLEN/2 complex bins and is rewritten in
 * place.  Returns the scale exponent the inverse transform is to be given.
 *
 * Separate from ambe_unvoiced_synth only so that it can be checked on its own
 * against the firmware's own intermediate - the exponent bookkeeping here is
 * three deep and an error in it is invisible in an end-to-end waveform
 * comparison until it is large enough to clip.
 */
short ambe_unvoiced_shape(int32_t *fft, int n, int cls, int L, int16_t f0_q19,
                          const int16_t *amps, int amp_exp,
                          const uint16_t *voiced, int16_t pitch, short fexp)
{
    enum { FLEN = 256, HALF = 128, TOPCUT = 0x79 };
    int16_t *buf = (int16_t *)fft;   /* the same store, seen as 128 bins */
    int i;
    {
        int mexp = 0, band, step, first, prev_b, dst;
        int64_t q;
        short gmant, gexp;
        unsigned int tail_exp;

        /* the loudest unvoiced harmonic, as a normalising exponent */
        if (L >= 1) {
            int mx = 0;
            for (band = 0; band < L; band++) {
                int a = (int)((uint32_t)(uint16_t)amps[band] << 16);
                if (voiced[band] == 0 && mx < a)
                    mx = a;
            }
            if (mx != 0)
                mexp = -(int)(ambe_lzcount32((uint32_t)mx) - 1);
        }

        /*
         * The gain common to every band.  0x86B6 is 34486, and the two
         * branches differ in more than the L: the normal path takes
         * sqrt(L*n) and divides each band by its own RMS below, so the
         * forward transform's exponent cancels and is dropped here; the
         * class-2 path applies a flat gain, so it has to carry that exponent
         * instead.  Which branch runs is `*pFrameParams == 2` - the stock
         * code computes it with `cmpnei r3,2; mvcv r12` at 0x0001B1E0, which
         * is worth naming because Ghidra's C-SKY module got that instruction
         * wrong; see docs/patches/csky-mvcv.patch.
         */
        if (cls != 2) {
            gexp  = 0x11;
            gmant = (short)ambe_sqrt((short)(L * n) * 0x86b6, &gexp);
        } else {
            short e = 0x11;
            gmant = (short)ambe_sqrt((short)L * 0x86b6, &e);
            gmant = (short)(((int)gmant * 0x57fa & 0x7fffffff) >> 15);
            gexp  = (short)((int)e + 1 + (int)(uint16_t)fexp);
        }
        tail_exp = (unsigned int)(uint16_t)(short)(gexp + mexp + 2);

        /*
         * Band boundaries.  The accumulator is Q16 bins and steps by
         * f0*FLEN/2048, so harmonic m owns the bins from (m-0.5) to (m+0.5)
         * fundamentals, rounded - the classic MBE partition, done without a
         * divide.
         */
        step   = (FLEN * (int)f0_q19 * 2) >> 4;
        first  = step + 0x10000;
        prev_b = (short)(first >> 17);
        q      = first >> 1;
        for (i = 0; i < 2 * prev_b; i++)
            buf[i] = 0;
        dst = 2 * prev_b;

        for (band = 0; band < L; band++) {
            int b, width, k;

            q += step;
            b = (int)(q >> 16);
            if (HALF < (short)b)
                b = HALF;
            width = b - prev_b;

            if (voiced[band] == 0) {
                short bexp, sc, e2 = 0;

                if (cls != 2) {
                    /*
                     * sqrt(width / energy): the band is flattened to unit RMS
                     * before the amplitude is applied, so the noise's own
                     * spectral tilt - and the forward transform's scaling -
                     * drop out.  The divide is set up so the quotient cannot
                     * overflow: the numerator is normalised and halved again
                     * if it would not stay below the denominator.
                     */
                    int16_t eexp = 0;
                    int32_t eng = uv_dot_norm(&eexp, buf + dst, buf + dst,
                                              2 * width);
                    short bmant = 0;

                    if ((eng >> 16) < 1) {
                        bmant = 0;
                        e2 = 0;
                    } else {
                        uint32_t w = (uint32_t)(width << 16);
                        int qq;

                        if (w == 0) {
                            e2 = 0xf;
                        } else {
                            short s = (short)(ambe_lzcount32(w) - 1);
                            w <<= (s & 0x3f);
                            s  = (short)(-s);
                            e2 = (short)(s + 0xf);
                            if ((eng >> 16) <= ((int32_t)w >> 16)) {
                                e2 = (short)(s + 0x10);
                                w  = (uint32_t)((int32_t)w >> 1);
                            }
                        }
                        qq = ambe_sdiv_half((int32_t)w, (short)(eng >> 16));
                        e2 = (short)(e2 - eexp);
                        bmant = (short)ambe_sqrt((int32_t)((uint32_t)qq << 16),
                                                 &e2);
                    }
                    bexp = (short)(e2 + gexp);
                    sc   = (short)(((int)gmant * (int)bmant * 2) >> 16);
                } else {
                    bexp = gexp;
                    sc   = gmant;
                }

                /* the harmonic's amplitude, against the loudest one's scale */
                {
                    int t = (int)sc * (int)amps[band] * 2;
                    short sh = (short)(-mexp);
                    sc = (short)((unsigned int)(sh < 0 ? (t >> ((-sh) & 0x3f))
                                                       : ambe_shl32(t, sh & 0x3f))
                                 >> 16);
                }
                {
                    short sh = (short)((uint16_t)(bexp + mexp)
                                       - (uint16_t)tail_exp + 1);
                    for (k = 0; k < 2 * width; k++) {
                        int t = (int)sc * (int)buf[dst + k];
                        if (sh < 0)
                            t = t >> ((-sh) & 0x3f);
                        else if (sh > 0)
                            t = ambe_shl32(t, sh & 0x3f);
                        buf[dst + k] = (short)((unsigned int)t >> 16);
                    }
                }
            } else {
                for (k = 0; k < 2 * width; k++)
                    buf[dst + k] = 0;
            }
            dst += 2 * width;
            prev_b = b;
        }

        /*
         * Two band-edge cuts the loop cannot make: everything above the last
         * harmonic - or above bin 121, 3781 Hz, whichever is lower - and
         * everything below the pitch, which is the same Q19 fraction of the
         * sample rate that f0 is.
         */
        {
            int t = HALF - TOPCUT, v = (short)(HALF - prev_b);

            if (t <= v)
                t = v;
            for (i = 2 * (HALF - t); i < 2 * HALF; i++)
                buf[i] = 0;
            for (i = 0; i < (((int)pitch * FLEN * 2) >> 20) * 2; i++)
                buf[i] = 0;
        }
        return (short)(amp_exp + (short)tail_exp);
    }
}

void ambe_unvoiced_synth(int32_t *acc, int n, ambe_unvoiced_state *u,
                         int cls, int L, int16_t f0_q19, const int16_t *amps,
                         int amp_exp, const uint16_t *voiced, int16_t pitch)
{
    enum { SIZE_BITS = 8, FLEN = 256 };
    int32_t fft[FLEN / 2];
    int16_t *buf = (int16_t *)fft;
    short seg_exp;
    int i;

    if (cls == 3) {
        /*
         * A silence frame produces no noise at all: the history is cleared
         * and the segment is zero, but the overlap-add still runs, so the
         * previous segment's tail is not lost.
         */
        for (i = 1; i <= HIST_END; i++)
            u->s[i] = 0;
        for (i = 0; i < FLEN; i++)
            buf[i] = 0;
        seg_exp = 0;
    } else {
        short fexp;

        ambe_unvoiced_window(buf, u, n, SIZE_BITS);
        fexp = ambe_fft_forward(fft, 0, SIZE_BITS, 0);
        seg_exp = ambe_fft_inverse(fft,
                                   ambe_unvoiced_shape(fft, n, cls, L, f0_q19,
                                                       amps, amp_exp, voiced,
                                                       pitch, fexp),
                                   SIZE_BITS, 0);
    }

    /*
     * Overlap-add.  The previous segment's tail runs out under the falling
     * half of the window and this one's head under the rising half, and since
     * the window is 32767*sqrt(k/n) the two squares sum to one - the noise is
     * windowed once before the transform and once here, and reconstructs.
     * Each side is brought to the accumulator's fixed scale by its own
     * block-float exponent, saturating rather than wrapping.
     */
    {
        int prev_e = (int)(short)(u->s[TAIL_EXP] - 2);
        int cur_e  = (int)(short)(seg_exp - 2);

        for (i = 0; i < n; i++) {
            int32_t a = uv_shift_sat(uv_mul2_sat(u->s[TAIL + i],
                                                 ambe_uv_window_q15[n - i]),
                                     prev_e);
            int32_t b = uv_shift_sat(uv_mul2_sat(buf[i],
                                                 ambe_uv_window_q15[i]),
                                     cur_e);
            acc[i] += a + b;
        }
        for (i = 0; i < n; i++)
            u->s[TAIL + i] = buf[n + i];
        for (i = n; i < HIST_END; i++)
            u->s[TAIL + i] = 0;
        u->s[TAIL_EXP] = seg_exp;
    }
}
