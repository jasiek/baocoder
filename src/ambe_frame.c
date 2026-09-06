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
 *   FUN_0001ABDC                       0x0001ABDC   here (was in ambe_unvoiced.c)
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
 *   Vocoder_MatchExcitationEnergy      0x000277F8   here
 *   Vocoder_SynthesizeFrame            0x00019DB8   not yet
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>

#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_tables.h"
#include "ambe_frame_int.h"
#include "ambe_voiced_int.h"

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
 * The tone branch reads Tone_ClassifyCtcssDcsCode 0x0001A434 - which is at
 * 0x0001A434 and not the 0x0002B0F4 an earlier version of this comment cited -
 * to tell a CTCSS tone from a DCS pair, and sets one flag or two accordingly.
 * The corpus holds two tone frames in 2052, so nothing here exercises it.
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
        /* one bin for a CTCSS tone at [3], two for a DCS pair, which [3]
           carries as two bytes rather than as an index */
        if (ambe_tone_class(params[1]) == 0) {
            flags[params[3]] = 1;
        } else if ((unsigned)ambe_tone_class(params[1]) < 4) {
            flags[(uint16_t)params[3] >> 8]   = 1;
            flags[(uint16_t)params[3] & 0xff] = 1;
        }
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

/*
 * FUN_0001abdc: twice the dot product of two int16 arrays, renormalised.
 *
 * Lived in ambe_unvoiced.c until Vocoder_MatchExcitationEnergy turned out to
 * want it too - two of the codec's three energy measurements are this same
 * function, called with the same array twice.  It is verified where it always
 * was, through test_unvoiced's 206 firmware calls.
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
int32_t ambe_dot_norm(int16_t *exp_out, const int16_t *a, const int16_t *b,
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
 * Vocoder_MatchExcitationEnergy 0x000277F8.
 *
 * The spectral amplitude enhancement, in block float, and it is the IMBE one:
 *
 *     W_l = sqrt(M_l) * [ (R0^2 + R1^2 - 2*R0*R1*cos(l*w0))
 *                         / (w0 * R0 * (R0^2 - R1^2)) ] ^ 0.25
 *
 *   R0 = sum M_l^2                 - FUN_0001ABDC over the array with itself
 *   R1 = sum M_l^2 * cos(l*w0)     - the same sum, weighted by the cosine
 *
 * the zeroth and first lags of the amplitude autocorrelation.  The fourth root
 * arrives as two nested Math_Sqrt calls with the amplitude folded in between
 * them, which is why a multiply sits between two square roots in the middle of
 * the loop; the cosine comes off g_awSineTable512 through a phase accumulator
 * stepping `pitch * 64`, so l*w0 lands on table index l*pitch/1024 and the
 * whole harmonic series stays inside the 512 entries exactly when it stays
 * under Nyquist, which is what makes an unmasked index safe here.
 *
 * Three things are worth naming against mbelib's mbe_spectralAmpEnhance, which
 * computes the same weights:
 *
 *   the unenhanced low band is L/8, not mbelib's L/4 - and those harmonics are
 *   not left alone, they are HALVED.  That reads as an attenuation and is a
 *   normalisation: the weights the rest get are clamped to [0x2000, 0x4CCD] in
 *   Q15, which is [0.25, 0.6], and measured against a low band sitting at 0.5
 *   those are mbelib's [0.5, 1.2] exactly;
 *
 *   the absolute factor washes out at the end anyway, and that wash is the last
 *   third of the function: Dsp_NormalizeArray, R0 measured a second time, then
 *   every harmonic scaled by sqrt(R0_before / R0_after).  The enhancement
 *   changes the shape of the envelope and not its energy;
 *
 *   the reference-energy pair the caller passes - pChannelState+0x7c0 and
 *   +0x7c2 - is read and rewritten here as a one-pole average, 0.95 of it plus
 *   0.8 of this frame's R0 at 2^-4, floored at an exponent of -0x11.  Nothing
 *   in this function consumes it.  It is state kept for someone else, and it
 *   is what makes the enhancement stateful even though its own arithmetic is
 *   not.
 *
 * `amps` is the 0x38-short amplitude block and `count` its harmonic count; the
 * cosine scratch is the same length, so a count past 0x38 runs off both, as it
 * does in the stock code.
 */

/* 0x00027C1C, the literal wedged between two branches inside the function
   itself: 0.9502 in Q30, the enhancement's numerator constant. */
#define MEE_NUMERATOR 0x3cd013a9

