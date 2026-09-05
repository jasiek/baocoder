/*
 * test_firmware_encode.c - the ENCODE path, against the firmware's own encoder.
 *
 * The decode side has exact parity (test_firmware).  This side does not, and
 * this test says by how much rather than asserting a standard nothing here
 * meets.  src/ambe_analysis.c is the one stage that is not a transcription -
 * its FFT front end is the radio's, its pitch and voicing decisions are not -
 * so a gap is expected; the point is to measure it against the radio instead
 * of against a proxy.
 *
 * The reference is Vocoder_ProcessFrameFec 0x00016F3C executed under the
 * p-code emulator over dm32_arc4_1.fwpcm - the radio's own decoded audio -
 * driven exactly as Vocoder_RxTask 0x0002E5A0 drives it (0x50 samples,
 * wFlags 0x800, bQuantise 0 then 1).  tools/fw_oracle/gen_encode_jobs.py
 * builds the job and export_encode.py writes the fixtures.
 *
 * Two alignments have to be right before any number here means anything, and
 * both were measured rather than assumed:
 *
 *   The analyser's output lags its input by 240 samples - one and a half
 *   frames - which is what Vocoder_ProcessFrame 0x00016E04's 258-sample
 *   rolling window implies.  Scored at lag zero instead, this decoder looks
 *   far worse than it is: L exact falls from 60% to 31% and the envelope
 *   residual doubles.  ANALYSIS_SKIP and ANALYSIS_LAG below encode it.
 *
 *   Voicing is compared against the firmware's own transmitted b1, expanded
 *   through ambe_vuv_packed.  It is NOT compared against the word at +0x08 of
 *   the analysis block: on the decode path that is the voicing field and
 *   test_firmware checks it exactly, but on this path its bits agree with b1's
 *   expansion only 57% of the time and it grows monotonically frame over frame
 *   in a way that does not track L.  Using it would have reported a confident
 *   and wrong number.
 *
 * What the firmware does NOT expose, checked and not merely assumed: no offset
 * in a 0x2000 window of g_VocoderRxCtx holds the pitch b0 decodes to, or b1's
 * codebook expansion.  The encoder searches for indices without materialising
 * what they mean, so the quantiser can only be scored index against index.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_tables.h"
#include "testutil.h"

/* measured: the firmware's analysis of frame i covers audio 240 samples back */
#define ANALYSIS_SKIP 80     /* samples dropped before framing        */
#define ANALYSIS_LAG  (-2)   /* then this many whole frames           */

#define VBAND(b1, j) ((ambe_vuv_packed[((b1) << 2) & 127] >> (30 - 2 * (j))) & 1u)

static const int FIELD_W[9] = { 7, 5, 5, 9, 7, 5, 4, 4, 3 };

/* The emitted payload is in field order - b0..b8 contiguous, MSB first - which
 * is the same order the decode-side oracle established, confirmed here by
 * searching all 43 offsets and both bit orders for the pitch field. */
static void split_fields(const char *bits, int b[9])
{
    int i, k, p = 0;
    for (i = 0; i < 9; i++) {
        b[i] = 0;
        for (k = 0; k < FIELD_W[i]; k++)
            b[i] = (b[i] << 1) | (bits[p++] == '1');
    }
}

static int fw_band(int l, int32_t f0)
{
    return (int)(((int64_t)l * 32 * f0) >> AMBE_Q_F0);
}

