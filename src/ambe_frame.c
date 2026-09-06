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
 *   Math_ArrayShiftCopy                0x0001AB58   here
 *   Math_PopCountBits                  0x000189F4   here
 *   Math_SqrtScaled                    0x000193E0   here
 *   Vocoder_CopyFrameParamsWithReset   0x00019CBC   here
 *   Vocoder_ResetFrameBuffer           0x00019D38   here
 *   Vocoder_SmoothPitchState           0x00022D7C   here
 *   Vocoder_NormalizeSpectralBlock     0x00022C18   here
 *   Vocoder_UpdatePitchHistoryBuffer   0x0001A9E8   here
 *   Dsp_HilbertTransform               0x00029D1C   here
 *   Dsp_NormalizeArray                 0x0001ADA0   here
 *   Math_ArrayShiftSaturate            0x0001AF5C   here
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

/*
 * Vocoder_UpdatePitchHistoryBuffer 0x0001A9E8.
 *
 * Moves the frame's single pitch candidate from wherever it was to a new index,
 * and recomputes the two things that depend on where it sits.  Despite the
 * name nothing is a history buffer: the block holds one amplitude per possible
 * harmonic at [8 + i] and one flag per harmonic in the array [0x40] points at,
 * and this carries one entry across.
 *
 *   the amplitude at the old index is moved, not copied - the old slot is
 *   zeroed first and the old flag cleared, so a candidate that does not move
 *   still ends up zeroed and then rewritten;
 *
 *   [6] becomes params[1] / (index + 1) as a block float scaled by the returned
 *   exponent plus four - the pitch implied by the new index;
 *
 *   [2], the harmonic count, is raised to index + 1 if the index has outgrown
 *   it and then clamped to [9, 0x38].  The lower bound is not a sanity check:
 *   nine harmonics is what the synthesiser below assumes it has.
 */
void ambe_update_pitch_history(int16_t *params, uint16_t *flags, int16_t cand)
{
    int16_t old = params[3];
    int16_t amp = params[8 + old];
    uint32_t num, den;
    int16_t ne, de, oe, L;
    uint16_t q;
    int32_t v;
    int sh;

    params[8 + old]  = 0;
    flags[old]       = 0;
    params[8 + cand] = amp;
    params[3]        = cand;
    flags[cand]      = 1;

    num = (uint32_t)(uint16_t)params[1] << 16;
    if ((uint16_t)params[1] == 0) {
        ne  = 0xf;
        num = 0;
    } else {
        sh  = ambe_nsh((int32_t)num);
        ne  = (int16_t)(0xf - sh);
        num = (uint32_t)ambe_lsl_hw((int32_t)num, sh);
    }

    den = (uint32_t)(((uint16_t)params[3] + 1) * 0x10000);
    if (den == 0) {
        de = 0x17;
        sh = 0;
    } else {
        sh = ambe_nsh((int32_t)den);
        de = (int16_t)(0x17 - sh);
    }

    q  = ambe_float_div_exp((int32_t)num, ne, ambe_nhi((int32_t)den, sh), de, &oe);
    sh = (int16_t)(oe + 4);
    v  = sh < 0 ? ambe_asr_hw(ambe_shl32((int32_t)(int16_t)q, 16), -sh)
                : ambe_lsl_hw(ambe_shl32((int32_t)(int16_t)q, 16), sh);
    params[6] = (int16_t)((uint32_t)v >> 16);

    L = params[2];
    if (L <= params[3])
        L = (int16_t)(params[3] + 1);
    if (L < 9)
        L = 9;
    if (L > 0x38)
        L = 0x38;
    params[2] = L;
}

