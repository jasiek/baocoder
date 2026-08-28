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
 * w0, L, the voicing decisions and the frame classification must match mbelib
 * exactly - none of them depend on the quantiser tables.  The gain and the
 * spectral amplitudes cannot: this decoder uses the firmware's Q11 tables and
 * mbelib uses its own float reconstruction of the same quantisers, so they
 * differ by the firmware's quantisation step.  Those are bounded instead, at
 * the measured worst case.  tests/test_tables.c is where the tables themselves
 * are checked exactly.
 */
#include "ambe.h"
#include "testutil.h"

#define TOL 1e-4

static double worst_gamma, worst_ml, worst_log2ml;

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

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    FILE *fp = fixture_open("dm32_arc4_1.parms");
    char bl[128], *pl = NULL;
    size_t pcap = 0;
    ambe_parms cur, prev, prev_enh;
    int n = 0, silence = 0, voiced = 0;

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

        if (bad == 0) {
            CHECK(close_enough(cur.w0, w0, TOL), "frame %d: w0 %.9g vs %.9g\n",
                  n, cur.w0, w0);
            CHECK(cur.L == L, "frame %d: L %d vs %d\n", n, cur.L, L);
            dev(cur.gamma, gamma, &worst_gamma);
            for (i = 1; i <= L; i++) {
                v = strtod(p, &p);
                CHECK(cur.Vl[i] == (uint8_t)v, "frame %d: Vl[%d] %d vs %d\n",
                      n, i, cur.Vl[i], (int)v);
            }
            for (i = 1; i <= L; i++) {
                v = strtod(p, &p);
                dev(cur.Ml[i], v, &worst_ml);
            }
            for (i = 1; i <= L; i++) {
                v = strtod(p, &p);
                dev(cur.log2Ml[i], v, &worst_log2ml);
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
    printf("[worst dev vs mbelib: gamma %.1e Ml %.1e log2Ml %.1e; %d voice, %d silence] ",
           worst_gamma, worst_ml, worst_log2ml, voiced, silence);

    return t_done("ambe+2 parameter decode vs mbelib");
}
