/*
 * ambe_params.c - 49 payload bits -> MBE speech model parameters, in fixed point.
 *
 * The AMBE+2 2450 payload carries nine quantiser indices b0..b8, scattered
 * across the 49 bits so that the Golay-protected prefix holds the perceptually
 * critical fields.  b0 selects the fundamental and the harmonic count, b1 the
 * voicing pattern, b2 the frame gain, b3/b4 the PRBA gain vectors and b5..b8
 * the higher-order spectral coefficients.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *
 *   Vocoder_DecodeFrameParameters       0x0001994C  top-level parameter decode
 *   Vocoder_DecodePitchIndex            0x00022B78  b0 -> log2(f0) in Q12
 *   Vocoder_PitchFromLog2               0x0002AD6C  2^x -> f0 in Q19
 *   Vocoder_HarmonicCountFromPitch      0x0002AD18  f0 -> L
 *   Vocoder_DecodeSpectralCodebookEntry 0x00022DB4  codebook fetch (PRBA/HOC)
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
 * Arithmetic: everything is integer.  The transcendentals come from
 * src/ambe_basop.c, which is the radio's own Log2/Pow2/Sqrt/cos with the
 * radio's coefficients, so this stage inherits their accuracy rather than
 * libm's - see tests/test_basop.c for the measured figures.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_tables.h"
#include "ambe_bitpos.h"

#define AMBE_B0_WIDTH 7

/* Constants, in the Q30 the code multiplies them at. */
#define K_065_Q30      697932186   /* 0.65,          eq. 43 prediction weight */
#define K_RCONST_Q30   379625062   /* 1/(2*sqrt(2)), the Ri -> Cik rotation   */
#define K_0P2046_Q30   219687577   /* 0.2046,        unvoiced amplitude scale */
#define K_TWO_PI_Q28  1686629713   /* 2*pi, for w0 = 2*pi*f0                  */

/*
 * exp(0.693*x) is what mbelib and this codec's float version compute where
 * 2^x is meant: 0.693 is a truncated ln2, so the result is 2^(0.9997877*x),
 * not 2^x.  The difference reaches 0.17% at the top of the observed log2Ml
 * range, which is larger than most of the error budget here, so it is
 * reproduced exactly rather than quietly corrected - otherwise this
 * implementation and the float one would not be computing the same function,
 * and no comparison between them would mean anything.
 */
#define K_0P693_OVER_LN2_Q30 1073513829

/* Math_SDiv 0x00018D74 semantics, on ints. */
static int sdiv(int a, int b)
{
    int sign = ((a < 0) != (b < 0)) ? -1 : 1;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (a < b) return 0;
    return sign * (a / b);
}

/*
 * Vocoder_PitchFromLog2 0x0002AD6C: 2^x for the pitch path, transcribed from
 * the disassembly at 0x0002AD98-0x0002ADCC.  `x` is log2(f0) in Q12.
 *
 * It range-reduces by adding 0x1000 until the argument is above -0x1000,
 * counting from -4, then evaluates 2^frac by a Horner polynomial with
 * coefficients 0x58B9 0x1EC0 0x71B 0x13B - ln2, (ln2)^2/2, (ln2)^3/6 and
 * (ln2)^4/24 in Q15, the Taylor series of 2^x.  `zext r2,r2,0x1b,0xc` takes
 * bits [27:12], the XOR with 0x8000 supplies the implicit leading bit, and
 * the arithmetic shift by the accumulated exponent puts the result in Q19.
 *
 * Using the radio's polynomial rather than a library pow() is not a rounding
 * detail: at b0 = 17 the two land on opposite sides of an L boundary.  See
 * tests/test_tables.c.
 */
static int32_t pitch_from_log2_q12(int16_t x)
{
    int32_t iv = (int16_t)x, t;
    int16_t k;
    uint32_t f;

    if (iv < -0x1000) {
        k = -4;
        do {
            x  = (int16_t)(uint16_t)(x + 0x1000);
            iv = (int16_t)x;
            k  = (int16_t)(uint16_t)(k + 1);
        } while (iv < -0x1000);
    } else {
        k = -4;
    }

    t = ((iv * 0x13b) >> 12) + 0x71b;
    t = (int16_t)(((t * iv) >> 12) + 0x1ec0);
    t = (int16_t)(((t * iv) >> 12) + 0x58b9);

    f = (uint32_t)(t * iv);
    f = (f >> 12) & 0xffff;
    f ^= 0x8000;
    return (int32_t)((int16_t)f) >> (k & 0x1f);
}

