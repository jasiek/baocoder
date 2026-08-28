/*
 * ambe_analysis.c - 160 PCM samples -> MBE model parameters.
 *
 * This is the one stage with no counterpart to transcribe: the stock analyser
 * is spread across the pitch/voicing cluster (Vocoder_SelectVoicingCandidate
 * 0x000177F0, Vocoder_RefinePitchEstimate 0x00025A60,
 * Vocoder_AnalyzeSpectrum 0x000205B8, Dsp_WindowAndComputeFft 0x00019B6C) and
 * is dense fixed-point DSP with its own multi-candidate search over energy
 * blocks.  What is implemented here is a conventional MBE analyser producing
 * the same parameter set, not a transcription of that search - the estimator is
 * ours, the quantiser it feeds (ambe_encode_params.c) is the firmware's.
 *
 * Pitch is found by normalised cross-correlation over lags 20..123 samples,
 * which is exactly the span the codec's pitch quantiser covers (f0 from 0.05
 * down to 0.0081 cycles/sample).  Octave errors - the characteristic failure of
 * correlation pitch detectors - are suppressed by preferring the shortest lag
 * whose score is within a few percent of the best.
 *
 * Voicing is decided per band from how much of the band's energy sits at the
 * harmonic peaks, and the spectral amplitudes are RMS values over each
 * harmonic's band of a Hamming-windowed 256-point DFT.
 *
 * SPDX-License-Identifier: ISC
 */
#include <math.h>
#include <string.h>
#include "ambe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFFT 256
#define PITCH_MIN 20        /* f0 = 0.0500 */
#define PITCH_MAX 123       /* f0 = 0.0081 */

/*
 * Amplitude scale between PCM units and the model's Ml.  The decoder's output
 * path is Ml -> overlap-added sinusoids -> x7 in ambe_float_to_s16, so the
 * inverse carries a fixed constant; it is calibrated so that encoding a decoded
 * frame reproduces its gain (tests/test_encode_pcm.c holds that to 1.5 dB).
 */
#define AMBE_ML_SCALE 45.5

static void dft256(const double *x, double *re, double *im)
{
    int k, n;
    for (k = 0; k <= NFFT / 2; k++) {
        double sr = 0.0, si = 0.0;
        double w = 2.0 * M_PI * (double)k / (double)NFFT;
        for (n = 0; n < NFFT; n++) {
            sr += x[n] * cos(w * (double)n);
            si -= x[n] * sin(w * (double)n);
        }
        re[k] = sr;
        im[k] = si;
    }
}

static double ncc(const float *x, int len, int lag)
{
    double num = 0.0, e0 = 0.0, e1 = 0.0;
    int n;
    for (n = lag; n < len; n++) {
        num += (double)x[n] * x[n - lag];
        e0  += (double)x[n] * x[n];
        e1  += (double)x[n - lag] * x[n - lag];
    }
    if (e0 <= 0.0 || e1 <= 0.0)
        return 0.0;
    return num / sqrt(e0 * e1);
}