/* w0 * R0 * X, the denominator, through the stock code's Q15 steps. */
static int32_t mee_denominator(uint16_t x, uint16_t r0m, int16_t pitch)
{
    int16_t t = (int16_t)ambe_asr_hw(
        (int32_t)((uint32_t)((int32_t)(int16_t)x * (int32_t)(int16_t)r0m) * 2u), 16);

    return (int32_t)((uint32_t)((int32_t)t * (int32_t)pitch) * 2u);
}

void ambe_match_excitation_energy(int16_t *amps, int16_t *exp_io,
                                  int16_t *ref_mant, int16_t *ref_exp,
                                  int16_t pitch, int16_t count)
{
    int16_t cosk[0x38];
    const int16_t e_in = *exp_io;
    int16_t e_r0, e_r1 = 0;
    uint16_t r0m, r1m = 0;
    int32_t v;
    int i, n, sh;

    memset(cosk, 0, sizeof cosk);

    v    = ambe_dot_norm(&e_r0, amps, amps, count);
    e_r0 = (int16_t)(e_r0 + (int16_t)(2 * e_in));
    r0m  = (uint16_t)((uint32_t)v >> 16);

    if (r0m != 0) {
        /* R1, and the cosine samples kept rather than recomputed below */
        int64_t acc = 0;
        int32_t step = ambe_asr_hw(
            (int32_t)((uint32_t)((int32_t)pitch * 0x200) * 2u), 4);
        int32_t ph = step;
        uint32_t lo, hi, mlo, mhi;
        int16_t shift = 0;
        int32_t mant;

        if (count >= 1) {
            n = (int)((((uint32_t)(uint16_t)count - 1) & 0xffff) + 1);
            for (i = 0; i < n; i++) {
                int16_t c  = ambe_cos512_q15[ambe_asr_hw(ph, 16)];
                int16_t sq = (int16_t)ambe_asr_hw(
                    (int32_t)((uint32_t)((int32_t)amps[i] * (int32_t)amps[i])
                              * 2u), 16);

                cosk[i] = c;
                acc += (int64_t)(int32_t)((uint32_t)((int32_t)sq * (int32_t)c)
                                          * 2u);
                ph += step;
            }
        }
        /* the same one's-complement normalisation ambe_dot_norm does, inlined
           in the stock code because the accumulator never leaves registers */
        lo = (uint32_t)acc;
        hi = (uint32_t)((uint64_t)acc >> 32);
        if (hi != 0 || lo != 0) {
            mhi = hi;
            mlo = lo;
            if ((int32_t)hi < 0) {
                mhi = ~hi;
                mlo = ~lo;
            }
            shift = (int16_t)(mhi != 0 ? (int)ambe_lzcount32(mhi) - 33
                                       : (int)ambe_lzcount32(mlo) - 1);
        }
        if (shift >= 0)
            mant = ambe_lsl_hw((int32_t)lo, shift);
        else if (-shift >= 32)
            mant = ambe_asr_hw((int32_t)hi, -shift - 32);
        else
            mant = (int32_t)((lo >> -shift)
                             | (uint32_t)ambe_shl32((int32_t)(hi << 1),
                                                    31 + shift));
        r1m = (uint16_t)((uint32_t)mant >> 16);
        if (r1m == 0x8000)
            r1m = 0x8001;               /* the one saturation in the function */
        e_r1 = (int16_t)((int16_t)(2 * e_in) - shift);
    }

    /* the caller's reference energy: 0.95 of it plus 0.8 of R0 at 2^-4, both
       aligned to the larger exponent, and a fixed 0x7FFF at -0x11 whenever the
       result will not normalise or falls below that floor */
    {
        int16_t m = e_r0;
        int32_t a, b, sum;
        int ok = 0;

        if (e_r0 < *ref_exp)
            m = *ref_exp;
        a  = (int32_t)((uint32_t)((int32_t)*ref_mant * 0x799a) * 2u);
        sh = (int16_t)(*ref_exp - m);
        a  = sh < 0 ? ambe_asr_hw(a, -sh) : ambe_lsl_hw(a, sh);
        b  = (int32_t)((uint32_t)((int32_t)(int16_t)r0m * 0x6666) * 2u);
        sh = (int16_t)((int16_t)(e_r0 - 4) - m);
        b  = sh < 0 ? ambe_asr_hw(b, -sh) : ambe_lsl_hw(b, sh);
        sum = (int32_t)((uint32_t)b + (uint32_t)a);

        if (sum != 0) {
            int s = ambe_nsh(sum);
            int32_t norm = ambe_lsl_hw(sum, s);
            int16_t e = (int16_t)(m - s);

            if (norm != 0 && e >= -0x10) {
                *ref_mant = (int16_t)((uint32_t)norm >> 16);
                *ref_exp  = e;
                ok = 1;
            }
        }
        if (!ok) {
            *ref_mant = 0x7fff;
            *ref_exp  = -0x11;
        }
    }

    if ((int16_t)r0m < 1)
        return;

    {
        int16_t e2 = (int16_t)(2 * e_r0 + 1);
        int32_t sq0, num = 0;
        uint16_t rsum = 0;
        /* the numerator's two halves as block floats: A is (R0^2 + R1^2) over
           the denominator, B the 2*R0*R1 the cosine multiplies */
        int16_t am = 0x7fff, ae = 0, bm = 0, be = 0;
        int have = 1;

        sh  = (int16_t)((int16_t)(2 * e_r0) - e2);       /* -1, always */
        sq0 = (int32_t)((uint32_t)((int32_t)(int16_t)r0m
                                   * (int32_t)(int16_t)r0m) * 2u);
        sq0 = sh < 0 ? ambe_asr_hw(sq0, -sh) : ambe_lsl_hw(sq0, sh);

        if (r1m == 0) {
            rsum = (uint16_t)((uint32_t)sq0 >> 16);
            num  = mee_denominator(rsum, r0m, pitch);
        } else {
            int32_t sq1;
            uint16_t rdiff;

            sh  = (int16_t)((int16_t)(2 * e_r1) - e2);
            sq1 = (int32_t)((uint32_t)((int32_t)(int16_t)r1m
                                       * (int32_t)(int16_t)r1m) * 2u);
            sq1 = sh < 0 ? ambe_asr_hw(sq1, -sh) : ambe_lsl_hw(sq1, sh);
            rdiff = (uint16_t)(((uint32_t)sq0 - (uint32_t)sq1) >> 16);
            if (rdiff == 0) {
                /* R0^2 and R1^2 agree to the top 16 bits - a single harmonic,
                   or one that dominates - and the whole weight collapses to a
                   flat 0x7FFF rather than dividing by nothing */
                have = 0;
            } else {
                rsum = (uint16_t)(((uint32_t)sq1 + (uint32_t)sq0) >> 16);
                num  = mee_denominator(rdiff, r0m, pitch);
            }
        }

        if (have) {
            int32_t at, bt;
            int16_t t, recip, ebase;
            int s = num != 0 ? ambe_nsh(num) : 0;
            int16_t d = (int16_t)ambe_asr_hw(ambe_lsl_hw(num, s), 16);

            /* `divs` traps on a zero divisor, so a zero here cannot come from
               anything the caller produces - a zero pitch would do it.  Zero
               rather than dividing by it, as ambe_float_div_exp does. */
            recip = d != 0 ? (int16_t)ambe_sdiv_half(MEE_NUMERATOR, d) : 0;
            ebase = (int16_t)((int16_t)(4 - (int16_t)(e2 + e_r0))
                              - (int16_t)(-s));

            at = (int32_t)((uint32_t)((int32_t)recip
                                      * (int32_t)(int16_t)rsum) * 2u);
            if (at == 0) {
                am = 0;
                ae = 0;
            } else {
                int s2 = ambe_nsh(at);

                am = (int16_t)((uint32_t)ambe_lsl_hw(at, s2) >> 16);
                ae = (int16_t)(-s2);
                if (am != 0)
                    ae = (int16_t)(ebase + e2 - s2);
            }

            t  = (int16_t)ambe_asr_hw(
                     (int32_t)((uint32_t)((int32_t)(int16_t)r1m
                               * (int32_t)(int16_t)r0m) * 2u), 16);
            t  = (int16_t)ambe_asr_hw(
                     (int32_t)((uint32_t)((int32_t)recip * (int32_t)t) * 2u), 16);
            bt = (int32_t)((uint32_t)((int32_t)t * 0x4000) * 2u);
            if (bt == 0) {
                bm = 0;
                be = 0;
            } else {
                int s3 = ambe_nsh(bt);

                bm = (int16_t)((uint32_t)ambe_lsl_hw(bt, s3) >> 16);
                be = (int16_t)(-s3);
                if (bm != 0)
                    be = (int16_t)(ebase + e_r0 + e_r1 + 2 - s3);
            }
        }

        {
            int16_t half = (int16_t)((((uint32_t)(int32_t)count) & 0x7ffff) >> 3);
            int16_t top;

            /* the low band: halved, and skipped by the loop below.  The stock
               code writes it as a 15-bit field sign-extended out of the
               shifted halfword, which is an arithmetic >>1 and nothing more. */
            for (i = 0; i < half; i++)
                amps[i] = (int16_t)ambe_asr_hw((int32_t)amps[i], 1);

            top = (int16_t)((be < ae) ? ae + 1 : be + 1);
            if (half < count) {
                int16_t sha = (int16_t)(ae - top);
                int16_t shb = (int16_t)(be - top);
                int32_t ahi  = ambe_shl32((int32_t)am, 16);
                /* loop-invariant, and the stock code hoists BOTH directions
                   out and picks between them per iteration with a sign test */
                int32_t asel = sha < 0 ? ambe_asr_hw(ahi, -sha)
                                       : ambe_lsl_hw(ahi, sha);
                int16_t k = half;

                do {
                    int32_t bterm = (int32_t)((uint32_t)((int32_t)cosk[k]
                                              * (int32_t)bm) * 2u);
                    int32_t diff;
                    int16_t g = 0;

                    bterm = shb < 0 ? ambe_asr_hw(bterm, -shb)
                                    : ambe_lsl_hw(bterm, shb);
                    diff  = (int32_t)((uint32_t)asel - (uint32_t)bterm);
                    if (diff != 0) {
                        int16_t se = top;
                        uint32_t r = ambe_float_sqrt(diff, &se);

                        se = (int16_t)(se + *exp_io);
                        r  = ambe_float_sqrt(
                                 (int32_t)((uint32_t)((int32_t)(int16_t)(r >> 16)
                                           * (int32_t)amps[k]) * 2u), &se);
                        if (se < 2) {
                            int16_t d2 = (int16_t)(se - 1);

                            g = d2 < 0
                                ? (int16_t)((uint32_t)ambe_asr_hw((int32_t)r,
                                                                  -d2) >> 16)
                                : (int16_t)((uint32_t)ambe_lsl_hw((int32_t)r,
                                                                  d2) >> 16);
                        } else {
                            g = 0x4ccd;
                        }
                    }
                    if (g >= 0x4cce)
                        g = 0x4ccd;
                    else if (g < 0x2000)
                        g = 0x2000;
                    amps[k] = (int16_t)ambe_asr_hw(
                        (int32_t)((uint32_t)((int32_t)g * (int32_t)amps[k])
                                  * 2u), 16);
                    k = (int16_t)(k + 1);
                } while (k < count);
            }
        }
    }

    /* and the wash: renormalise, measure R0 again, and put back the energy the
       weights took out */
    *exp_io = (int16_t)(*exp_io + 1);
    ambe_normalize_array(amps, amps, count, exp_io);
    {
        int16_t e3, oe = 0, gm;
        uint16_t r0b, q;
        uint32_t rt;

        v   = ambe_dot_norm(&e3, amps, amps, count);
        e3  = (int16_t)((int16_t)(2 * *exp_io) + e3);
        r0b = (uint16_t)((uint32_t)v >> 16);
        if (r0b == 0)
            return;
        q  = ambe_float_div_exp(ambe_shl32((int32_t)(int16_t)r0m, 16), e_r0,
                                (int32_t)(int16_t)r0b, e3, &oe);
        rt = ambe_float_sqrt(ambe_shl32((int32_t)q, 16), &oe);
        gm = (int16_t)(rt >> 16);

        if (count >= 1) {
            n = (int)((((uint32_t)(uint16_t)count - 1) & 0xffff) + 1);
            for (i = 0; i < n; i++)
                amps[i] = (int16_t)ambe_asr_hw((int32_t)amps[i] * (int32_t)gm,
                                               15);
        }
        *exp_io = (int16_t)(*exp_io + oe);
        if (*exp_io > 0) {
            ambe_array_shift_saturate(amps, amps, count, *exp_io, 0);
            *exp_io = 0;
        }
    }
}

