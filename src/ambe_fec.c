/*
 * ambe_fec.c - DMR on-air AMBE frame <-> 49-bit AMBE+2 payload.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *
 *   Vocoder_DescrambleVoiceFrame  0x0001893C  the whole TX-side assembly:
 *        Dsp_PackBits(bits, 12)                -> u0
 *        Vocoder_ComputeParityCodeWord(u0, 12) -> 24-bit codeword, unpacked
 *        Dsp_PackBits(bits+12, 12)             -> u1
 *        Vocoder_ComputeParityCodeWord(u1, 11) -> 23-bit codeword, unpacked
 *        Dsp_LcgSignScramble(c1, 23, u0, 1)    -> scramble c1 with a PRNG
 *                                                 seeded from u0
 *        Dsp_CopyShortArray(rest, bits+24, n-24)
 *        Vocoder_DeinterleaveVoiceBits(out, buf)
 *      i.e. 12 + 12 + 25 = 49 payload bits become 24 + 23 + 25 = 72 on-air bits.
 *
 *   Vocoder_DeinterleaveVoiceBits 0x000230A4  out[4k+j] = in[k + 18j], k<18.
 *   Dsp_PackBits                  0x000194A4  MSB-first bit packing
 *   Dsp_UnpackBits                0x000194D8  MSB-first bit unpacking
 *   Dsp_LcgSignScramble           0x00021FE0  x = (seed<<4); x = x*0xAD+0x3619;
 *                                             flip the bit while x < 0 (s16)
 *
 * Two things worth recording, both established here rather than assumed:
 *
 * 1. The firmware's interleaver, out[4k+j] = in[k+18j] over a flat MSB-first
 *    concatenation c0(24)|c1(23)|c2(11)|c3(14), is bit-for-bit the same
 *    permutation as the rW/rX/rY/rZ schedule that DSD and mbelib use for DMR -
 *    all 72 positions agree.  The stock firmware independently confirms the
 *    open-source interleave schedule.
 *
 * 2. Dsp_LcgSignScramble is exactly mbelib's mbe_demodulateAmbe3600x2450Data
 *    generator: seed<<4, x = 173*x + 13849 mod 65536, take the sign bit.  The
 *    constants 0xAD and 0x3619 in the firmware are 173 and 13849.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe.h"

/* Flat MSB-first layout of the four codewords, 72 bits total. */
#define C0_OFF 0
#define C0_LEN 24
#define C1_OFF 24
#define C1_LEN 23
#define C2_OFF 47
#define C2_LEN 11
#define C3_OFF 58
#define C3_LEN 14

/* on-air bit position p carries flat bit index perm(p) */
static int perm(int p)
{
    return (p / 4) + 18 * (p % 4);
}

static void air_to_flat(const uint8_t frame[AMBE_DMR_BYTES], uint8_t f[72])
{
    int p;
    for (p = 0; p < 72; p++) {
        int bit = (frame[p >> 3] >> (7 - (p & 7))) & 1;
        f[perm(p)] = (uint8_t)bit;
    }
}

static void flat_to_air(const uint8_t f[72], uint8_t frame[AMBE_DMR_BYTES])
{
    int p;
    memset(frame, 0, AMBE_DMR_BYTES);
    for (p = 0; p < 72; p++) {
        if (f[perm(p)])
            frame[p >> 3] |= (uint8_t)(1u << (7 - (p & 7)));
    }
}

static uint32_t pack(const uint8_t *bits, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 1) | (bits[i] & 1u);
    return v;
}

static void unpack(uint32_t v, uint8_t *bits, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        bits[i] = (uint8_t)(v & 1u);
        v >>= 1;
    }
}

/*
 * Dsp_LcgSignScramble 0x00021FE0 restricted to the 1-bit case the vocoder uses:
 * fold constant 1 turns "*p = fold - *p" into a bit flip.
 */
static void lcg_sign_scramble(uint8_t *bits, int n, uint16_t seed)
{
    uint16_t x = (uint16_t)(seed << 4);
    int i;
    for (i = 0; i < n; i++) {
        x = (uint16_t)(x * 173u + 13849u);
        if (x & 0x8000u)
            bits[i] ^= 1u;
    }
}

/* --------------------------------------------------------------------- */

void ambe_dmr_deinterleave(const uint8_t frame[AMBE_DMR_BYTES], uint8_t fr[4][24])
{
    uint8_t f[72];
    int i;
    air_to_flat(frame, f);
    memset(fr, 0, 4 * 24);
    /* mbelib's ambe_fr[][] stores each codeword LSB-first in its row */
    for (i = 0; i < C0_LEN; i++) fr[0][C0_LEN - 1 - i] = f[C0_OFF + i];
    for (i = 0; i < C1_LEN; i++) fr[1][C1_LEN - 1 - i] = f[C1_OFF + i];
    for (i = 0; i < C2_LEN; i++) fr[2][C2_LEN - 1 - i] = f[C2_OFF + i];
    for (i = 0; i < C3_LEN; i++) fr[3][C3_LEN - 1 - i] = f[C3_OFF + i];
}

