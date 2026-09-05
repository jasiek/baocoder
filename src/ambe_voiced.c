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
 *   Vocoder_SynthesizeHarmonicSpectrum  0x0001D4C8   here
 *   Vocoder_InterpolateSpectralEnvelope 0x0001D9F0   here
 *   Vocoder_SynthesizeVoiced            0x0001DE10   not yet
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>

#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_fft.h"
#include "ambe_tables.h"
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

/*
 * Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8, whole.
 *
 * This is where the radio's voiced synthesis actually happens, and where it
 * parts company with ambe_synth.c for good: one complex bin per voiced
 * harmonic, amplitude and phase, then a single 256-point inverse transform.
 * ambe_synth.c sums sinusoids in the time domain instead.  The transform is
 * Dsp_FftInverse 0x00025704, which this library already has bit-exact
 * (tests/test_fft_firmware.c, 93 of 93), so this function is the bookkeeping
 * around it rather than a second numerical problem.
 *
 * Harmonic p writes bin p+1: bin 0 is DC and the stock code never touches it.
 * The buffer is 0x100 shorts seen as 128 complex words, plus one more short -
 * `pDest[0x100] = *pDest` wraps the first sample to the end for the overlap-add
 * above, which is why the state block has 0x101 shorts of room there.
 *
 * Three things in it are quirks rather than arithmetic, and all three matter:
 *
 *   The block-float exponent comes from the LARGEST amplitude over the voiced
 *   harmonics only, found in a first pass, so an unvoiced harmonic with a big
 *   amplitude does not cost the voiced ones their headroom.
 *
 *   The phase is folded to its magnitude before the table lookup and the sine
 *   is taken as cos(|phase| + 3/4 turn).  cos is even so the real part does not
 *   care, but the imaginary part does: a negative phase gets sin(|phase|), not
 *   -sin(|phase|).  The stock code drops that sign and this reproduces it.
 *
 *   Two phases saturate rather than fold - 0x80000000 exactly, at two different
 *   points - and land on table entries 0x1FF and 0x17F.  Both are unreachable
 *   and are transcribed anyway.  The value tested is a short shifted left ten,
 *   so it spans [-0x2000000, 0x1FFFC00] and cannot be 0x80000000; the second
 *   tests bits 31..16 of `v * -0x400`, which over the same range cannot be
 *   either.  A sweep says the same thing the weaker way: mutating both branches
 *   away changes nothing on 417 cases.
 *
 * Returns the number of bins written; zero means the caller gets silence, and
 * then the exponent written back is -0x20 rather than the computed one.
 */
short ambe_voiced_harmonic_spectrum(int32_t *fft, int16_t *exp_out,
                                    int16_t step, const uint16_t *phase,
                                    const int16_t *voiced,
                                    const uint16_t *amp, int16_t exp_bias,
                                    int end, int start, int16_t mark)
{
    int16_t *dest = (int16_t *)fft;      /* 0x100 shorts as 128 complex bins */
    int32_t peak = 0;
    int16_t e;
    int shift, p, written = 0;

    if (start >= end) {
        memset(dest, 0, 0x100 * sizeof(int16_t));
        *exp_out = -0x20;
        dest[0x100] = dest[0];
        return 0;
    }

    /* pass one: the largest amplitude among the voiced harmonics */
    for (p = start; p < end; p++)
        if (voiced[p] == mark && peak < (int32_t)((uint32_t)amp[p] << 16))
            peak = (int32_t)((uint32_t)amp[p] << 16);

    shift = peak ? 1 - (int)ambe_lzcount32((uint32_t)peak) : 0;
    memset(dest, 0, 0x100 * sizeof(int16_t));

    for (p = start; p < end; p++) {
        uint32_t acc, ic, is;
        int32_t v, re, im;
        int bin = p + 1;

        if (voiced[p] != mark)
            continue;

        /* the phase this bin is at, as a signed 16-bit count of turns */
        acc = (uint32_t)(((int32_t)((uint32_t)phase[p] << 16) >> 6) + 0x8000
                         + bin * (int32_t)step * 0x400);
        /* a multiply by 64 that keeps the sign bit where it is */
        v = (int16_t)(((acc & 0x80000000u) + ((acc * 0x40) & 0x7FFFFFFFu)) >> 16);

        ic = (uint32_t)ambe_shl32(v, 10);
        if ((int32_t)ic < 0) {
            if (ic == 0x80000000u) {
                ic = 0x80010000u;
            } else {
                ic = (uint32_t)(v * -0x400) & 0xFFFF0000u;
                if (ic == 0x80000000u) {
                    ic = 0x1FF;              /* both saturate rather than fold */
                    is = 0x17F;
                    goto lookup;
                }
                ic = (uint32_t)(-(int32_t)ic);
            }
        }
        ic = (ic & 0x1FFFFFFu) >> 16;
        is = ((ic * 0x10000u + 0x1800000u) & 0x1FFFFFFu) >> 16;
lookup:
        /* cos(|phase|) and cos(|phase| + 3/4 turn), which is sin(|phase|) */
        re = (int32_t)ambe_cos512_q15[ic] * (int16_t)amp[p] * 2;
        im = (int32_t)ambe_cos512_q15[is] * (int16_t)amp[p] * 2;
        /* `shift` is 1 - ff1(peak) over a positive peak, so it lands in
           [-30, 0] and neither direction can reach 32 - lsl_hw's masking is
           belt and braces here, unlike in ambe_basop.c where it decides
           answers.  Mutating it to a plain shift changes nothing on 417 cases. */
        if (-shift < 0) {
            dest[bin * 2]     = (int16_t)((uint32_t)(re >> (shift & 0x3f)) >> 16);
            dest[bin * 2 + 1] = (int16_t)((uint32_t)(im >> (shift & 0x3f)) >> 16);
        } else {
            dest[bin * 2]     = (int16_t)((uint32_t)lsl_hw(re, -shift) >> 16);
            dest[bin * 2 + 1] = (int16_t)((uint32_t)lsl_hw(im, -shift) >> 16);
        }
        written++;
    }

    e = (int16_t)(shift + exp_bias + 0x17);
    *exp_out = e;
    if (written == 0) {
        *exp_out = -0x20;
        dest[0x100] = dest[0];
        return 0;
    }
    *exp_out = ambe_fft_inverse(fft, e, 8, 0);
    dest[0x100] = dest[0];
    return (short)written;
}