void ambe_analyse(ambe_analysis *a, const int16_t pcm[AMBE_PCM_SAMPLES],
                  ambe_parms *out)
{
    double win[NFFT], re[NFFT / 2 + 1], im[NFFT / 2 + 1], mag[NFFT / 2 + 1];
    double best, score;
    float f0;
    int i, k, l, L, lag, bestlag;

    /* slide the history window on by one frame */
    memmove(a->win, a->win + AMBE_PCM_SAMPLES,
            (AMBE_ANALYSIS_HISTORY - AMBE_PCM_SAMPLES) * sizeof(float));
    for (i = 0; i < AMBE_PCM_SAMPLES; i++)
        a->win[AMBE_ANALYSIS_HISTORY - AMBE_PCM_SAMPLES + i] = (float)pcm[i];
    a->primed = 1;

    /* ---- pitch: normalised cross-correlation, shortest-lag octave guard */
    best = -2.0;
    bestlag = PITCH_MIN;
    for (lag = PITCH_MIN; lag <= PITCH_MAX; lag++) {
        score = ncc(a->win, AMBE_ANALYSIS_HISTORY, lag);
        if (score > best) { best = score; bestlag = lag; }
    }
    /*
     * Octave guard.  A correlation peak at lag T is always accompanied by peaks
     * at 2T, 3T..., so the raw maximum is biased towards multiples of the true
     * period.  Only submultiples of the winning lag are considered, and only if
     * they score nearly as well - scanning all shorter lags instead, as an
     * earlier version did, picks up spurious short-lag peaks and produces
     * up-octave errors of its own.
     */
    {
        int div;
        for (div = 4; div >= 2; div--) {
            int cand = bestlag / div;
            if (cand < PITCH_MIN)
                continue;
            if (ncc(a->win, AMBE_ANALYSIS_HISTORY, cand) > best * 0.85) {
                bestlag = cand;
                break;
            }
        }
    }

    /* snap to the quantiser grid so L is the one the decoder will use */
    {
        double target = 1.0 / (double)bestlag;
        int bb = 0, b;
        double bd = 1e300;
        for (b = 0; b < 120; b++) {
            float cf;
            int cl;
            double d;
            ambe_pitch_from_b0(b, 7, &cf, &cl);
            d = fabs(log((double)cf) - log(target));
            if (d < bd) { bd = d; bb = b; }
        }
        ambe_pitch_from_b0(bb, 7, &f0, &L);
    }
    out->w0 = (float)((double)f0 * 2.0 * M_PI);
    out->L  = L;

    /* ---- spectrum of the most recent 256 samples, Hamming windowed */
    for (i = 0; i < NFFT; i++) {
        double h = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(NFFT - 1));
        win[i] = (double)a->win[i] * h;
    }
    dft256(win, re, im);
    for (k = 0; k <= NFFT / 2; k++)
        mag[k] = sqrt(re[k] * re[k] + im[k] * im[k]);

    /* ---- per-harmonic amplitudes, and per-band voicing */
    {
        double bandtot[8], bandpk[8];
        double bins_per_harmonic = (double)f0 * (double)NFFT;
        for (i = 0; i < 8; i++) { bandtot[i] = 0.0; bandpk[i] = 0.0; }

        for (l = 1; l <= L; l++) {
            double c = (double)l * bins_per_harmonic;
            int lo = (int)(c - bins_per_harmonic * 0.5 + 0.5);
            int hi = (int)(c + bins_per_harmonic * 0.5 + 0.5);
            double sum = 0.0, pk = 0.0;
            int cnt = 0, jl;
            if (lo < 1) lo = 1;
            if (hi > NFFT / 2) hi = NFFT / 2;
            for (k = lo; k <= hi; k++) {
                sum += mag[k] * mag[k];
                if (fabs((double)k - c) <= bins_per_harmonic * 0.25)
                    pk += mag[k] * mag[k];
                cnt++;
            }
            if (cnt < 1) { sum = 0.0; cnt = 1; }
            out->Ml[l] = (float)(sqrt(sum / (double)cnt) / AMBE_ML_SCALE);

            jl = (int)((double)l * 16.0 * (double)f0);
            if (jl > 7) jl = 7;
            bandtot[jl] += sum;
            bandpk[jl]  += pk;
        }

        for (i = 0; i < 8; i++) {
            /* a voiced band concentrates its energy at the harmonic centres */
            double r = (bandtot[i] > 0.0) ? bandpk[i] / bandtot[i] : 0.0;
            bandtot[i] = (r > 0.60) ? 1.0 : 0.0;
        }
        /* a frame with no periodicity at all is entirely unvoiced */
        if (best < 0.35)
            for (i = 0; i < 8; i++) bandtot[i] = 0.0;

        for (l = 1; l <= L; l++) {
            int jl = (int)((double)l * 16.0 * (double)f0);
            if (jl > 7) jl = 7;
            out->Vl[l] = (uint8_t)(bandtot[jl] > 0.5 ? 1 : 0);
        }
    }

    /* ---- log2 amplitudes, inverting the decoder's voiced/unvoiced scaling */
    {
        double unvc = 0.2046 / sqrt((double)out->w0);
        for (l = 1; l <= L; l++) {
            double m = (double)out->Ml[l];
            if (m < 1e-8) m = 1e-8;
            if (!out->Vl[l]) m /= unvc;
            out->log2Ml[l] = (float)(log(m) / 0.693);
        }
    }
    for (l = L + 1; l <= AMBE_MAX_HARMONICS; l++) {
        out->Ml[l] = 0.0f;
        out->log2Ml[l] = 0.0f;
        out->Vl[l] = 0;
    }
    out->gamma = 0.0f;
    out->repeat = 0;
}
