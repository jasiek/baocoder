/*
 * ambe_synth.c - MBE spectral enhancement and speech synthesis, in fixed point.
 *
 * Voiced harmonics are summed as overlap-added sinusoids across the 20 ms frame
 * boundary; unvoiced bands are filled with a band-limited multi-sine mix whose
 * phases come from the decoder's own PRNG.  That PRNG is a seeded xorshift so
 * that this decoder is bit-reproducible run to run (unlike mbelib, which calls
 * rand()); exact sample-level parity with mbelib is therefore not achievable
 * for frames containing unvoiced bands, and the tests compare those with an
 * energy/correlation tolerance instead.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *   Vocoder_SynthesizeFrame            0x00019DB8
 *   Vocoder_SynthesizeVoiced           0x0001DE10
 *   Vocoder_SynthesizeUnvoiced         0x0001AFE0
 *   Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8
 *   Vocoder_ApplySynthesisWindow       0x00029D1C
 *   Vocoder_ComputeHarmonicGains       0x0001D71C
 *   Math_TableInterpLookup             0x00019000  (the cos this uses)
 *
 * The stock implementation synthesises through an inverse FFT
 * (Dsp_FftInverse 0x00025704) where this one sums sinusoids directly; the
 * model is the same, the arithmetic is not.  What is shared is the number
 * system: integers throughout, with the radio's own cos and sqrt.
 *
 * Two things are exact here that were not in floating point:
 *
 *   Phase.  PSIl accumulates every frame without bound, so in a float it
 *   loses mantissa bits as a transmission goes on.  Here it is a wrapping
 *   uint32 in Q32 turns, which is exact modulo one turn for ever.
 *
 *   The window.  Every tap of the trapezoid is k/50 for an integer k, and
 *   both taps at a given sample carry the same 1/50, so the divisor factors
 *   out of the harmonic sum entirely.  It is applied once at the end together
 *   with the codec's x7 output gain, as a single x7/50.  The window
 *   contributes no rounding error at all.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_tables.h"

#define N 160

/* Q15 constants of the unvoiced mix, as the float version had them. */
#define UVSINE_Q15   121062   /* 1.3591409 * e = 3.694529                      */
#define UVRAND_Q15    65536   /* 2.0                                          */
#define QFACTOR3_Q15  12000   /* log(3)/3 = 0.3662041, the uvquality = 3 case  */

/*
 * 0.3375 turns.  The float version compares cw0*l against 2700*pi/4000
 * radians; divided by 2*pi that is exactly 2700/8000 = 0.3375 of a turn, so
 * in the Q19 f0 units the comparison is against this integer.
 */
#define UVTHRESHOLD_Q19 176947

/*
 * uvrand * (w0*l - threshold) as Q15 radians, from the Q19 turns the library
 * carries: 2 * 2*pi * X / 2^19 * 2^15 = (pi/4) * X, and pi/4 in Q19 is this.
 */
#define K_PI_OVER_4_Q19 411775

/* 0.96 * pi, Q24, for the spectral enhancement weight */
#define K_096PI_Q24 50598891

