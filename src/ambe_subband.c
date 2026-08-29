/*
 * ambe_subband.c - the analyser's 16-channel filterbank, transcribed.
 *
 * Vocoder_AnalyzeSpectrum 0x000205B8 runs two filterbanks.  This is the inner
 * one, Vocoder_AnalyzeSubbandSpectrum 0x00023AA8, which produces the 16 x 49
 * array the outer eight-band loop reads.  It is a conventional uniform DFT
 * filterbank - window, transform, decimate - and the two pieces here are the
 * first two of those.
 *
 * That the array is 16 x 49 is not inferred from strides: the function's state
 * shorts begin at param_1 + 0x620, and 0x620 bytes is 784 shorts is exactly
 * 16 x 49, so the ring ends precisely where the state starts.
 *
 * THE WINDOW is 32 taps, stored as 16 and folded: sample j and sample 31-j
 * share tap j.  It is not analytic - the best raised-cosine fit leaves 1309
 * LSB against a peak of 30913 - so it is verbatim bytes like every other table
 * here.  What identifies it is its sum, 2**19 + 2, a DC gain of exactly 16 in
 * Q15, one unit per output channel.
 *
 * THE TRANSFORM is a 32-point real DFT that the radio evaluates *directly*,
 * not by an FFT, which is worth saying because the radio has a perfectly good
 * FFT (ambe_fft.c) and does not use it here.  At 32 points a stored matrix is
 * competitive, and the Hermitian fold halves it again: 15 half-sums and 15
 * half-differences against 15 rows of 16 cosines and 16 sines.  Bin 0 needs no
 * multiply at all, since all its coefficients are 1.
 *
 * The scaling is the part worth checking rather than reading.  Each bin is
 * acc >> 18 where acc is Q30, and bin 0 is total >> 3 where total is Q15 - two
 * different-looking constants that have to agree, and do: bin 0's implicit
 * coefficients are 2**15, so its accumulator would be total << 15, and
 * (total << 15) >> 18 is total >> 3.  Both give X[k]/16, and the 16 is exactly
 * the window's DC gain.  So the stage as a whole is unity, which is the check
 * that says the two constants were read correctly.
 *
 * A DECOMPILER DEFECT, the third of this kind in this project.  The halves of
 * bins 0 and 16 are taken by `sext r2,r2,0xf,0x1` at 0x0002AFEE and
 * 0x0002B03C - a *signed* bitfield extract of bits [15:1], i.e. an arithmetic
 * shift right by one.  Ghidra renders it as `(v & 0x7fff) >> 1`, which is a
 * logical shift of the low fifteen bits and differs on every negative sample.
 * docs/fixed-point.md records the same defect in Math_Pow2's fraction extract;
 * the giveaway here is that the paired samples two lines away use `asri`, and
 * there is no reason for the unpaired ones to be treated differently.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe_subband.h"
#include "ambe_tables.h"

/*
 * The window, folded.  The stock code walks two pointers out from the centre
 * and one down the table, which is the same thing.
 */
void ambe_subband_window(int16_t out[32], const int16_t in[32])
{
    int j;

    for (j = 0; j < 16; j++) {
        int32_t w = ambe_subband_win_q15[j];
        /* the stock rounding is + 0x4000 before the >> 15 */
        out[j]      = (int16_t)(((int32_t)in[j]      * w + 0x4000) >> 15);
        out[31 - j] = (int16_t)(((int32_t)in[31 - j] * w + 0x4000) >> 15);
    }
}

void ambe_subband_dft32(int16_t x[32])
{
    int16_t sum[16], diff[16];
    int32_t half0, total;
    int j, k;

    /*
     * The Hermitian fold.  x[0] and x[16] have no partner, so they are halved
     * on their own - arithmetically, see the defect note above.
     */
    half0 = (int32_t)x[0] >> 1;
    total = half0;
    for (j = 1; j <= 15; j++) {
        int32_t s = ((int32_t)x[j] + (int32_t)x[32 - j]) >> 1;
        sum[j - 1]  = (int16_t)s;
        diff[j - 1] = (int16_t)(((int32_t)x[j] - (int32_t)x[32 - j]) >> 1);
        total += s;
    }
    sum[15]  = (int16_t)((int32_t)x[16] >> 1);
    total   += sum[15];
    /*
     * The stock code leaves the sixteenth difference uninitialised, and can:
     * its coefficient is -sin(2*pi*k*16/32) = -sin(pi*k), which is zero in
     * every one of the fifteen rows.  Zeroing it here computes the same thing
     * without reading uninitialised memory.
     */
    diff[15] = 0;

    /* bin 0 is real, and needs no multiply: every coefficient is 1 */
    x[0] = (int16_t)(total >> 3);
    x[1] = 0;

    for (k = 1; k <= 15; k++) {
        const short *cr = &ambe_subband_dft_q15[(k - 1) * 32];
        const short *ci = cr + 16;
        /* the j = 0 term, whose cosine is 1.0 and so will not fit in the
           table's int16 - the stock code shifts it in by hand */
        int64_t re = (int64_t)half0 << 15;
        int64_t im = 0;

        for (j = 0; j < 16; j++) {
            re += (int64_t)sum[j]  * cr[j];
            im += (int64_t)diff[j] * ci[j];
        }
        x[2 * k]     = (int16_t)(re >> 18);
        x[2 * k + 1] = (int16_t)(im >> 18);
    }
}
