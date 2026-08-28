/*
 * ambe_params.c - 49 payload bits -> MBE speech model parameters.
 *
 * The AMBE+2 2450 payload carries nine quantiser indices b0..b8, scattered
 * across the 49 bits so that the Golay-protected prefix holds the perceptually
 * critical fields.  b0 selects the fundamental and the harmonic count, b1 the
 * voicing pattern, b2 the frame gain, b3/b4 the PRBA gain vectors and b5..b8
 * the higher-order spectral coefficients.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin) - this stage is a
 * behavioural reimplementation rather than a transcription, because the stock
 * code is dense fixed-point DSP; the functions that correspond to each step:
 *
 *   Vocoder_DecodeFrameParameters      0x0001994C  top-level parameter decode
 *   Vocoder_DecodeSpectralCodebookEntry 0x00022DB4 codebook fetch (PRBA/HOC)
 *   Vocoder_BlendSpectralCodebookEntries 0x0002343C
 *   Vocoder_ExpandSpectralCodebookEntry 0x0002AFAC
 *   Vocoder_ComputeHarmonicResampleRatio 0x000269B0 eq. 40/41 (prevL/curL)
 *   Vocoder_ResampleSpectralEnvelope    0x00026A84  eq. 43 blend of log2Ml
 *   Vocoder_ComputeFrameGainValue       0x00022E70  gamma accumulation
 *   Vocoder_UnpackSilenceDescriptor     0x000295F8  silence-frame path
 *   Vocoder_CodeSpectralCoefficients    0x000220D4  the codebook fetches, and
 *                                       the source of the bit widths used here:
 *                                       it passes 9, 7, 5, 4, 4, 3 for b3..b8
 *   Vocoder_CodebookVectorLookup        0x0002369C  the fetch itself
 *   Vocoder_LookupAndBlendGain          0x0002AE70  the gain codebook fetch
 *
 * The firmware has no LSP stage: the envelope is coded as a gain vector plus
 * higher-order coefficients combined through an inverse DCT, which is what is
 * implemented here.
 *
 * SPDX-License-Identifier: ISC
 */
#include <math.h>
#include <string.h>
#include "ambe.h"
#include "ambe_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int bits_to_int(const uint8_t *d, const int *idx, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++)
        v = (v << 1) | (d[idx[i]] & 1);
    return v;
}

/* Bit positions of each quantiser index inside the 49-bit payload. */
static const int B0_IDX[7] = {  0,  1,  2,  3, 37, 38, 39 };
static const int B1_IDX[5] = {  4,  5,  6,  7, 35 };
static const int B2_IDX[5] = {  8,  9, 10, 11, 36 };
static const int B3_IDX[9] = { 12, 13, 14, 15, 16, 17, 18, 19, 40 };
static const int B4_IDX[7] = { 20, 21, 22, 23, 41, 42, 43 };
static const int B5_IDX[5] = { 24, 25, 26, 27, 44 };
static const int B6_IDX[4] = { 28, 29, 30, 45 };
static const int B7_IDX[4] = { 31, 32, 33, 46 };
static const int B8_IDX[3] = { 34, 47, 48 };

void ambe_move_parms(const ambe_parms *src, ambe_parms *dst)
{
    int l;
    dst->w0     = src->w0;
    dst->L      = src->L;
    dst->gamma  = src->gamma;
    dst->repeat = src->repeat;
    for (l = 0; l <= AMBE_MAX_HARMONICS; l++) {
        dst->Ml[l]     = src->Ml[l];
        dst->Vl[l]     = src->Vl[l];
        dst->log2Ml[l] = src->log2Ml[l];
        dst->PHIl[l]   = src->PHIl[l];
        dst->PSIl[l]   = src->PSIl[l];
    }
}

void ambe_init_parms(ambe_parms *cur, ambe_parms *prev, ambe_parms *prev_enh)
{
    int l;
    memset(prev, 0, sizeof(*prev));
    prev->w0    = 0.09378f;
    prev->L     = 30;
    prev->gamma = 0.0f;
    for (l = 0; l <= AMBE_MAX_HARMONICS; l++) {
        prev->Ml[l]     = 0.0f;
        prev->Vl[l]     = 0;
        prev->log2Ml[l] = 0.0f;
        prev->PHIl[l]   = 0.0f;
        prev->PSIl[l]   = (float)(M_PI / 2.0);
    }
    prev->repeat = 0;
    ambe_move_parms(prev, cur);
    if (prev_enh)
        ambe_move_parms(prev, prev_enh);
}

