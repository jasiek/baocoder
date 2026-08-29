/*
 * ambe_analysis.c - 160 PCM samples -> MBE model parameters, in fixed point.
 *
 * This is the one stage with no counterpart transcribed.  The stock analyser
 * is spread across the pitch/voicing cluster (Vocoder_SelectVoicingCandidate
 * 0x000177F0, Vocoder_RefinePitchEstimate 0x00025A60,
 * Vocoder_AnalyzeSpectrum 0x000205B8, Dsp_WindowAndComputeFft 0x00019B6C) and
 * is a dense multi-candidate search with its own energy-block structure.  What
 * is implemented here is a conventional MBE analyser producing the same
 * parameter set - the estimator is ours, the quantiser it feeds
 * (ambe_encode_params.c) is the firmware's.
 *
 * It is integer arithmetic like the rest of the library, and for the same
 * reason: the radio's analyser is fixed point too, so a float estimator here
 * would be the only floating-point code in a codec whose original had none.
 * That is a change of number system, not of algorithm - the pitch search,
 * the octave guard and the voicing rule are the ones the float version had.
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
 * Two of its decisions are thresholds rather than arithmetic - the voicing
 * ratio and the octave guard - and both are evaluated here by exact integer
 * cross-multiplication rather than by dividing first.  A threshold is where
 * rounding stops being a small numerical error and becomes a different answer,
 * so those comparisons are the ones worth making exact.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe.h"
#include "ambe_basop.h"

#define NFFT 256
#define PITCH_MIN 20        /* f0 = 0.0500 */
#define PITCH_MAX 123       /* f0 = 0.0081 */

/*
 * Amplitude scale between PCM units and the model's Ml.  The decoder's output
 * path is Ml -> overlap-added sinusoids -> x7 on the way to int16, so the
 * inverse carries a fixed constant; it is calibrated so that encoding a decoded
 * frame reproduces its gain (tests/test_encode_pcm.c holds that to 1.5 dB).
 */
#define AMBE_ML_SCALE_Q16 2981888        /* 45.5 in Q16 */

/* ln2 / 0.693, undoing the truncated ln2 the decoder's amplitude step uses */
#define K_LN2_OVER_0P693_Q30 1074765414

/* the voicing threshold, 0.60, and the octave guard, 0.85, as ratios */
#define VOICE_NUM 60
#define VOICE_DEN 100
#define OCTAVE_NUM 85
#define OCTAVE_DEN 100
/* "no periodicity at all": a normalised correlation below 0.35, Q15 */
#define UNVOICED_SCORE_Q15 11469

static int16_t hamming_q15[NFFT];
static int     hamming_ready;

static void hamming_init(void)
{
    int i;
    if (hamming_ready)
        return;
    for (i = 0; i < NFFT; i++) {
        /* 0.54 - 0.46*cos(2*pi*i/(N-1)); the phase is i/(N-1) of a turn */
        int phase = (int)(((int32_t)i * 32768 + (NFFT - 1) / 2) / (NFFT - 1));
        int32_t c = ambe_cos_q15(phase);
        hamming_q15[i] = (int16_t)(17694 - (int32_t)((15073LL * c) >> 15));
    }
    hamming_ready = 1;
}

/*
 * Normalised cross-correlation at one lag, as a block float.  The sums are
 * exact int64; only the division and square root round.
 */
static ambe_bf ncc(const int16_t *x, int len, int lag)
{
    int64_t num = 0, e0 = 0, e1 = 0;
    ambe_bf z;
    int n;

    for (n = lag; n < len; n++) {
        num += (int64_t)x[n] * x[n - lag];
        e0  += (int64_t)x[n] * x[n];
        e1  += (int64_t)x[n - lag] * x[n - lag];
    }
    if (e0 <= 0 || e1 <= 0) {
        z.mant = 0; z.exp = 0;
        return z;
    }
    return ambe_bf_div(ambe_bf_from_i64(num, 0),
                       ambe_bf_sqrt(ambe_bf_mul(ambe_bf_from_i64(e0, 0),
                                                ambe_bf_from_i64(e1, 0))));
}

