/*
 * ambe_postfilter.c - the output filter the radio runs over synthesised samples.
 *
 * FUN_00018a2c, still unnamed in the Ghidra database because nothing but
 * execution identified it.  Vocoder_SynthesizeFrame 0x00019DB8 calls it between
 * the harmonic synthesis and the final scaling:
 *
 *     Vocoder_SynthesizeUnvoiced(acc, n, ctx + 0x648, params, pitch);
 *     Vocoder_SynthesizeVoiced  (acc, n, ctx + 0x18,  params, prev, pitch);
 *     FUN_00018a2c(acc, acc, ctx, n);                 <- this
 *     Math_ArrayShiftSaturateInt(pOut, 0xf, acc, 0x11, n);
 *
 * It is two second-order sections in cascade over 32-bit samples: a DC blocker
 * followed by a shaping biquad, with Q15 coefficients 0x78AB, 0x7C20 and 0x78AF
 * (0.9427, 0.9699 and 0.9428) and six words of state at the very start of the
 * channel context.
 *
 * It is not synthesis-only.  Vocoder_ScaleSamplesForAnalysis 0x00018BCC calls
 * it twice as well, so the same cascade shapes the input audio before analysis;
 * the state it uses there is a different block.  That makes this the codec's
 * general shaping filter rather than an output stage.
 *
 * Two things are load-bearing and neither is obvious from the source:
 *
 *   The second section continues from the UNSATURATED 64-bit result of the
 *   first, while the value stored into the state words is the saturated one.
 *   Saturating once and using that result for both is the natural way to write
 *   this, and it is wrong.
 *
 *   Each coefficient product is truncated to 32 bits before it joins the
 *   64-bit accumulation - the firmware reassembles bits [46:15] of the product
 *   into one register and then sign-extends that.
 *
 * Verified bit-exact against the firmware executed under the p-code emulator:
 * tests/test_postfilter.c replays every captured call, both from the captured
 * state and carrying its own, and requires the predicted state to match too.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"

#define C_DCBLOCK  0x78AB
#define C_POLE     0x7C20
#define C_ZERO     0x78AF

static int32_t sat32(int64_t v)
{
    if (v > (int64_t)0x7FFFFFFF)  return (int32_t)0x7FFFFFFF;
    if (v < -(int64_t)0x80000000) return (int32_t)0x80000000;
    return (int32_t)v;
}

/* bits [46:15] of the product, as the firmware assembles them, sign-extended */
static int64_t coef_mul(int32_t s, int32_t c)
{
    return (int64_t)(int32_t)(uint32_t)((uint64_t)((int64_t)s * c) >> 15);
}

void ambe_postfilter_reset(ambe_postfilter_state *f)
{
    f->s[0] = f->s[1] = f->s[2] = f->s[3] = f->s[4] = f->s[5] = 0;
}

void ambe_postfilter(ambe_postfilter_state *f, int32_t *acc, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int32_t x = acc[i];
        int32_t s2 = f->s[2], s3 = f->s[3], s4 = f->s[4], s5 = f->s[5];
        int64_t a, out64;
        int32_t y1;

        a  = (int64_t)x - f->s[0] + coef_mul(f->s[1], C_DCBLOCK);
        y1 = sat32(a);                       /* saturated: what the state keeps */

        /* ...but the second section continues from the unsaturated `a` */
        out64  = a + (-2 * (int64_t)s2 + (int64_t)s4);
        out64 += coef_mul(s3, C_POLE);
        out64 += coef_mul(s3, C_POLE);
        out64 -= coef_mul(s5, C_ZERO);

        f->s[0] = x;
        f->s[1] = y1;
        f->s[4] = s2;
        f->s[2] = y1;
        f->s[5] = s3;
        f->s[3] = sat32(out64);
        acc[i]  = f->s[3];
    }
}

/*
 * Math_ArrayShiftSaturateInt, the last thing Vocoder_SynthesizeFrame does:
 * the accumulator becomes int16 PCM.
 *
 *     Math_ArrayShiftSaturateInt(pOutPcm, 0xf, acc, 0x11, nSampleCount)
 *
 * The two constants look like Q formats whose difference is the shift, which
 * would be two.  Measured against 166 calls and 13 280 samples it is a shift
 * of FOURTEEN, with rounding - truncation reproduces none of them.  So this is
 * what the machine does, not what the arguments suggest.
 */
void ambe_synth_output(int16_t *pcm, const int32_t *acc, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int32_t v = (int32_t)(((int64_t)acc[i] + (1 << 13)) >> 14);
        pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}
