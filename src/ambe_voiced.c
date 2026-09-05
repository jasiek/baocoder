/*
 * ambe_voiced.c - Vocoder_SynthesizeVoiced 0x0001DE10 and its helpers.
 *
 * The last stage of the codec with no transcription, and the one the decoder's
 * audio path is waiting on: the radio synthesises voiced harmonics through an
 * inverse FFT, ambe_synth.c sums sinusoids in the time domain instead, and
 * until this exists the bit-exact blend, unvoiced synthesiser and output filter
 * have nothing to hang off.
 *
 * Being written helper-first, innermost outwards, each one swept against the
 * firmware before the next is written.  tests/fixtures/dm32_arc4_1.fwvoiced is
 * the whole-function oracle at the end of it - 617 calls with all six
 * arguments, the accumulator either side and the channel state either side -
 * but a mismatch there is 2378 bytes wide, so it is the last question asked,
 * not the first.
 *
 *   Vocoder_ComputeHarmonicGains        0x0001D71C   here
 *   Vocoder_InterpolateSpectralEnvelope 0x0001D9F0   not yet
 *   Vocoder_SynthesizeHarmonicSpectrum  0x0001D4C8   not yet
 *   Vocoder_SynthesizeVoiced            0x0001DE10   not yet
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_voiced_int.h"

/*
 * The two halves of the normalise-and-take-the-high-half idiom the stock code
 * repeats at every step: count the leading redundant sign bits, shift them out,
 * keep the top 16.  `ff1` on the complement when negative, at 0x0001D7C4 and
 * everywhere like it, is what keeps the sign bit and the bit below it distinct.
 *
 * nsh(0) is 0 rather than 31: the stock code branches around the shift for a
 * zero operand and leaves the register holding zero either way.
 */
static int nsh(int32_t v)
{
    if (v == 0)
        return 0;
    return (int)ambe_lzcount32((uint32_t)(v < 0 ? ~v : v)) - 1;
}

static int16_t nhi(int32_t v, int sh)
{
    return (int16_t)((uint32_t)ambe_shl32(v, sh) >> 16);
}

/* The shift amounts the machine masks to six bits, as in ambe_basop.c.  The
   clamp below scales by an exponent difference that is not bounded by anything,
   so both directions can exceed 31. */
static int32_t asr_hw(int32_t v, int n)
{
    n &= 0x3f;
    if (n >= 32)
        return v < 0 ? -1 : 0;
    return v >> n;
}

static int32_t lsl_hw(int32_t v, int n)
{
    n &= 0x3f;
    return n >= 32 ? 0 : ambe_shl32(v, n);
}

/*
 * Vocoder_ComputeHarmonicGains 0x0001D71C, whole.
 *
 * One (gain, exponent) pair per harmonic, and what it is computing is the
 * distance from each harmonic of the previous frame's pitch to the nearest
 * multiple of this frame's - the term the synthesiser needs to carry a
 * harmonic across a pitch change.  With `p` the previous pitch, `n` the sample
 * count and `d` the pitch difference, each harmonic k forms
 *
 *     t = (k << 16) - phase          the phase error at this harmonic
 *     g = t*n / (sqrt(2*p^2 * 2*n^2 + 2*t*n*d) + 2*p*n)
 *
 * in block float throughout, every intermediate renormalised, and the result
 * clamped to `n` when it would exceed it.  The three products the loop hoists -
 * 2*p^2, 2*n^2 and 2*p*n - are the reason it is written this way round.
 *
 * The gain is returned as a 16-bit mantissa and the exponent alongside it, so
 * `index` is not an index despite the stock parameter name: it is the exponent
 * of the value in `gain`, and the two are read back as a pair.
 */