/*
 * Vocoder_SynthesizeFrame 0x00019DB8.
 *
 * The top of the synthesis layer, and the last untranscribed function of the
 * codec.  One 80-sample half-frame: the decoded parameter block goes in, PCM
 * comes out, and between them the block is rewritten four times before either
 * synthesiser sees it.  That rewriting is the whole reason the exact stages
 * below could not simply be called in order.
 *
 *   the envelope, log2 -> linear.  Dsp_HilbertTransform phase-shifts it along
 *   the harmonic axis into the voiced synthesiser's own state at ctx+0x172,
 *   which is that state's [0xAD] and not a separate array; then
 *   Vocoder_NormalizeSpectralBlock turns log2 into linear with a block
 *   exponent, and Vocoder_MatchExcitationEnergy applies the IMBE enhancement.
 *   All three run only on a voice frame, and the whole group is skipped when
 *   bit 0 of ctx+0x7ba is set;
 *
 *   the voicing flags.  Both blocks get an array planted on this function's
 *   stack and filled by Vocoder_ResetFrameBuffer - the current one and the
 *   previous one, because the voiced synthesiser reads both - and the pointer
 *   is cleared again before the block is copied away, so it never outlives the
 *   call.  This transcription passes the arrays rather than the pointer;
 *
 *   the low-frequency ramp.  The smoothed pitch is a one-pole track of the
 *   frame's own, held on a repeat frame, clamped at 0x3333, and its reciprocal
 *   comes from Math_FloatDivExponent(2^30, 1, ...).  Every harmonic below the
 *   clamp - k*w0 under about 500 Hz - is multiplied by (k*pitch/smoothed)^2,
 *   which is the codec's low-frequency rolloff, and the loop simply stops at
 *   the first harmonic above it;
 *
 *   and the high-frequency tilt, which is the surprise.  Walking DOWN from the
 *   top harmonic while k*w0 stays above 0.6*pi, each amplitude is multiplied by
 *   (k*w0)/(0.6*pi) - 0x6AAB is 5/6 in Q15 and the doubling makes it 5/3, and
 *   0x4CCD, where the walk stops, is exactly where that product is one.  So the
 *   band above 2400 Hz is tilted up, by 1.0 at the corner rising to 1.67 at
 *   Nyquist.  Each harmonic keeps its own exponent through that, and they are
 *   flattened back onto the largest of them afterwards.
 *
 * Then the three sample-domain stages, all of them already exact, in the order
 * the stock code runs them: the unvoiced synthesiser into a zeroed
 * accumulator, the voiced one adding to it, the output filter, and the scaling
 * to int16 - and last the block is copied into ctx+0x470 to be the next
 * frame's previous.
 *
 * The TONE path, pFrameParams[1] != 0xFF, is ambe_frame_tone_rewrite below.
 *
 * The ramp walks the amplitude array with no bound of its own: what stops
 * it is the pitch, one harmonic per iteration until k*pitch reaches the clamp.
 * A pitch small enough to run past the block would have the stock code writing
 * over its own fields, and a pitch of zero would hang it, neither of which a
 * caller produces; this stops at the end of the block and says so.
 *
 * Four things in here are transcribed and NOT exercised by the 617-call
 * sequence, and mutating each of them out leaves it green, so they are recorded
 * as unverified rather than presented as tested: the 0x3333 clamp, because the
 * smoothed pitch stays under it on every frame of the capture; the previous
 * block's voicing flags, which the stock code builds for
 * Vocoder_SynthesizeVoiced's pPrev[0x40] and src/ambe_voiced.c does not read;
 * the copy into the silence slot, which nothing reads back; and the zero fill
 * of ctx+0x172 on a non-voice frame, which no frame of the capture reads.
 */