/*
 * Dsp_HilbertTransform 0x00029D1C.
 *
 * Named Vocoder_ApplySynthesisWindow in an earlier pass of the
 * reverse-engineering and it is not a window: it is a ten-tap antisymmetric FIR
 * whose taps are 18411/(2k+1) for k = 0..9 - the odd-harmonic series, which is
 * the textbook ideal Hilbert transformer h[m] = 2/(pi*m) for odd m.  It runs
 * along the HARMONIC axis, not along time, so it phase-shifts a per-harmonic
 * array.  README, "The synthesis window is not a firmware object", has the
 * consequence: the radio has no overlap-add window at all.
 *
 * The ten taps are written out rather than generated, and that is a correction
 * rather than a preference.  Generating them as 18411/(2k+1) in C gives 2045 and
 * 1673 where the image holds 2046 and 1674: the series is ROUNDED, not
 * truncated, and integer division truncates.  The plate comment in the
 * reverse-engineering database lists the values and I generated them anyway; the
 * sweep caught it on the first case.  The derivation stays in the comment
 * because it is what identifies the filter, but the constants are the ones the
 * radio holds.
 *
 * The interesting part is the extension, because the filter reaches 19 places
 * either side of every output and the array is only `count` long:
 *
 *   left, a reflection about index -1 - W[-2-k] = src[k], with W[-1] itself
 *   pinned to zero, which is what makes the transform antisymmetric at the
 *   bottom end rather than merely truncated;
 *
 *   right, a RAMP: from src[count-1] the value falls by 1477 per step,
 *   saturating, out to index 0x40.  Not a reflection - the harmonic envelope is
 *   decaying there and a reflection would turn the decay back upwards;
 *
 *   and right again by reflection past 0x40, but only when count > 0x2D, which
 *   is exactly when an output at count-1 can reach past 0x40.
 */
void ambe_hilbert_transform(int16_t *dst, const int16_t *src, int count)
{
    static const int OFF = 24;                 /* room for the left reflection */
    int16_t w[OFF + 0x4c];
    int32_t v;
    int i, k;

    for (i = 0; i < (int)(sizeof(w) / sizeof(w[0])); i++)
        w[i] = 0;
    for (i = 0; i < count; i++)
        w[OFF + i] = src[i];
    w[OFF - 1] = 0;                            /* `st.h r3,(r6,0x26)` at entry */

    /* right: the falling ramp, saturating, out to 0x3F.  Not 0x40: the loop
       counter starts at count+1 and increments before the store rather than
       after it, so the last index written is one below where reading the
       decompiler's `while (sVar3 < 0x41)` suggests. */
    v = (int32_t)((uint32_t)(uint16_t)src[count - 1] << 16);
    for (i = count; i <= 0x3f; i++) {
        int32_t n = (int32_t)((uint32_t)v + 0xfa3b0000u);

        /* the machine's overflow test, and it has to be done on SIGNED values:
           0xfa3affff is an unsigned constant in C, so `(n ^ 0xfa3affff) < 0`
           promotes the whole expression to unsigned and can never be true - the
           saturation then never fires and a full-scale input runs off the end */
        if ((int32_t)(((uint32_t)n ^ (uint32_t)v)
                      & ((uint32_t)n ^ 0xfa3affffu)) < 0)
            n = v < 0 ? (int32_t)0x80000000 : 0x7fffffff;
        w[OFF + i] = (int16_t)((uint32_t)n >> 16);
        v = n;
    }
    /* right past 0x40, by reflection, only when an output can reach there */
    if (count > 0x2d)
        for (i = 0; i <= 0x4a - 0x40; i++)
            w[OFF + 0x40 + i] = w[OFF + 0x3e - i];
    /* left: the reflection about -1 */
    for (k = 0; k <= 18; k++)
        w[OFF - 2 - k] = w[OFF + k];

    for (i = 0; i < count; i++) {
        int32_t acc = 0;

        for (k = 0; k < 10; k++) {
            /* 18411/(2k+1) rounded: SRAM 0x18003958, file 0x067018 */
            static const int16_t tapv[10] = {
                18411, 6137, 3682, 2630, 2046, 1674, 1416, 1227, 1083, 969
            };
            int32_t tap = tapv[k];

            acc += (int32_t)(int16_t)w[OFF + i + 1 + 2 * k] * tap * 2
                 - (int32_t)(int16_t)w[OFF + i - 1 - 2 * k] * tap * 2;
        }
        dst[i] = (int16_t)((((uint32_t)acc & 0x80000000u)
                            + (((uint32_t)acc * 2) & 0x7fffffffu)) >> 16);
    }
    memset(dst + count, 0, (size_t)(int16_t)(0x38 - count) * sizeof(int16_t));
}

/*
 * Math_ArrayShiftCopy 0x0001AB58.
 *
 * Copy `n` shorts, shifting each by `shift` - left when positive, arithmetic
 * right when negative, and a plain copy at zero, which the stock code branches
 * out to rather than shifting by nothing.
 *
 * The shift is applied to the sign-extended short and the result truncated back
 * to 16 bits, so a left shift discards the top rather than saturating.  That is
 * what makes the caller's leading-zero count load-bearing: Dsp_NormalizeArray
 * chooses a shift that cannot overflow, and nothing here would catch it if it
 * chose wrong.
 */
