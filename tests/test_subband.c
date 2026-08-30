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
     * The per-band magnitudes.  Two properties, and the second is the one that
     * says the exponent bookkeeping was read right.
     *
     * First, they are |X[b]| - the plain bin magnitude.  That is not obvious
     * from the code, which roots the energy |X|^2 * 2: ambe_sqrt returns its
     * mantissa carrying a factor of 1/sqrt(2), and that cancels the 2 exactly.
     * The confirmation is band 0, which takes no root at all and is just the
     * real part - so all sixteen bands agree on one definition, which they
     * would not if either the 2 or the 1/sqrt(2) had been misread.
     *
     * Second, and this is the check that pins r4, they are *invariant to the
     * input exponent*.  The stock code takes the roots against 2*e_in and
     * aligns the results back to e_in, which is the identity - so a channel
     * sample carries the units the PCM did, and feeding the same spectrum with
     * a different e_in must give the same magnitudes.  Any other alignment
     * would leave the 16 x 49 ring in units nothing downstream knows, and this
     * is what would catch it.
     */
    {
        int16_t bins[32], mag[16], mag2[16];
        double worst = 0.0;
        int b, trial;
        uint32_t s2 = 7u;

        for (trial = 0; trial < 32; trial++) {
            for (i = 0; i < 32; i++) {
                s2 = s2 * 1103515245u + 12345u;
                x[i] = (int16_t)((int32_t)((s2 >> 16) & 0xffff) - 32768) / 8;
            }
            memcpy(bins, x, sizeof(bins));
            ambe_subband_window(bins, bins);
            ambe_subband_dft32(bins);

            ambe_subband_magnitudes(mag, bins, 0);

            /* the roots themselves: plain |X[b]|, see below */
            for (b = 1; b < 16; b++) {
                double re = bins[2 * b], im = bins[2 * b + 1];
                double want = sqrt(re * re + im * im);
                double d = fabs((double)mag[b] - want);
                if (d > worst) worst = d;
                /*
                 * The bar is absolute, not relative.  The output is a 16-bit
                 * integer, so one count of disagreement is unavoidable, and on
                 * a small magnitude that is several percent - quoting a
                 * relative figure would be quoting the quantiser.  What is
                 * asserted is the radio's own sqrt accuracy (1.3e-3 relative,
                 * see the basic-operator table) plus that one count.
                 */
                CHECK(d <= 1.0 + 1.3e-3 * want,
                      "magnitude[%d] = %d, |X| is %.2f (off by %.2f)\n",
                      b, mag[b], want, d);
            }
            /* band 0 is the clamped real part, never a root */
            CHECK(mag[0] == (bins[0] > 0 ? bins[0] : 0),
                  "magnitude[0] = %d, want the clamped bin-0 real part %d\n",
                  mag[0], bins[0]);

            /* invariant to the input exponent */
            for (b = 0; b < 4; b++) {
                ambe_subband_magnitudes(mag2, bins, b * 3);
                CHECK(memcmp(mag, mag2, sizeof(mag)) == 0,
                      "magnitudes change with e_in = %d - the alignment is "
                      "not the identity it must be\n", b * 3);
            }
        }
        printf("\n    magnitude 32 frames, worst %.2f LSB vs |X|, "
               "invariant to e_in", worst);
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

    /*
     * The decimator, against a plain convolution.  Two things are asserted
     * separately: that it is the symmetric 7-tap filter the table says, run
     * down one channel at a stride of 16 and advancing two sets per output;
     * and that it passes a constant through unchanged, which is the taps
     * summing to 65536 seen from the other side.
     */
    {
        int16_t sets[16 * 64];
        int16_t out[16];
        int chan, n, worst = 0;
        uint32_t s = 999u;

        for (i = 0; i < 16 * 64; i++) {
            s = s * 1103515245u + 12345u;
            sets[i] = (int16_t)((int32_t)((s >> 16) & 0xffff) - 32768) / 4;
        }
        for (chan = 0; chan < 16; chan++) {
            ambe_subband_decimate(out, sets, chan, 16);
            for (n = 0; n < 16; n++) {
                double acc = 0.0;
                int t;
                static const int tp[7] = { 0, 1, 2, 3, 2, 1, 0 };
                for (t = 0; t < 7; t++)
                    acc += (double)ambe_subband_fir_q15[tp[t]] *
                           sets[chan + 16 * (2 * n + t)];
                acc = (acc + 32768.0) / 65536.0;
                {
                    int d = out[n] - (int)floor(acc);
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                    CHECK(d <= 1,
                          "decimate ch %d out %d: %d, convolution gives %.2f\n",
                          chan, n, out[n], acc);
                }
            }
        }
        printf("\n    decimate  16 channels x 16 outputs, worst %d LSB", worst);

        /* unity gain: a constant must survive the filter */
        for (i = 0; i < 16 * 64; i++) sets[i] = 4321;
        ambe_subband_decimate(out, sets, 5, 16);
        for (n = 0; n < 16; n++)
            CHECK(out[n] == 4321, "decimate of a constant gave %d, want 4321\n",
                  out[n]);
        printf("\n              constant 4321 passes through unchanged");
    }

    /*
     * The energy path.  Parseval is the check that ties the whole stage
     * together: for a real 32-point input the sum of |X[k]|^2 over all 32 bins
     * is 32 times the sum of x[n]^2, and bins 16..31 mirror bins 1..15.  The
     * accumulator carries |X|^2 * 2 >> 7 on the X/16 scale, so the expected
     * total is (32 * sum x^2) / (256 * 64), and matching that says the >> 7
     * and the DFT's own scaling were both read right.
     */
    {
        int32_t energy[16];
        int16_t bins[32];
        double want, got = 0.0;
        int b;

        for (i = 0; i < 32; i++)
            x[i] = (int16_t)(9000.0 * cos(2.0 * M_PI * 5 * i / 32.0)
                             + 3000.0 * sin(2.0 * M_PI * 2 * i / 32.0));

        memset(energy, 0, sizeof(energy));
        ambe_subband_frame(energy, bins, x);

        /* the window is applied inside, so compare against the windowed input */
        {
            int16_t w[32];
            double sw2 = 0.0;
            ambe_subband_window(w, x);
            for (i = 0; i < 32; i++) sw2 += (double)w[i] * w[i];
            /* sum over the 16 stored bins double-counts nothing at 0 and 16,
               so fold: total = E[0] + 2*sum(E[1..14]) + E[15] on the |X|^2
               scale, then the stage's own 2 and >> 7 */
            for (b = 0; b < 16; b++)
                got += (double)energy[b] * ((b == 0 || b == 15) ? 1.0 : 2.0);
            want = 32.0 * sw2 / 256.0 * 2.0 / 128.0;
            CHECK(fabs(got - want) < 0.02 * want,
                  "Parseval: energies total %.0f, want %.0f (%.1f%% off)\n",
                  got, want, 100.0 * fabs(got - want) / want);
            printf("\n    energy    Parseval holds to %.2f%%",
                   100.0 * fabs(got - want) / want);
        }
    }

    /*
     * The frame scheduler.  The property that matters is conservation: over a
     * long run the samples consumed must equal the samples supplied, with the
     * carried remainder accounting for the difference exactly.  A resampler
     * that loses or duplicates a sample every few frames would still look
     * plausible frame by frame and would drift the analysis window against the
     * audio, which is the kind of fault that shows up much later as bad pitch.
     *
     * The second property is the buffer bound: the output is 16 x 11, so no
     * frame size the caller can pass may ever ask for a twelfth sample.
     */
    {
        int nf, worst_count = 0;
        for (nf = 76; nf <= 84; nf++) {
            int16_t acc = 0;
            long supplied = 0, consumed = 0;
            int f;
            for (f = 0; f < 5000; f++) {
                int nsamp = 0, off = 0;
                int c = ambe_subband_advance(&acc, nf, &nsamp, &off);
                supplied += nf;
                consumed += 8 * c;
                CHECK(c >= 0 && c <= AMBE_SUBBAND_MAX_OUT,
                      "frame size %d asked for %d outputs, buffer holds %d\n",
                      nf, c, AMBE_SUBBAND_MAX_OUT);
                CHECK(acc >= 0 && acc < 8,
                      "frame size %d left a remainder of %d\n", nf, acc);
                CHECK(nsamp == 8 * c + AMBE_SUBBAND_OVERLAP,
                      "frame size %d wants %d samples for %d outputs\n",
                      nf, nsamp, c);
                CHECK(off >= 0 && off + nsamp <= AMBE_SUBBAND_HISTORY,
                      "frame size %d reads [%d,%d) of a %d-sample history\n",
                      nf, off, off + nsamp, AMBE_SUBBAND_HISTORY);
                if (c > worst_count) worst_count = c;
            }
            CHECK(consumed + acc == supplied,
                  "frame size %d: supplied %ld, consumed %ld, carried %d\n",
                  nf, supplied, consumed, acc);
        }
        printf("\n    schedule  76..84 x 5000 frames, exact conservation, "
               "peak %d of %d", worst_count, AMBE_SUBBAND_MAX_OUT);
    }

    /*
     * The eight-band loop's per-channel spectrum: the 58-tap window folded,
     * zero-padded to 64, transformed, squared.  Checked the way test_fft.c
     * checks the big transform - a block-float spectrum is right when its dB
     * offset against a reference is a *constant*, since the common scale is
     * arbitrary - and separately that the fold really is symmetric, which a
     * magnitude test cannot see.
     */
    {
        int16_t seg[58];
        int32_t magsq[32];
        double worst = 0.0;
        int trial, j, k;
        uint32_t s3 = 31u;

        for (trial = 0; trial < 8; trial++) {
            double w[64], d[32], mean = 0.0, spread = 0.0, ref[32];
            int n = 0, rk = 2, pk = 2;

            for (j = 0; j < 58; j++) {
                double v = 9000.0 * sin(2.0 * M_PI * (0.06 + 0.05 * trial) * j)
                         + 2500.0 * cos(2.0 * M_PI * 0.21 * j + 0.4);
                s3 = s3 * 1103515245u + 12345u;
                v += 250.0 * (((double)((s3 >> 16) & 0x7fff) / 16384.0) - 1.0);
                seg[j] = (int16_t)v;
            }

            /* the reference applies the same folded window, then a plain DFT */
            for (j = 0; j < 64; j++) w[j] = 0.0;
            for (j = 0; j < 29; j++) {
                double t = ambe_subwin_q15[j] / 32768.0;
                w[j]      = seg[j] * t;
                w[57 - j] = seg[57 - j] * t;
            }
            for (k = 0; k < 32; k++) {
                double re = 0.0, im = 0.0;
                for (j = 0; j < 64; j++) {
                    double a = 2.0 * M_PI * (double)k * (double)j / 64.0;
                    re += w[j] * cos(a);
                    im -= w[j] * sin(a);
                }
                ref[k] = re * re + im * im;
            }

            ambe_band_spectrum(magsq, seg, 0);

            CHECK(magsq[0] == 0 && magsq[1] == 0,
                  "bins 0 and 1 are %d and %d, the stock code zeroes both\n",
                  magsq[0], magsq[1]);

            for (k = 3; k < 32; k++) {
                if (ref[k] > ref[rk]) rk = k;
                if (magsq[k] > magsq[pk]) pk = k;
            }
            CHECK(pk == rk, "band trial %d: peak bin %d, reference says %d\n",
                  trial, pk, rk);
            for (k = 2; k < 32; k++) {
                if (ref[k] < ref[rk] * 1e-4) continue;
                d[n++] = 10.0 * log10((double)magsq[k] + 1.0)
                       - 10.0 * log10(ref[k] + 1.0);
            }
            CHECK(n >= 3, "band trial %d: only %d usable bins\n", trial, n);
            for (k = 0; k < n; k++) mean += d[k];
            mean /= (n ? n : 1);
            for (k = 0; k < n; k++) {
                double e2 = d[k] - mean;
                if (e2 < 0.0) e2 = -e2;
                if (e2 > spread) spread = e2;
            }
            if (spread > worst) worst = spread;
            CHECK(spread < 1.5,
                  "band trial %d: dB offset varies by %.2f (mean %.2f)\n",
                  trial, spread, mean);
        }
        printf("\n    band      58-tap window into 64-pt, dB offset "
               "constant to %.2f dB", worst);

        /*
         * The fold, which the dB-offset test above cannot see: it would pass
         * just as well if seg[j] and seg[57-j] took different taps.
         *
         * Reversing the segment must leave the magnitude spectrum alone.  For
         * a real sequence, reversal conjugates the transform and multiplies it
         * by a unit phase, so |X| is unchanged - and it holds here despite the
         * zero padding, because the pad sits at 58..63 in both cases.  If the
         * fold were wrong the two windowed sequences would not be reversals of
         * each other and the magnitudes would differ grossly.
         *
         * The bar is 3% of the peak rather than equality, and that is measured
         * rather than chosen: the windowed integers are exact reversals, but
         * the transform rounds through five butterfly stages and rounding is
         * not reversal-invariant, which costs 1.6% on the peak bin here.
         * Asserting equality would be asserting that a fixed-point FFT is
         * exact.  The bar is still load-bearing - giving seg[57-j] the tap
         * ambe_subwin_q15[28-j] instead of [j], the plausible way to get the
         * fold wrong, moves the peak bin by 56% - that was checked, not assumed.
         */
        {
            int32_t m1[32], m2[32];
            int16_t rev[58];
            double wf = 0.0;
            for (j = 0; j < 58; j++) seg[j] = (int16_t)(1000 + 37 * j);
            for (j = 0; j < 58; j++) rev[j] = seg[57 - j];
            ambe_band_spectrum(m1, seg, 0);
            ambe_band_spectrum(m2, rev, 0);
            {
                double peak = 1.0;
                for (k = 2; k < 32; k++)
                    if ((double)m1[k] > peak) peak = (double)m1[k];
                for (k = 2; k < 32; k++) {
                    double d2 = fabs((double)m1[k] - (double)m2[k]) / peak;
                    if (d2 > wf) wf = d2;
                    CHECK(d2 < 0.03,
                          "reversing the segment moved bin %d by %.2f%% of "
                          "the peak (%d vs %d) - the fold is not symmetric\n",
                          k, 100.0 * d2, m1[k], m2[k]);
                }
            }
            printf("\n              fold symmetric under reversal to %.2f%% "
                   "of peak", 100.0 * wf);
        }
    }

    /*
     * The exponent-aligned add.  What must hold is that it adds the two
     * *values*, not the two mantissas: block floats here are mantissa * 2^exp,
     * so the operand with the larger exponent is kept and the other shifted
     * right.  Getting the sense backwards still produces a plausible spectrum,
     * just one where the quieter channel drowns the louder.
     */
    {
        int32_t a[32], b[32], out[32];
        int ea, eb, k;
        double worst = 0.0;
        uint32_t s4 = 5u;

        for (k = 0; k < 32; k++) {
            s4 = s4 * 1103515245u + 12345u;
            a[k] = (int32_t)((s4 >> 8) & 0x00ffffff);
            s4 = s4 * 1103515245u + 12345u;
            b[k] = (int32_t)((s4 >> 8) & 0x00ffffff);
        }
        for (ea = -4; ea <= 4; ea++) {
            for (eb = -4; eb <= 4; eb++) {
                short e = ambe_band_add(out, a, (short)ea, b, (short)eb);
                CHECK(e == (short)(ea > eb ? ea : eb),
                      "add(%d,%d) returned exponent %d\n", ea, eb, e);
                for (k = 0; k < 32; k++) {
                    double want = a[k] * pow(2.0, ea) + b[k] * pow(2.0, eb);
                    double got  = out[k] * pow(2.0, e);
                    double rel  = fabs(got - want) / (fabs(want) + 1.0);
                    if (rel > worst) worst = rel;
                    /*
                     * The stock shift truncates, so up to 2^d is lost from
                     * mantissas of order 2^24 - about 1e-5 at the widest
                     * alignment here.  The tolerance is set by that, not
                     * chosen: a swapped sense would miss by orders of
                     * magnitude, which is what this is here to catch.
                     */
                    CHECK(rel < 1e-4,
                          "add(%d,%d) bin %d: %.1f, want %.1f\n",
                          ea, eb, k, got, want);
                }
            }
        }
        printf("\n              exponent-aligned add exact to %.1e relative",
               worst);
    }

    printf("\n    dft32     %d cases, worst %.2f LSB vs a direct DFT",
           2 * 15 + 4 + 64, worst_abs);
    printf("\n                         ");
    return t_done("sub-band filterbank: 32-point real DFT");
}
