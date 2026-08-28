/*
 * ambe_synth.c - MBE spectral enhancement and speech synthesis.
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
 *   g_awSineTable512                   0x18001630  (512-entry sine table the
 *                                       stock harmonic synthesiser indexes with
 *                                       a 9-bit wrapping phase accumulator)
 *
 * The stock implementation is fixed point and uses an FFT-based synthesis
 * (Dsp_FftInverse 0x00025704) where this one sums sinusoids directly; the model
 * is the same, the arithmetic is not.
 *
 * SPDX-License-Identifier: ISC
 */
#include <math.h>
#include "ambe.h"
#include "ambe_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#define N 160

static float rng_next(uint32_t *s)
{
    uint32_t x = *s;
    if (x == 0)
        x = 0x2450A17Bu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)((double)(x >> 8) / 16777216.0);   /* [0,1) */
}

static float rng_phase(uint32_t *s)
{
    return rng_next(s) * (float)(2.0 * M_PI) - (float)M_PI;
}

void ambe_enhance_spectrum(ambe_parms *cur)
{
    float Rm0 = 0.0f, Rm1 = 0.0f, R2m0, R2m1, Wl, sum, gain, M;
    int l;

    for (l = 1; l <= cur->L; l++) {
        float m2 = cur->Ml[l] * cur->Ml[l];
        Rm0 += m2;
        Rm1 += m2 * (float)cos((double)cur->w0 * (double)l);
    }
    R2m0 = Rm0 * Rm0;
    R2m1 = Rm1 * Rm1;

    for (l = 1; l <= cur->L; l++) {
        if (cur->Ml[l] == 0.0f)
            continue;
        Wl = (float)sqrt((double)cur->Ml[l]) *
             (float)pow((0.96 * M_PI *
                         ((double)(R2m0 + R2m1) -
                          2.0 * (double)Rm0 * (double)Rm1 *
                          cos((double)cur->w0 * (double)l))) /
                        ((double)cur->w0 * (double)Rm0 * (double)(R2m0 - R2m1)),
                        0.25);
        if ((8 * l) <= cur->L) {
            /* the lowest octave is left alone */
        } else if (Wl > 1.2f) {
            cur->Ml[l] = 1.2f * cur->Ml[l];
        } else if (Wl < 0.5f) {
            cur->Ml[l] = 0.5f * cur->Ml[l];
        } else {
            cur->Ml[l] = Wl * cur->Ml[l];
        }
    }

    sum = 0.0f;
    for (l = 1; l <= cur->L; l++) {
        M = cur->Ml[l] < 0.0f ? -cur->Ml[l] : cur->Ml[l];
        sum += M * M;
    }
    gain = (sum == 0.0f) ? 1.0f : (float)sqrt((double)Rm0 / (double)sum);
    for (l = 1; l <= cur->L; l++)
        cur->Ml[l] *= gain;
}

/*
 * The overlap-add synthesis window.  This belongs to *this* decoder's
 * time-domain synthesis, not to the codec: the stock firmware synthesises
 * through an inverse FFT (Dsp_FftInverse 0x00025704) and has no equivalent
 * array.  What Vocoder_ApplySynthesisWindow 0x00029D1C actually applies is a
 * ten-tap antisymmetric FIR at SRAM 0x18003958 (file 0x067018) whose taps are
 * exactly 18411/(2k+1) for k = 0..9 - the odd-harmonic series, i.e. a Hilbert
 * transformer, not a window.  The function's name is a misnomer.
 *
 * mbelib's Ws is a plain trapezoid: 56 zeros, a linear ramp in steps of 0.02,
 * a flat top, then the mirror image.  It is generated here from that definition
 * rather than carried as 321 copied constants; the two agree exactly at all 321
 * points.
 */
static float ws_window[321];
static int   ws_ready;

static void ws_init(void)
{
    int n;
    if (ws_ready)
        return;
    for (n = 0; n < 321; n++) {
        if (n < 56 || n > 264)      ws_window[n] = 0.0f;
        else if (n <= 105)          ws_window[n] = (float)(n - 55) * 0.02f;
        else if (n >= 215)          ws_window[n] = (float)(265 - n) * 0.02f;
        else                        ws_window[n] = 1.0f;
    }
    ws_ready = 1;
}

