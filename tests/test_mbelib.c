/*
 * test_mbelib.c - how well mbelib decodes the frames this radio transmits.
 *
 * mbelib used to be this project's oracle.  It is not any more: the firmware's
 * own decoder is, executed under the p-code emulator and compared frame by
 * frame in tests/test_firmware.c.  What is left for mbelib is a different and
 * still useful question - given that the radio is the definition of correct,
 * how close does the reference open-source decoder get?  So this file measures
 * rather than gates, and where it does gate, it gates on the things the two
 * provably do the same way.
 *
 * Four things the radio does that mbelib does not, and this decoder follows the
 * radio on all four:
 *
 *   the pitchless branch  Vocoder_DecodePitchlessGainMode 0x00027EF0
 *   b0 >= 120             one fixed 0x4027 / L = 15, not three cases
 *   b3 / b4's low bits    d[43] and d[40..42], not d[40] and d[41..43]
 *   the predictor         a b0 >= 120 frame does not advance it
 *
 * The first two are frame-selective and those frames are skipped here.  The
 * other two are not.  The bit assignment changes the spectral envelope of every
 * frame where payload bits 40 and 43 differ, which is most of them; the
 * predictor rule makes the two decoders' envelope and gain state diverge for
 * good after the first silence descriptor.  So gamma and the amplitudes below
 * are a measurement of the gap, not a bound on this decoder's error - and w0,
 * L and the voicing decisions, which no predictor touches, stay exact.
 */
#include "ambe.h"
#include "testutil.h"

static int close_enough(double a, double b, double tol)
{
    double d = fabs(a - b);
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return d <= tol * (m > 1.0 ? m : 1.0);
}

static double reldev(double a, double b)
{
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return fabs(a - b) / (m > 1.0 ? m : 1.0);
}

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    FILE *fp = fixture_open("dm32_arc4_1.parms");
    char bl[128], *pl = NULL;
    size_t pcap = 0;
    ambe_parms cur, prev, prev_enh;
    int n = 0, silence = 0, voiced = 0, skipped = 0;
    int vl_same = 0, vl_total = 0, l_same = 0, l_total = 0;
    double worst_w0 = 0, worst_gamma = 0, sum_ml = 0, worst_ml = 0, sum_gamma = 0;
    long ml_n = 0, gamma_n = 0;

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        int i, bad, L;
        double w0, gamma, v;
        char *p;

        if (strlen(bl) < AMBE_BITS)
            continue;
        if (getline(&pl, &pcap, fp) < 0)
            break;
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] == '1');

        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);

        p     = pl;
        bad   = (int)strtol(p, &p, 10);
        w0    = strtod(p, &p);
        L     = (int)strtol(p, &p, 10);
        gamma = strtod(p, &p);

        /*
         * Frame classification is the one thing the two cannot differ on: it
         * is the escape codes, not a model choice.
         */
        CHECK((type == AMBE_FRAME_VOICE || type == AMBE_FRAME_SILENCE)
                  == (bad == 0),
              "frame %d: classification %d vs mbelib bad=%d\n", n, (int)type, bad);

        if (bad != 0) {
            n++;
            continue;
        }

        /* the two branches mbelib does not have: measured elsewhere, skipped here */
        if (ambe_pitchless_gain_mode(info.b[1]) || info.b[0] >= 120) {
            skipped++;
            if (type == AMBE_FRAME_SILENCE) silence++;
            /* a b0 >= 120 frame does not advance the predictor - see ambe.h */
            if (type == AMBE_FRAME_VOICE)
                ambe_move_parms(&cur, &prev);
            n++;
            continue;
        }

        /*
         * On an ordinary voice frame the pitch and the voicing are the same
         * algorithm on both sides, so these stay exact.  w0 is bounded only at
         * the law-vs-table divergence test_tables measures, and L only at the
         * single index (b0 = 17) where that divergence crosses a boundary.
         */
        if (reldev(t_w0(&cur), w0) > worst_w0) worst_w0 = reldev(t_w0(&cur), w0);
        CHECK(close_enough(t_w0(&cur), w0, 3e-3), "frame %d: w0 %.9g vs %.9g\n",
              n, t_w0(&cur), w0);
        l_total++;
        if (cur.L == L)
            l_same++;
        else
            CHECK(info.b[0] == 17, "frame %d: L %d vs %d at b0=%d\n",
                  n, cur.L, L, info.b[0]);

        /*
         * gamma is predictor state, and the two predictors diverge by
         * construction the moment a b0 >= 120 frame goes by: the radio holds
         * its envelope and gain across one, mbelib propagates through it.  So
         * this is measured, like the amplitudes, and not gated.
         */
        {
            double dg = reldev(t_gamma(&cur), gamma);
            sum_gamma += dg;
            gamma_n++;
            if (dg > worst_gamma) worst_gamma = dg;
        }

        for (i = 1; i <= L; i++) {
            v = strtod(p, &p);
            vl_total++;
            if (cur.Vl[i] == (uint8_t)v)
                vl_same++;
            CHECK(cur.Vl[i] == (uint8_t)v, "frame %d: Vl[%d] %d vs %d\n",
                  n, i, cur.Vl[i], (int)v);
        }
        /* Ml: measured, not gated - see the header */
        for (i = 1; i <= L; i++) {
            double dv;
            v  = strtod(p, &p);
            dv = reldev(t_ml(&cur, i), v);
            sum_ml += dv;
            ml_n++;
            if (dv > worst_ml) worst_ml = dv;
        }

        if (type == AMBE_FRAME_SILENCE) silence++; else voiced++;
        if (type == AMBE_FRAME_VOICE)
            ambe_move_parms(&cur, &prev);
        n++;
    }
    free(pl);
    fclose(fb);
    fclose(fp);

    CHECK(n == 360, "expected 360 frames, read %d\n", n);
    CHECK(voiced > 200, "only %d voice frames in the fixture\n", voiced);
    CHECK(silence > 50, "only %d silence frames in the fixture\n", silence);
    CHECK(skipped > 100, "only %d frames took a branch mbelib lacks; the "
          "fixture no longer exercises the divergence\n", skipped);
    /*
     * A sanity floor, not a bound: if mbelib and this decoder ever disagreed
     * on the envelope by more than a factor of two on average, one of them
     * would have broken outright rather than drifted.
     */
    CHECK(sum_ml / ml_n < 1.0,
          "mean amplitude gap vs mbelib %.3f - not a drift, a break\n",
          sum_ml / ml_n);
    CHECK(sum_gamma / gamma_n < 1.0,
          "mean gain gap vs mbelib %.3f - not a drift, a break\n",
          sum_gamma / gamma_n);
    (void)worst_gamma;

    printf("[on the %d frames mbelib and this decoder model the same way: "
           "w0 within %.1e, L %d/%d, voicing %d/%d exact; predictor state "
           "differs - gamma %.1f%% mean, amplitudes %.1f%%; %d frames skipped, "
           "the radio's own branches] ",
           l_total, worst_w0, l_same, l_total, vl_same, vl_total,
           100.0 * sum_gamma / gamma_n, 100.0 * sum_ml / ml_n, skipped);

    return t_done("mbelib on the same frames, measured");
}
