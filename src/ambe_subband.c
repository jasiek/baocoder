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
#include <string.h>
#include "ambe_subband.h"
#include "ambe_basop.h"
#include "ambe_fft.h"
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

/*
 * One frame of the sweep: window, transform, accumulate.
 *
 * The `>> 7` is the stock code's, and the energies are int32 accumulators that
 * many frames add into - Vocoder_AnalyzeSubbandSpectrum zeroes them once, at
 * the top, and sweeps the whole input before anyone reads them.
 *
 * The squared magnitude is computed on *signed* halves.  Ghidra renders the
 * high half of the packed complex word as `(v >> 0x10)` on a `uint`, i.e.
 * unsigned, while sign-extending the low half two lines away - the same
 * inconsistency that produced overflowing magnitudes in ambe_fft.c, recorded
 * in tests/test_fft.c.  Squaring does not rescue it: (65536 - x)^2 is not x^2.
 */
void ambe_subband_frame(int32_t energy[16], int16_t bins[32],
                        const int16_t in[32])
{
    int b;

    ambe_subband_window(bins, in);
    ambe_subband_dft32(bins);

    for (b = 0; b < 16; b++) {
        int32_t re = bins[2 * b], im = bins[2 * b + 1];
        energy[b] += ((re * re + im * im) * 2) >> 7;
    }
}

/*
 * The sixteen per-band magnitudes, from the block at 0x00023E3E - unrolled
 * sixteen times there, and rendered by Ghidra as goto soup with a one-band
 * pipeline offset, so this was read against the disassembly instead.
 *
 * The root is Math_Sqrt 0x00019364 inlined, which ambe_basop.c already carries
 * as ambe_sqrt(): the instructions at 0x00023E4E..0x00023E7A are the same
 * two-deep Horner in the normalised mantissa, the same sqrt(2) correction
 * selected by the parity of the exponent, and the same (e + 1) >> 1.  So this
 * is a call.
 *
 * The exponent bookkeeping is the part that needed the disassembly, and it
 * turns out to say something simple.  Three stack slots are involved:
 *
 *   sp+0x128  e_in, the exponent Dsp_NormalizeArray gave the input block
 *   sp+0x4c   2*e_in, which is what the roots are taken against - the
 *             energies are squares, so their exponent is doubled
 *   sp+0x104  e_in again (r4), the exponent everything is aligned back to
 *
 * Halving 2*e_in and aligning to e_in is the identity, which is the point: a
 * channel sample comes out in the same block-float units as the PCM that went
 * in, so the decimator and the 16 x 49 ring never have to carry an exponent of
 * their own.  That is the check on having read it right - any other value of
 * r4 would leave the ring in units nothing downstream knows.
 *
 * What comes out is |X[b]|, the plain bin magnitude, and that is worth stating
 * because the code roots the *energy* |X|^2 * 2 and so looks like it should
 * come out sqrt(2) too big.  It does not: ambe_sqrt returns its mantissa
 * carrying a factor of 1/sqrt(2), which cancels the 2 exactly.  Measured
 * against libm the ratio is 0.70711 at every input, not approximately.
 *
 * Band 0 is not a root.  Bin 0 of a real DFT has no imaginary part, so its
 * magnitude is just its real part, and the stock code clamps it at zero rather
 * than taking an absolute value - a negative DC becomes silence, not a
 * reflection.  That it lands on the same definition as the other fifteen, with
 * no root and no scaling, is the check that the 2 and the 1/sqrt(2) were both
 * read correctly; getting either wrong would leave band 0 inconsistent with
 * its neighbours by sqrt(2).
 */
void ambe_subband_magnitudes(int16_t out[16], const int16_t bins[32], int e_in)
{
    int b;

    out[0] = bins[0] > 0 ? bins[0] : 0;

    for (b = 1; b < 16; b++) {
        int32_t re = bins[2 * b], im = bins[2 * b + 1];
        int32_t e2 = (re * re + im * im) * 2;
        short   e  = (short)(2 * e_in);
        uint32_t m;
        int sh;

        if (e2 <= 0) { out[b] = 0; continue; }
        m  = ambe_sqrt(e2, &e);          /* e becomes (2*e_in - shift + 1) >> 1 */
        sh = (int)e - e_in;
        /* the stock code carries the root in the high half and rounds on the
           way back down, which is what the + 0x8000 before the >> 16 is */
        if (sh >= 0)
            out[b] = (int16_t)((int32_t)(((m << 16) << sh) + 0x8000) >> 16);
        else if (-sh < 32)
            out[b] = (int16_t)((int32_t)(((m << 16) >> -sh) + 0x8000) >> 16);
        else
            out[b] = 0;
    }
}

