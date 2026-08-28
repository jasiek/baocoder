/*
 * ambe_encode_params.c - MBE model parameters -> the nine AMBE indices -> 49 bits.
 *
 * The exact inverse of ambe_params.c.  The firmware runs the same code in both
 * directions: Vocoder_CodeSpectralCoefficients 0x000220D4 and
 * Vocoder_CodebookVectorLookup 0x0002369C take a direction flag, and on the
 * encode side the lookup searches the codebook for the nearest entry and emits
 * its index with Dsp_UnpackBitsAdvance instead of reading one with
 * Dsp_PackBitsAdvance.  That is what this file does, with the codebooks
 * extracted from the image.
 *
 * Deriving the inverse of the envelope stage is the only part that needs care.
 * The decoder computes, with P[l] the 0.65-weighted resampled previous frame:
 *
 *     Sum43     = mean(P)
 *     BigGamma  = gamma - 0.5*log2(L) - mean(Tl)
 *     log2Ml[l] = Tl[l] + P[l] - mean(P) + BigGamma
 *
 * so with D[l] = log2Ml[l] - P[l] + mean(P):
 *
 *     D[l]      = Tl[l] - mean(Tl) + gamma - 0.5*log2(L)
 *     mean(D)   = gamma - 0.5*log2(L)          -> gamma is recoverable
 *     D - mean(D) = Tl - mean(Tl)              -> Tl up to its mean
 *
 * mean(Tl) is genuinely not carried: adding a constant to every Tl cancels in
 * the decoder.  That is not a problem, because the constant only moves Gm[1],
 * which the codec forces to zero.  Adding c to every block's DC term adds c to
 * every Ri, and Gm[m] = (1/8) * sum_i Ri[i] * cos(pi*(m-1)*(i-0.5)/8) has
 * sum_i cos(...) = 0 for every m > 1.  So Gm[2..8] - the only part b3 and b4
 * carry - is exactly recoverable, and the higher-order coefficients Cik[i][k>=2]
 * are unaffected by the shift as well.
 *
 * SPDX-License-Identifier: ISC
 */
#include <math.h>
#include <string.h>
#include "ambe.h"
#include "ambe_tables.h"
#include "ambe_bitpos.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void put_bits(uint8_t *d, const int *idx, int n, int v)
{
    int i;
    for (i = 0; i < n; i++)
        d[idx[i]] = (uint8_t)((v >> (n - 1 - i)) & 1);
}

/*
 * Nearest codebook entry under squared euclidean distance.  `stride` is the
 * table's row length; `dim` is how many of those the decoder will actually
 * consume, which for the HOC tables depends on the block length and is often
 * fewer than the row.  Comparing over the unused columns would pick a
 * different entry than the one the radio sent.
 */