ambe_frame_type ambe_decode_parms(const uint8_t ambe_d[AMBE_BITS],
                                  ambe_parms *cur, ambe_parms *prev,
                                  ambe_frame_info *info)
{
    int b0, b1, b2, b3, b4, b5, b6, b7, b8;
    int i, j, k, l, L, silence = 0;
    int Ji[5], intkl[AMBE_MAX_HARMONICS + 1];
    float f0, unvc;
    float Gm[9], Ri[9], Cik[5][18], Tl[AMBE_MAX_HARMONICS + 1];
    float flokl[AMBE_MAX_HARMONICS + 1], deltal[AMBE_MAX_HARMONICS + 1];
    float sum, Sum42, Sum43, BigGamma, deltaGamma, c1, c2;
    const float rconst = (float)(1.0 / (2.0 * 1.4142135623730951));

    cur->repeat = prev->repeat;

    b0 = bits_to_int(ambe_d, B0_IDX, 7);
    if (info) {
        memset(info->b, 0, sizeof(info->b));
        info->b[0] = b0;
    }

    if (b0 >= 120 && b0 <= 123) {
        if (info) info->type = AMBE_FRAME_ERASURE;
        return AMBE_FRAME_ERASURE;
    }
    if (b0 == 126 || b0 == 127) {
        if (info) info->type = AMBE_FRAME_TONE;
        return AMBE_FRAME_TONE;
    }
    if (b0 == 124 || b0 == 125) {
        silence  = 1;
        cur->w0  = (float)(2.0 * M_PI / 32.0);
        f0       = 1.0f / 32.0f;
        L        = 14;
        cur->L   = 14;
        for (l = 1; l <= L; l++)
            cur->Vl[l] = 0;
    } else {
        f0      = ambe_w0_table[b0];
        cur->w0 = (float)(f0 * 2.0 * M_PI);
        L       = ambe_l_table[b0];
        cur->L  = L;
    }

    unvc = (float)(0.2046 / sqrt((double)cur->w0));

    /* ---- b1: voicing decisions, one per 500 Hz band, mapped per harmonic */
    b1 = bits_to_int(ambe_d, B1_IDX, 5);
    if (!silence) {
        /*
         * Vocoder_DecodeSpectralCodebookEntry 0x00022DB4 left-justifies the b1
         * field to seven bits before indexing, and
         * Vocoder_ExpandSpectralCodebookEntry 0x0002AFAC reads the entry as
         * 2-bit crumbs, most significant first.  The low bit of each crumb is
         * the band's voiced flag.  b1 is five bits here, so the shift is 2.
         */
        unsigned int vuv = ambe_vuv_packed[(b1 << 2) & 127];
        for (l = 1; l <= L; l++) {
            int jl = (int)((float)l * 16.0f * f0);
            if (jl > 7) jl = 7;
            cur->Vl[l] = (uint8_t)((vuv >> (30 - 2 * jl)) & 1u);
        }
    }

    /* ---- b2: differential frame gain */
    b2         = bits_to_int(ambe_d, B2_IDX, 5);
    deltaGamma = ambe_dg_q11[b2] / AMBE_Q11;
    cur->gamma = deltaGamma + 0.5f * prev->gamma;

    /* ---- b3/b4: PRBA vectors -> Ri via an 8-point inverse cosine transform */
    b3 = bits_to_int(ambe_d, B3_IDX, 9);
    b4 = bits_to_int(ambe_d, B4_IDX, 7);
    Gm[1] = 0.0f;
    Gm[2] = ambe_prba24_q11[b3 * 3 + 0] / AMBE_Q11;
    Gm[3] = ambe_prba24_q11[b3 * 3 + 1] / AMBE_Q11;
    Gm[4] = ambe_prba24_q11[b3 * 3 + 2] / AMBE_Q11;
    Gm[5] = ambe_prba58_q11[b4 * 4 + 0] / AMBE_Q11;
    Gm[6] = ambe_prba58_q11[b4 * 4 + 1] / AMBE_Q11;
    Gm[7] = ambe_prba58_q11[b4 * 4 + 2] / AMBE_Q11;
    Gm[8] = ambe_prba58_q11[b4 * 4 + 3] / AMBE_Q11;

    for (i = 1; i <= 8; i++) {
        sum = 0.0f;
        for (k = 1; k <= 8; k++) {
            float am = (k == 1) ? 1.0f : 2.0f;
            sum += am * Gm[k] *
                   (float)cos((M_PI * (double)(k - 1) * ((double)i - 0.5)) / 8.0);
        }
        Ri[i] = sum;
    }

    memset(Cik, 0, sizeof(Cik));
    Cik[1][1] = 0.5f * (Ri[1] + Ri[2]);
    Cik[1][2] = rconst * (Ri[1] - Ri[2]);
    Cik[2][1] = 0.5f * (Ri[3] + Ri[4]);
    Cik[2][2] = rconst * (Ri[3] - Ri[4]);
    Cik[3][1] = 0.5f * (Ri[5] + Ri[6]);
    Cik[3][2] = rconst * (Ri[5] - Ri[6]);
    Cik[4][1] = 0.5f * (Ri[7] + Ri[8]);
    Cik[4][2] = rconst * (Ri[7] - Ri[8]);

    /* ---- b5..b8: higher-order coefficients, block lengths from L */
    b5 = bits_to_int(ambe_d, B5_IDX, 5);
    b6 = bits_to_int(ambe_d, B6_IDX, 4);
    b7 = bits_to_int(ambe_d, B7_IDX, 4);
    b8 = bits_to_int(ambe_d, B8_IDX, 3);

    Ji[1] = ambe_lmprbl[L * 4 + 0];
    Ji[2] = ambe_lmprbl[L * 4 + 1];
    Ji[3] = ambe_lmprbl[L * 4 + 2];
    Ji[4] = ambe_lmprbl[L * 4 + 3];

    for (k = 3; k <= Ji[1]; k++)
        Cik[1][k] = (k > 6) ? 0.0f : ambe_hoc_b5_q11[b5 * 4 + (k - 3)] / AMBE_Q11;
    for (k = 3; k <= Ji[2]; k++)
        Cik[2][k] = (k > 6) ? 0.0f : ambe_hoc_b6_q11[b6 * 4 + (k - 3)] / AMBE_Q11;
    for (k = 3; k <= Ji[3]; k++)
        Cik[3][k] = (k > 6) ? 0.0f : ambe_hoc_b7_q11[b7 * 4 + (k - 3)] / AMBE_Q11;
    for (k = 3; k <= Ji[4]; k++)
        Cik[4][k] = (k > 6) ? 0.0f : ambe_hoc_b8_q11[b8 * 4 + (k - 3)] / AMBE_Q11;

    if (info) {
        info->b[1] = b1; info->b[2] = b2; info->b[3] = b3; info->b[4] = b4;
        info->b[5] = b5; info->b[6] = b6; info->b[7] = b7; info->b[8] = b8;
    }

    /* ---- inverse DCT of each block gives the prediction residual Tl */
    l = 1;
    for (i = 1; i <= 4; i++) {
        int ji = Ji[i];
        for (j = 1; j <= ji; j++) {
            sum = 0.0f;
            for (k = 1; k <= ji; k++) {
                float ak = (k == 1) ? 1.0f : 2.0f;
                sum += ak * Cik[i][k] *
                       (float)cos((M_PI * (double)(k - 1) * ((double)j - 0.5)) / (double)ji);
            }
            Tl[l++] = sum;
        }
    }

    /*
     * The envelope is coded as a residual against the previous frame resampled
     * onto this frame's harmonic count (eq. 40-43).  When L grows, the previous
     * frame's top harmonic is held.
     */
    if (cur->L > prev->L) {
        for (l = prev->L + 1; l <= cur->L; l++) {
            prev->Ml[l]     = prev->Ml[prev->L];
            prev->log2Ml[l] = prev->log2Ml[prev->L];
        }
    }
    prev->log2Ml[0] = prev->log2Ml[1];
    prev->Ml[0]     = prev->Ml[1];

    Sum43 = 0.0f;
    for (l = 1; l <= cur->L; l++) {
        flokl[l]  = ((float)prev->L / (float)cur->L) * (float)l;
        intkl[l]  = (int)flokl[l];
        deltal[l] = flokl[l] - (float)intkl[l];
        Sum43 += (1.0f - deltal[l]) * prev->log2Ml[intkl[l]] +
                 deltal[l] * prev->log2Ml[intkl[l] + 1];
    }
    Sum43 = (0.65f / (float)cur->L) * Sum43;

    Sum42 = 0.0f;
    for (l = 1; l <= cur->L; l++)
        Sum42 += Tl[l];
    Sum42 /= (float)cur->L;

    BigGamma = cur->gamma - 0.5f * (float)(log((double)cur->L) / log(2.0)) - Sum42;

    for (l = 1; l <= cur->L; l++) {
        c1 = 0.65f * (1.0f - deltal[l]) * prev->log2Ml[intkl[l]];
        c2 = 0.65f * deltal[l] * prev->log2Ml[intkl[l] + 1];
        cur->log2Ml[l] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
        if (cur->Vl[l])
            cur->Ml[l] = (float)exp(0.693 * (double)cur->log2Ml[l]);
        else
            cur->Ml[l] = unvc * (float)exp(0.693 * (double)cur->log2Ml[l]);
    }

    if (info)
        info->type = silence ? AMBE_FRAME_SILENCE : AMBE_FRAME_VOICE;
    return silence ? AMBE_FRAME_SILENCE : AMBE_FRAME_VOICE;
}