void ambe_dmr_interleave(const uint8_t fr[4][24], uint8_t frame[AMBE_DMR_BYTES])
{
    uint8_t f[72];
    int i;
    for (i = 0; i < C0_LEN; i++) f[C0_OFF + i] = fr[0][C0_LEN - 1 - i];
    for (i = 0; i < C1_LEN; i++) f[C1_OFF + i] = fr[1][C1_LEN - 1 - i];
    for (i = 0; i < C2_LEN; i++) f[C2_OFF + i] = fr[2][C2_LEN - 1 - i];
    for (i = 0; i < C3_LEN; i++) f[C3_OFF + i] = fr[3][C3_LEN - 1 - i];
    flat_to_air(f, frame);
}

void ambe_descramble_c1(uint8_t fr[4][24])
{
    uint8_t c1[C1_LEN];
    uint16_t seed;
    int i;
    /* seed = the 12 data bits of codeword 0, MSB first */
    seed = 0;
    for (i = 23; i >= 12; i--)
        seed = (uint16_t)((seed << 1) | (fr[0][i] & 1u));
    for (i = 0; i < C1_LEN; i++)
        c1[i] = fr[1][C1_LEN - 1 - i];
    lcg_sign_scramble(c1, C1_LEN, seed);
    for (i = 0; i < C1_LEN; i++)
        fr[1][C1_LEN - 1 - i] = c1[i];
}

/* --------------------------------------------------------------------- */

int ambe_fec_decode(uint8_t fr[4][24], uint8_t ambe_d[AMBE_BITS],
                    ambe_frame_info *info)
{
    uint8_t f0[C0_LEN], f1[C1_LEN];
    uint32_t cw, fixed, e;
    int i, k, e0, e1;

    for (i = 0; i < C0_LEN; i++) f0[i] = fr[0][C0_LEN - 1 - i];

    /* codeword 0: 23-bit Golay body, the 24th bit is the overall parity */
    cw = pack(f0, 23);
    e0 = ambe_golay23_decode(cw, &fixed);
    e  = cw ^ fixed;
    e0 = 0;
    for (i = 11; i < 23; i++)          /* count data-bit corrections only */
        e0 += (int)((e >> i) & 1u);
    unpack(fixed, f0, 23);
    for (i = 0; i < 23; i++) fr[0][C0_LEN - 1 - i] = f0[i];

    /* codeword 1 is scrambled by a PRNG seeded from the corrected codeword 0 */
    ambe_descramble_c1(fr);

    for (i = 0; i < C1_LEN; i++) f1[i] = fr[1][C1_LEN - 1 - i];
    cw = pack(f1, 23);
    e1 = ambe_golay23_decode(cw, &fixed);
    e  = cw ^ fixed;
    e1 = 0;
    for (i = 11; i < 23; i++)
        e1 += (int)((e >> i) & 1u);
    unpack(fixed, f1, 23);
    for (i = 0; i < C1_LEN; i++) fr[1][C1_LEN - 1 - i] = f1[i];

    k = 0;
    for (i = 0; i < 12; i++) ambe_d[k++] = f0[i];                 /* c0 data */
    for (i = 0; i < 12; i++) ambe_d[k++] = f1[i];                 /* c1 data */
    for (i = C2_LEN - 1; i >= 0; i--) ambe_d[k++] = fr[2][i];
    for (i = C3_LEN - 1; i >= 0; i--) ambe_d[k++] = fr[3][i];

    if (info) {
        info->errs_c0 = e0;
        info->errs_c1 = e1;
        info->uncorrectable = 0;
    }
    return 0;
}

void ambe_fec_encode(const uint8_t ambe_d[AMBE_BITS], uint8_t frame[AMBE_DMR_BYTES])
{
    uint8_t f[72];
    uint32_t u0, u1;
    int i;

    u0 = pack(ambe_d, 12);
    u1 = pack(ambe_d + 12, 12);

    unpack(ambe_golay24_encode(u0), f + C0_OFF, C0_LEN);
    unpack(ambe_golay23_encode(u1), f + C1_OFF, C1_LEN);
    lcg_sign_scramble(f + C1_OFF, C1_LEN, (uint16_t)u0);

    for (i = 0; i < C2_LEN; i++) f[C2_OFF + i] = ambe_d[24 + i];
    for (i = 0; i < C3_LEN; i++) f[C3_OFF + i] = ambe_d[35 + i];

    flat_to_air(f, frame);
}
