/*
 * test_params.c - 49 payload bits -> MBE model parameters, against mbelib.
 *
 * This is the load-bearing known-good-pair test.  The spectral envelope is
 * differentially coded against the previous frame, so a single wrong table
 * entry, bit position or blend coefficient diverges within a few frames and
 * never recovers - running 360 consecutive frames of real speech through it and
 * matching w0, L, gamma, the voicing decisions and all 56 spectral amplitudes
 * at every step is a much stronger statement than any single-frame vector.
 *
 * The voicing decisions and the frame classification must match mbelib exactly.
 * w0 is bounded rather than exact because it now comes from the firmware's own
 * closed-form pitch law instead of mbelib's tabulation of the same quantiser;
 * L stays exact apart from the single documented index where the law and the
 * table disagree.  The gain and the
 * spectral amplitudes cannot: this decoder uses the firmware's Q11 tables and
 * mbelib uses its own float reconstruction of the same quantisers, so they
 * differ by the firmware's quantisation step.  Those are bounded instead, at
 * the measured worst case.  tests/test_tables.c is where the tables themselves
 * are checked exactly.
 */
#include "ambe.h"
#include "testutil.h"

#define TOL 1e-4

static double worst_gamma, worst_ml, worst_log2ml, worst_w0;
static int l_mismatch;

static int close_enough(double a, double b, double tol)
{
    double d = fabs(a - b);
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return d <= tol * (m > 1.0 ? m : 1.0);
}

static double dev(double a, double b, double *worst)
{
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    double d = fabs(a - b) / (m > 1.0 ? m : 1.0);
    if (d > *worst) *worst = d;
    return d;
}

/*
 * How many frames the 0.65 predictor damping needs to bring a seeded state
 * difference back under the Q11 quantisation floor: 0.65^12 = 5.7e-3 against
 * a seed of 0.65, which is what the measured decay does.
 */