/*
 * One windowed, interpolated sample of the synthesised block.
 *
 * `pos` is a Q16 position into `block`, whose integer part indexes it and whose
 * fraction interpolates between that sample and the next - which is why the
 * block carries a wrap sample at [0x100]: at index 0xFF this reads 0x100.
 * `phase` drives the raised cosine (0x8000 - cos) >> 1.
 *
 * The product is taken as bits [46:15] of a 64-bit multiply, `(hi << 17) |
 * (lo >> 15)`, the same reassembly FUN_00018a2c does - the machine has no
 * 64-bit shift, so it rebuilds the field from the register pair.  Written as
 * `(int32_t)(prod >> 15)` it is the same 32 bits and the tests cannot tell them
 * apart; it is kept in the machine's form because that is what the code says.
 */
static int32_t win_sample(const uint16_t *block, int32_t pos, int32_t phase)
{
    uint32_t idx = (uint32_t)(pos >> 16) & 0xff;
    int32_t base = (int32_t)((uint32_t)block[idx] << 16) >> 1;
    int32_t next = (int32_t)((uint32_t)block[idx + 1] << 16) >> 1;
    int32_t frac = pos - ambe_shl32(pos >> 16, 16);
    int32_t lerp = base + (int32_t)(int16_t)(((uint32_t)frac & 0x1ffff) >> 1)
                        * (int32_t)(int16_t)((uint32_t)(next - base) >> 16) * 2;
    int32_t w = (int32_t)(int16_t)((((uint32_t)0x8000
                    - (uint32_t)ambe_cos_q15((int16_t)(((uint32_t)phase & 0x3fffff) >> 6)))
                    & 0x1ffff) >> 1);
    int64_t prod = (int64_t)w * (int64_t)lerp;

    return (int32_t)(((uint32_t)(prod >> 32) << 17) | ((uint32_t)prod >> 15));
}

/* the same without the window, which is what the flat middle runs */
static int32_t flat_sample(const uint16_t *block, int32_t pos)
{
    uint32_t idx = (uint32_t)(pos >> 16) & 0xff;
    int32_t base = (int32_t)((uint32_t)block[idx] << 16) >> 1;
    int32_t next = (int32_t)((uint32_t)block[idx + 1] << 16) >> 1;
    int32_t frac = pos - ambe_shl32(pos >> 16, 16);

    return base + (int32_t)(int16_t)((uint32_t)(next - base) >> 16)
                * (int32_t)(int16_t)(((uint32_t)frac & 0x1ffff) >> 1) * 2;
}

/*
 * Vocoder_InterpolateSpectralEnvelope 0x0001D9F0, whole.
 *
 * The name is the decompiler's and it misleads: nothing here touches the
 * spectral envelope.  This resamples the 256-sample block the harmonic
 * synthesiser produced - `block` is that buffer - at the harmonic's own pitch,
 * windows it, and adds it into the output accumulator.  It runs once per
 * harmonic, and it is where the previous frame's harmonics fade out against
 * this frame's.
 *
 * Three loops: taper in under a rising raised cosine, run flat, taper out under
 * a falling one.  The flat middle is not the window at 1 - it skips the cosine
 * lookup and the 64-bit multiply entirely.
 *
 * `mant` and `exp` are a block float saying where in the accumulator this
 * harmonic starts; the end is that plus 1/pitch, clamped to 0xA6.  A start
 * whose integer part is below -0x10 writes nothing and returns.
 *
 * The counts are 16-bit truncations and that is load-bearing: for a start
 * before the accumulator the rising half's count is `(uint16_t)start + 0x10`,
 * which is 0x10 + start rather than 0x10, so the taper begins part-way through.
 */