/*
 * The frame scheduling, from the prologue at 0x00023AAE.
 *
 * The stage consumes eight input samples per output sample per channel, but
 * the caller's frame size is not a multiple of eight - Vocoder_ProcessFrameFec
 * 0x00016F3C clamps it to [0x4C, 0x54], i.e. 76..84, and calls
 * Vocoder_ProcessFrame twice per 20 ms frame.  So there is a fractional
 * resampler: a remainder is carried at param_1 + 0x6DC and the count varies
 * between calls, which is why the output buffer is 16 x 11 rather than 16 x 10
 * and why Vocoder_AnalyzeSpectrum's tail shifts the ring by the returned count
 * rather than by a constant.
 *
 * The clamp and the buffer size corroborate each other, which is the check
 * that both were read right.  With a remainder of up to 7 carried in, a frame
 * size f asks for at most (f + 7) / 8 outputs: at the clamp's maximum of 84
 * that is exactly 11, the buffer's capacity, and it stays 11 up to f = 88 and
 * overflows at 89.  So the clamp is what bounds the buffer, with a little
 * margin rather than none.
 *
 * The 28 extra input samples are the 32-tap window's overlap: sample-sets step
 * four samples apart, so beyond the 8 per output the window still reaches 32
 * minus 4 = 28 samples back.
 *
 * The count is `sext r12,r0,0xf,0x3` at 0x00023ABC - an arithmetic shift, the
 * same signed bitfield extract that Ghidra renders as `(v & 0x7fff) >> 3` and
 * that this project has now met four times.  Here the value is a sample count
 * and cannot be negative, so the two readings agree; it is transcribed as the
 * instruction rather than as the rendering anyway, because the next one might
 * not be so lucky.  Note the zexth before it, which is real: the accumulator
 * and the frame size are summed as 16 bits and wrap there.
 */
int ambe_subband_advance(int16_t *acc, int nframe, int *nsamp, int *offset)
{
    int total = (int)(uint16_t)((int)*acc + nframe);
    int count = (int)(int16_t)total >> 3;
    int rem   = total - count * 8;
    int n     = count * 8 + AMBE_SUBBAND_OVERLAP;

    *acc = (int16_t)rem;
    if (nsamp)  *nsamp  = n;
    if (offset) *offset = AMBE_SUBBAND_HISTORY - n - rem;
    return count;
}

/*
 * The decimator, from the tail of Vocoder_AnalyzeSubbandSpectrum at
 * 0x00024EAA.  Seven symmetric taps read at a stride of 16 shorts, so they
 * walk one channel of the interleaved sets; the output pointer advances two
 * sets, which is where the factor of two in the stage's overall decimation
 * comes from.  Rounding is + 0x8000 before the >> 16, and the taps sum to
 * 65536 so the whole thing is unity gain.
 */
void ambe_subband_decimate(int16_t *out, const int16_t *sets, int chan,
                           int nout)
{
    static const int tap[7] = { 0, 1, 2, 3, 2, 1, 0 };
    int i, t;

    for (i = 0; i < nout; i++) {
        int64_t acc = 0x8000;
        for (t = 0; t < 7; t++)
            acc += (int64_t)ambe_subband_fir_q15[tap[t]] *
                   sets[chan + 16 * (2 * i + t)];
        out[i] = (int16_t)(acc >> 16);
    }
}

/*
 * ---- the eight-band loop ------------------------------------------------
 *
 * Vocoder_AnalyzeSpectrum 0x000205B8 builds a 128-entry spectrum from eight
 * bands of two channels each, writing 32 entries per band at a stride of 16 so
 * neighbouring bands overlap by half.  8 x 16 = 128 is exactly the array
 * Vocoder_SelectSpectralSubbands reads.
 *
 * That a band is two *channels* rather than two time segments is what the
 * strides say: the ring pointer advances 49 within a band and 98 between them,
 * and 98 = 2 x 49.  Eight bands of two channels is the sixteen the stage above
 * produces.
 */

