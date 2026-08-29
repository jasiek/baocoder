/*
 * test_basop.c - the radio's fixed-point math primitives, against libm.
 *
 * src/ambe_basop.c is a transcription of the stock code, and its coefficients
 * are bytes lifted out of the firmware image.  Neither of those facts proves
 * the transcription is right: the shift-and-mask scalings that carry each
 * coefficient's Q format are easy to get wrong, and a decompiler that types a
 * signed 16-bit value as `uint` will hand you a plausible-looking expression
 * that computes something else.  Both happened while this was written.
 *
 * What settles it is sweeping each primitive across its *whole* input domain
 * against libm.  A wrong scaling is not a small error - it is a factor of two
 * or a sign - so the bounds below are loose enough to be the radio's own
 * accuracy and tight enough that no misreading survives them.
 *
 * The bounds are the radio's, not ours, and two of them are worth stating
 * plainly because they propagate into everything above:
 *
 *   log2 is a four-term Taylor series about 1/sqrt(2), not a minimax fit.  Its
 *   error is ~1e-6 near the centre and 2.6e-3 at the ends of [0.5, 1).  The
 *   four coefficients match the Taylor coefficients of log2 about 1/sqrt(2) -
 *   2.0402, -1.4427, 1.3603, -1.4427 - to five digits, which is what confirms
 *   the polynomial's shape independently of the numerical agreement.
 *
 *   cos is a 512-entry table with linear interpolation, so its error is the
 *   interpolation residue, (2*pi/512)^2/8 = 6.0e-5, and the measured worst
 *   case is exactly that.
 *
 * This test may use floating point.  The library may not.
 */
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_tables.h"
#include "testutil.h"

