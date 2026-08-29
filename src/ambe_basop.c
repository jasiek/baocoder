/*
 * ambe_basop.c - the radio's fixed-point math primitives, transcribed.
 *
 * The stock code is ITU-T G.191 style basic operators: everything below is a
 * rounded Q15 multiply
 *
 *     mult_r(a, b) = (a*b + 0x4000) >> 15
 *
 * written the way the compiler emitted it, (a*b*2 + 0x8000) >> 16, plus
 * leading-zero normalisation and a Horner polynomial whose coefficients live
 * in the firmware image (src/ambe_tables_fw.c, extracted by
 * tools/extract_tables.py from SRAM 0x18001600..0x1800182F).
 *
 * These are transcriptions, not reimplementations: the expression structure is
 * the decompiler's, so the shift-and-mask scalings that carry each
 * coefficient's Q format are preserved exactly rather than being re-derived.
 * tests/test_basop.c sweeps every one of them across its whole input domain
 * against libm, which is what establishes that the transcription is right.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *   Math_SDiv                0x00018D74
 *   Math_SDivHalf            0x00018C9C
 *   Math_SDivHalfSat         0x00018D00
 *   Math_FloatAdd            0x00018DD8
 *   Math_FloatSub            0x00018E5C
 *   Math_FloatGreater        0x00018F98
 *   Math_FloatLess           0x00018FCC
 *   Math_TableInterpLookup   0x00019000
 *   Math_Log2                0x0001903C
 *   Math_Pow2                0x000191C0
 *   Math_Sqrt                0x00019364
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe_basop.h"
#include "ambe_tables.h"

/* The coefficients, read as the stock code reads them: unsigned 16-bit loads
   of what are signed Q15 values.  Keeping them unsigned here reproduces the
   firmware's exact 32-bit wrap-around behaviour. */
#define LOG2C(i)  ((uint32_t)(uint16_t)ambe_log2_coeff_q15[i])
#define POW2C(i)  ((uint32_t)(uint16_t)ambe_pow2_coeff_q15[i])
#define SQRTC(i)  ((uint32_t)(uint16_t)ambe_sqrt_coeff_q15[i])