/*
 * The 58-tap window is applied folded, and the table is stored edge-first
 * because the stock code reaches it only through its far end (0x180014BC) and
 * walks downwards.  So seg[j] and seg[57-j] share tap j, the centre pair
 * seg[28]/seg[29] taking the peak.
 */
short ambe_band_spectrum(int32_t magsq[32], const int16_t seg[58], short exp)
{
    int32_t buf[AMBE_BAND_NFFT / 2];
    int16_t *slot = (int16_t *)buf;
    short e;
    int j;

    for (j = 0; j < 29; j++) {
        int32_t w = ambe_subwin_q15[j];
        slot[j]      = (int16_t)((w * (int32_t)seg[j]      + 0x4000) >> 15);
        slot[57 - j] = (int16_t)((w * (int32_t)seg[57 - j] + 0x4000) >> 15);
    }
    /* Dsp_FillShortArray(..., 0, 6): the zero pad from 58 up to 64 */
    for (j = 58; j < AMBE_BAND_NFFT; j++)
        slot[j] = 0;

    e = ambe_fft_forward(buf, exp, 6, 0);
    e = (short)(e * 2);                    /* the spectrum is squared below */

    for (j = 0; j < AMBE_BAND_BINS; j++) {
        int32_t re = (int32_t)(int16_t)(buf[j] & 0xffff);
        int32_t im = (int32_t)(int16_t)((uint32_t)buf[j] >> 16);
        magsq[j] = (re * re + im * im) * 2;
    }
    /* the stock code drops DC and the first bin outright */
    magsq[0] = 0;
    magsq[1] = 0;
    return e;
}

/*
 * The exponent-aligned add.  Block floats here are mantissa * 2^exp - see
 * normalize_array in ambe_fft.c, which shifts data left and decrements the
 * exponent - so aligning means keeping the operand with the *larger* exponent
 * and shifting the other's mantissas right by the difference.  The result
 * carries the larger exponent.
 */
short ambe_band_add(int32_t dst[32], const int32_t a[32], short ea,
                    const int32_t b[32], short eb)
{
    const int32_t *keep = a, *shift = b;
    int d = (int)ea - (int)eb;
    short e = ea;
    int j;

    if (d < 0) {
        d = -d;
        keep = b;
        shift = a;
        e = eb;
    }
    if (d > 31) {
        for (j = 0; j < AMBE_BAND_BINS; j++)
            dst[j] = keep[j];
        return e;
    }
    for (j = 0; j < AMBE_BAND_BINS; j++)
        dst[j] = (shift[j] >> d) + keep[j];
    return e;
}

/*
 * The segment shift register, from the prologue at 0x00023AE6..0x00023C20.
 *
 * A channel's 58-sample segment is assembled from several calls' worth of
 * output, and each call's data was normalised to its own block-float exponent.
 * Rather than renormalise the whole history every time, the stock code
 * remembers where each call's contribution starts and what exponent it
 * carries, and the eight-band loop shifts each stretch down to a common
 * maximum on the way into the transform.
 *
 * Per call every boundary drops by `count` - the number of new samples - and
 * clamps at zero as the oldest data falls off the front; the array shifts one
 * slot down; and the newest segment enters at the top starting at 58 - count.
 * An exponent whose segment has been squeezed to zero width is replaced by
 * 0x8000, which is -32768 read as a short: a sentinel, not an exponent, and
 * the eight-band loop skips those stretches rather than shifting by 32768.
 *
 * Ghidra types the newest exponent slot at 0x62C as an `int`, which would
 * overlap the first boundary at 0x62E.  It does not: every access in the
 * disassembly is ld.h/st.h.  The one genuine word access is the comparison at
 * 0x00023C80, which reads 0x62C and 0x62E together and so only ever takes the
 * "unchanged, just copy" fast path when both halves match; the slow path is
 * Math_ArrayShiftSaturate from the old exponent to the new, which is the
 * identity when they are equal, so the two are equivalent and always
 * re-aligning is correct.
 */