/* Dsp_FillShortArray 0x00019494, the two-line memset the stock code calls. */
static void fill_shorts(int16_t *dst, int16_t v, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = v;
}

void ambe_frame_state_reset(ambe_frame_state *st)
{
    memset(st, 0, sizeof *st);
}

void ambe_frame_synthesize(int16_t *params, int16_t *pcm, int n, int repeat,
                           ambe_frame_state *st)
{
    uint16_t voi[0x38], prev_voi[0x38];
    int32_t acc[84];
    int16_t amp[0x38], aexp[0x38];
    int16_t L;
    int i;

    /* the stock code zeroes 2n shorts of an 84-int frame and reads none of the
       rest; zeroing all of it is the same behaviour and a defined one */
    memset(acc, 0, sizeof acc);
    memset(amp, 0, sizeof amp);
    memset(aexp, 0, sizeof aexp);

    if ((st->bypass & 1) == 0) {
        if (params[0] == 1)
            ambe_hilbert_transform(st->voiced + 0xAD, params + 8, params[2]);
        else
            fill_shorts(st->voiced + 0xAD, 0, 0x38);
        ambe_normalize_spectral_block(params + 8, params + 0x42, params[2]);
        if (params[0] == 1)
            ambe_match_excitation_energy(params + 8, params + 0x42,
                                         &st->ref_mant, &st->ref_exp,
                                         params[6], params[2]);
        if (params[0] == 2)
            ambe_frame_params_copy(st->silence, params);
    }

    /* both blocks get their flags, and the previous one's are rebuilt from the
       previous block rather than remembered - `st.w r3,(r6,0x4f0)` is
       pPrev[0x40], planted the same way.  ambe_voiced_synth takes one flag
       array and not two, so prev_voi goes nowhere; it is built anyway because
       the stock code builds it, and because a transcription of the voiced
       synthesiser that grew a second array would want it here. */
    ambe_frame_reset_buffer(params, voi);
    ambe_frame_reset_buffer(st->prev, prev_voi);
    (void)prev_voi;

    if ((int16_t)params[1] != 0xff) {
        ambe_frame_tone_rewrite(params, st->prev, st->tone_mode, voi);
    } else {
        int16_t sp, clamp, ed, md, ee, v;
        uint16_t q;
        int16_t oe = 0;
        int32_t val;

        if (repeat == 0) {
            uint32_t vuv = ((uint32_t)(uint16_t)params[5] << 16)
                         | (uint32_t)(uint16_t)params[4];

            st->pitch = (int16_t)ambe_smooth_pitch_state(
                (uint32_t)(int32_t)st->pitch, (uint16_t)params[6], vuv);
        }
        sp = st->pitch;
        if (sp < 0x3334) {
            clamp = sp;
            val   = ambe_shl32((int32_t)sp, 16);
        } else {
            clamp = 0x3333;
            val   = 0x33330000;
        }
        if (val == 0) {
            ed = -4;
            md = 0;
        } else {
            int s = ambe_nsh(val);

            ed = (int16_t)(-4 - s);
            md = (int16_t)((uint32_t)ambe_lsl_hw(val, s) >> 16);
        }
        /* 2^30 at exponent 1 over the smoothed pitch: the ramp's reciprocal */
        q  = ambe_float_div_exp(0x40000000, 1, (int32_t)md, ed, &oe);
        ee = (int16_t)(oe - 4);

        /* --- the low-frequency ramp, (k*pitch/smoothed)^2 per harmonic --- */
        v = params[6];
        if (v < clamp) {
            int32_t phi = ambe_shl32((int32_t)(int16_t)params[6], 16);

            for (i = 0; i < 68 - 8; i++) {
                int32_t prod = (int32_t)((uint32_t)((int32_t)v
                                         * (int32_t)(int16_t)q) * 2u);
                int32_t sq, vhi, sum;
                int16_t shift;

                if (prod == 0) {
                    sq    = 0;
                    shift = (int16_t)(ee * 2);
                } else {
                    int s = ambe_nsh(prod);
                    int16_t m = (int16_t)((uint32_t)ambe_lsl_hw(prod, s) >> 16);

                    shift = (int16_t)((int16_t)(ee - s) * 2);
                    sq = (int32_t)((uint32_t)((int32_t)m * (int32_t)m) * 2u);
                }
                sq = shift < 0 ? ambe_asr_hw(sq, -shift) : ambe_lsl_hw(sq, shift);

                vhi = ambe_shl32((int32_t)v, 16);
                sum = (int32_t)((uint32_t)vhi + (uint32_t)phi);
                params[8 + i] = (int16_t)ambe_asr_hw(
                    (int32_t)((uint32_t)((int32_t)(int16_t)((uint32_t)sq >> 16)
                              * (int32_t)params[8 + i]) * 2u), 16);
                /* the machine's signed-overflow test on the 16-bit step, done
                   in the high half of a word: a positive one ends the ramp, a
                   negative one restarts it from -0x8000 */
                if ((int32_t)(((uint32_t)sum ^ (uint32_t)vhi)
                              & ((uint32_t)sum ^ (uint32_t)phi)) < 0) {
                    if (vhi >= 0)
                        break;
                    v = (int16_t)0x8000;
                    continue;
                }
                v = (int16_t)((uint32_t)sum >> 16);
                if (v >= clamp)
                    break;
            }
        }

        /* --- the high-frequency tilt, walking down from the top harmonic --- */
        L = params[2];
        if (L > 0) {
            int cnt = (int)((((uint32_t)(uint16_t)L - 1) & 0xffff) + 1);
            int16_t be = params[0x42];

            for (i = 0; i < cnt; i++) {
                amp[i]  = params[8 + i];
                aexp[i] = be;
            }
        }
        {
            uint32_t t = (uint32_t)((int32_t)(int16_t)L
                                    * (int32_t)(int16_t)params[6]) * 2u;
            int16_t x = (int16_t)((t >> 4) & 0xffff);

            if (x >= 0x4cce) {
                int16_t be1 = (int16_t)(params[0x42] + 1);
                int32_t step = ambe_shl32((int32_t)(int16_t)params[6], 13);
                int k = L - 1;

                do {
                    /* 0x6AAB is 5/6 in Q15 and the product is doubled, so the
                       factor is x*5/3 >> 15 - one at x = 0x4CCD exactly */
                    int16_t g = (int16_t)ambe_asr_hw(
                        (int32_t)((uint32_t)((int32_t)x * 0x6aab) * 2u), 16);
                    int32_t prod = (int32_t)((uint32_t)((int32_t)amp[k]
                                             * (int32_t)g) * 2u);
                    int s = prod != 0 ? ambe_nsh(prod) : 0;
                    int32_t xn = (int32_t)((uint32_t)ambe_shl32((int32_t)x, 16)
                                           - (uint32_t)step);

                    aexp[k] = (int16_t)(be1 - s);
                    amp[k]  = (int16_t)ambe_asr_hw(ambe_lsl_hw(prod, s), 16);
                    x = (int16_t)((uint32_t)xn >> 16);
                    k--;
                } while (x >= 0x4cce && k >= 0);
            }
        }

        /* --- and back onto one exponent, the largest of them --- */
        {
            int16_t mx = aexp[0];

            if (L >= 2)
                for (i = 1; i < L; i++)
                    if (mx < aexp[i])
                        mx = aexp[i];
            if (L >= 1) {
                int cnt = (int)((((uint32_t)(uint16_t)L - 1) & 0xffff) + 1);

                for (i = 0; i < cnt; i++) {
                    int16_t sh = (int16_t)(aexp[i] - mx);
                    int32_t w = (int32_t)((uint32_t)(uint16_t)amp[i] << 16);

                    params[8 + i] = (int16_t)ambe_asr_hw(
                        sh < 0 ? ambe_asr_hw(w, -sh) : ambe_lsl_hw(w, sh), 16);
                }
            }
            params[0x42] = mx;
        }
    }

    /* the three sample-domain stages, in the stock code's order */
    ambe_unvoiced_synth(acc, n, &st->unvoiced, params[0], params[2], params[6],
                        params + 8, params[0x42], voi, st->pitch);
    ambe_voiced_synth(acc, (uint16_t)n, st->voiced, params, st->prev,
                      (const int16_t *)voi, st->pitch);
    ambe_postfilter(&st->post, acc, n);
    ambe_synth_output(pcm, acc, n);

    /* the flag pointer is cleared before the copy, which is why
       Vocoder_CopyFrameParamsWithReset zeroes [0x40..0x41] rather than copying
       them: two blocks must never share one array */
    params[0x40] = 0;
    params[0x41] = 0;
    ambe_frame_params_copy(st->prev, params);
}

