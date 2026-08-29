/*
 * ambe_fft.c - the radio's spectral front end, transcribed.
 *
 * The DM-32UV's speech analyser windows 199 samples into a 256-point
 * transform and works on the magnitude spectrum that comes out.  This is that
 * transform: a 128-point *complex* FFT over the 256 real samples packed
 * even/odd into re/im, with the real spectrum unpacked afterwards, and block
 * floating point carried through it - the array is renormalised between
 * stages and the accumulated exponent is returned rather than the data being
 * allowed to overflow.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *   Dsp_WindowAndComputeFft     0x00019B6C  window, fold, pack, transform
 *   Dsp_FftForward              0x000256D0  permute+stages, then unpack
 *   Dsp_FftBitReverseScale      0x00025224  peak scan, permutation, stages
 *   Dsp_FftStageButterfly       0x00025160  the Q15 butterfly, >>15
 *   Dsp_FftFinalStageButterfly  0x0002509C  the same, >>16, exponent step
 *   Dsp_FftButterflyStage       0x00025520  real-spectrum unpacking
 *   Dsp_FftButterflyRecurse     0x00025484  its twiddled half
 *   Dsp_NormalizeArray          0x0001ADA0  block-float normalise
 *   Math_ArrayShiftCopy         0x0001AB58
 *   Dsp_ComputeMagnitudeSquared 0x0001AB38
 *
 * The two butterfly kernels only decompile at all because of
 * docs/patches/csky-muls-family.patch: the Ghidra C-SKY module was missing the
 * two multiply families they are built from, and Ghidra truncates a function
 * at the first instruction it cannot decode.  Before that fix these were 68
 * bytes of a 190-byte function.
 *
 * Complex samples are packed one to an int32, real in the low half and
 * imaginary in the high half, which is the layout the stock code uses and
 * what makes the even/odd real packing free.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_fft.h"
#include "ambe_tables.h"

#define RE(w)  ((int16_t)((uint32_t)(w) & 0xFFFFu))
#define IM(w)  ((int16_t)((uint32_t)(w) >> 16))
#define PACK(re, im) ((int32_t)(((uint32_t)(uint16_t)(im) << 16) | (uint16_t)(re)))

/*
 * Math_ArrayShiftCopy 0x0001AB58: a positive count shifts left, a negative
 * one shifts right by its magnitude.
 */
static void array_shift_copy(int16_t *dst, const int16_t *src, int n, int shift)
{
    int i;
    if (n <= 0)
        return;
    if (shift == 0) {
        for (i = 0; i < n; i++) dst[i] = src[i];
    } else if (shift < 0) {
        int s = -shift;
        for (i = 0; i < n; i++) dst[i] = (int16_t)((int32_t)src[i] >> (s & 0x3f));
    } else {
        for (i = 0; i < n; i++)
            dst[i] = (int16_t)((uint32_t)src[i] << (shift & 0x3f));
    }
}

/*
 * Dsp_NormalizeArray 0x0001ADA0: shift the array up until its largest
 * magnitude uses the full range, and subtract that shift from the exponent.
 */
static void normalize_array(int16_t *dst, const int16_t *src, int n, short *exp)
{
    uint32_t mx = 0, shift;
    int i;

    for (i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(int32_t)src[i];
        if ((int32_t)v < 0)
            v = ~v + 1;
        if ((int32_t)mx < (int32_t)v)
            mx = v;
    }
    mx <<= 16;
    if (mx == 0) {
        array_shift_copy(dst, src, n, 0);
        return;
    }
    if (mx & 0x80000000u)
        mx = ~mx;
    shift = ambe_lzcount32(mx) - 1u;
    array_shift_copy(dst, src, n, (int)(int16_t)shift);
    *exp = (short)(*exp - (short)shift);
}