void ambe_subband_segs_advance(ambe_subband_segs *s, int count, int new_exp)
{
    int i;

    for (i = 0; i < AMBE_SEGS - 1; i++) {
        int b = (int)s->bound[i + 1] - count;
        s->bound[i] = (int16_t)(b < 0 ? 0 : b);
    }
    s->bound[AMBE_SEGS - 1] = (int16_t)(AMBE_BAND_SEG - count);

    /*
     * The exponents shift with them, and the emptiness test is made against
     * the *new* boundaries - a segment whose start has caught up with the next
     * one no longer covers any samples.
     */
    for (i = 0; i < AMBE_SEGS - 1; i++)
        s->exp[i] = (s->bound[i + 1] - s->bound[i] < 1)
                        ? (int16_t)AMBE_SEG_EMPTY : s->exp[i + 1];
    s->exp[AMBE_SEGS - 1] = (int16_t)new_exp;
}

/*
 * ---- the stage assembled -------------------------------------------------
 *
 * Dsp_NormalizeArray in miniature: shift the block left until the largest
 * magnitude is just below saturation, and report how far.  Block floats here
 * are mantissa * 2^exp, so shifting left by s means the exponent falls by s.
 */
static short normalise_block(int16_t *dst, const int16_t *src, int n)
{
    int32_t mx = 0;
    int shift = 0, i;

    for (i = 0; i < n; i++) {
        int32_t v = src[i] < 0 ? -(int32_t)src[i] : (int32_t)src[i];
        if (v > mx) mx = v;
    }
    if (mx == 0) {
        for (i = 0; i < n; i++) dst[i] = 0;
        return 0;
    }
    while (mx < 0x4000 && shift < 15) { mx <<= 1; shift++; }
    for (i = 0; i < n; i++)
        dst[i] = (int16_t)((int32_t)src[i] << shift);
    return (short)(-shift);
}

void ambe_subband_init(ambe_subband *s)
{
    int i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < AMBE_SEGS; i++) {
        s->segs.bound[i] = AMBE_BAND_SEG;
        s->segs.exp[i]   = (int16_t)AMBE_SEG_EMPTY;
    }
}

int ambe_subband_process(ambe_subband *s,
                         const int16_t history[AMBE_SUBBAND_HISTORY],
                         int nframe, int32_t energy[16])
{
    /* the largest the scheduler can ask for, so nothing here is dynamic */
    int16_t work[8 * AMBE_SUBBAND_MAX_OUT + AMBE_SUBBAND_OVERLAP];
    int16_t sets[(AMBE_SUBBAND_HIST_SETS + 2 * AMBE_SUBBAND_MAX_OUT) * 16];
    int16_t out[AMBE_SUBBAND_MAX_OUT];
    int32_t bins32[32];
    int16_t bins[32];
    short   e_in;
    int count, nsamp, off, nsets, set, c, o, i;

    count = ambe_subband_advance(&s->acc, nframe, &nsamp, &off);
    for (i = 0; i < 16; i++) energy[i] = 0;
    if (count <= 0)
        return 0;

    e_in = normalise_block(work, history + off, nsamp);

    /* the carried sets come first, then this call's */
    memcpy(sets, s->hist, sizeof(s->hist));
    nsets = 2 * count;
    for (set = 0; set < nsets; set++) {
        int16_t *dst = sets + (AMBE_SUBBAND_HIST_SETS + set) * 16;
        ambe_subband_window(bins, work + 4 * set);
        ambe_subband_dft32(bins);
        for (i = 0; i < 16; i++) {
            int32_t re = bins[2 * i], im = bins[2 * i + 1];
            energy[i] += ((re * re + im * im) * 2) >> 7;
        }
        ambe_subband_magnitudes(dst, bins, e_in);
    }
    (void)bins32;

    /*
     * Decimate into the fresh buffer.  The ring is deliberately left alone:
     * the eight-band loop reads it and the fresh samples as two pieces, and
     * the stock code only slides it afterwards, in Vocoder_AnalyzeSpectrum's
     * tail.  ambe_subband_commit does that.
     */
    for (c = 0; c < 16; c++) {
        ambe_subband_decimate(out, sets, c, count);
        for (o = 0; o < count; o++)
            s->fresh[c * AMBE_SUBBAND_MAX_OUT + o] = out[o];
    }
    s->nfresh = count;

    /* carry the last five sets, which is what the next call's decimator needs */
    memcpy(s->hist, sets + nsets * 16, sizeof(s->hist));

    ambe_subband_segs_advance(&s->segs, count, e_in);
    s->primed = 1;
    return count;
}