void ambe_voiced_harmonic_gains(uint16_t *gain, uint16_t *index, int32_t phase,
                                int16_t prev_pitch, int16_t pitch_delta,
                                uint16_t n, int L)
{
    int32_t pp = (int32_t)prev_pitch * prev_pitch * 2;      /* 2*p^2 */
    int32_t nn = (int32_t)(int16_t)n * (int16_t)n * 2;      /* 2*n^2 */
    int32_t pn = (int32_t)prev_pitch * (int16_t)n * 2;      /* 2*p*n */
    int32_t n_hi = (int32_t)((uint32_t)n << 16);
    int k;

    for (k = 1; k <= L; k++) {
        int32_t t = (int32_t)((uint32_t)k << 16) - phase;
        int32_t prod, v, u, scaled;
        int32_t root;
        int16_t mt = 0, m1, m2, mq, sum;
        int e_t, e_p, e1, e2, sh;
        uint16_t g;

        /* t, then t scaled by n - two normalisations, and either can land on
           zero, in which case the mantissa is zero and the exponent stands */
        if (t == 0) {
            e_t = 0x1f;
        } else {
            sh = nsh(t);
            e_t = 0x1f - sh;
            t = (int32_t)(int16_t)n * nhi(t, sh) * 2;
            if (t != 0) {
                sh = nsh(t);
                e_t -= sh;
                mt = nhi(t, sh);
            }
        }

        /* 2*p^2 * 2*n^2, as a product of two normalised mantissas */
        if (pp == 0) {
            e_p = 0x16;
            m1 = 0;
        } else {
            sh = nsh(pp);
            e_p = 0x16 - sh;
            m1 = nhi(pp, sh);
        }
        if (nn == 0) {
            /* 0x0001D982.  n is the only way here and the term is then zero
               too, so this adds nothing - it is transcribed rather than dropped
               because the stock code computes it. */
            e_p += (int16_t)((int16_t)n * (int16_t)n) * -2;
            prod = (int32_t)m1 * nhi(nn, 0);
        } else {
            sh = nsh(nn);
            e_p -= sh;
            prod = (int32_t)m1 * nhi(nn, sh);
        }

        v = prod * 2;
        sh = nsh(v);
        e1 = e_p - sh;
        m1 = nhi(v, sh);

        u = (int32_t)mt * pitch_delta * 2;
        sh = nsh(u);
        m2 = nhi(u, sh);
        e2 = (e_t - 4) - sh;

        {   /* the exponent is read and written through the same short */
            int16_t e = (int16_t)e1;
            sum = (int16_t)ambe_float_add(m1, e1, (uint16_t)m2, e2, &e);
            /* the root is normalised as the 32-bit value it is - the stock
               code runs ff1 over the whole register at 0x0001D8C0, not over a
               16-bit truncation of it */
            root = (int32_t)ambe_float_sqrt(ambe_shl32((int32_t)sum, 16), &e);
            sh = nsh(root);
            e1 = e - sh;
            mq = nhi(root, sh);

            if (pn == 0) {
                e2 = 0xb;
                m2 = 0;
            } else {
                sh = nsh(pn);
                e2 = 0xb - sh;
                m2 = nhi(pn, sh);
            }
            e = (int16_t)e1;
            sum = (int16_t)ambe_float_add(m2, e2, (uint16_t)mq, e1, &e);
            g = ambe_float_div_exp(ambe_shl32((int32_t)mt, 16), e_t,
                                   (uint16_t)sum, e, &e);
            e1 = e;
        }

        /* the clamp: the gain may not exceed the sample count */
        sh = (int16_t)(e1 - 15);
        scaled = (int32_t)((uint32_t)g << 16);
        scaled = sh < 0 ? asr_hw(scaled, -sh) : lsl_hw(scaled, sh);
        /* `addu` on the machine: the scaled gain can overflow into the sign
           bit - a mantissa of 0x401B shifted up by one does - and the
           comparison is made on the wrapped result, which is what puts these
           frames on the clamp.  Done in signed C it is undefined behaviour, and
           the compiler is entitled to assume the overflow cannot happen, which
           costs eight of these 1584 cases. */
        if ((int32_t)((uint32_t)scaled - (uint32_t)n_hi) >= 0) {
            if (n == 0) {
                e1 = 0xf;
                g = 0;
            } else {
                sh = nsh(n_hi);
                e1 = 0xf - sh;
                g = (uint16_t)nhi(n_hi, sh);
            }
        }
        gain[k - 1]  = g;
        index[k - 1] = (uint16_t)(int16_t)e1;
    }
}
