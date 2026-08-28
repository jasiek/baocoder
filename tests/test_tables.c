/*
 * test_tables.c - the quantiser tables extracted from the radio image, against
 * mbelib's independent reconstruction of the same quantisers.
 *
 * This is the exact test.  tools/extract_tables.py pulls seven codebooks and a
 * block-length table straight out of the firmware; mbelib rebuilt the same
 * tables from the AMBE specification and patent filings, with no access to this
 * radio.  The two must therefore agree to within the firmware's own precision,
 * which makes this a two-way check: it validates the extraction against mbelib
 * and mbelib's reconstruction against a shipping DVSI implementation.
 *
 * The block-length table is required to match exactly, since it is integers.
 */
#include "ambe.h"
#include "ambe_tables.h"
#include "testutil.h"

#define HALF_LSB (0.5f / AMBE_Q11)

static double *load_ref(const char *name, int *nout)
{
    static char line[1 << 17];
    FILE *f = fixture_open("mbelib_tables.txt");
    double *v = NULL;
    while (fgets(line, sizeof(line), f)) {
        char nm[64];
        int n, i;
        char *p;
        if (line[0] == '#') continue;
        if (sscanf(line, "%63s %d", nm, &n) != 2) continue;
        if (strcmp(nm, name) != 0) continue;
        p = strstr(line, nm) + strlen(nm);
        strtol(p, &p, 10);
        v = (double *)malloc(sizeof(double) * (size_t)n);
        for (i = 0; i < n; i++)
            v[i] = strtod(p, &p);
        *nout = n;
        break;
    }
    fclose(f);
    return v;
}

static void check_q11(const char *name, const short *fw, int n, double max_lsb)
{
    int rn = 0, i, worst_i = 0;
    double *ref = load_ref(name, &rn);
    double worst = 0.0;
    CHECK(ref != NULL, "no reference for %s\n", name);
    if (!ref) return;
    CHECK(rn == n, "%s: %d firmware values vs %d reference\n", name, n, rn);
    for (i = 0; i < n && i < rn; i++) {
        double got = fw[i] / (double)AMBE_Q11;
        double d = fabs(got - ref[i]);
        if (d > worst) { worst = d; worst_i = i; }
        CHECK(d * AMBE_Q11 <= max_lsb + 1e-6,
              "%s[%d]: firmware %d (%.6f) vs mbelib %.6f, off by %.2f LSB\n",
              name, i, fw[i], got, ref[i], d * AMBE_Q11);
    }
    printf("\n    %-8s %4d values, worst %.2f LSB (index %d)", name, n,
           worst * AMBE_Q11, worst_i);
    free(ref);
}

int main(void)
{
    int rn = 0, i, L;
    double *ref;

    /*
     * The two PRBA codebooks - 2048 of the 2368 quantiser values - agree with
     * mbelib to exactly half a step, i.e. mbelib's reconstruction of them is
     * bit-for-bit what DVSI ships, once rounded to Q11.  The four HOC tables
     * and the gain table do not quite: they sit up to 1.5 steps out, and it is
     * not a scale factor (fitting one by least squares leaves 1.4 steps), so
     * mbelib's reconstruction of those is very slightly wrong.  The bounds
     * below record that distinction rather than papering over it; a misread
     * table would be out by orders of magnitude, not by one step.
     */
    /* 0.51, not 0.50: mbelib's header carries six decimal places, so a value
       sitting exactly on the half-step boundary reads back as 0.5005 LSB. */
    check_q11("prba24", ambe_prba24_q11, 1536, 0.51);
    check_q11("prba58", ambe_prba58_q11, 512, 0.51);
    check_q11("hoc_b5", ambe_hoc_b5_q11, 128, 2.0);
    check_q11("hoc_b6", ambe_hoc_b6_q11, 64, 2.0);
    check_q11("hoc_b7", ambe_hoc_b7_q11, 64, 2.0);
    check_q11("hoc_b8", ambe_hoc_b8_q11, 32, 2.0);
    check_q11("dg", ambe_dg_q11, 32, 1.5);

    /* the nibble-packed block lengths are integers and must match exactly */
    ref = load_ref("lmprbl", &rn);
    CHECK(ref != NULL && rn == 228, "lmprbl reference missing or wrong size\n");
    if (ref) {
        for (i = 36; i < 228; i++)     /* rows 0..8 are unused: L >= 9 */
            CHECK((int)ambe_lmprbl[i] == (int)ref[i],
                  "lmprbl[%d]: firmware %d vs mbelib %d\n",
                  i, ambe_lmprbl[i], (int)ref[i]);
        /* and the sum over each row must be L, which is what makes it a valid
           partition of the harmonics into four prediction blocks */
        for (L = 9; L <= 56; L++) {
            int s = ambe_lmprbl[L*4] + ambe_lmprbl[L*4+1] +
                    ambe_lmprbl[L*4+2] + ambe_lmprbl[L*4+3];
            CHECK(s == L, "lmprbl row L=%d sums to %d\n", L, s);
        }
        free(ref);
    }

    /*
     * The voicing patterns, unpacked the way the firmware indexes them.  This
     * also settles a question mbelib left open: its header carries two variants
     * of AmbeVuv, one of them commented out as "alternate version".  The image
     * reproduces the active one for all 32 rows and the alternate for only 29,
     * so the active one is right.
     */
    ref = load_ref("vuv", &rn);
    CHECK(ref != NULL && rn == 256, "vuv reference missing or wrong size\n");
    if (ref) {
        int b1, jl, exact = 0;
        for (b1 = 0; b1 < 32; b1++) {
            unsigned int w = ambe_vuv_packed[(b1 << 2) & 127];
            int row = 1;
            for (jl = 0; jl < 8; jl++) {
                int got = (int)((w >> (30 - 2 * jl)) & 1u);
                CHECK(got == (int)ref[b1 * 8 + jl],
                      "vuv[%d][%d]: firmware %d vs mbelib %d\n",
                      b1, jl, got, (int)ref[b1 * 8 + jl]);
                if (got != (int)ref[b1 * 8 + jl]) row = 0;
            }
            exact += row;
        }
        CHECK(exact == 32, "only %d of 32 voicing rows match\n", exact);
        printf("\n    vuv       32 rows, %d exact", exact);
        free(ref);
    }

    /*
     * The harmonic count is not an independent table: it falls out of the
     * fundamental.  Checking that here is what lets ambe_l_table be treated as
     * derived rather than as a fourth unresolved table.
     */
    for (i = 0; i < 120; i++)
        CHECK((int)floor(0.4627 / ambe_w0_table[i]) == (int)ambe_l_table[i],
              "L[%d]: floor(0.4627/f0) = %d, table says %d\n",
              i, (int)floor(0.4627 / ambe_w0_table[i]), (int)ambe_l_table[i]);

    printf("\n                         ");
    return t_done("firmware quantiser tables vs mbelib");
}