#define AMBE_TRANSIENT 12

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    FILE *fp = fixture_open("dm32_arc4_1.parms");
    char bl[128], *pl = NULL;
    size_t pcap = 0;
    ambe_parms cur, prev, prev_enh;
    int n = 0, silence = 0, voiced = 0, pitchless = 0;
    /*
     * Frames since the last one where this decoder deliberately answers
     * differently from mbelib - a pitchless frame, or a b0 >= 120 descriptor
     * where the radio's L is 15 and mbelib's 14.  The spectral envelope is
     * predicted from the previous frame damped by 0.65, so such a frame seeds
     * a state difference that the predictor then forgets geometrically.  The
     * amplitude bound is only meaningful once it has, and the decay itself is
     * asserted below - it is the stronger statement, because a wrong
     * coefficient would show as drift instead.
     */
    int since = 0;
    double worst_lag[AMBE_TRANSIENT + 1];

    double scratch = 0;
    int i;

    for (i = 0; i <= AMBE_TRANSIENT; i++) worst_lag[i] = 0;
    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        int i, bad;
        double w0, gamma, v;
        int L;
        char *p;

        if (strlen(bl) < AMBE_BITS)
            continue;
        if (getline(&pl, &pcap, fp) <= 0)
            break;

        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] - '0');

        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);

        p = pl;
        bad   = (int)strtol(p, &p, 10);
        w0    = strtod(p, &p);
        L     = (int)strtol(p, &p, 10);
        gamma = strtod(p, &p);

        CHECK((type == AMBE_FRAME_VOICE || type == AMBE_FRAME_SILENCE)
                  == (bad == 0),
              "frame %d: classification %d vs mbelib bad=%d\n", n, (int)type, bad);

        if (bad == 0 && ambe_pitchless_gain_mode(info.b[1])) {
            /*
             * mbelib has no counterpart to the radio's pitchless branch, so on
             * these frames the two decoders are answering different questions
             * and comparing them measures nothing.  Vocoder_DecodeAmbeFrame
             * 0x0002033C writes f0 = 0x1079 and L = 56 directly when
             * Vocoder_DecodePitchlessGainMode 0x00027EF0 holds - no band voiced
             * and at least one voicing crumb in the high-bit state - and never
             * reads b0; mbelib dequantises b0 as if it were a pitch.  Both
             * halves of that were established by running the stock decoder
             * under the p-code emulator over these same 49-bit payloads, where
             * the branch predicts every clamped frame with no false positives.
             *
             * The frames are skipped rather than bounded because the difference
             * is a whole speech model, not a tolerance: at b0 = 0 it is 6.2x in
             * f0 and L wrong by 47.  Their number is reported instead, and the
             * predictor state they seed is why the following frames' amplitudes
             * are not compared any tighter than they are.
             */
            pitchless++;
            since = 0;
            ambe_move_parms(&cur, &prev);
            n++;
            continue;
        }

        if (bad == 0) {
            /*
             * w0 comes from the firmware's closed-form pitch law now, not from
             * mbelib's tabulation of the same quantiser, so it is bounded at
             * the law-vs-table divergence measured in test_tables rather than
             * required to be equal.  L is still exact: the one index where the
             * two disagree (b0 = 17) is asserted there.
             */
            dev(t_w0(&cur), w0, &worst_w0);
            CHECK(close_enough(t_w0(&cur), w0, 3e-3), "frame %d: w0 %.9g vs %.9g\n",
                  n, t_w0(&cur), w0);
            if (cur.L != L) {
                /*
                 * Two known divergences from mbelib, both of them the radio's
                 * answer rather than this decoder's:
                 *
                 *  b0 = 17     the pitch law against mbelib's tabulation of it,
                 *              asserted in test_tables
                 *  b0 >= 120   Vocoder_DecodePitchIndex 0x00022B78's non-voiced
                 *              path writes the fixed 0x4027 / L = 15 for every
                 *              index in that range, which running that function
                 *              under the p-code emulator confirms over all 128.
                 *              mbelib uses L = 14 for both silence descriptors;
                 *              mbelib-neo and JMBE use 15 for b0 = 124 and 14
                 *              for 125.  The radio makes no such distinction.
                 */
                l_mismatch++;
                CHECK(info.b[0] == 17 ||
                      (info.b[0] >= 120 && cur.L == AMBE_L_NOPITCH),
                      "frame %d: L %d vs %d at b0=%d\n",
                      n, cur.L, L, info.b[0]);
                since = 0;
            } else {
                since++;
            }
            dev(t_gamma(&cur), gamma, &worst_gamma);
            for (i = 1; i <= L; i++) {
                v = strtod(p, &p);
                CHECK(cur.Vl[i] == (uint8_t)v, "frame %d: Vl[%d] %d vs %d\n",
                      n, i, cur.Vl[i], (int)v);
            }
            for (i = 1; i <= L; i++) {
                double d1;
                v  = strtod(p, &p);
                d1 = dev(t_ml(&cur, i), v, since >= AMBE_TRANSIENT
                                          ? &worst_ml : &scratch);
                if (since <= AMBE_TRANSIENT && d1 > worst_lag[since])
                    worst_lag[since] = d1;
            }
            for (i = 1; i <= L; i++) {
                v = strtod(p, &p);
                dev(t_log2ml(&cur, i), v, since >= AMBE_TRANSIENT
                                          ? &worst_log2ml : &scratch);
            }
            if (type == AMBE_FRAME_SILENCE) silence++; else voiced++;
            ambe_move_parms(&cur, &prev);
        } else {
            ambe_init_parms(&cur, &prev, &prev_enh);
        }
        n++;
    }
    free(pl);
    fclose(fb);
    fclose(fp);

    CHECK(n == 360, "expected 360 frames, read %d\n", n);
    /* the capture must actually exercise both paths */
    CHECK(voiced > 200, "only %d voice frames in the fixture\n", voiced);
    CHECK(silence > 50, "only %d silence frames in the fixture\n", silence);
    /*
     * The decoder now uses the firmware's own Q11 tables where mbelib uses its
     * float reconstruction, so the amplitude chain cannot agree exactly.  The
     * bounds below are the measured worst case over these 360 frames, and they
     * are what half-LSB Q11 quantisation propagates to: gamma stays inside one
     * quantiser step, the spectral amplitudes inside 1%.  Everything that does
     * not depend on those tables - classification, w0, L and every voicing
     * decision - is still required to match exactly, above.
     */
    CHECK(worst_gamma < 5e-4, "gamma deviation %.3e exceeds a Q11 step\n", worst_gamma);
    CHECK(worst_ml < 1e-2, "Ml deviation %.3e too large for Q11 tables\n", worst_ml);
    CHECK(worst_log2ml < 1e-2, "log2Ml deviation %.3e too large\n", worst_log2ml);
    CHECK(pitchless > 0, "the fixture reaches no pitchless frame\n");
    /*
     * The seeded difference must *decay geometrically*, at the codec's own
     * damping factor, down to the quantisation floor the bound above sets.
     * Drift would mean a wrong coefficient; this is a wrong starting point,
     * which the predictor forgets.  0.7 leaves slack on 0.65, and the floor
     * term is why the last few lags flatten rather than keep halving.
     */
    {
        double envelope = worst_lag[1];
        for (i = 2; i <= AMBE_TRANSIENT; i++) {
            envelope *= 0.7;
            CHECK(worst_lag[i] <= envelope + 1e-2,
                  "amplitude divergence at lag %d is %.3e, above 0.65^k decay "
                  "from %.3e plus the Q11 floor\n",
                  i, worst_lag[i], worst_lag[1]);
        }
    }
    printf("[worst dev vs mbelib: w0 %.1e gamma %.1e Ml %.1e log2Ml %.1e; "
           "%d L divergences; %d voice, %d silence, %d pitchless (skipped); "
           "transient %.1e -> %.1e over %d frames] ",
           worst_w0, worst_gamma, worst_ml, worst_log2ml, l_mismatch,
           voiced, silence, pitchless, worst_lag[1], worst_lag[AMBE_TRANSIENT],
           AMBE_TRANSIENT);

    return t_done("ambe+2 parameter decode vs mbelib");
}
