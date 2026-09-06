/*
 * ambe_frame.c - Vocoder_SynthesizeFrame 0x00019DB8 and the helpers it drives.
 *
 * The layer between the decoded parameters and the two synthesisers.  It is
 * what stands between src/ambe_voiced.c being exact and the decode path being
 * able to use it: the parameter block the synthesisers consume is not the block
 * the decoder produces, it is one this function rewrites on the way down -
 * normalisation, the excitation match, the voicing flags, the gain ramp - and
 * then hands to Vocoder_SynthesizeUnvoiced, Vocoder_SynthesizeVoiced, the
 * output filter and the final scaling in that order.
 *
 * Being written leaves-first like the voiced synthesiser was:
 *
 *   Math_PopCountBits                  0x000189F4   here
 *   Math_SqrtScaled                    0x000193E0   here
 *   Vocoder_CopyFrameParamsWithReset   0x00019CBC   here
 *   Vocoder_ResetFrameBuffer           0x00019D38   here
 *   Vocoder_SmoothPitchState           0x00022D7C   here
 *   Vocoder_NormalizeSpectralBlock     0x00022C18   here
 *   Vocoder_NormalizeSpectralBlock     0x00022C18   not yet
 *   Dsp_HilbertTransform               0x00029D1C   not yet
 *   Vocoder_UpdatePitchHistoryBuffer   0x0001A9E8   not yet
 *   Vocoder_SynthesizeFrame            0x00019DB8   not yet
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>

#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_tables.h"
#include "ambe_frame_int.h"

/*
 * Math_SqrtScaled 0x000193E0.
 *
 * Math_Sqrt's polynomial over the same four coefficients at SRAM 0x18001618,
 * with the answer shifted into a caller-chosen Q format instead of being left
 * as a (mantissa, exponent) pair.  It is a separate entry point in the stock
 * code rather than a wrapper, and the difference is visible: this one rounds by
 * 0x8000 into the high half at every step INCLUDING the final scaling, where
 * Math_Sqrt stops at the pair.
 *
 * A zero mantissa returns the exponent scaled with a zero value rather than
 * short-circuiting, which is why the tail runs unconditionally.
 */
uint32_t ambe_sqrt_scaled(int32_t mant, uint32_t exp, int16_t q)
{
    int16_t e = (int16_t)exp;
    uint32_t v = 0, acc;
    int sh;

    if (mant != 0) {
        uint32_t d = (exp & 0xFFFF) - ((ambe_lzcount32((uint32_t)mant) - 1u) & 0xFFFF);
        int16_t m;

        sh = (int16_t)(ambe_lzcount32((uint32_t)mant) - 1u);
        m  = (int16_t)((uint32_t)ambe_lsl_hw(mant, sh) >> 16);

        acc = (uint32_t)((int32_t)((uint32_t)(uint16_t)ambe_sqrt_coeff_q15[2] << 16)
                         + 0x8000
                         + (int32_t)m * (int32_t)(int16_t)(
                             (uint32_t)((int32_t)((uint32_t)(uint16_t)ambe_sqrt_coeff_q15[1] << 16)
                                        + 0x8000
                                        + (int32_t)m * (int32_t)ambe_sqrt_coeff_q15[0] * 2) >> 16)
                           * 2);
        v = acc & 0xFFFF0000u;
        if ((d & 1) != 0)                        /* the odd-exponent correction */
            v = (uint32_t)((int32_t)ambe_sqrt_coeff_q15[3]
                           * (int32_t)(int16_t)(acc >> 16) * 2);
        v = (v + 0x8000) & 0xFFFF0000u;
        e = (int16_t)((((int32_t)(int16_t)d + 1) & 0x1ffff) >> 1);
    }
    sh = (int16_t)(e - q);
    return sh >= 0 ? (uint32_t)(ambe_lsl_hw((int32_t)v, sh) + 0x8000) >> 16
                   : (uint32_t)(ambe_asr_hw((int32_t)v, -sh) + 0x8000) >> 16;
}

/*
 * Vocoder_CopyFrameParamsWithReset 0x00019CBC.
 *
 * A field-by-field copy of the 68-short parameter block, plus the 0x38-short
 * amplitude tail, with one deliberate omission: the pointer at [0x40..0x41] is
 * zeroed rather than copied.  That pointer is the destination's own voicing-flag
 * array and copying it would leave two blocks sharing one, which is why the
 * stock code spells the copy out instead of memcpy-ing 68 shorts.
 *
 * Fields 0..7, 0x42 and the tail; [0x41] and everything from [0x43] up are not
 * copied at all.
 */
void ambe_frame_params_copy(int16_t *dst, const int16_t *src)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    dst[4] = src[4];            /* the voicing word, copied as one 32-bit field */
    dst[5] = src[5];
    dst[6] = src[6];
    dst[7] = src[7];
    dst[0x40] = 0;
    dst[0x41] = 0;
    dst[0x42] = src[0x42];
    memcpy(dst + 8, src + 8, 0x38 * sizeof(int16_t));
}

/*
 * Vocoder_ResetFrameBuffer 0x00019D38.
 *
 * Fills the block's voicing-flag array according to the frame class in [0]:
 *
 *   1  voice     - Vocoder_BuildFrameResetPattern from the voicing word, which
 *                  this library already has as ambe_unvoiced_voicing()
 *   2  silence   - all flags cleared
 *   3  tone      - cleared, then one or two flags set at the tone's own bins
 *
 * Any other class leaves the array untouched, which includes the erasure class.
 *
 * The tone branch needs Tone_ClassifyCtcssDcsCode 0x0002B0F4 to tell a CTCSS or
 * DCS code from a single tone, and this project has no transcription of it.
 * Tone frames are classified and muted rather than synthesised - README,
 * "Limitations" - and the corpus holds two of them in 2052 frames, so the branch
 * is left explicit and unimplemented rather than guessed at: it returns without
 * touching the flags and says so.
 */
