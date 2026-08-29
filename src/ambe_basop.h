/*
 * ambe_basop.h - the radio's fixed-point math primitives.
 *
 * The DM-32UV has no FPU.  Its toolchain was configured soft-float, and the
 * vocoder does not even call that: the DSP path runs ITU-T G.191 style basic
 * operators over a block-floating-point package in IRAM, whose 50 callers are
 * the vocoder (baofeng-dm32uv-reveng docs/FINDINGS.md:186-194, :359).
 *
 * These are those operators, transcribed from the stock code, with their
 * coefficients read out of the image by tools/extract_tables.py.  Every
 * function cites the firmware function it comes from.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_BASOP_H
#define AMBE_BASOP_H

#include <stdint.h>

/*
 * Signed divide, Math_SDiv 0x00018D74.  Returns 0 when |a| < |b| rather than
 * rounding, saturates the two extreme operands, and carries the sign through
 * an XOR of the inputs.
 */
int ambe_sdiv(int a, short b);

/*
 * Math_SDivHalf 0x00018C9C and Math_SDivHalfSat 0x00018D00: the same divide
 * with a final >>1, the second saturating to 0x7FFF instead of dividing when
 * the quotient would not fit.
 */
int ambe_sdiv_half(int a, short b);
int ambe_sdiv_half_sat(int a, short b);

/*
 * log2 of a (mantissa, exponent) pair, Math_Log2 0x0001903C.  The mantissa is
 * normalised to [0.5, 1) in Q15 by a leading-zero count, a 4-deep Horner
 * polynomial centred on 1/sqrt(2) evaluates log2 of it, and the exponent is
 * folded in as the integer part.  Result is Q16.
 */
int ambe_log2(int mant, int exp);

/*
 * 2^x, Math_Pow2 0x000191C0.  `log2v` is Q16 - integer part in the high half,
 * fraction in the low.  Returns a Q31 mantissa and writes the exponent, so the
 * value is (mant / 2^31) * 2^exp_out.  The mantissa lands in [0.5, 1), which
 * is why the stock code bumps the exponent by one.
 */
int ambe_pow2(unsigned int log2v, short *exp_out);

/*
 * Square root of a (mantissa, exponent) pair, Math_Sqrt 0x00019364.
 * Normalise, 2-deep Horner, then a 1/sqrt(2) correction when the exponent is
 * odd, because halving an odd exponent leaves a factor of sqrt(2) behind.
 *
 * The two sides use different mantissa scales, which is the stock code's
 * convention, not a tidy-up opportunity: the input is  mant * 2^(exp - 31),
 * so a caller can hand it a 32-bit accumulator directly, and the result is
 * mant * 2^(exp - 15) with the mantissa in Q15.  `exp` is read and written
 * in place and is signed.
 */
unsigned int ambe_sqrt(int mant, short *exp);

/*
 * cos, Math_TableInterpLookup 0x00019000.  The phase is Q15 turns - a full
 * turn is 32768 - taken modulo 2^16 as a signed 16-bit quantity, and cos is
 * even, so the stock code takes |phase| and interpolates linearly between
 * adjacent entries of the 512-point table.  Result is Q15.
 *
 * The table is named g_awSineTable512 in the reverse-engineering ledger but
 * holds cosine: it matches 32767*cos(2*pi*i/512) to within 1 LSB at all 512
 * points (tests/test_tables.c asserts this).
 */
int ambe_cos_q15(int phase_q15turns);

/* sin(x) = cos(x - quarter turn), for callers that want it. */
int ambe_sin_q15(int phase_q15turns);

/*
 * Leading-zero count, the ff1/LZCOUNT the stock code normalises with.
 * lzcount32(0) is 32.
 */
unsigned int ambe_lzcount32(unsigned int x);

/* ---------------------------------------------------------- block float */

/*
 * The (mantissa, exponent) package at Math_FloatAdd 0x00018DD8 and its
 * neighbours.  Not IEEE and not the compiler's: explicit pairs, aligned by
 * shift and renormalised with ff1.  This is how the radio carries quantities
 * whose dynamic range does not fit a single Q format - the spectral
 * amplitudes above all.
 *
 * The value is  mant * 2^(exp - 30), the mantissa normalised to 30
 * significant bits - see ambe_basop.c for why that is wider than the radio's.
 */
typedef struct {
    int32_t mant;
    int32_t exp;
} ambe_bf;

ambe_bf ambe_bf_add(ambe_bf a, ambe_bf b);   /* Math_FloatAdd 0x00018DD8 */
ambe_bf ambe_bf_sub(ambe_bf a, ambe_bf b);   /* Math_FloatSub 0x00018E5C */
int     ambe_bf_gt(ambe_bf a, ambe_bf b);    /* Math_FloatGreater 0x00018F98 */
int     ambe_bf_lt(ambe_bf a, ambe_bf b);    /* Math_FloatLess 0x00018FCC */

/*
 * Multiply, divide and square root on the pair.  The radio's package has no
 * multiply of its own - its callers do that inline against Math_SDiv and
 * Math_Sqrt - so these are ours, built on its primitives and its convention.
 */
ambe_bf ambe_bf_mul(ambe_bf a, ambe_bf b);
ambe_bf ambe_bf_div(ambe_bf a, ambe_bf b);
ambe_bf ambe_bf_sqrt(ambe_bf a);           /* uses Math_Sqrt 0x00019364 */
ambe_bf ambe_bf_from_i64(int64_t v, int q);

/* Value as a plain integer scaled by 2^q, saturating.  For handing block
   floats back to code that wants a fixed Q format. */
int32_t ambe_bf_to_q(ambe_bf a, int q);

/* Renormalise so the mantissa uses the full Q15 range. */
ambe_bf ambe_bf_norm(ambe_bf a);
/* Build a block float from a plain integer scaled by 2^-q. */
ambe_bf ambe_bf_from_q(int32_t v, int q);

#endif /* AMBE_BASOP_H */