unsigned int ambe_lzcount32(unsigned int x)
{
    unsigned int n = 0;
    if (x == 0)
        return 32;
    while ((x & 0x80000000u) == 0) {
        x <<= 1;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- division */

int ambe_sdiv(int a, short b)
{
    unsigned int sign = (unsigned int)a ^ (unsigned int)(int)b;
    unsigned int ua, ub;

    ua = (unsigned int)a;
    if (a < 0)
        ua = (a == (int)0x80000000) ? 0x7fffffffu : (unsigned int)(-a);
    ub = (unsigned int)(int)b;
    if (b < 0)
        ub = (b == (short)0x8000) ? 0x7fffu : (unsigned int)(-(int)b);

    if ((int)ua < (int)ub)
        return 0;
    return ((int)sign < 0) ? -(int)((int)ua / (int)ub) : (int)((int)ua / (int)ub);
}

int ambe_sdiv_half(int a, short b)
{
    unsigned int sign = (unsigned int)a ^ (unsigned int)(int)b;
    unsigned int ua, ub;
    int q;

    ua = (unsigned int)a;
    if (a < 0)
        ua = (a == (int)0x80000000) ? 0x7fffffffu : (unsigned int)(-a);
    ub = (unsigned int)(int)b;
    if (b < 0)
        ub = (b == (short)0x8000) ? 0x7fffu : (unsigned int)(-(int)b);

    q = ((int)ua < (int)ub) ? 0 : (int)(((int)ua / (int)ub) >> 1);
    return ((int)sign < 0) ? -q : q;
}

int ambe_sdiv_half_sat(int a, short b)
{
    unsigned int sign = (unsigned int)a ^ (unsigned int)(int)b;
    unsigned int ua, ub;
    int q;

    ua = (unsigned int)a;
    if (a < 0)
        ua = (a == (int)0x80000000) ? 0x7fffffffu : (unsigned int)(-a);
    ub = (unsigned int)(int)b;
    if (b < 0)
        ub = (b == (short)0x8000) ? 0x7fffu : (unsigned int)(-(int)b);

    if ((ua >> 16) >= ub)
        q = 0x7fff;
    else
        q = ((int)ua < (int)ub) ? 0 : (int)(((int)ua / (int)ub) >> 1);
    return ((int)sign < 0) ? -q : q;
}

/* ----------------------------------------------------------------- log2 */

/*
 * The rounded Q15 multiply every polynomial below is built from.  The stock
 * code emits it as (a*b*2 + 0x8000) >> 16 with a 16-bit truncation, which is
 * the ITU-T G.191 mult_r() exactly.
 */
static int32_t mult_r(int32_t a, int32_t b)
{
    return (int16_t)(((a * b * 2) + 0x8000) >> 16);
}

/* 16-bit truncation, the & 0xffff the stock code applies between stages. */
static int32_t s16(int32_t x)
{
    return (int16_t)x;
}

/*
 * Math_Log2 0x0001903C.  Value is mant * 2^(exp - 15) with mant in Q15;
 * result is log2 of it in Q16.  The mantissa is normalised to [0.5, 1) by a
 * leading-zero count, and the polynomial is centred on 1/sqrt(2) - coefficient
 * 0 is 23170 = 0.70709 Q15 - with constant term -0.5 = log2(1/sqrt(2)).
 */
int ambe_log2(int mant, int exp)
{
    uint32_t shift, norm;
    int32_t u, t, h;

    if (mant <= 0)
        return 0;

    shift = ambe_lzcount32((uint32_t)mant << 16) - 1;
    norm  = ((uint32_t)mant << 16) << (shift & 0x3f);
    u     = s16((int32_t)(norm >> 16) - ambe_log2_coeff_q15[0]);

    t = mult_r(u, ambe_log2_coeff_q15[1]);
    t = mult_r(u, s16(t + ambe_log2_coeff_q15[2]));
    t = mult_r(u, s16(t + ambe_log2_coeff_q15[3]));

    h = s16((((int32_t)ambe_log2_coeff_q15[4] << 16) + (t << 15)) >> 16);
    t = mult_r(h, u);

    return ((exp - (int)shift) << 16) +
           (((t << 18) + ((int32_t)ambe_log2_coeff_q15[5] << 16)) >> 15);
}

/* ----------------------------------------------------------------- pow2 */

/*
 * Math_Pow2 0x000191C0, the inverse of the above.  `log2v` is Q16 - integer
 * part in the high half, fraction in the low.  The fraction is taken to Q14,
 * a 5-deep Horner evaluates 2^f over it, and the exponent is bumped by one
 * because the polynomial lands in [0.5, 1).  Returns a Q31 mantissa: the value
 * is (mant / 2^31) * 2^exp_out.
 *
 * The shifts between stages differ - 13, 13, 14, 15, 15 - because each
 * coefficient carries its own Q format; they are the stock code's, not
 * re-derived.
 */
int ambe_pow2(unsigned int log2v, short *exp_out)
{
    /* zext r2,r0,0xf,0x1 at 0x000191C6: bits [15:1] of the Q16 input, i.e. the
       full 16-bit fraction carried as Q15.  Ghidra renders the bitfield
       extract as (v & 0x7fff) >> 1, which masks before shifting and loses the
       fraction's top bit; the disassembly settles it. */
    int32_t u = (int32_t)((log2v >> 1) & 0x7fff);
    int32_t a;

    *exp_out = (short)((short)(log2v >> 16) + 1);

    a = mult_r(u, ambe_pow2_coeff_q15[0]) << 13;
    a = s16(((((int32_t)ambe_pow2_coeff_q15[1]) << 16) + a) >> 16);
    a = mult_r(u, a) << 13;
    a = s16(((((int32_t)ambe_pow2_coeff_q15[2]) << 15) + a) >> 16);
    a = mult_r(u, a) << 14;
    a = s16(((((int32_t)ambe_pow2_coeff_q15[3]) << 15) + a) >> 16);
    a = mult_r(u, a) << 15;
    a = s16(((((int32_t)ambe_pow2_coeff_q15[4]) << 16) + a) >> 16);
    a = mult_r(a, u) << 15;

    return a + (((int32_t)ambe_pow2_coeff_q15[5]) << 16);
}

/* ----------------------------------------------------------------- sqrt */

/*
 * Math_Sqrt 0x00019364.  Normalise, evaluate a 2-deep Horner, then multiply by
 * 1/sqrt(2) - coefficient 3, 23170 Q15 - when the exponent is odd, because
 * halving an odd exponent leaves that factor behind.
 *
 * Input is mant * 2^(exp - 31) - a 32-bit accumulator handed over directly -
 * and the result is mant * 2^(exp - 15), Q15.  The asymmetry is the stock
 * code's.
 */
unsigned int ambe_sqrt(int mant, short *exp)
{
    int32_t norm, inner, horner, out;
    uint32_t shift;
    int32_t e;

    if (mant <= 0)
        return 0;

    shift = ambe_lzcount32((uint32_t)mant) - 1;
    norm  = (int32_t)(((uint32_t)mant << (shift & 0x3f)) >> 16);
    e     = (int32_t)*exp - (int32_t)shift;

    inner  = s16(ambe_sqrt_coeff_q15[1] + mult_r(norm, ambe_sqrt_coeff_q15[0]));
    horner = s16(ambe_sqrt_coeff_q15[2] + mult_r(norm, inner));

    if ((e & 1) != 0)
        out = mult_r(horner, ambe_sqrt_coeff_q15[3]);
    else
        out = horner;

    *exp = (short)((e + 1) >> 1);
    return (unsigned int)out;
}

/* ------------------------------------------------------------------ cos */

/*
 * Math_TableInterpLookup 0x00019000.  The phase is Q15 turns taken modulo
 * 2^16 as a signed 16-bit quantity; cos is even so the stock code takes the
 * absolute value, splits it into a 9-bit table index and a 16-bit fraction,
 * and interpolates linearly.
 */
int ambe_cos_q15(int phase_q15turns)
{
    uint32_t u = (uint32_t)((phase_q15turns << 16) >> 6);
    uint32_t idx, frac;
    int32_t a, b;

    if ((int32_t)u < 0)
        u = ~u + 1;
    idx  = (u >> 16) & 0x1ff;
    frac = u & 0xffff;
    a = ambe_cos512_q15[idx];
    b = ambe_cos512_q15[(idx + 1) & 0x1ff];

    /* The stock code's return type decompiles as uint, but the shift is
       arithmetic: the interpolated value is signed and the table is negative
       over half its range. */
    return (int)(((int32_t)((uint32_t)((int32_t)frac * b) + (uint32_t)(a * 0x10000) -
                            (uint32_t)((int32_t)frac * a))) >> 16);
}

int ambe_sin_q15(int phase_q15turns)
{
    /* sin(x) = cos(x - 90 degrees); a quarter turn is 8192 in Q15 turns. */
    return ambe_cos_q15(phase_q15turns - 8192);
}

/* ---------------------------------------------------------- block float */

ambe_bf ambe_bf_norm(ambe_bf a)
{
    uint32_t shift;

    if (a.mant == 0) {
        a.exp = 0;
        return a;
    }
    shift = ambe_lzcount32((uint32_t)(a.mant < 0 ? ~a.mant : a.mant)) - 1;
    a.mant <<= shift;
    a.exp -= (int32_t)shift;
    /* keep the mantissa in Q15 as the Math_Float* package does */
    a.mant >>= 16;
    a.exp += 16;
    return a;
}

ambe_bf ambe_bf_from_q(int32_t v, int q)
{
    ambe_bf r;
    r.mant = v;
    r.exp = 15 - q;
    return ambe_bf_norm(r);
}

ambe_bf ambe_bf_add(ambe_bf a, ambe_bf b)
{
    ambe_bf r;
    int32_t d;

    if (a.mant == 0) return b;
    if (b.mant == 0) return a;

    /* align the smaller exponent up, as Math_FloatAdd 0x00018DD8 does */
    if (a.exp < b.exp) {
        ambe_bf t = a; a = b; b = t;
    }
    d = a.exp - b.exp;
    if (d > 31)
        return a;
    r.mant = a.mant + (b.mant >> d);
    r.exp  = a.exp;
    return ambe_bf_norm(r);
}

ambe_bf ambe_bf_sub(ambe_bf a, ambe_bf b)
{
    b.mant = -b.mant;
    return ambe_bf_add(a, b);
}

int ambe_bf_gt(ambe_bf a, ambe_bf b)
{
    int32_t d;
    if (a.mant == 0 || b.mant == 0)
        return a.mant > b.mant;
    if (a.exp == b.exp)
        return a.mant > b.mant;
    d = a.exp - b.exp;
    if (d > 31)  return a.mant > 0;
    if (d < -31) return b.mant < 0;
    if (d > 0)
        return a.mant > (b.mant >> d);
    return (a.mant >> -d) > b.mant;
}

int ambe_bf_lt(ambe_bf a, ambe_bf b)
{
    return ambe_bf_gt(b, a);
}
