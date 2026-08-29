/*
 * test_subband.c - the 16-channel filterbank's transform, against a direct DFT.
 *
 * src/ambe_subband.c transcribes Vocoder_SubbandSumDifference 0x0002AFE8, which
 * evaluates a 32-point real DFT from a stored matrix.  Like the basic operators
 * and the big FFT, it is checked against what it is supposed to compute rather
 * than against itself: the reference below is a textbook DFT in doubles, and
 * any difference is the transcription's own doing.
 *
 * Three things are worth asserting separately, because they fail differently.
 *
 *   - The *scale*.  Every bin should come out as X[k]/16, and the two paths to
 *     that number look nothing alike in the firmware - bin 0 is `total >> 3`
 *     and the rest are `acc >> 18`.  Getting either constant wrong leaves a
 *     transform that is still a correct DFT of something, just at the wrong
 *     level, and only a scale check catches it.
 *
 *   - The *sign of the imaginary part*.  The stored sine table is already
 *     negated, so transcribing the multiply "correctly" and also negating gives
 *     a conjugated spectrum.  That is invisible to a magnitude test, which is
 *     exactly what the stage does with the result downstream, so it has to be
 *     caught here or not at all.
 *
 *   - The *fold*, on negative samples.  x[0] and x[16] are halved by a signed
 *     bitfield extract that Ghidra renders as a logical one.  A DC-free signal
 *     will not show the difference; the test therefore includes signals with a
 *     large negative offset, where the wrong shift is off by 32768.
 *
 * The tolerance is in LSB of the 16-bit output, not a correlation.  The
 * transform truncates rather than rounds at three points - the two halvings and
 * the >> 18 - so a couple of counts of disagreement with an exact DFT is the
 * expected behaviour, not slack.
 */
#include "ambe.h"
#include "ambe_subband.h"
#include "ambe_tables.h"
#include "testutil.h"

/* X[k]/16 for a real 32-point input, evaluated directly */
static void reference(const int16_t x[32], double re[16], double im[16])
{
    int k, n;
    for (k = 0; k < 16; k++) {
        double sr = 0.0, si = 0.0;
        for (n = 0; n < 32; n++) {
            double th = 2.0 * M_PI * (double)k * (double)n / 32.0;
            sr += (double)x[n] * cos(th);
            si -= (double)x[n] * sin(th);
        }
        re[k] = sr / 16.0;
        im[k] = si / 16.0;
    }
}

static double worst_abs = 0.0;

static void one_case(const char *what, const int16_t in[32], double tol)
{
    int16_t b[32];
    double re[16], im[16];
    int k;

    memcpy(b, in, sizeof(b));
    ambe_subband_dft32(b);
    reference(in, re, im);

    CHECK(b[1] == 0, "%s: bin 0 imaginary part is %d, not 0\n", what, b[1]);
    for (k = 0; k < 16; k++) {
        double dr = fabs((double)b[2 * k] - re[k]);
        double di = fabs((double)b[2 * k + 1] - im[k]);
        if (dr > worst_abs) worst_abs = dr;
        if (di > worst_abs) worst_abs = di;
        CHECK(dr <= tol, "%s: bin %d real %d, direct DFT/16 gives %.2f\n",
              what, k, b[2 * k], re[k]);
        CHECK(di <= tol, "%s: bin %d imag %d, direct DFT/16 gives %.2f\n",
              what, k, b[2 * k + 1], im[k]);
    }
}

int main(void)
{
    int16_t x[32];
    int i, k;

    /*
     * A single bin at a time.  This is the sharpest test of the sign
     * convention: a cosine must land wholly in the real part and a sine wholly
     * in the imaginary one, with the sign the e^-i convention demands.
     */
    for (k = 1; k <= 15; k++) {
        char nm[48];
        for (i = 0; i < 32; i++)
            x[i] = (int16_t)(8000.0 * cos(2.0 * M_PI * k * i / 32.0));
        sprintf(nm, "cos bin %d", k);
        one_case(nm, x, 3.0);

        for (i = 0; i < 32; i++)
            x[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * k * i / 32.0));
        sprintf(nm, "sin bin %d", k);
        one_case(nm, x, 3.0);
    }

    /* DC, which exercises the bin-0 path and nothing else */
    for (i = 0; i < 32; i++) x[i] = 10000;
    one_case("dc +10000", x, 3.0);

    /*
     * A large *negative* offset.  x[0] and x[16] are halved by a signed
     * bitfield extract; transcribing Ghidra's `(v & 0x7fff) >> 1` literally
     * puts those two samples out by 32768 and nothing else in the test would
     * notice.
     */
    for (i = 0; i < 32; i++) x[i] = -10000;
    one_case("dc -10000", x, 3.0);

    for (i = 0; i < 32; i++)
        x[i] = (int16_t)(-12000 + 6000.0 * cos(2.0 * M_PI * 3 * i / 32.0));
    one_case("negative-biased cos", x, 3.0);

    /* full-scale extremes at exactly the two unpaired samples */
    memset(x, 0, sizeof(x));
    x[0] = -32768; x[16] = -32767;
    one_case("unpaired samples at -full scale", x, 3.0);

    /* a deterministic pseudo-random signal, the general case */
    {
        uint32_t s = 12345u;
        int trial;
        for (trial = 0; trial < 64; trial++) {
            char nm[48];
            for (i = 0; i < 32; i++) {
                s = s * 1103515245u + 12345u;
                x[i] = (int16_t)((int32_t)((s >> 16) & 0xffff) - 32768) / 4;
            }
            sprintf(nm, "random %d", trial);
            one_case(nm, x, 3.0);
        }
    }

    /*
     * The window's gain, which is the other half of the "stage is unity"
     * claim: 32 folded taps applied to a constant must scale it by 16.
     */
    {
        int16_t w[32];
        int32_t sum = 0;
        for (i = 0; i < 32; i++) x[i] = 1000;
        ambe_subband_window(w, x);
        for (i = 0; i < 32; i++) sum += w[i];
        /* 16 * 1000, to within the per-tap rounding */
        CHECK(sum >= 16000 - 32 && sum <= 16000 + 32,
              "windowed DC sums to %d, want 16 x 1000\n", sum);
        printf("\n    window    32 folded taps, DC 1000 -> %d (want 16000)", sum);
    }

    printf("\n    dft32     %d cases, worst %.2f LSB vs a direct DFT",
           2 * 15 + 4 + 64, worst_abs);
    printf("\n                         ");
    return t_done("sub-band filterbank: 32-point real DFT");
}