void ambe_frame_reset_buffer(int16_t *params, uint16_t *flags)
{
    switch (params[0]) {
    case 1:
        ambe_unvoiced_voicing(flags,
                              (uint32_t)(((uint32_t)(uint16_t)params[5] << 16)
                                         | (uint16_t)params[4]),
                              params[6], params[2]);
        return;
    case 2:
        memset(flags, 0, 0x38 * sizeof(uint16_t));
        return;
    case 3:
        memset(flags, 0, 0x38 * sizeof(uint16_t));
        /* the tone bins would be set here; see the comment above */
        return;
    default:
        return;
    }
}

/*
 * Math_PopCountBits 0x000189F4.
 *
 * The population count of the LOW `n` bits, done the way a machine with no
 * popcount instruction does it: shift the wanted bits up so the unwanted ones
 * fall off the top (`subu r1,r2,r1 / lsl r1,r0,r1` with r2 = 0x20), then sum
 * four byte lookups into a 256-entry table at 0x00018A28.
 *
 * The table is not extracted.  A byte popcount table has exactly one possible
 * content, and the sweep confirms it: if the radio's held anything else this
 * would not agree on 4096 cases.
 */
int ambe_popcount_bits(uint32_t x, int n)
{
    uint32_t v = ambe_lsl_hw((int32_t)x, 32 - n);
    int c = 0;

    while (v) {
        c += (int)(v & 1u);
        v >>= 1;
    }
    return c;
}

/*
 * Vocoder_SmoothPitchState 0x00022D7C.
 *
 * A one-pole smoother on the pitch state, applied only when the frame is voiced
 * enough to trust: `Math_PopCountBits(vuv & 0x55555555, 32) > 7`, which counts
 * the voiced bands using the same crumb mask everything else in the codec uses,
 * and leaves the state untouched below eight of them.
 *
 * The coefficients read as 0xCCCC and 0xE666 in the decompilation and are
 * neither: the machine holds 0x6666 and 0x7333 and doubles each product, which
 * is the Q15 multiply, so the weights are 0.8 and 0.9 rather than something
 * with a sign bit in it.  0.8 arrives >> 3, giving 0.1 of the new pitch against
 * 0.9 of the old.
 */
uint32_t ambe_smooth_pitch_state(uint32_t state, uint16_t target, uint32_t vuv)
{
    if (ambe_popcount_bits(vuv & 0x55555555u, 0x20) <= 7)
        return state & 0xFFFF;

    return (uint32_t)(ambe_asr_hw((int32_t)(int16_t)target * 0x6666 * 2, 3)
                      + (int32_t)(int16_t)state * 0x7333 * 2) >> 16;
}

/*
 * Vocoder_NormalizeSpectralBlock 0x00022C18.
 *
 * Clamp, find the peak, exponentiate everything against it, and write the
 * common exponent out - the block float the synthesisers are handed their
 * amplitudes in.  Three passes over the same array in the stock code and three
 * here, because they are not fusable: the peak of the CLAMPED values is what
 * sets the exponent, and every element is then scaled by it.
 *
 *   the clamp is +/-0x77FF, not +/-0x7FFF.  It leaves two bits of headroom
 *   below saturation, which is what keeps the Math_Pow2Scaled call below from
 *   overflowing on a full-scale amplitude;
 *
 *   the peak is taken over `value << 16` as a SIGNED comparison seeded at
 *   INT32_MIN, so a block that is entirely negative still finds its largest;
 *
 *   the exponent is `((peak >> 11) + 0x10000) >> 16`, and the values are then
 *   Math_Pow2Scaled(value, 4, that) - an exponentiation, not a shift, because
 *   the amplitudes are logarithmic at this point and become linear here.
 *
 * The tail from `count` to 0x38 is zeroed whatever happens, including on the
 * empty-block path where the exponent is the fixed -0x1E.
 */
void ambe_normalize_spectral_block(int16_t *coeffs, int16_t *exp_out, int count)
{
    int32_t peak = INT32_MIN;
    int16_t e;
    int i, n;

    if (count < 1) {
        e = -0x1e;
    } else {
        n = (int)((((uint32_t)(uint16_t)count - 1) & 0xffff) + 1);
        for (i = 0; i < n; i++) {
            if (coeffs[i] < -0x77ff)
                coeffs[i] = -0x77ff;
            if (coeffs[i] > 0x77ff)
                coeffs[i] = 0x77ff;
        }
        for (i = 0; i < n; i++) {
            int32_t v = (int32_t)((uint32_t)(uint16_t)coeffs[i] << 16);

            if (peak < v)
                peak = v;
        }
        e = (int16_t)((uint32_t)(ambe_asr_hw(peak, 11) + 0x10000) >> 16);
        for (i = 0; i < count; i++)
            coeffs[i] = (int16_t)ambe_pow2_scaled(coeffs[i], 4, e);
        e = (int16_t)(e - 0xf);
    }
    *exp_out = e;
    memset(coeffs + count, 0, (size_t)(int16_t)(0x38 - count) * sizeof(int16_t));
}