void ambe_voiced_interp_envelope(int32_t *env, int32_t mant, int16_t exp,
                                 uint16_t pitch, const uint16_t *block,
                                 int16_t block_exp)
{
    int32_t scaled, pos, step = (int32_t)(int16_t)pitch * 0x20;
    int32_t win_up, win_dn;
    uint32_t first, cursor, nrise, endp1, byteoff;
    int16_t start_i, end_i, limit, count, e = (int16_t)(block_exp - 0x10);
    int32_t *outp;
    int sh, i;

    /* where this harmonic starts, rounded up to the next whole sample */
    sh = (int16_t)(exp - 0xf);
    scaled = sh < 0 ? asr_hw(ambe_shl32(mant, 16), -sh)
                    : lsl_hw(ambe_shl32(mant, 16), sh);
    first = (uint32_t)scaled + 0xffff;
    start_i = (int16_t)(first >> 16);

    /* and where it ends: this position plus one period */
    {
        uint32_t ph = (uint32_t)pitch << 16;
        int16_t pe, e1, e2;
        uint16_t q;
        uint32_t v;

        if (pitch == 0) { pe = -4; sh = 0; }
        else            { sh = nsh((int32_t)ph); pe = (int16_t)(-4 - sh); }
        q = ambe_float_div_exp(0x40000000, 1, nhi((int32_t)ph, sh), pe, &e1);
        q = ambe_float_add(mant, exp, q, e1, &e2);

        sh = (int16_t)(e2 - 0xf);
        v = (uint32_t)(sh < 0 ? asr_hw(ambe_shl32((int32_t)(uint32_t)q, 16), -sh)
                              : lsl_hw(ambe_shl32((int32_t)(uint32_t)q, 16), sh));
        end_i = (int16_t)(v >> 16);
        if (end_i > 0xa6) {
            end_i = 0xa6;
            v = (uint32_t)0xa6 << 16;
        }
        if (end_i < 0x97) {
            if ((int16_t)(end_i + 0x10) < 1)
                return;                    /* wholly before the accumulator */
            limit = (int16_t)(end_i + 0x11);
        } else {
            limit = 0xa7;
        }
        /* the falling window's phase, and the flat section's end */
        win_dn = (asr_hw((int32_t)((v - (uint32_t)ambe_shl32((int32_t)(v >> 16), 16)) * 0x8000), 15)
                  & (int32_t)0xfffffffe) + 0xf0000;
        endp1 = (uint32_t)((uint16_t)end_i + 1);
    }

    {   /* the fractional part of the start drives both accumulators */
        uint32_t frac = ((uint32_t)(ambe_shl32(start_i, 16) - scaled) & 0x1ffff) >> 1;

        win_up = asr_hw(ambe_shl32((int32_t)frac, 16), 15);
        pos    = asr_hw((int32_t)(int16_t)pitch * (int32_t)(int16_t)frac * 2, 11);
    }

    if (start_i < 0) {
        int32_t back = start_i < -0x10 ? 0x100000 : start_i * -0x10000;

        pos += (int32_t)(int16_t)pitch
             * (int32_t)(int16_t)(-(int16_t)(first >> 16)) * 0x20;
        win_up += back;
        if (end_i < 0)
            win_dn += (int32_t)(endp1 << 16);
        cursor  = 0;
        byteoff = 0;
    } else {
        cursor  = first >> 16;
        byteoff = (uint32_t)start_i;
    }
    outp  = env + byteoff;
    nrise = (uint32_t)((((first >> 16) + 0x10) - cursor) & 0xffff);
    count = (int16_t)(((first >> 16) + 0x10) - cursor);

    /* 1. taper in */
    if (count > 0) {
        cursor = (cursor + nrise) & 0xffff;
        for (i = 0; i < count; i++) {
            outp[i] += e < 0 ? asr_hw(win_sample(block, pos, win_up), -e)
                             : lsl_hw(win_sample(block, pos, win_up), e);
            win_up += 0x10000;
            pos    += step;
        }
        /* the stock code advances the position by the UNSIGNED count and the
           loop by the signed one; they are the same 16 bits and the loop only
           runs when that is positive, so advancing in the loop is equivalent */
        outp += nrise;
    }

    /* 2. run flat */
    {
        int16_t nflat = (int16_t)(endp1 - cursor);

        if (nflat > 0) {
            uint32_t n = (endp1 - cursor - 1) & 0xffff;

            cursor += (uint32_t)nflat;
            for (i = 0; i <= (int)n; i++) {
                outp[i] += e < 0 ? asr_hw(flat_sample(block, pos), -e)
                                 : lsl_hw(flat_sample(block, pos), e);
                pos += step;
            }
            outp += n + 1;
        }
    }

    /* 3. taper out */
    {
        int16_t ntail = (int16_t)(limit - (int16_t)cursor);

        for (i = 0; i < ntail; i++) {
            int32_t v = win_sample(block, pos, win_dn);

            pos    += step;
            win_dn -= 0x10000;
            outp[i] += e < 0 ? asr_hw(v, -e) : lsl_hw(v, e);
        }
    }
}