/*
 * Vocoder_HarmonicCountFromPitch 0x0002AD18.  0x3B39C / 2^19 = 0.462685 is the
 * 0.4627 constant of the AMBE literature sitting in the firmware as an
 * integer; the guard below it is the Nyquist limit on the top harmonic.
 */
static int harmonic_count(int32_t f0_q19)
{
    int L = sdiv(0x3B39C, f0_q19) & 0xFFFF;

    if ((((2 * L + 1) & 0xFFFF) * f0_q19) > 0x7FFFF)
        L = ((sdiv(0x80000, f0_q19) * 0x10000 - 0x10000) >> 0x11) & 0xFFFF;
    if (L >= 0x38) L = 0x38;
    else if (L < 9) L = 9;
    return L;
}

void ambe_pitch_from_b0(int b0, int width, int32_t *f0_q19, int *L_out)
{
    int q, x;
    int32_t f0;

    /* Vocoder_DecodePitchIndex 0x00022B78 */
    q = sdiv(((2 * b0 + 1) & 0xFFFF) * 0x2A52, (0x3C << (width - 6)) * 2);
    x = -(q + 0x44FD);

    f0 = pitch_from_log2_q12((int16_t)x);
    if (f0 < 0x1079) f0 = 0x1079;
    if (f0 > 0x6BCA) f0 = 0x6BCA;

    *f0_q19 = f0;
    *L_out  = harmonic_count(f0);
}

int32_t ambe_w0_q24(const ambe_parms *p)
{
    /* 2*pi*f0: Q19 x Q28 -> Q24 */
    return (int32_t)(((int64_t)p->f0 * K_TWO_PI_Q28) >> 23);
}

static int bits_to_int(const uint8_t *d, const int *idx, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++)
        v = (v << 1) | (d[idx[i]] & 1);
    return v;
}

/*
 * Q11 -> Q24, a left shift of thirteen that is exact for every table entry.
 * The tables are signed and many entries are negative, and shifting a negative
 * value left is undefined in C99, so the shift runs on the unsigned bit
 * pattern - the same round-trip the rest of the fixed-point code uses - and
 * comes back to int32_t with the value the stock code's shift produced.
 */
static int32_t q11_to_q24(int32_t v)
{
    return ambe_shl32(v, AMBE_Q_LOG - 11);
}

/* Bit positions of each quantiser index inside the 49-bit payload. */
const int ambe_b0_idx[7] = {  0,  1,  2,  3, 37, 38, 39 };
const int ambe_b1_idx[5] = {  4,  5,  6,  7, 35 };
const int ambe_b2_idx[5] = {  8,  9, 10, 11, 36 };
const int ambe_b3_idx[9] = { 12, 13, 14, 15, 16, 17, 18, 19, 40 };
const int ambe_b4_idx[7] = { 20, 21, 22, 23, 41, 42, 43 };
const int ambe_b5_idx[5] = { 24, 25, 26, 27, 44 };
const int ambe_b6_idx[4] = { 28, 29, 30, 45 };
const int ambe_b7_idx[4] = { 31, 32, 33, 46 };
const int ambe_b8_idx[3] = { 34, 47, 48 };
#define B0_IDX ambe_b0_idx
#define B1_IDX ambe_b1_idx
#define B2_IDX ambe_b2_idx
#define B3_IDX ambe_b3_idx
#define B4_IDX ambe_b4_idx
#define B5_IDX ambe_b5_idx
#define B6_IDX ambe_b6_idx
#define B7_IDX ambe_b7_idx
#define B8_IDX ambe_b8_idx