void ambe_subband_commit(ambe_subband *s)
{
    int c, o;

    if (s->nfresh <= 0)
        return;
    for (c = 0; c < 16; c++) {
        int16_t *r = &s->ring[c * AMBE_SUBBAND_RING];
        memmove(r, r + s->nfresh,
                (size_t)(AMBE_SUBBAND_RING - s->nfresh) * sizeof(int16_t));
        for (o = 0; o < s->nfresh; o++)
            r[AMBE_SUBBAND_RING - s->nfresh + o] =
                s->fresh[c * AMBE_SUBBAND_MAX_OUT + o];
    }
    s->nfresh = 0;
}

/*
 * The segment the band loop transforms: 58 - n samples of ring followed by
 * this call's n new ones, where the ring read starts at n - 9.  With n = 10
 * that is ring[1..48] - the oldest sample of the 49 is dropped - and with the
 * smallest n the caller's clamp allows, 9, it is ring[0..48], the whole ring
 * exactly.  So the 49-deep ring and the 76-sample lower clamp are two numbers
 * from different functions that just meet, the same way the 16 x 11 buffer and
 * the 84-sample upper clamp did.
 */
int ambe_subband_segment(const ambe_subband *s, int chan, int16_t seg[58])
{
    int n = s->nfresh, from, take, i;

    if (n < 9 || n > AMBE_SUBBAND_MAX_OUT)
        return 0;
    from = n - 9;
    take = AMBE_BAND_SEG - n;
    if (from + take > AMBE_SUBBAND_RING)
        return 0;
    for (i = 0; i < take; i++)
        seg[i] = s->ring[chan * AMBE_SUBBAND_RING + from + i];
    for (i = 0; i < n; i++)
        seg[take + i] = s->fresh[chan * AMBE_SUBBAND_MAX_OUT + i];
    return 1;
}

/*
 * The eight-band loop, from Vocoder_AnalyzeSpectrum 0x000205B8.
 *
 * Eight bands of two channels each.  Every channel's 58-sample segment goes
 * through the 58-tap window into the 64-point transform; the two spectra are
 * combined by exponent-aligned add, and the band's 32 bins are overlap-added
 * into a 128-entry array at a stride of 16, so neighbouring bands share half
 * their bins.  8 x 16 = 128 is exactly the array the voicing rule reads.
 *
 * The stock code carries a common exponent picked by
 * Math_ArrayMax(param_1 + 0x310, 7) across the seven segment stretches and
 * shifts each stretch down to it before transforming.  Here that alignment is
 * already in the samples: ambe_subband_magnitudes returns every channel sample
 * in the input's own units (see there), so the ring is homogeneous and the
 * per-stretch shifts are all zero.  The segment register is still maintained
 * and still says which stretches are live, which is what a bit-exactness
 * comparison against the firmware will need; it is not needed to compute the
 * spectrum.
 */
int ambe_band_analyse(const ambe_subband *s, int32_t spec[128],
                      short band_exp[8])
{
    int32_t a[32], b[32], band[32];
    int16_t seg[AMBE_BAND_SEG];
    short ea, eb, e;
    int i, bd;

    for (i = 0; i < 128; i++)
        spec[i] = 0;

    for (bd = 0; bd < AMBE_BANDS; bd++) {
        if (!ambe_subband_segment(s, 2 * bd, seg))
            return 0;
        ea = ambe_band_spectrum(a, seg, 0);
        if (!ambe_subband_segment(s, 2 * bd + 1, seg))
            return 0;
        eb = ambe_band_spectrum(b, seg, 0);

        e = ambe_band_add(band, a, ea, b, eb);
        band_exp[bd] = e;

        /*
         * Overlap-add into the running spectrum.  The first 16 bins land on
         * the previous band's tail, which is already written; the second 16
         * start fresh.  The stock code does the same add with the exponents
         * aligned, and the eighth band's tail runs into the adjacent buffer,
         * which is why the array is exactly 128 and not 8 x 32.
         */
        for (i = 0; i < AMBE_BAND_BINS; i++) {
            int at = bd * AMBE_BAND_STRIDE + i;
            if (at >= 128)
                break;
            spec[at] += band[i];
        }
    }
    return 1;
}