/*
 * Tone_ClassifyCtcssDcsCode 0x0001A434 and Tone_CtcssDcsCodeToTableIndex
 * 0x0001A478.
 *
 * These two are why the tone branches in this file were holes, and they are 66
 * and 74 bytes.  Nothing about them was hard; they were simply never read.
 *
 * The classifier splits the tone code into five families by range - 5..0x7A
 * CTCSS, 0x80..0x8F DCS, 0x90..0x9F DCS inverted, 0xA0..0xA3 a fourth group,
 * anything else none - and the index function turns a code into the harmonic
 * bin it occupies.  For CTCSS that is a closed form, `ceil(code * 5 / 64) - 1`
 * floored at zero, written the way the machine writes it: multiply by 0x5000,
 * double, arithmetic-shift down three, add 0xFFFF and take the high half,
 * which is the ceiling.  For the three DCS families it is a table lookup at
 * SRAM 0x1800331C, two bins per code because a DCS tone is a pair, and the
 * entry is returned MINUS ONE - so the table holds 1-based harmonic numbers
 * and the codec wants 0-based.
 */
int ambe_tone_class(int16_t code)
{
    if ((uint16_t)(code - 5) < 0x76)
        return 0;
    if ((uint16_t)(code - 0x80) < 0x10)
        return 1;
    if ((uint16_t)(code - 0x90) < 0x10)
        return 2;
    if ((uint16_t)(code - 0xa0) < 4)
        return 3;
    return 4;
}