void ambe_synthesize(float out[AMBE_PCM_SAMPLES], ambe_parms *cur,
                     ambe_parms *prev, int uvquality, uint32_t *rng)
{
    const float *Ws;
    const float uvsine = (float)(1.3591409 * M_E);
    const float uvrand = 2.0f;
    const float uvthreshold = (float)(2700.0 * M_PI / 4000.0);
    float loguv, uvstep, uvoffset, qfactor;
    float rphase[64], rphase2[64];
    float cw0, pw0, cw0l, pw0l, C1, C2, C3, C4;
    int i, l, n, maxl, numUv;

    ws_init();
    Ws = ws_window;

    if (uvquality < 1 || uvquality > 64)
        uvquality = 3;
    loguv    = (uvquality == 1) ? (float)(1.0 / M_E)
                                : (float)(log((double)uvquality) / (double)uvquality);
    uvstep   = 1.0f / (float)uvquality;
    qfactor  = loguv;
    uvoffset = (uvstep * (float)(uvquality - 1)) / 2.0f;

    numUv = 0;
    for (l = 1; l <= cur->L; l++)
        if (cur->Vl[l] == 0)
            numUv++;

    cw0 = cur->w0;
    pw0 = prev->w0;

    for (n = 0; n < N; n++)
        out[n] = 0.0f;

    /* pad whichever parameter set has fewer harmonics (eq. 128/129) */
    if (cur->L > prev->L) {
        maxl = cur->L;
        for (l = prev->L + 1; l <= maxl; l++) {
            prev->Ml[l] = 0.0f;
            prev->Vl[l] = 1;
        }
    } else {
        maxl = prev->L;
        for (l = cur->L + 1; l <= maxl; l++) {
            cur->Ml[l] = 0.0f;
            cur->Vl[l] = 1;
        }
    }

    /* phase continuation, eq. 139/140 */
    for (l = 1; l <= AMBE_MAX_HARMONICS; l++) {
        cur->PSIl[l] = prev->PSIl[l] + (pw0 + cw0) * ((float)(l * N) / 2.0f);
        if (l <= cur->L / 4)
            cur->PHIl[l] = cur->PSIl[l];
        else
            cur->PHIl[l] = cur->PSIl[l] + ((float)numUv * rng_phase(rng)) / (float)cur->L;
    }

    for (l = 1; l <= maxl; l++) {
        cw0l = cw0 * (float)l;
        pw0l = pw0 * (float)l;

        if (cur->Vl[l] == 0 && prev->Vl[l] == 1) {
            /* voiced -> unvoiced transition, eq. 131 */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                C1 = Ws[n + N] * prev->Ml[l] *
                     (float)cos((double)(pw0l * (float)n) + (double)prev->PHIl[l]);
                C3 = 0.0f;
                for (i = 0; i < uvquality; i++) {
                    C3 += (float)cos((double)(cw0 * (float)n *
                                              ((float)l + (float)i * uvstep - uvoffset)) +
                                     (double)rphase[i]);
                    if (cw0l > uvthreshold)
                        C3 += (cw0l - uvthreshold) * uvrand * rng_next(rng);
                }
                C3 *= uvsine * Ws[n] * cur->Ml[l] * qfactor;
                out[n] += C1 + C3;
            }
        } else if (cur->Vl[l] == 1 && prev->Vl[l] == 0) {
            /* unvoiced -> voiced transition, eq. 132 */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                C1 = Ws[n] * cur->Ml[l] *
                     (float)cos((double)(cw0l * (float)(n - N)) + (double)cur->PHIl[l]);
                C3 = 0.0f;
                for (i = 0; i < uvquality; i++) {
                    C3 += (float)cos((double)(pw0 * (float)n *
                                              ((float)l + (float)i * uvstep - uvoffset)) +
                                     (double)rphase[i]);
                    if (pw0l > uvthreshold)
                        C3 += (pw0l - uvthreshold) * uvrand * rng_next(rng);
                }
                C3 *= uvsine * Ws[n + N] * prev->Ml[l] * qfactor;
                out[n] += C1 + C3;
            }
        } else if (cur->Vl[l] == 1 || prev->Vl[l] == 1) {
            /* voiced in both frames, eq. 133 */
            for (n = 0; n < N; n++) {
                C1 = Ws[n + N] * prev->Ml[l] *
                     (float)cos((double)(pw0l * (float)n) + (double)prev->PHIl[l]);
                C2 = Ws[n] * cur->Ml[l] *
                     (float)cos((double)(cw0l * (float)(n - N)) + (double)cur->PHIl[l]);
                out[n] += C1 + C2;
            }
        } else {
            /* unvoiced in both frames */
            for (i = 0; i < uvquality; i++)
                rphase[i] = rng_phase(rng);
            for (i = 0; i < uvquality; i++)
                rphase2[i] = rng_phase(rng);
            for (n = 0; n < N; n++) {
                C3 = 0.0f;
                for (i = 0; i < uvquality; i++) {
                    C3 += (float)cos((double)(pw0 * (float)n *
                                              ((float)l + (float)i * uvstep - uvoffset)) +
                                     (double)rphase[i]);
                    if (pw0l > uvthreshold)
                        C3 += (pw0l - uvthreshold) * uvrand * rng_next(rng);
                }
                C3 *= uvsine * Ws[n + N] * prev->Ml[l] * qfactor;
                C4 = 0.0f;
                for (i = 0; i < uvquality; i++) {
                    C4 += (float)cos((double)(cw0 * (float)n *
                                              ((float)l + (float)i * uvstep - uvoffset)) +
                                     (double)rphase2[i]);
                    if (cw0l > uvthreshold)
                        C4 += (cw0l - uvthreshold) * uvrand * rng_next(rng);
                }
                C4 *= uvsine * Ws[n] * cur->Ml[l] * qfactor;
                out[n] += C3 + C4;
            }
        }
    }
}

void ambe_float_to_s16(const float in[AMBE_PCM_SAMPLES], int16_t out[AMBE_PCM_SAMPLES])
{
    int i;
    for (i = 0; i < AMBE_PCM_SAMPLES; i++) {
        float a = 7.0f * in[i];
        if (a > 32760.0f)  a = 32760.0f;
        if (a < -32760.0f) a = -32760.0f;
        out[i] = (int16_t)a;
    }
}