int main(void)
{
    double worst_cos = 0, worst_log2 = 0, worst_pow2 = 0, worst_sqrt = 0;
    double worst_rt = 0;
    int i, m, e, q;

    /* ---- cos: every phase in one turn, exhaustively */
    for (i = -32768; i < 32768; i++) {
        double got  = ambe_cos_q15(i) / 32768.0;
        double want = cos(2.0 * M_PI * (double)i / 32768.0);
        double d = fabs(got - want);
        if (d > worst_cos) worst_cos = d;
    }
    CHECK(worst_cos < 7e-5, "cos worst error %.3e exceeds the table's "
          "interpolation residue\n", worst_cos);

    /* the table itself is cosine, despite the g_awSineTable512 name */
    for (i = 0; i < 512; i++) {
        int want = (int)(32767.0 * cos(2.0 * M_PI * (double)i / 512.0) +
                         (cos(2.0 * M_PI * (double)i / 512.0) < 0 ? -0.5 : 0.5));
        int d = ambe_cos512_q15[i] - want;
        CHECK(d >= -1 && d <= 1, "cos512[%d] = %d, cos gives %d\n",
              i, ambe_cos512_q15[i], want);
    }

    /* ---- log2: value = mant * 2^(exp - 15) */
    for (e = -30; e <= 30; e++) {
        for (m = 1; m < 32768; m += 3) {
            double v = (double)m * pow(2.0, (double)e - 15.0);
            double d = fabs(ambe_log2(m, e) / 65536.0 - log2(v));
            if (d > worst_log2) worst_log2 = d;
        }
    }
    CHECK(worst_log2 < 3e-3, "log2 worst error %.3e\n", worst_log2);

    /* the polynomial's coefficients are the Taylor series of log2 about
       1/sqrt(2); coefficient 0 is the centre and coefficient 5 the constant */
    CHECK(fabs(ambe_log2_coeff_q15[0] / 32768.0 - 1.0 / sqrt(2.0)) < 1e-4,
          "log2 centre %d is not 1/sqrt(2)\n", ambe_log2_coeff_q15[0]);
    CHECK(fabs(ambe_log2_coeff_q15[5] / 32768.0 + 0.5) < 1e-4,
          "log2 constant %d is not log2(1/sqrt(2))\n", ambe_log2_coeff_q15[5]);
    {
        /* d^n log2(x)/dx^n at 1/sqrt(2), as the assembly consumes them */
        double x0 = 1.0 / sqrt(2.0), ln2 = log(2.0);
        double want[4];
        int idx[4] = { 4, 3, 2, 1 };
        double scale[4] = { 4.0, 2.0, 2.0, 2.0 };
        want[0] =  1.0 / (x0 * ln2);
        want[1] = -1.0 / (2.0 * x0 * x0 * ln2);
        want[2] =  1.0 / (3.0 * x0 * x0 * x0 * ln2);
        want[3] = -1.0 / (4.0 * x0 * x0 * x0 * x0 * ln2);
        for (i = 0; i < 4; i++) {
            double got = scale[i] * ambe_log2_coeff_q15[idx[i]] / 32768.0;
            CHECK(fabs(got - want[i]) < 1e-3,
                  "log2 term %d: %.5f vs Taylor %.5f\n", i + 1, got, want[i]);
        }
    }

    /* ---- pow2: value = (mant / 2^31) * 2^exp */
    for (q = -20 * 65536; q < 20 * 65536; q += 97) {
        short ex;
        int mant = ambe_pow2((unsigned int)q, &ex);
        double got  = (double)mant / 2147483648.0 * pow(2.0, (double)ex);
        double want = pow(2.0, (double)q / 65536.0);
        double d = fabs(got - want) / want;
        if (d > worst_pow2) worst_pow2 = d;
    }
    CHECK(worst_pow2 < 2e-4, "pow2 worst relative error %.3e\n", worst_pow2);

    /* ---- sqrt: in  mant * 2^(exp - 31),  out  mant * 2^(exp - 15) */
    for (e = -20; e <= 20; e++) {
        for (m = 1 << 16; m > 0 && m < (1 << 30); m += 7919 * 997) {
            short ex = (short)e;
            unsigned int r = ambe_sqrt(m, &ex);
            double v    = (double)m * pow(2.0, (double)e - 31.0);
            double got  = (double)r * pow(2.0, (double)ex - 15.0);
            double want = sqrt(v);
            double d = fabs(got - want) / want;
            if (d > worst_sqrt) worst_sqrt = d;
        }
    }
    CHECK(worst_sqrt < 2e-3, "sqrt worst relative error %.3e\n", worst_sqrt);

    /* ---- the two log-domain primitives must invert each other */
    for (e = -10; e <= 10; e++) {
        for (m = 1024; m < 32768; m += 11) {
            double v = (double)m * pow(2.0, (double)e - 15.0);
            short ex;
            int mant = ambe_pow2((unsigned int)ambe_log2(m, e), &ex);
            double got = (double)mant / 2147483648.0 * pow(2.0, (double)ex);
            double d = fabs(got - v) / v;
            if (d > worst_rt) worst_rt = d;
        }
    }
    CHECK(worst_rt < 4e-3, "pow2(log2(x)) worst relative error %.3e\n", worst_rt);

    /* ---- the divides, against plain C semantics */
    {
        int a;
        for (a = -100000; a <= 100000; a += 337) {
            short b;
            for (b = -300; b <= 300; b = (short)(b + 37)) {
                int want, got;
                if (b == 0)
                    continue;
                got = ambe_sdiv(a, b);
                want = (abs(a) < abs((int)b)) ? 0 : (abs(a) / abs((int)b));
                if ((a < 0) != (b < 0))
                    want = -want;
                CHECK(got == want, "sdiv(%d, %d) = %d, want %d\n",
                      a, (int)b, got, want);
            }
        }
    }

    printf("[cos %.1e  log2 %.1e  pow2 %.1e  sqrt %.1e  pow2(log2) %.1e] ",
           worst_cos, worst_log2, worst_pow2, worst_sqrt, worst_rt);
    return t_done("firmware fixed-point primitives vs libm");
}