void ambe_move_parms(const ambe_parms *src, ambe_parms *dst)
{
    int l;
    dst->f0     = src->f0;
    dst->L      = src->L;
    dst->gamma  = src->gamma;
    dst->Ml_exp = src->Ml_exp;
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
    prev->f0     = 7825;      /* 0.09378 rad/sample, the float version's seed,
                                 as 0.0149253 turns per sample in Q19 */
    prev->L      = 30;
    prev->gamma  = 0;
    prev->Ml_exp = 0;
    for (l = 0; l <= AMBE_MAX_HARMONICS; l++) {
        prev->Ml[l]     = 0;
        prev->Vl[l]     = 0;
        prev->log2Ml[l] = 0;
        prev->PHIl[l]   = 0;
        prev->PSIl[l]   = 0x40000000u;   /* pi/2 is a quarter turn, Q32 */
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
    int Ji[5], intkl[AMBE_MAX_HARMONICS + 1], nextkl[AMBE_MAX_HARMONICS + 1];
    int32_t f0, unvc_q30;
    int32_t Gm[9], Ri[9], Cik[5][18], Tl[AMBE_MAX_HARMONICS + 1];
    int32_t deltal[AMBE_MAX_HARMONICS + 1];
    int32_t Sum42, Sum43, BigGamma, dgamma;
    int64_t acc;

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
        silence = 1;
        f0      = 1 << (AMBE_Q_F0 - 5);    /* 1/32 turn per sample, exactly */
        L       = 14;
        cur->f0 = f0;
        cur->L  = 14;
        for (l = 1; l <= L; l++)
            cur->Vl[l] = 0;
    } else {
        ambe_pitch_from_b0(b0, AMBE_B0_WIDTH, &f0, &L);
        cur->f0 = f0;
        cur->L  = L;
    }

    /* unvc = 0.2046 / sqrt(w0), the unvoiced amplitude scale */
    {
        int32_t w0_q28 = (int32_t)(((int64_t)f0 * K_TWO_PI_Q28) >> AMBE_Q_F0);
        short   e = 31 - 28;               /* w0 = w0_q28 * 2^(e - 31) */
        unsigned int m = ambe_sqrt(w0_q28, &e);
        /* sqrt(w0) = m * 2^(e - 15); unvc = 0.2046 / that, in Q30 */
        int64_t den = (int64_t)m << (e - 15 + 30);
        unvc_q30 = den ? (int32_t)(((int64_t)K_0P2046_Q30 << 30) / den) : 0;
    }

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
            int jl = (int)(((int64_t)l * 16 * f0) >> AMBE_Q_F0);
            if (jl > 7) jl = 7;
            cur->Vl[l] = (uint8_t)((vuv >> (30 - 2 * jl)) & 1u);
        }
    }

    /* ---- b2: differential frame gain.  Q11 -> Q24 is exact. */
    b2         = bits_to_int(ambe_d, B2_IDX, 5);
    dgamma     = q11_to_q24(ambe_dg_q11[b2]);
    cur->gamma = dgamma + ((prev->gamma + 1) >> 1);

    /* ---- b3/b4: PRBA vectors -> Ri via an 8-point inverse cosine transform */
    b3 = bits_to_int(ambe_d, B3_IDX, 9);
    b4 = bits_to_int(ambe_d, B4_IDX, 7);
    Gm[1] = 0;
    Gm[2] = q11_to_q24(ambe_prba24_q11[b3 * 3 + 0]);
    Gm[3] = q11_to_q24(ambe_prba24_q11[b3 * 3 + 1]);
    Gm[4] = q11_to_q24(ambe_prba24_q11[b3 * 3 + 2]);
    Gm[5] = q11_to_q24(ambe_prba58_q11[b4 * 4 + 0]);
    Gm[6] = q11_to_q24(ambe_prba58_q11[b4 * 4 + 1]);
    Gm[7] = q11_to_q24(ambe_prba58_q11[b4 * 4 + 2]);
    Gm[8] = q11_to_q24(ambe_prba58_q11[b4 * 4 + 3]);

    /*
     * cos(pi*(k-1)*(i-0.5)/8) is cos of (k-1)*(2i-1)/32 turns, and a turn is
     * 32768 in the phase units ambe_cos_q15 takes, so the phase is the exact
     * integer (k-1)*(2i-1)*1024 - no rounding before the table lookup.
     */
    for (i = 1; i <= 8; i++) {
        acc = 0;
        for (k = 1; k <= 8; k++) {
            int32_t am = (k == 1) ? 1 : 2;
            acc += (int64_t)am * Gm[k] * ambe_cos_q15((k - 1) * (2 * i - 1) * 1024);
        }
        Ri[i] = (int32_t)((acc + (1 << 14)) >> 15);
    }

    memset(Cik, 0, sizeof(Cik));
    for (i = 1; i <= 4; i++) {
        int32_t a = Ri[2 * i - 1], b = Ri[2 * i];
        Cik[i][1] = (a + b + 1) >> 1;
        Cik[i][2] = (int32_t)(((int64_t)(a - b) * K_RCONST_Q30 + (1 << 29)) >> 30);
    }

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
        Cik[1][k] = (k > 6) ? 0 : q11_to_q24(ambe_hoc_b5_q11[b5 * 4 + (k - 3)]);
    for (k = 3; k <= Ji[2]; k++)
        Cik[2][k] = (k > 6) ? 0 : q11_to_q24(ambe_hoc_b6_q11[b6 * 4 + (k - 3)]);
    for (k = 3; k <= Ji[3]; k++)
        Cik[3][k] = (k > 6) ? 0 : q11_to_q24(ambe_hoc_b7_q11[b7 * 4 + (k - 3)]);
    for (k = 3; k <= Ji[4]; k++)
        Cik[4][k] = (k > 6) ? 0 : q11_to_q24(ambe_hoc_b8_q11[b8 * 4 + (k - 3)]);

    if (info) {
        info->b[1] = b1; info->b[2] = b2; info->b[3] = b3; info->b[4] = b4;
        info->b[5] = b5; info->b[6] = b6; info->b[7] = b7; info->b[8] = b8;
    }

    /*
     * Inverse DCT of each block gives the prediction residual Tl.  Here the
     * cosine argument is (k-1)*(2j-1)/(4*ji) turns, which is not an exact
     * multiple of a phase unit, so the phase is rounded to nearest - the only
     * rounding in the transform besides the table's own.
     */
    l = 1;
    for (i = 1; i <= 4; i++) {
        int ji = Ji[i];
        for (j = 1; j <= ji; j++) {
            acc = 0;
            for (k = 1; k <= ji; k++) {
                int32_t ak = (k == 1) ? 1 : 2;
                int phase;
                if (Cik[i][k] == 0)
                    continue;
                phase = (int)(((int64_t)(k - 1) * (2 * j - 1) * 16384 + ji) / (2 * ji));
                acc += (int64_t)ak * Cik[i][k] * ambe_cos_q15(phase);
            }
            Tl[l++] = (int32_t)((acc + (1 << 14)) >> 15);
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

    acc = 0;
    for (l = 1; l <= cur->L; l++) {
        /* flokl = (prevL/curL)*l, carried as Q16 */
        int32_t flokl = (int32_t)((((int64_t)prev->L * l) << 16) / cur->L);
        intkl[l]  = flokl >> 16;
        deltal[l] = flokl & 0xFFFF;
        /* intkl reaches AMBE_MAX_HARMONICS only when prev->L == cur->L == 56,
           and there deltal is exactly zero, so holding the index leaves the
           sum unchanged and keeps the upper tap inside the array. */
        nextkl[l] = intkl[l] < AMBE_MAX_HARMONICS ? intkl[l] + 1 : intkl[l];
        acc += ((int64_t)(65536 - deltal[l]) * prev->log2Ml[intkl[l]] +
                (int64_t)deltal[l] * prev->log2Ml[nextkl[l]]) >> 16;
    }
    Sum43 = (int32_t)(((acc * K_065_Q30) / cur->L) >> 30);

    acc = 0;
    for (l = 1; l <= cur->L; l++)
        acc += Tl[l];
    Sum42 = (int32_t)(acc / cur->L);

    BigGamma = cur->gamma - ambe_half_log2_q24[cur->L] - Sum42;

    /* ---- amplitudes.  Block floating point: one exponent for the frame. */
    {
        int32_t mant[AMBE_MAX_HARMONICS + 1];
        short   ex[AMBE_MAX_HARMONICS + 1];
        int     maxex = -32768;

        for (l = 1; l <= cur->L; l++) {
            int64_t c = ((int64_t)(65536 - deltal[l]) * prev->log2Ml[intkl[l]] +
                         (int64_t)deltal[l] * prev->log2Ml[nextkl[l]]) >> 16;
            int64_t x;
            c = (c * K_065_Q30) >> 30;
            cur->log2Ml[l] = Tl[l] + (int32_t)c - Sum43 + BigGamma;

            /* Ml = exp(0.693 * log2Ml): see K_0P693_OVER_LN2_Q30 above.  The
               Q24 log is taken to the Q16 that ambe_pow2 wants. */
            x = ((int64_t)cur->log2Ml[l] * K_0P693_OVER_LN2_Q30) >> 30;
            mant[l] = ambe_pow2((unsigned int)(int32_t)(x >> (AMBE_Q_LOG - 16)), &ex[l]);
            if (!cur->Vl[l])
                mant[l] = (int32_t)(((int64_t)mant[l] * unvc_q30) >> 30);
            if (ex[l] > maxex)
                maxex = ex[l];
        }
        /*
         * Three bits of headroom below 2^31.  ambe_pow2's mantissas land in
         * [2^30, 2^31), and the enhancement stage that runs next multiplies
         * them by up to 1.2 and then by an energy-restoring gain that can
         * reach 2, so without this the products overflow int32 and the
         * amplitude comes out negative.
         */
        for (l = 1; l <= cur->L; l++) {
            int sh = maxex - ex[l] + 3;
            cur->Ml[l] = (sh >= 31) ? 0 : (mant[l] >> sh);
        }
        for (l = cur->L + 1; l <= AMBE_MAX_HARMONICS; l++)
            cur->Ml[l] = 0;
        cur->Ml_exp = maxex + 3;
    }

    if (info)
        info->type = silence ? AMBE_FRAME_SILENCE : AMBE_FRAME_VOICE;
    return silence ? AMBE_FRAME_SILENCE : AMBE_FRAME_VOICE;
}