int main(void)
{
    FILE *fp = fixture_open("dm32_arc4_1.fwpcm");
    FILE *fa = fixture_open("dm32_arc4_1.fwanalysis");
    FILE *fe = fixture_open("dm32_arc4_1.fwaenv");
    FILE *fb = fixture_open("dm32_arc4_1.fwencbits");
    ambe_analysis an;
    ambe_parms *ours;
    char line[512], *el = NULL;
    size_t ecap = 0;
    int ncap = 0, nfw = 0, i, l;
    int *fwL, *fwF0, *fwCls, (*fwB)[9];
    int **fwEnv;
    short pcm[AMBE_PCM_SAMPLES];
    int cmp = 0, lex = 0, f0ok = 0, oct = 0;
    long vtot = 0, vfw = 0, vours = 0, vagree = 0;
    double envsum = 0.0;
    int envn = 0;

    /* ---- this decoder's analysis, on the same audio, skipped into alignment */
    fseek(fp, ANALYSIS_SKIP * 2, SEEK_SET);
    ours = (ambe_parms *)calloc(1024, sizeof(*ours));
    memset(&an, 0, sizeof(an));
    while (ncap < 1024 &&
           fread(pcm, sizeof(short), AMBE_PCM_SAMPLES, fp) == AMBE_PCM_SAMPLES)
        ambe_analyse(&an, pcm, &ours[ncap++]);
    fclose(fp);

    /* ---- the firmware's own answers */
    fwL = (int *)calloc(1024, sizeof(int));
    fwF0 = (int *)calloc(1024, sizeof(int));
    fwCls = (int *)calloc(1024, sizeof(int));
    fwB = (int (*)[9])calloc(1024, sizeof(int[9]));
    fwEnv = (int **)calloc(1024, sizeof(int *));
    while (nfw < 1024 && fgets(line, sizeof(line), fa)) {
        unsigned w;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%d %d %d %x", &fwCls[nfw], &fwL[nfw], &fwF0[nfw], &w) != 4)
            continue;
        do {
            if (getline(&el, &ecap, fe) < 0)
                goto loaded;
        } while (el[0] == '#');
        {
            char *q = el;
            int n = (fwL[nfw] > 0 && fwL[nfw] <= 56) ? fwL[nfw] : 0;
            fwEnv[nfw] = (int *)calloc(57, sizeof(int));
            for (l = 1; l <= n; l++)
                fwEnv[nfw][l] = (int)strtol(q, &q, 10);
        }
        do {
            if (!fgets(line, sizeof(line), fb))
                goto loaded;
        } while (line[0] == '#');
        split_fields(line, fwB[nfw]);
        nfw++;
    }
loaded:
    fclose(fa); fclose(fe); fclose(fb);
    CHECK(nfw > 300, "only %d firmware frames loaded\n", nfw);
    CHECK(ncap > 300, "only %d frames analysed\n", ncap);

    for (i = 0; i < nfw; i++) {
        int j = i + ANALYSIS_LAG;
        const ambe_parms *b;
        double d[AMBE_MAX_HARMONICS + 1], mean = 0, acc = 0;
        double r;

        if (j < 0 || j >= ncap)
            continue;
        if (fwCls[i] != 1 || fwL[i] < 1 || fwL[i] > 56)
            continue;
        b = &ours[j];
        if (b->L < 1)
            continue;
        cmp++;
        if (b->L == fwL[i])
            lex++;
        r = (double)b->f0 / fwF0[i];
        if (r > 0.94 && r < 1.06)
            f0ok++;
        if (r > 1.6 || r < 0.62)
            oct++;

        /* envelope shape, mean removed: log2Ml is Q24 here and Q11 there, and
         * the two have different absolute gain conventions, so only the shape
         * is a comparable quantity */
        if (b->L == fwL[i]) {
            for (l = 1; l <= fwL[i]; l++) {
                d[l] = ldexp((double)b->log2Ml[l], -AMBE_Q_LOG)
                     - ldexp((double)fwEnv[i][l], -11);
                mean += d[l];
            }
            mean /= fwL[i];
            for (l = 1; l <= fwL[i]; l++)
                acc += (d[l] - mean) * (d[l] - mean);
            envsum += sqrt(acc / fwL[i]);
            envn++;
        }

        /* voicing, against the firmware's own transmitted b1 */
        if (fwB[i][0] < 120) {
            int32_t f0c;
            int Lc, m;
            ambe_pitch_from_b0(fwB[i][0], 7, &f0c, &Lc);
            m = Lc < b->L ? Lc : b->L;
            for (l = 1; l <= m; l++) {
                int bd = fw_band(l, f0c);
                int v, o;
                if (bd > 15)
                    bd = 15;
                v = (int)VBAND(fwB[i][1], bd);
                o = b->Vl[l] ? 1 : 0;
                vtot++; vfw += v; vours += o;
                if (v == o)
                    vagree++;
            }
        }
    }

    CHECK(cmp > 200, "only %d frames comparable\n", cmp);
    CHECK(envn > 100, "only %d frames had matching L for the envelope\n", envn);

    /*
     * Regression floors, set just under what is measured today.  They are
     * deliberately floors and not targets: nothing on this path is exact, and
     * the numbers are here to stop it silently getting worse.
     */
    CHECK(100.0 * lex / cmp > 50.0,
          "harmonic count agreement %.0f%% has fallen below 50%%\n", 100.0 * lex / cmp);
    CHECK(100.0 * f0ok / cmp > 78.0,
          "pitch within 6%% on %.0f%% of frames, below 78%%\n", 100.0 * f0ok / cmp);
    CHECK(100.0 * oct / cmp < 8.0,
          "octave errors on %.0f%% of frames, above 8%%\n", 100.0 * oct / cmp);
    CHECK(6.0206 * envsum / envn < 3.2,
          "envelope shape residual %.2f dB, above 3.2\n", 6.0206 * envsum / envn);
    /*
     * Voicing has no floor to assert, because there is nothing yet to protect:
     * agreement is BELOW the trivial always-voiced answer.  The analyser calls
     * 15% of harmonics voiced where the radio calls 65% - a bias, not noise
     * (precision is high, recall is not), so the lever is the threshold rather
     * than the measure.  test_encode_voicing scores the same decision at 72%
     * against 67% on this decoder's OWN decoded audio; that it does worse here
     * is the finding, not a contradiction.
     */
    CHECK(vtot > 5000, "only %ld voicing decisions compared\n", vtot);

    printf("[%d frames vs the radio's own analysis, aligned at %d samples: "
           "L exact %.0f%%, pitch within 6%% %.0f%% with %.0f%% octave errors, "
           "envelope shape %.2f dB; voicing is the gap - firmware calls %.0f%% "
           "of harmonics voiced and this analyser %.0f%%, agreeing on %.0f%% "
           "against an always-voiced %.0f%%] ",
           cmp, -(ANALYSIS_SKIP + ANALYSIS_LAG * AMBE_PCM_SAMPLES),
           100.0 * lex / cmp, 100.0 * f0ok / cmp, 100.0 * oct / cmp,
           6.0206 * envsum / envn, 100.0 * vfw / vtot, 100.0 * vours / vtot,
           100.0 * vagree / vtot, 100.0 * vfw / vtot);

    free(el); free(ours); free(fwL); free(fwF0); free(fwCls); free(fwB);
    for (i = 0; i < nfw; i++) free(fwEnv[i]);
    free(fwEnv);
    return t_done("the encode path, against the firmware's own encoder");
}
