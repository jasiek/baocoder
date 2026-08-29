/*
 * test_fft.c - the radio's spectral front end, against a reference DFT.
 *
 * src/ambe_fft.c is a transcription of Dsp_WindowAndComputeFft 0x00019B6C and
 * the Dsp_Fft* cluster, and like the basic operators it is checked against
 * what it is supposed to compute rather than against itself.  The reference
 * here applies the same window in the same units and then evaluates the DFT
 * directly, so any difference is the transform's own doing.
 *
 * Three transcription errors were caught by exactly this test, and none of
 * them was visible by reading the code:
 *
 *   - Ghidra renders mulsh's sign extension as `& 0xffff` on the operands,
 *     so the squared magnitudes came out as unsigned products and overflowed.
 *   - The stock code's buffer pointer is `ushort *`, so `+ (1 << size_bits)`
 *     advances half as far as the same expression on the int32 complex words;
 *     transcribing it straight overran the buffer by 2x.
 *   - In the real-spectrum unpacking the second output's halves are crossed -
 *     the real combination is written into the imaginary slot and vice versa.
 *     With that missed, the spectrum came out mirrored about N/2, which looks
 *     like a plausible spectrum until you check where the peak is.
 *
 * The bar is a ratio, not a correlation.  A block-floating-point transform
 * carries an arbitrary common scale, so if it is right then the difference
 * between its dB spectrum and the reference's is a *constant* - and measuring
 * how constant it is says far more than a correlation does.  Over the bins
 * within 40 dB of the peak that difference holds to a fraction of a decibel;
 * below that the fixed-point quantisation floor takes over and the
 * measurement would be of the floor rather than of the transform.
 */
#include "ambe.h"
#include "ambe_fft.h"
#include "ambe_tables.h"
#include "testutil.h"

#define NIN  AMBE_ANWIN_N     /* 199 */
#define NFFT 256

/* the same window, applied in the natural order, then a direct DFT */
static void reference(const int16_t *x, double *mag2)
{
    double w[NFFT];
    int i, k;

    for (i = 0; i < NFFT; i++)
        w[i] = 0.0;
    for (i = 0; i < NIN; i++) {
        int t = (i < 100) ? ambe_anwin_q15[i] : ambe_anwin_q15[NIN - 1 - i];
        w[i] = (double)x[i] * (double)t / 32768.0;
    }
    for (k = 0; k <= NFFT / 2; k++) {
        double re = 0.0, im = 0.0;
        for (i = 0; i < NFFT; i++) {
            double a = 2.0 * M_PI * (double)k * (double)i / (double)NFFT;
            re += w[i] * cos(a);
            im -= w[i] * sin(a);
        }
        mag2[k] = re * re + im * im;
    }
}

int main(void)
{
    int16_t x[NIN];
    int32_t buf[NFFT], mag[NFFT];
    double ref[NFFT / 2 + 1];
    double worst_spread = 0.0;
    unsigned int seed = 12345u;
    int trial, exact_peaks = 0;

    for (trial = 0; trial < 10; trial++) {
        double f1 = 0.021 + 0.013 * trial;
        double f2 = 0.081 + 0.017 * trial;
        double d[NFFT / 2], mean = 0.0, spread = 0.0;
        int i, k, n = 0, pk = 1, rk = 1;

        for (i = 0; i < NIN; i++) {
            double v = 6000.0 * sin(2.0 * M_PI * f1 * (double)i)
                     + 2500.0 * sin(2.0 * M_PI * f2 * (double)i + 1.1);
            seed = seed * 1103515245u + 12345u;
            v += 300.0 * (((double)((seed >> 16) & 0x7fff) / 16384.0) - 1.0);
            x[i] = (int16_t)v;
        }

        memset(buf, 0, sizeof(buf));
        ambe_fft_window(mag, x, NIN, -7, buf, 8, 1);
        reference(x, ref);

        for (k = 2; k < NFFT / 2; k++) {
            if (mag[k] > mag[pk]) pk = k;
            if (ref[k] > ref[rk]) rk = k;
        }
        CHECK(pk == rk, "trial %d: peak bin %d, reference says %d\n", trial, pk, rk);
        if (pk == rk)
            exact_peaks++;

        /* the dB offset, which must be one constant across the spectrum */
        for (k = 1; k < NFFT / 2; k++) {
            if (ref[k] < ref[rk] * 1e-4)      /* 40 dB below the peak */
                continue;
            d[n++] = 10.0 * log10((double)mag[k] + 1.0) - 10.0 * log10(ref[k] + 1.0);
        }
        CHECK(n > 8, "trial %d: only %d bins within 40 dB of the peak\n", trial, n);
        for (k = 0; k < n; k++)
            mean += d[k];
        mean /= n;
        for (k = 0; k < n; k++) {
            double e = d[k] - mean;
            if (e < 0.0) e = -e;
            if (e > spread) spread = e;
        }
        if (spread > worst_spread)
            worst_spread = spread;
        CHECK(spread < 1.5,
              "trial %d: dB offset varies by %.2f across the spectrum "
              "(mean %.2f) - the scale is not constant\n", trial, spread, mean);

        /* every magnitude must be a magnitude */
        for (k = 0; k <= NFFT / 2; k++)
            CHECK(mag[k] >= 0, "trial %d: mag[%d] = %d is negative\n",
                  trial, k, mag[k]);
    }

    /* a constant input has all its energy at DC */
    {
        int i, k;
        int32_t worst_leak = 0;
        for (i = 0; i < NIN; i++)
            x[i] = 4000;
        memset(buf, 0, sizeof(buf));
        ambe_fft_window(mag, x, NIN, -7, buf, 8, 1);
        for (k = 4; k <= NFFT / 2; k++)
            if (mag[k] > worst_leak)
                worst_leak = mag[k];
        CHECK(mag[0] > worst_leak * 16,
              "constant input: DC %d is not clear of the leakage floor %d\n",
              mag[0], worst_leak);
    }

    printf("[%d/10 peak bins exact, dB offset constant to %.2f dB] ",
           exact_peaks, worst_spread);
    return t_done("firmware FFT vs a reference DFT");
}