/*
 * Dsp_FftStageButterfly 0x00025160 and Dsp_FftFinalStageButterfly 0x0002509C.
 *
 *     a = X + W*Y,   b = X - W*Y
 *     Re(W*Y) = wr*yr - wi*yi,   Im(W*Y) = wi*yr + wr*yi
 *
 * X is brought to the twiddles' Q15 by the multiply against 0x8000 the stock
 * code does literally.  The two differ only in the final shift: the ordinary
 * stage keeps Q15 and the final one drops a bit, which is the block-float
 * step the caller accounts for in the exponent.
 */
static void stage_butterfly(int32_t *base, int32_t *b_base, int tw_stride,
                            int stride, int groups, int final)
{
    int g, k;

    for (g = 0; g < groups; g++) {
        int32_t *a = base   + (ptrdiff_t)g * stride * 2;
        int32_t *b = b_base + (ptrdiff_t)g * stride * 2;
        const int16_t *w = ambe_fft_twiddle_q15;
        for (k = 0; k < stride; k++) {
            int32_t xr = RE(*a), xi = IM(*a);
            int32_t yr = RE(*b), yi = IM(*b);
            int32_t wr = w[0],   wi = w[1];
            int32_t br, ar, bi, ai;

            br = (xr * 0x8000 - wr * yr) + wi * yi;
            ar = (xr * 0x8000 + wr * yr) - wi * yi;
            bi = (xi * 0x8000 - wi * yr) - wr * yi;
            ai =  xi * 0x8000 + wi * yr  + wr * yi;

            if (final) {
                *b = PACK((int16_t)((uint32_t)br >> 16), (int16_t)((uint32_t)bi >> 16));
                *a = PACK((int16_t)((uint32_t)ar >> 16), (int16_t)((uint32_t)ai >> 16));
            } else {
                *b = PACK((int16_t)(br >> 15), (int16_t)(bi >> 15));
                *a = PACK((int16_t)(ar >> 15), (int16_t)(ai >> 15));
            }
            a++;
            b++;
            w += 2 * tw_stride;
        }
    }
}

/*
 * Dsp_FftBitReverseScale 0x00025224: pick a scale from the array's peak,
 * apply the bit-reversal permutation, run the first radix-2 pass at that
 * scale, then the remaining stages - choosing per stage between the two
 * butterflies according to how much headroom is left.
 */
static short bitrev_scale(int32_t *buf, short scale_exp, int order)
{
    int n = 1 << order;
    int32_t peak;
    short shift_out, first;
    int i;

    /*
     * Peak search.  Ghidra renders these as `& 0xffff` products because the
     * underlying instruction is mulsh, whose sign extension lives inside the
     * opcode rather than in the operands; the halves are signed.
     */
    peak = 0x1000000;
    for (i = 0; i < n; i++) {
        int32_t re = RE(buf[i]), im = IM(buf[i]);
        int32_t e = re * re + im * im;
        if (peak < e)
            peak = e;
    }
    shift_out = (short)(ambe_lzcount32((uint32_t)peak) - 2u);
    if (shift_out < 2) {
        if (shift_out < 0) { shift_out = -2; first = -1; }
        else               { shift_out = -1; first =  0; }
    } else {
        shift_out = 0; first = 1;
    }

    /* the bit-reversal permutation, delta coded */
    {
        const int16_t *tab = (n == 32) ? ambe_fft_bitrev32 : ambe_fft_bitrev128;
        int count = tab[0], a = 0, b = 0, k;
        for (k = 0; k < count; k++) {
            int32_t t;
            a += tab[1 + 2 * k] * 2;
            b += tab[2 + 2 * k] * 2;
            t = buf[a / 4];
            buf[a / 4] = buf[b / 4];
            buf[b / 4] = t;
        }
    }

    /* the first radix-2 pass, at one of three scales */
    for (i = 0; i < n; i += 2) {
        int32_t p = buf[i], q = buf[i + 1];
        int32_t pr = RE(p), pi = IM(p), qr = RE(q), qi = IM(q);
        int sh = (first == 1) ? 0 : (first == -1 ? 2 : 1);
        buf[i]     = PACK((int16_t)((qr + pr) >> sh), (int16_t)((qi + pi) >> sh));
        buf[i + 1] = PACK((int16_t)((pr - qr) >> sh), (int16_t)((pi - qi) >> sh));
    }

    /* the remaining stages */
    if (order > 1) {
        int stride = 2, tw_stride = 0x80, groups = n >> 2;
        int stage = 1, rescanned = 0;
        short head = (short)(ambe_lzcount32((uint32_t)peak) - 2u - 2u);

        while (stage < order) {
            if (head >= 2) {
                stage_butterfly(buf, buf + stride, tw_stride, stride, groups, 0);
            } else {
                stage_butterfly(buf, buf + stride, tw_stride, stride, groups, 1);
                shift_out--;
            }
            stage++;
            if (stage >= order)
                break;
            if (!(rescanned & 1)) {
                int32_t p2 = 0x1000000;
                for (i = 0; i < n; i++) {
                    int32_t re = RE(buf[i]), im = IM(buf[i]);
                    int32_t e = re * re + im * im;
                    if (p2 < e) p2 = e;
                }
                head = (short)(ambe_lzcount32((uint32_t)p2) - 2u);
            }
            head = (short)(head - 2);
            stride *= 2;
            tw_stride >>= 1;
            groups >>= 1;
            rescanned++;
        }
    }
    return (short)(scale_exp - shift_out);
}