int16_t ambe_tone_bin(int cls, uint16_t code, int16_t flag)
{
    int16_t v;

    if (cls == 0) {
        int32_t t = (int32_t)((uint32_t)((int32_t)(int16_t)code * 0x5000) * 2u);

        v = (int16_t)(((uint32_t)(ambe_asr_hw(t, 3) + 0xffff) >> 16) - 1);
        return v < 0 ? 0 : v;
    }
    if ((unsigned)cls < 4)
        return (int16_t)(ambe_dcs_bins[((int16_t)code - 0x80) * 2 + flag] - 1);
    return 0;
}

/*
 * The tone branch of Vocoder_SynthesizeFrame 0x00019DB8, which runs instead of
 * the two frequency tilts whenever pFrameParams[1] is not 0xFF - that is,
 * whenever the frame carries a tone code rather than speech.  It is two
 * different things chosen by ctx+0x7a0, and neither is reachable from this
 * corpus: pFrameParams[1] is 0xFF on all 617 calls of it.
 *
 * ctx+0x7a0 == 1 is tone CONTINUATION.  A CTCSS tone whose previous frame was
 * also a CTCSS tone gets its harmonic index recomputed rather than taken from
 * the block: if the index moved by less than two and the code still fits under
 * the old index, the old index is kept; otherwise the index is
 * `(prev[3] + 1) * code / prev[1]`, rounded by adding half the divisor, and
 * then walked up in steps of 16 until it passes the code.  Either way
 * Vocoder_UpdatePitchHistoryBuffer moves the amplitude and recomputes the
 * pitch that follows from where it now sits.
 *
 * Anything else is tone SHAPING.  Every harmonic that is not one of the tone's
 * own bins is multiplied by 0x51E doubled - 0.04, a 28 dB notch of everything
 * around the tone - and then two corrections: a DCS code, 0x80..0x9F, replaces
 * both bins of its pair with the root of the sum of their squares, so a pair
 * carries the energy of the pair rather than of each half; and codes 0x14..0x16
 * get their pitch recomputed as `Math_SDivHalf(code << 20, (params[3] + 1) *
 * 0x100)`, which is three codes out of the 118 CTCSS ones and I have no reading
 * of why those three.
 *
 * The `& 0x20` on the DCS test is worth a line: it covers 0x80..0x9F, which is
 * the classifier's families 1 and 2 but NOT family 3 at 0xA0..0xA3.  Family 3
 * gets bins out of the table like a DCS pair and is not merged like one.
 */