static uint32_t rng_next_u32(uint32_t *s)
{
    uint32_t x = *s;
    if (x == 0)
        x = 0x2450A17Bu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/*
 * A uniform draw on [0,1) as Q32 turns.  The float version computes
 * (x >> 8) / 2^24, which is exactly (x >> 8) << 8 here - the same value, so
 * the two implementations draw identical phases and their output can be
 * compared sample by sample.
 */
static uint32_t rng_turn(uint32_t *s)
{
    return (rng_next_u32(s) >> 8) << 8;
}

/* The same draw mapped to [-0.5, 0.5) turns, the float version's [-pi, pi). */
static uint32_t rng_phase(uint32_t *s)
{
    return rng_turn(s) - 0x80000000u;
}

/* Q15 magnitude of a draw on [0,1), for the unvoiced noise gain. */
static int32_t rng_unit_q15(uint32_t *s)
{
    return (int32_t)(rng_next_u32(s) >> 17);
}

/*
 * The overlap-add synthesis window, as integer numerators over 50.
 *
 * This belongs to *this* decoder's time-domain synthesis, not to the codec:
 * the stock firmware synthesises through an inverse FFT (Dsp_FftInverse
 * 0x00025704) and has no equivalent array.  What Vocoder_ApplySynthesisWindow
 * 0x00029D1C actually applies is a ten-tap antisymmetric FIR at SRAM
 * 0x18003958 (file 0x067018) whose taps are exactly 18411/(2k+1) for
 * k = 0..9 - the odd-harmonic series, i.e. a Hilbert transformer, not a
 * window.  The function's name is a misnomer.
 *
 * mbelib's Ws is a plain trapezoid: 56 zeros, a linear ramp in steps of 0.02,
 * a flat top, then the mirror image.  0.02 is 1/50, so every tap is an
 * integer over 50 and the table below is exact where a Q15 copy would not be.
 */
static uint8_t ws_num[321];
static int     ws_ready;

static void ws_init(void)
{
    int n;
    if (ws_ready)
        return;
    for (n = 0; n < 321; n++) {
        if (n < 56 || n > 264)      ws_num[n] = 0;
        else if (n <= 105)          ws_num[n] = (uint8_t)(n - 55);
        else if (n >= 215)          ws_num[n] = (uint8_t)(265 - n);
        else                        ws_num[n] = 50;
    }
    ws_ready = 1;
}

/*
 * Spectral amplitude enhancement.  The weight is
 *
 *   Wl = sqrt(Ml) * [ 0.96*pi*(R0^2 + R1^2 - 2*R0*R1*cos(w0*l))
 *                     / (w0 * R0 * (R0^2 - R1^2)) ] ^ 0.25
 *
 * and the fourth root is two square roots, which is why no pow() is needed.
 * The moments span too many octaves for one Q format, so they are carried in
 * the radio's (mantissa, exponent) pairs.
 */
void ambe_enhance_spectrum(ambe_parms *cur)
{
    ambe_bf Rm0, Rm1, R2m0, R2m1, num, den, w0bf, gain, sum;
    int64_t a0 = 0, a1 = 0, asum = 0;
    int32_t A[AMBE_MAX_HARMONICS + 1];
    int l, sh = 0;
    int32_t w0_q24 = ambe_w0_q24(cur);

    /* work on mantissas scaled so that sum(A^2) cannot overflow int64 */
    {
        int32_t mx = 0;
        for (l = 1; l <= cur->L; l++) {
            int32_t v = cur->Ml[l] < 0 ? -cur->Ml[l] : cur->Ml[l];
            if (v > mx) mx = v;
        }
        while ((mx >> sh) > 0x7fff)
            sh++;
    }
    for (l = 1; l <= cur->L; l++)
        A[l] = cur->Ml[l] >> sh;

    for (l = 1; l <= cur->L; l++) {
        int64_t m2 = (int64_t)A[l] * A[l];
        a0 += m2;
        a1 += (m2 * ambe_cos_q15((int)(((int64_t)cur->f0 * l * 32768) >> AMBE_Q_F0))) >> 15;
    }
    if (a0 == 0)
        return;

    Rm0  = ambe_bf_from_i64(a0, 0);
    Rm1  = ambe_bf_from_i64(a1, 0);
    R2m0 = ambe_bf_mul(Rm0, Rm0);
    R2m1 = ambe_bf_mul(Rm1, Rm1);
    w0bf = ambe_bf_from_q(w0_q24, AMBE_Q_LOG);

    den = ambe_bf_mul(ambe_bf_mul(w0bf, Rm0), ambe_bf_sub(R2m0, R2m1));
    if (den.mant == 0)
        return;

    for (l = 1; l <= cur->L; l++) {
        ambe_bf w, cosbf, t;
        int32_t Wl_q15;

        if (cur->Ml[l] == 0)
            continue;

        cosbf = ambe_bf_from_q(
            ambe_cos_q15((int)(((int64_t)cur->f0 * l * 32768) >> AMBE_Q_F0)), 15);
        t = ambe_bf_mul(ambe_bf_mul(Rm0, Rm1), cosbf);
        num = ambe_bf_sub(ambe_bf_add(R2m0, R2m1),
                          ambe_bf_add(t, t));            /* -2*R0*R1*cos */
        num = ambe_bf_mul(num, ambe_bf_from_q(K_096PI_Q24, AMBE_Q_LOG));

        w = ambe_bf_div(num, den);
        if (w.mant <= 0)
            continue;
        w = ambe_bf_sqrt(ambe_bf_sqrt(w));               /* ^0.25 */
        w = ambe_bf_mul(w, ambe_bf_sqrt(ambe_bf_from_i64((int64_t)A[l], 0)));

        Wl_q15 = ambe_bf_to_q(w, 15);

        if ((8 * l) <= cur->L) {
            /* the lowest octave is left alone */
        } else if (Wl_q15 > 39322) {                     /* 1.2 */
            cur->Ml[l] = (int32_t)(((int64_t)cur->Ml[l] * 39322) >> 15);
        } else if (Wl_q15 < 16384) {                     /* 0.5 */
            cur->Ml[l] = (int32_t)(((int64_t)cur->Ml[l] * 16384) >> 15);
        } else {
            cur->Ml[l] = (int32_t)(((int64_t)cur->Ml[l] * Wl_q15) >> 15);
        }
    }

    /* renormalise so the enhanced frame carries the original energy */
    for (l = 1; l <= cur->L; l++) {
        int32_t v = (cur->Ml[l] >> sh);
        asum += (int64_t)v * v;
    }
    if (asum == 0)
        return;
    sum  = ambe_bf_from_i64(asum, 0);
    gain = ambe_bf_sqrt(ambe_bf_div(Rm0, sum));
    {
        int32_t g_q15 = ambe_bf_to_q(gain, 15);
        for (l = 1; l <= cur->L; l++)
            cur->Ml[l] = (int32_t)(((int64_t)cur->Ml[l] * g_q15) >> 15);
    }
}

/*
 * Bring both parameter sets' amplitudes onto one exponent and scale them so
 * the harmonic sum cannot overflow an int64 accumulator.  Returns the shift
 * such that a mantissa of 1 is worth 2^shift in the output's units.
 */
static int align_amplitudes(int32_t *ca, int32_t *pa, const ambe_parms *cur,
                            const ambe_parms *prev, int maxl)
{
    int e = cur->Ml_exp > prev->Ml_exp ? cur->Ml_exp : prev->Ml_exp;
    int l, sh = 0;
    int32_t mx = 0;

    for (l = 0; l <= maxl; l++) {
        int dc = e - cur->Ml_exp, dp = e - prev->Ml_exp;
        ca[l] = (dc >= 31) ? 0 : (cur->Ml[l] >> dc);
        pa[l] = (dp >= 31) ? 0 : (prev->Ml[l] >> dp);
        if (ca[l] > mx) mx = ca[l];
        if (pa[l] > mx) mx = pa[l];
        if (-ca[l] > mx) mx = -ca[l];
        if (-pa[l] > mx) mx = -pa[l];
    }
    /* 23 bits of mantissa leaves room for x50 window, x2^15 cos and 56 terms */
    while ((mx >> sh) > 0x7fffff)
        sh++;
    for (l = 0; l <= maxl; l++) {
        ca[l] >>= sh;
        pa[l] >>= sh;
    }
    return e - AMBE_Q_ML + sh;
}

void ambe_synthesize(int16_t out[AMBE_PCM_SAMPLES], ambe_parms *cur,
                     ambe_parms *prev, int uvquality, uint32_t *rng)
{
    int32_t ca[AMBE_MAX_HARMONICS + 1], pa[AMBE_MAX_HARMONICS + 1];
    int64_t acc[N];
    uint32_t rphase[64], rphase2[64];
    uint32_t cw0, pw0;
    int i, l, n, maxl, numUv, amp_shift;
    int32_t qfactor_q15;

    ws_init();

    if (uvquality < 1 || uvquality > 64)
        uvquality = 3;
    /*
     * qfactor is log(uvquality)/uvquality, and 1/e when uvquality is 1.  Only
     * the default 3 has a constant here; the rest come from the radio's log2,
     * converted from log2 to ln by * ln2.
     */
    if (uvquality == 3)      qfactor_q15 = QFACTOR3_Q15;
    else if (uvquality == 1) qfactor_q15 = 12055;          /* 1/e */
    else {
        int32_t lg = ambe_log2(uvquality, 15);             /* Q16 log2 */
        int64_t ln = ((int64_t)lg * 22713) >> 15;          /* * ln2, Q16 */
        qfactor_q15 = (int32_t)((ln >> 1) / uvquality);
    }

    numUv = 0;
    for (l = 1; l <= cur->L; l++)
        if (cur->Vl[l] == 0)
            numUv++;

    cw0 = (uint32_t)cur->f0  << (32 - AMBE_Q_F0);   /* Q32 turns per sample */
    pw0 = (uint32_t)prev->f0 << (32 - AMBE_Q_F0);

    for (n = 0; n < N; n++)
        acc[n] = 0;

    /* pad whichever parameter set has fewer harmonics (eq. 128/129) */
    if (cur->L > prev->L) {
        maxl = cur->L;
        for (l = prev->L + 1; l <= maxl; l++) {
            prev->Ml[l] = 0;
            prev->Vl[l] = 1;
        }
    } else {
        maxl = prev->L;
        for (l = cur->L + 1; l <= maxl; l++) {
            cur->Ml[l] = 0;
            cur->Vl[l] = 1;
        }
    }

    amp_shift = align_amplitudes(ca, pa, cur, prev, maxl);

    /* phase continuation, eq. 139/140.  Q32 turns, so this wraps exactly. */
    for (l = 1; l <= AMBE_MAX_HARMONICS; l++) {
        cur->PSIl[l] = prev->PSIl[l] +
                       (uint32_t)((pw0 + cw0) * (uint32_t)(l * (N / 2)));
        if (l <= cur->L / 4) {
            cur->PHIl[l] = cur->PSIl[l];
        } else {
            int32_t ph = (int32_t)rng_phase(rng);
            cur->PHIl[l] = cur->PSIl[l] +
                           (uint32_t)(int32_t)(((int64_t)ph * numUv) / cur->L);
        }
    }

    for (l = 1; l <= maxl; l++) {
        uint32_t cw0l = cw0 * (uint32_t)l;
        uint32_t pw0l = pw0 * (uint32_t)l;
        int c_over = ((int64_t)cur->f0  * l) > UVTHRESHOLD_Q19;
        int p_over = ((int64_t)prev->f0 * l) > UVTHRESHOLD_Q19;
        /* (w0*l - threshold) * 2, Q15, the unvoiced noise gain */
        int32_t c_gain = c_over ? (int32_t)((((int64_t)cur->f0 * l - UVTHRESHOLD_Q19)
                                             * K_PI_OVER_4_Q19) >> AMBE_Q_F0) : 0;
        int32_t p_gain = p_over ? (int32_t)((((int64_t)prev->f0 * l - UVTHRESHOLD_Q19)
                                             * K_PI_OVER_4_Q19) >> AMBE_Q_F0) : 0;

        if (cur->Vl[l] == 0 && prev->Vl[l] == 1) {
            /* voiced -> unvoiced transition, eq. 131 */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                int64_t C1, C3 = 0;
                C1 = (int64_t)ws_num[n + N] * pa[l] *
                     ambe_cos_q15((int)((pw0l * (uint32_t)n + prev->PHIl[l]) >> 17));
                for (i = 0; i < uvquality; i++) {
                    int32_t num = l * 2 * uvquality + 2 * i - (uvquality - 1);
                    uint32_t ph = (uint32_t)(((int64_t)cw0 * n * num) / (2 * uvquality));
                    C3 += ambe_cos_q15((int)((ph + rphase[i]) >> 17));
                    if (c_over)
                        C3 += ((int64_t)c_gain * rng_unit_q15(rng)) >> 15;
                }
                C3 = (((C3 * UVSINE_Q15) >> 15) * qfactor_q15) >> 15;
                acc[n] += C1 + C3 * ws_num[n] * ca[l];
            }
        } else if (cur->Vl[l] == 1 && prev->Vl[l] == 0) {
            /* unvoiced -> voiced transition, eq. 132 */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                int64_t C1, C3 = 0;
                C1 = (int64_t)ws_num[n] * ca[l] *
                     ambe_cos_q15((int)((cw0l * (uint32_t)(n - N) + cur->PHIl[l]) >> 17));
                for (i = 0; i < uvquality; i++) {
                    int32_t num = l * 2 * uvquality + 2 * i - (uvquality - 1);
                    uint32_t ph = (uint32_t)(((int64_t)pw0 * n * num) / (2 * uvquality));
                    C3 += ambe_cos_q15((int)((ph + rphase[i]) >> 17));
                    if (p_over)
                        C3 += ((int64_t)p_gain * rng_unit_q15(rng)) >> 15;
                }
                C3 = (((C3 * UVSINE_Q15) >> 15) * qfactor_q15) >> 15;
                acc[n] += C1 + C3 * ws_num[n + N] * pa[l];
            }
        } else if (cur->Vl[l] == 1 || prev->Vl[l] == 1) {
            /* voiced in both frames, eq. 133 */
            for (n = 0; n < N; n++) {
                int64_t C1, C2;
                C1 = (int64_t)ws_num[n + N] * pa[l] *
                     ambe_cos_q15((int)((pw0l * (uint32_t)n + prev->PHIl[l]) >> 17));
                C2 = (int64_t)ws_num[n] * ca[l] *
                     ambe_cos_q15((int)((cw0l * (uint32_t)(n - N) + cur->PHIl[l]) >> 17));
                acc[n] += C1 + C2;
            }
        } else {
            /* unvoiced in both frames */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (i = 0; i < uvquality; i++)
                rphase2[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                int64_t C3 = 0, C4 = 0;
                for (i = 0; i < uvquality; i++) {
                    int32_t num = l * 2 * uvquality + 2 * i - (uvquality - 1);
                    uint32_t ph = (uint32_t)(((int64_t)pw0 * n * num) / (2 * uvquality));
                    C3 += ambe_cos_q15((int)((ph + rphase[i]) >> 17));
                    if (p_over)
                        C3 += ((int64_t)p_gain * rng_unit_q15(rng)) >> 15;
                }
                C3 = (((C3 * UVSINE_Q15) >> 15) * qfactor_q15) >> 15;
                for (i = 0; i < uvquality; i++) {
                    int32_t num = l * 2 * uvquality + 2 * i - (uvquality - 1);
                    uint32_t ph = (uint32_t)(((int64_t)cw0 * n * num) / (2 * uvquality));
                    C4 += ambe_cos_q15((int)((ph + rphase2[i]) >> 17));
                    if (c_over)
                        C4 += ((int64_t)c_gain * rng_unit_q15(rng)) >> 15;
                }
                C4 = (((C4 * UVSINE_Q15) >> 15) * qfactor_q15) >> 15;
                acc[n] += C3 * ws_num[n + N] * pa[l] + C4 * ws_num[n] * ca[l];
            }
        }
    }

    /*
     * One scaling for the whole frame: undo the cos's Q15 and the amplitude
     * mantissas' exponent, divide the window's 1/50 out, and apply the codec's
     * x7 output gain - all at once, with a single rounding.
     */
    for (n = 0; n < N; n++) {
        int64_t v = acc[n];
        int sh = 15 - amp_shift;
        v *= 7;
        if (sh > 0)
            v = (v + ((int64_t)1 << (sh - 1))) >> sh;
        else if (sh < 0)
            v <<= -sh;
        v /= 50;
        if (v >  32760) v =  32760;
        if (v < -32760) v = -32760;
        out[n] = (int16_t)v;
    }
}