void ambe_array_shift_copy(int16_t *dst, const int16_t *src, int n, int shift)
{
    int i;

    for (i = 0; i < n; i++) {
        if (shift == 0)
            dst[i] = src[i];
        else if (shift < 0)
            dst[i] = (int16_t)ambe_asr_hw((int32_t)src[i], -shift);
        else
            dst[i] = (int16_t)ambe_lsl_hw((int32_t)src[i], shift);
    }
}

/*
 * Dsp_NormalizeArray 0x0001ADA0.
 *
 * Find the largest magnitude in the array, shift the whole array up by the
 * headroom that leaves, and SUBTRACT that shift from the exponent the caller is
 * carrying - so the value the pair represents does not change, only where its
 * bits sit.
 *
 * Two details are the stock code's and neither is obvious:
 *
 *   the magnitude is `~v + 1` on a negative value, which is a true negation and
 *   so turns -0x8000 into -0x8000 rather than saturating.  The comparison that
 *   follows is signed, so that one value compares as smaller than everything
 *   rather than larger - a block whose only extreme is -0x8000 normalises as if
 *   its peak were whatever is next largest;
 *
 *   an all-zero array is not a special case of the general one.  It copies with
 *   a shift of zero and writes the exponent back UNCHANGED - `*pOutExponent =
 *   *pOutExponent`, which reads as a no-op and is the point: the caller's
 *   exponent survives a silent block instead of being driven to a floor.
 */
void ambe_normalize_array(int16_t *dst, const int16_t *src, int count,
                          int16_t *exp_io)
{
    uint32_t peak = 0;
    int i, sh;

    if (count < 1) {
        ambe_array_shift_copy(dst, src, count, 0);
        return;                       /* the exponent is left as it was */
    }
    for (i = 0; i < (int)((((uint32_t)(uint16_t)count - 1) & 0xffff) + 1); i++) {
        uint32_t m = (uint32_t)(int32_t)src[i];

        if ((int32_t)m < 0)
            m = ~m + 1;
        if ((int32_t)peak < (int32_t)m)
            peak = m;
    }
    peak <<= 16;
    if (peak == 0) {
        ambe_array_shift_copy(dst, src, count, 0);
        return;
    }
    if (peak & 0x80000000u)
        peak = ~peak;
    sh = (int16_t)(ambe_lzcount32(peak) - 1u);
    ambe_array_shift_copy(dst, src, count, sh);
    *exp_io = (int16_t)(*exp_io - sh);
}

/*
 * Math_ArrayShiftSaturate 0x0001AF5C.
 *
 * Rescale an array from one block-float exponent to another, saturating rather
 * than wrapping when the new exponent cannot hold it.  Not the same function as
 * Math_ArrayShiftSaturateInt 0x0001AE14, which is the final scaling to int16
 * PCM and lives in ambe_postfilter.c as ambe_synth_output.
 *
 * Shifting DOWN never saturates and is not even checked: the value is shifted
 * arithmetically and the low 16 bits taken.  Shifting UP is checked per element
 * against that element's own headroom, so one loud sample saturates alone
 * rather than forcing the whole array down - which is the difference between
 * this and simply renormalising.
 *
 * A zero element short-circuits to zero without going through either path,
 * which matters because the headroom of zero would otherwise be 31 and let it
 * through any shift at all.
 */
void ambe_array_shift_saturate(int16_t *dst, const int16_t *src, int count,
                               int16_t dst_exp, int16_t src_exp)
{
    int sh = (int16_t)(dst_exp - src_exp);
    int i;

    for (i = 0; i < count; i++) {
        uint32_t v = (uint32_t)(uint16_t)src[i] << 16;

        if ((uint16_t)src[i] == 0) {
            dst[i] = 0;
        } else if (sh < 0) {
            dst[i] = (int16_t)((uint32_t)ambe_asr_hw((int32_t)v, -sh) >> 16);
        } else {
            uint32_t m = ((int32_t)v < 0) ? ~v : v;

            if ((int)(int16_t)((int16_t)ambe_lzcount32(m) - 1) < sh)
                dst[i] = (v & 0x80000000u) ? (int16_t)0x8000 : (int16_t)0x7fff;
            else
                dst[i] = (int16_t)((uint32_t)ambe_lsl_hw((int32_t)v, sh) >> 16);
        }
    }
}