void ambe_frame_tone_rewrite(int16_t *params, const int16_t *prev,
                             int16_t tone_mode, uint16_t *flags)
{
    uint16_t code = (uint16_t)params[1];

    if (tone_mode == 1) {
        int16_t idx, d;
        int32_t q, t;

        if (ambe_tone_class(params[1]) != 0 || prev[0] != 3
            || ambe_tone_class(prev[1]) != 0)
            return;
        idx = prev[3];
        d   = (int16_t)(params[3] - idx);
        if (d < 0)
            d = (int16_t)(~d + 1);          /* `abs`, and it is a true one */
        if (d < 2 && (int16_t)code < (int16_t)((idx + 1) * 0x10)) {
            ambe_update_pitch_history(params, flags, idx);
            return;
        }
        /* + prev[1]/2 is the rounding, and it is C's /2 - toward zero */
        q = ambe_sdiv((int32_t)(idx + 1) * (int32_t)(int16_t)code
                      + (int32_t)prev[1] / 2, prev[1]);
        q = (int16_t)(uint16_t)q;
        for (t = (int32_t)(int16_t)q * 16; t <= (int32_t)(int16_t)code;
             t += 0x10)
            q = (int16_t)(uint16_t)(q + 1);
        ambe_update_pitch_history(params, flags, (int16_t)(q - 1));
        return;
    }

    {
        int cls    = ambe_tone_class(params[1]);
        int16_t b0 = ambe_tone_bin(cls, code, 0);
        int16_t b1 = ambe_tone_bin(cls, code, 1);
        int16_t L  = params[2];
        int16_t i;

        for (i = 0; i < L; i = (int16_t)(i + 1))
            if (b0 != i && b1 != i)
                params[8 + i] = (int16_t)ambe_asr_hw(
                    (int32_t)((uint32_t)((int32_t)params[8 + i] * 0x51e) * 2u),
                    16);
        if ((uint16_t)(code - 0x80) < 0x20) {
            uint32_t sum = (uint32_t)((int32_t)params[8 + b0]
                                      * (int32_t)params[8 + b0])
                         + (uint32_t)((int32_t)params[8 + b1]
                                      * (int32_t)params[8 + b1]);
            int16_t root = (int16_t)ambe_sqrt_scaled((int32_t)sum, 8, 4);

            params[8 + b1] = root;
            params[8 + b0] = root;
        }
        if ((uint16_t)(code - 0x14) < 3)
            params[6] = (int16_t)ambe_sdiv_half(
                ambe_shl32((int32_t)code, 20), (int16_t)((params[3] + 1) * 0x100));
    }
}
