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

    /*
     * The transform at 64 points, which is the size the analyser's eight-band
     * loop uses - Dsp_FftForward(..., 6, 0) - and a different code path from
     * everything above: one fewer radix-2 stage, the N = 32 bit-reversal table
     * instead of the N = 128 one, and a shallower unpack recursion.  Until now
     * ambe_fft.c was only ever exercised at 256 points, so a size-dependent
     * error would have been invisible.
     *
     * This calls ambe_fft_forward directly, without a window, because that is
     * how the band loop calls it: it does its own windowing first.  Samples go
     * into the int16 slots in natural order, which is the packing the
     * transform expects (slot 2m is the real half of word m).
     */
    {
        double worst64 = 0.0;
        int trial64;

        for (trial64 = 0; trial64 < 8; trial64++) {
            int32_t b64[32];
            int16_t *slot = (int16_t *)b64;
            double xr[64], d[32], mean = 0.0, spread = 0.0;
            double ref2[32];
            int i, k, n = 0;
            short e;

            for (i = 0; i < 64; i++) {
                double v = 7000.0 * sin(2.0 * M_PI * (0.05 + 0.04 * trial64) * i)
                         + 2000.0 * cos(2.0 * M_PI * 0.17 * i + 0.7);
                seed = seed * 1103515245u + 12345u;
                v += 200.0 * (((double)((seed >> 16) & 0x7fff) / 16384.0) - 1.0);
                xr[i] = (double)(int16_t)v;
                slot[i] = (int16_t)v;
            }

            for (k = 0; k < 32; k++) {
                double re = 0.0, im = 0.0;
                for (i = 0; i < 64; i++) {
                    double a = 2.0 * M_PI * (double)k * (double)i / 64.0;
                    re += xr[i] * cos(a);
                    im -= xr[i] * sin(a);
                }
                ref2[k] = re * re + im * im;
            }

            e = ambe_fft_forward(b64, 0, 6, 0);
            (void)e;

            /* |X|^2 over the 32 bins, the way the band loop takes it */
            {
                int32_t m64[32];
                int rk = 1;
                for (k = 0; k < 32; k++) {
                    int32_t re = (int32_t)(int16_t)(b64[k] & 0xffff);
                    int32_t im = (int32_t)(int16_t)((uint32_t)b64[k] >> 16);
                    m64[k] = re * re + im * im;
                }
                for (k = 2; k < 32; k++)
                    if (ref2[k] > ref2[rk]) rk = k;
                {
                    int pk = 1;
                    for (k = 2; k < 32; k++)
                        if (m64[k] > m64[pk]) pk = k;
                    CHECK(pk == rk,
                          "64-pt trial %d: peak bin %d, reference says %d\n",
                          trial64, pk, rk);
                }
                for (k = 1; k < 32; k++) {
                    if (ref2[k] < ref2[rk] * 1e-4)
                        continue;
                    d[n++] = 10.0 * log10((double)m64[k] + 1.0)
                           - 10.0 * log10(ref2[k] + 1.0);
                }
                CHECK(n >= 3, "64-pt trial %d: only %d usable bins\n",
                      trial64, n);
                for (k = 0; k < n; k++) mean += d[k];
                mean /= (n ? n : 1);
                for (k = 0; k < n; k++) {
                    double ee = d[k] - mean;
                    if (ee < 0.0) ee = -ee;
                    if (ee > spread) spread = ee;
                }
                if (spread > worst64) worst64 = spread;
                CHECK(spread < 1.5,
                      "64-pt trial %d: dB offset varies by %.2f (mean %.2f)\n",
                      trial64, spread, mean);
            }
        }
        printf("[%d/10 peak bins exact, dB offset constant to %.2f dB; "
               "64-pt to %.2f dB] ", exact_peaks, worst_spread, worst64);
    }

    return t_done("firmware FFT vs a reference DFT");
}