static int nearest(const short *tbl_q11, int entries, int stride, int dim,
                   const float *v)
{
    int best = 0, i, k;
    double bestd = 1e300;
    for (i = 0; i < entries; i++) {
        double d = 0.0;
        for (k = 0; k < dim; k++) {
            double e = (double)tbl_q11[i * stride + k] / (double)AMBE_Q11 - v[k];
            d += e * e;
        }
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* b0: the pitch quantiser is a monotone law, so scan its 120 values */
static int quantise_b0(float w0)
{
    double target = log((double)w0 / (2.0 * M_PI));
    int best = 0, b;
    double bestd = 1e300;
    for (b = 0; b < 120; b++) {
        float f0;
        int L;
        double d;
        ambe_pitch_from_b0(b, 7, &f0, &L);
        d = fabs(log((double)f0) - target);
        if (d < bestd) { bestd = d; best = b; }
    }
    return best;
}

void ambe_encode_silence(uint8_t ambe_d[AMBE_BITS])
{
    memset(ambe_d, 0, AMBE_BITS);
    put_bits(ambe_d, ambe_b0_idx, 7, 124);
}

int ambe_encode_parms(const ambe_parms *cur, ambe_parms *prev,
                      uint8_t ambe_d[AMBE_BITS], ambe_frame_info *info)
{
    int b[9];
    int i, j, k, l, L, b0;
    int Ji[5], intkl[AMBE_MAX_HARMONICS + 1];
    float f0, unvc;
    float P[AMBE_MAX_HARMONICS + 1], D[AMBE_MAX_HARMONICS + 1];
    float Tl[AMBE_MAX_HARMONICS + 1];
    float flokl, deltal[AMBE_MAX_HARMONICS + 1];
    float Cik[5][18], Ri[9], Gm[9], probe[4];
    double meanP, meanD, gamma, dgamma;

    memset(b, 0, sizeof(b));
    memset(Cik, 0, sizeof(Cik));

    /* ---- b0: pitch, and the harmonic count it forces */
    b0 = quantise_b0(cur->w0);
    ambe_pitch_from_b0(b0, 7, &f0, &L);
    b[0] = b0;

    /* ---- b1: the voicing pattern whose bands match best */
    {
        int best = 0, bestscore = -1;
        for (i = 0; i < 32; i++) {
            unsigned int vuv = ambe_vuv_packed[(i << 2) & 127];
            int score = 0;
            for (l = 1; l <= L && l <= cur->L; l++) {
                int jl = (int)((float)l * 16.0f * f0);
                int v;
                if (jl > 7) jl = 7;
                v = (int)((vuv >> (30 - 2 * jl)) & 1u);
                if (v == (int)cur->Vl[l]) score++;
            }
            if (score > bestscore) { bestscore = score; best = i; }
        }
        b[1] = best;
    }

    /* ---- the envelope inverse */
    if (cur->L > prev->L) {
        for (l = prev->L + 1; l <= cur->L; l++) {
            prev->Ml[l]     = prev->Ml[prev->L];
            prev->log2Ml[l] = prev->log2Ml[prev->L];
        }
    }
    if (L > prev->L) {
        for (l = prev->L + 1; l <= L; l++) {
            prev->Ml[l]     = prev->Ml[prev->L];
            prev->log2Ml[l] = prev->log2Ml[prev->L];
        }
    }
    prev->log2Ml[0] = prev->log2Ml[1];
    prev->Ml[0]     = prev->Ml[1];

    meanP = 0.0;
    for (l = 1; l <= L; l++) {
        flokl     = ((float)prev->L / (float)L) * (float)l;
        intkl[l]  = (int)flokl;
        deltal[l] = flokl - (float)intkl[l];
        P[l] = 0.65f * ((1.0f - deltal[l]) * prev->log2Ml[intkl[l]] +
                        deltal[l] * prev->log2Ml[intkl[l] + 1]);
        meanP += P[l];
    }
    meanP /= (double)L;

    meanD = 0.0;
    for (l = 1; l <= L; l++) {
        double src = (l <= cur->L) ? cur->log2Ml[l] : cur->log2Ml[cur->L];
        D[l] = (float)(src - P[l] + meanP);
        meanD += D[l];
    }
    meanD /= (double)L;

    /* mean(D) = gamma - 0.5*log2(L) */
    gamma  = meanD + 0.5 * (log((double)L) / log(2.0));
    dgamma = gamma - 0.5 * (double)prev->gamma;

    /* ---- b2: the gain quantiser */
    {
        int best = 0;
        double bestd = 1e300;
        for (i = 0; i < 32; i++) {
            double d = fabs((double)ambe_dg_q11[i] / (double)AMBE_Q11 - dgamma);
            if (d < bestd) { bestd = d; best = i; }
        }
        b[2] = best;
    }

    for (l = 1; l <= L; l++)
        Tl[l] = (float)(D[l] - meanD);

    /* ---- forward DCT of each prediction block */
    Ji[1] = ambe_lmprbl[L * 4 + 0];
    Ji[2] = ambe_lmprbl[L * 4 + 1];
    Ji[3] = ambe_lmprbl[L * 4 + 2];
    Ji[4] = ambe_lmprbl[L * 4 + 3];

    l = 1;
    for (i = 1; i <= 4; i++) {
        int ji = Ji[i];
        for (k = 1; k <= ji; k++) {
            double sum = 0.0;
            for (j = 1; j <= ji; j++)
                sum += (double)Tl[l + j - 1] *
                       cos((M_PI * (double)(k - 1) * ((double)j - 0.5)) / (double)ji);
            Cik[i][k] = (float)(sum / (double)ji);
        }
        l += ji;
    }

    /* ---- Cik[i][1..2] -> Ri -> Gm, then the two PRBA codebooks */
    for (i = 1; i <= 4; i++) {
        double c1 = Cik[i][1], c2 = Cik[i][2];
        Ri[2 * i - 1] = (float)(c1 + 1.4142135623730951 * c2);
        Ri[2 * i]     = (float)(c1 - 1.4142135623730951 * c2);
    }
    for (k = 1; k <= 8; k++) {
        double sum = 0.0;
        for (i = 1; i <= 8; i++)
            sum += (double)Ri[i] *
                   cos((M_PI * (double)(k - 1) * ((double)i - 0.5)) / 8.0);
        Gm[k] = (float)(sum / 8.0);
    }
    /* Gm[1] is the free constant the codec pins to zero; it is not transmitted */
    probe[0] = Gm[2]; probe[1] = Gm[3]; probe[2] = Gm[4];
    b[3] = nearest(ambe_prba24_q11, 512, 3, 3, probe);
    probe[0] = Gm[5]; probe[1] = Gm[6]; probe[2] = Gm[7]; probe[3] = Gm[8];
    b[4] = nearest(ambe_prba58_q11, 128, 4, 4, probe);

    /* ---- higher-order coefficients */
    {
        const short *hoc[5];
        int entries[5], nb;
        hoc[1] = ambe_hoc_b5_q11; entries[1] = 32;
        hoc[2] = ambe_hoc_b6_q11; entries[2] = 16;
        hoc[3] = ambe_hoc_b7_q11; entries[3] = 16;
        hoc[4] = ambe_hoc_b8_q11; entries[4] = 8;
        for (i = 1; i <= 4; i++) {
            nb = Ji[i] - 2;
            if (nb > 4) nb = 4;
            if (nb < 0) nb = 0;
            for (k = 0; k < 4; k++)
                probe[k] = (k < nb) ? Cik[i][k + 3] : 0.0f;
            b[4 + i] = (nb > 0) ? nearest(hoc[i], entries[i], 4, nb, probe) : 0;
        }
    }

    /* ---- pack */
    memset(ambe_d, 0, AMBE_BITS);
    put_bits(ambe_d, ambe_b0_idx, 7, b[0]);
    put_bits(ambe_d, ambe_b1_idx, 5, b[1]);
    put_bits(ambe_d, ambe_b2_idx, 5, b[2]);
    put_bits(ambe_d, ambe_b3_idx, 9, b[3]);
    put_bits(ambe_d, ambe_b4_idx, 7, b[4]);
    put_bits(ambe_d, ambe_b5_idx, 5, b[5]);
    put_bits(ambe_d, ambe_b6_idx, 4, b[6]);
    put_bits(ambe_d, ambe_b7_idx, 4, b[7]);
    put_bits(ambe_d, ambe_b8_idx, 3, b[8]);

    if (info) {
        memcpy(info->b, b, sizeof(b));
        info->type = AMBE_FRAME_VOICE;
    }
    (void)unvc;
    return 0;
}