void ambe_analyse(ambe_analysis *a, const int16_t pcm[AMBE_PCM_SAMPLES],
                  ambe_parms *out)
{
    int32_t re[NFFT / 2 + 1], im[NFFT / 2 + 1];
    ambe_bf mag[NFFT / 2 + 1];
    ambe_bf best, score;
    int32_t f0;
    int i, k, l, L, lag, bestlag;

    hamming_init();

    /* slide the history window on by one frame */
    memmove(a->win, a->win + AMBE_PCM_SAMPLES,
            (AMBE_ANALYSIS_HISTORY - AMBE_PCM_SAMPLES) * sizeof(int16_t));
    for (i = 0; i < AMBE_PCM_SAMPLES; i++)
        a->win[AMBE_ANALYSIS_HISTORY - AMBE_PCM_SAMPLES + i] = pcm[i];
    a->primed = 1;

    /* ---- pitch: normalised cross-correlation, shortest-lag octave guard */
    best.mant = 0; best.exp = 0;
    bestlag = PITCH_MIN;
    for (lag = PITCH_MIN; lag <= PITCH_MAX; lag++) {
        score = ncc(a->win, AMBE_ANALYSIS_HISTORY, lag);
        if (ambe_bf_gt(score, best)) { best = score; bestlag = lag; }
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
        ambe_bf bar = ambe_bf_mul(best, ambe_bf_from_q(OCTAVE_NUM * 65536 /
                                                       OCTAVE_DEN, 16));
        for (div = 4; div >= 2; div--) {
            int cand = bestlag / div;
            if (cand < PITCH_MIN)
                continue;
            if (ambe_bf_gt(ncc(a->win, AMBE_ANALYSIS_HISTORY, cand), bar)) {
                bestlag = cand;
                break;
            }
        }
    }

    /*
     * Snap to the quantiser grid so L is the one the decoder will use.  The
     * target is 1/bestlag in Q19 turns, and "closest in the log domain" is the
     * same exact cross-multiplication the quantiser uses.
     */
    {
        int32_t target = (int32_t)(((int64_t)1 << AMBE_Q_F0) / bestlag);
        int bb = 0, b, first = 1;
        int32_t bmin = 0, bmax = 0;
        for (b = 0; b < 120; b++) {
            int32_t cf;
            int cl;
            int32_t lo, hi;
            ambe_pitch_from_b0(b, 7, &cf, &cl);
            lo = cf < target ? cf : target;
            hi = cf < target ? target : cf;
            if (first || (int64_t)hi * bmin < (int64_t)bmax * lo) {
                first = 0; bb = b; bmin = lo; bmax = hi;
            }
        }
        ambe_pitch_from_b0(bb, 7, &f0, &L);
    }
    out->f0 = f0;
    out->L  = L;

    /* ---- spectrum of the most recent 256 samples, Hamming windowed */
    {
        int32_t win[NFFT];
        for (i = 0; i < NFFT; i++)
            win[i] = ((int32_t)a->win[i] * hamming_q15[i]) >> 15;
        for (k = 0; k <= NFFT / 2; k++) {
            int64_t sr = 0, si = 0;
            for (i = 0; i < NFFT; i++) {
                /* the phase is k*i/NFFT of a turn, exact in Q15 units */
                int phase = (k * i * (32768 / NFFT)) & 0x7FFF;
                sr += (int64_t)win[i] * ambe_cos_q15(phase);
                si -= (int64_t)win[i] * ambe_sin_q15(phase);
            }
            re[k] = (int32_t)(sr >> 15);
            im[k] = (int32_t)(si >> 15);
        }
        for (k = 0; k <= NFFT / 2; k++)
            mag[k] = ambe_bf_sqrt(ambe_bf_from_i64((int64_t)re[k] * re[k] +
                                                   (int64_t)im[k] * im[k], 0));
    }

    /* ---- per-harmonic amplitudes, and per-band voicing */
    {
        int64_t bandtot[8], bandpk[8];
        int voiced[8];
        int32_t mant[AMBE_MAX_HARMONICS + 1];
        short   ex[AMBE_MAX_HARMONICS + 1];
        int     maxex = -32768;
        /* bins per harmonic, Q16: f0 * NFFT */
        int32_t bph_q16 = (int32_t)(((int64_t)f0 * NFFT) >> (AMBE_Q_F0 - 16));

        for (i = 0; i < 8; i++) { bandtot[i] = 0; bandpk[i] = 0; }

        for (l = 1; l <= L; l++) {
            int32_t c_q16 = (int32_t)((int64_t)l * bph_q16);
            int lo = (c_q16 - bph_q16 / 2 + 32768) >> 16;
            int hi = (c_q16 + bph_q16 / 2 + 32768) >> 16;
            int64_t sum = 0, pk = 0;
            int cnt = 0, jl;
            ambe_bf amp;

            if (lo < 1) lo = 1;
            if (hi > NFFT / 2) hi = NFFT / 2;
            for (k = lo; k <= hi; k++) {
                /* |mag|^2 with the block float taken back to an int64 */
                int64_t m = (int64_t)re[k] * re[k] + (int64_t)im[k] * im[k];
                int32_t d = (k << 16) - c_q16;
                sum += m;
                if (d < 0) d = -d;
                if (d <= bph_q16 / 4)
                    pk += m;
                cnt++;
            }
            if (cnt < 1) { sum = 0; cnt = 1; }

            /* Ml = sqrt(sum/cnt) / 45.5 */
            amp = ambe_bf_sqrt(ambe_bf_from_i64(sum / cnt, 0));
            amp = ambe_bf_div(amp, ambe_bf_from_q(AMBE_ML_SCALE_Q16, 16));
            {
                int sh = 0;
                int32_t m = amp.mant;
                /* carry it as a mantissa with the block-float exponent the
                   parameter struct uses */
                mant[l] = m;
                ex[l]   = (short)(amp.exp - 30 + AMBE_Q_ML);
                if (m == 0) ex[l] = -32768;
                if (ex[l] > maxex) maxex = ex[l];
                (void)sh;
            }

            jl = (int)(((int64_t)l * 16 * f0) >> AMBE_Q_F0);
            if (jl > 7) jl = 7;
            bandtot[jl] += sum;
            bandpk[jl]  += pk;
        }

        for (i = 0; i < 8; i++) {
            /* a voiced band concentrates its energy at the harmonic centres:
               pk/tot > 0.60, cross-multiplied so nothing is divided */
            voiced[i] = (bandtot[i] > 0) &&
                        (bandpk[i] * VOICE_DEN > bandtot[i] * VOICE_NUM);
        }
        /* a frame with no periodicity at all is entirely unvoiced */
        if (ambe_bf_gt(ambe_bf_from_q(UNVOICED_SCORE_Q15, 15), best))
            for (i = 0; i < 8; i++) voiced[i] = 0;

        for (l = 1; l <= L; l++) {
            int jl = (int)(((int64_t)l * 16 * f0) >> AMBE_Q_F0);
            if (jl > 7) jl = 7;
            out->Vl[l] = (uint8_t)(voiced[jl] ? 1 : 0);
        }

        if (maxex == -32768)
            maxex = 0;
        for (l = 1; l <= L; l++) {
            int sh = maxex - ex[l];
            out->Ml[l] = (ex[l] == -32768 || sh >= 31) ? 0 : (mant[l] >> sh);
        }
        out->Ml_exp = maxex;

        /* ---- log2 amplitudes, inverting the decoder's voiced/unvoiced scale */
        {
            int32_t w0_q28 = (int32_t)(((int64_t)f0 * 1686629713) >> AMBE_Q_F0);
            short   e = 31 - 28;
            unsigned int m = ambe_sqrt(w0_q28, &e);
            int64_t den = (int64_t)m << (e - 15 + 30);
            int32_t unvc_q30 = den ? (int32_t)(((int64_t)219687577 << 30) / den) : 0;

            for (l = 1; l <= L; l++) {
                ambe_bf v;
                int32_t lg;
                if (out->Ml[l] == 0) {
                    out->log2Ml[l] = 0;
                    continue;
                }
                v.mant = out->Ml[l];
                v.exp  = maxex - AMBE_Q_ML + 30;
                v = ambe_bf_norm(v);
                if (!out->Vl[l] && unvc_q30)
                    v = ambe_bf_div(v, ambe_bf_from_q(unvc_q30, 30));
                /* ambe_log2 wants mant * 2^(exp - 15) */
                lg = ambe_log2(v.mant >> 15, v.exp - 30 + 15 + 15);
                /* Q16 log2 -> Q24, and undo the decoder's 0.693 for ln2 */
                out->log2Ml[l] = (int32_t)((((int64_t)lg << (AMBE_Q_LOG - 16)) *
                                            K_LN2_OVER_0P693_Q30) >> 30);
            }
        }
    }

    for (l = L + 1; l <= AMBE_MAX_HARMONICS; l++) {
        out->Ml[l] = 0;
        out->log2Ml[l] = 0;
        out->Vl[l] = 0;
    }
    out->gamma = 0;
    out->repeat = 0;
}