/*
 * Dsp_FftButterflyRecurse 0x00025484: the twiddled half of the real-spectrum
 * unpacking, pairing bin k with bin N-k.
 */
static void unpack_recurse(int32_t *lo, int32_t *hi, int tw_stride, int count)
{
    const int16_t *w = ambe_fft_twiddle_q15;

    while (count > 0) {
        int32_t xr, xi, wr, wi, yr, yi;
        w += 2 * tw_stride;
        xr = RE(*lo); xi = IM(*lo);
        wr = w[0];    wi = w[1];
        yr = RE(*hi); yi = IM(*hi);

        *hi = PACK((int16_t)((uint32_t)((xr * 0x8000 - wr * yr) + wi * yi) >> 16),
                   (int16_t)((uint32_t)(xi * -0x8000 + wi * yr + wr * yi) >> 16));
        *lo = PACK((int16_t)((uint32_t)((xr * 0x8000 + wr * yr) - wi * yi) >> 16),
                   (int16_t)((uint32_t)(xi *  0x8000 + wi * yr + wr * yi) >> 16));
        count--;
        lo++;
        hi--;
    }
}

/*
 * Dsp_FftButterflyStage 0x00025520: recover the 2N-point real spectrum from
 * the N-point complex transform.  DC and Nyquist are the special case at the
 * ends; the rest is the conjugate-symmetric combination plus the twiddled
 * part above.
 */
static short unpack_real(int32_t *buf, short scale_exp, int size_bits, int shift)
{
    int n = 1 << size_bits;
    int half = (n >> 2) - 1;
    /* the stock pointer is ushort*, so +n there is +n/2 complex words */
    int32_t *top = buf + (n >> 1);
    int i;

    if (shift == 0) {
        int32_t r = ((int32_t)RE(buf[0]) << 16) >> 1;
        int32_t m = ((int32_t)IM(buf[0]) << 16) >> 1;
        buf[0] = PACK((int16_t)((uint32_t)(m + r) >> 16),
                      (int16_t)((uint32_t)(r - m) >> 16));
    } else {
        int32_t r = ((int32_t)RE(buf[0]) << 16) >> 1;
        int32_t m = ((int32_t)IM(buf[0]) << 16) >> 1;
        buf[0]  = PACK((int16_t)((uint32_t)(m + r) >> 16), 0);
        top[0]  = PACK((int16_t)((uint32_t)(r - m) >> 16), 0);
    }

    /*
     * The conjugate-symmetric combination.  Note that the second output's
     * halves are crossed: the stock code writes the real combination into
     * b's imaginary slot and the imaginary one into b's real slot, which is
     * how this arrangement of the real-FFT unpacking works and is easy to
     * transcribe straight past.
     */
    for (i = 0; i < half; i++) {
        int32_t *a = buf + 1 + i;
        int32_t *b = top - 1 - i;
        int32_t ar = RE(*a), ai = IM(*a);
        int32_t br = RE(*b), bi = IM(*b);
        *a = PACK((int16_t)((br + ar) >> 1), (int16_t)((ai - bi) >> 1));
        *b = PACK((int16_t)((ai + bi) >> 1), (int16_t)((br - ar) >> 1));
    }

    unpack_recurse(buf + 1, top - 1, 0x200 >> size_bits, half);

    /*
     * The transform's midpoint is the packed real FFT's self-conjugate point:
     * with z[k] = x[2k] + j*x[2k+1], bin N/4 pairs with itself and the general
     * combination collapses to X[N/4] = conj(Z[N/4]).  The stock code reaches
     * it through a degenerate pass of the loops above, whose iteration count
     * Ghidra does not recover - the argument is left in a register.  Both
     * candidate counts leave this one bin wrong by several dB against a
     * reference DFT, so it is written directly from the identity instead, at
     * the same half scale the neighbouring bins carry.  One bin of 129, and
     * the only place in this file that is derived rather than transcribed.
     */
    {
        int32_t *m = buf + (n >> 2);
        *m = PACK((int16_t)(RE(*m) >> 1), (int16_t)((-(int32_t)IM(*m)) >> 1));
    }
    return (short)(scale_exp + 1);
}

/* Dsp_FftForward 0x000256D0 */
static short fft_forward(int32_t *buf, short scale_exp, int size_bits, int shift)
{
    short e = bitrev_scale(buf, scale_exp, size_bits - 1);
    short r = unpack_real(buf, e, size_bits, shift);
    if (shift == 0)
        buf[0] = PACK(RE(buf[0]), 0);
    return r;
}

/* Dsp_ComputeMagnitudeSquared 0x0001AB38 */
static void magnitude_squared(int32_t *out, const int32_t *in, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int32_t re = RE(in[i]), im = IM(in[i]);
        out[i] = (re * re + im * im) * 2;
    }
}

short ambe_fft_window(int32_t *magsq_out, const int16_t *in, int in_len,
                      short scale_bias, int32_t *fft_buf, int size_bits,
                      int shift)
{
    int16_t work[AMBE_FFT_MAX];
    int16_t *dst = (int16_t *)fft_buf;
    int16_t *back;
    short local_exp = 0, e;
    int half = in_len >> 1;
    int fft_size = 1 << size_bits;
    int j;

    memcpy(work, in, (size_t)in_len * sizeof(int16_t));
    normalize_array(work, work, in_len, &local_exp);

    /*
     * The window is folded: the table holds half of a symmetric window, and
     * the same tap serves one sample from each end of the frame.  The upper
     * half is written forward from the start of the buffer and the lower half
     * backward from its end, which is the packing the transform expects.
     */
    back = dst + fft_size;
    {
        const int16_t *src = work;
        if (in_len & 1) {
            *dst++ = (int16_t)(((int32_t)ambe_anwin_q15[half] * work[half] + 0x4000) >> 15);
            src = work + 1;
        }
        for (j = 0; j < half; j++) {
            int32_t w = ambe_anwin_q15[half - 1 - j];
            back--;
            dst[j]  = (int16_t)((w * (int32_t)src[j + half] + 0x4000) >> 15);
            *back   = (int16_t)((w * (int32_t)work[half - 1 - j] + 0x4000) >> 15);
        }
        dst += half;
    }
    memset(dst, 0, (size_t)(fft_size - in_len) * sizeof(int16_t));

    e = fft_forward(fft_buf, (short)(scale_bias + local_exp), size_bits, shift);
    if (magsq_out) {
        e = (short)(e * 2);
        magnitude_squared(magsq_out, fft_buf, shift + (fft_size >> 1));
    }
    return e;
}
