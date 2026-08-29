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
 * The pitch and harmonic count are not tables at all - the firmware computes
 * them - so its law is checked against mbelib's tabulation instead.
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
     * The FFT's twiddles and its two bit-reversal permutations.  Like the
     * cosine table, these are identified by what they reproduce rather than
     * by where they sit: the twiddles must be exp(-j*2*pi*k/512), and the
     * permutation tables must decode to the bit-reversal of their size.
     */
    {
        int worst = 0;
        for (i = 0; i < 256; i++) {
            double th = 2.0 * M_PI * (double)i / 512.0;
            int wr = (int)floor(32767.0 * cos(th) + 0.5);
            int wi = (int)floor(-32767.0 * sin(th) + 0.5);
            int dr = ambe_fft_twiddle_q15[2*i] - wr;
            int di = ambe_fft_twiddle_q15[2*i+1] - wi;
            if (dr < 0) dr = -dr;
            if (di < 0) di = -di;
            if (dr > worst) worst = dr;
            if (di > worst) worst = di;
            CHECK(dr <= 1 && di <= 1, "twiddle[%d] = (%d,%d), want (%d,%d)\n",
                  i, ambe_fft_twiddle_q15[2*i], ambe_fft_twiddle_q15[2*i+1], wr, wi);
        }
        printf("\n    twiddle   256 pairs, exp(-j2pi k/512), worst %d LSB", worst);
    }
    {
        /* delta-coded swap pairs -> the bit-reversal permutation */
        const short *tab[2];
        int bits[2], t;
        tab[0] = ambe_fft_bitrev32;  bits[0] = 5;
        tab[1] = ambe_fft_bitrev128; bits[1] = 7;
        for (t = 0; t < 2; t++) {
            int n = tab[t][0], a = 0, b = 0, k, good = 0;
            for (k = 0; k < n; k++) {
                int lo, hi, rev, x;
                a += tab[t][1 + 2*k] * 2;
                b += tab[t][2 + 2*k] * 2;
                lo = a / 4; hi = b / 4;
                /* bit-reverse lo over bits[t] bits and require it to be hi */
                rev = 0; x = lo;
                for (i = 0; i < bits[t]; i++) { rev = (rev << 1) | (x & 1); x >>= 1; }
                if (rev == hi) good++;
                CHECK(rev == hi, "bitrev%d pair %d: %d <-> %d, reverse is %d\n",
                      1 << bits[t], k, lo, hi, rev);
            }
            printf("\n    bitrev%-3d %d pairs, %d exact", 1 << bits[t], n, good);
        }
    }

    /*
     * The analyser's window, SRAM 0x180010A8.  Half of a 199-point Hamming,
     * folded symmetrically by Dsp_WindowAndComputeFft 0x00019B6C.  Checking it
     * against the Hamming definition is what identifies it - the values alone
     * would only say "a smooth taper".
     */
    {
        int worst = 0;
        for (i = 0; i < 100; i++) {
            double h = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i /
                                         (double)(AMBE_ANWIN_N - 1));
            int want = (int)(h * (double)AMBE_ANWIN_PEAK + 0.5);
            int d = ambe_anwin_q15[i] - want;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
            CHECK(d <= 1, "anwin[%d] = %d, Hamming gives %d\n",
                  i, ambe_anwin_q15[i], want);
        }
        /* symmetric about the centre tap, which must be the peak */
        CHECK(ambe_anwin_q15[99] == AMBE_ANWIN_PEAK,
              "anwin centre %d is not the recorded peak %d\n",
              ambe_anwin_q15[99], AMBE_ANWIN_PEAK);
        printf("\n    anwin     100 values, 199-pt Hamming, worst %d LSB", worst);
    }

    /*
     * The pitch and harmonic count are not tables at all: the firmware computes
     * them, and ambe_pitch_from_b0 evaluates its law with its constants.  This
     * checks that law against mbelib's tabulated approximation of the same
     * quantiser.
     *
     * The law now evaluates 2^x with the firmware's own four-term Taylor
     * polynomial (Vocoder_PitchFromLog2 0x0002AD6C) rather than with pow(),
     * and that closes the gap to mbelib rather than widening it: f0 agrees to
     * 4.8e-5 relative where an exact pow() left 2.4e-3, and all 120 harmonic
     * counts match where the exact version disagreed at b0 = 17.
     *
     * That is worth stating plainly, because it is evidence and not just a
     * tolerance: b0 = 17 sits on the L = 11/12 boundary, an exact pow() lands
     * one side of it and the radio's polynomial the other, and mbelib's
     * independently-derived table agrees with the radio.  Two implementations
     * that share an approximation agree on a boundary case an exact
     * computation gets wrong - which says the approximation is the one the
     * codec is specified in terms of, not an artefact of this radio.
     *
     * Both figures are asserted so a regression cannot quietly restore them.
     */
    {
        int rn2 = 0, mism = 0;
        double *w0ref = load_ref("w0", &rn);
        double *lref  = load_ref("ltab", &rn2);
        double worst = 0.0;
        CHECK(w0ref && lref && rn == 120 && rn2 == 120,
              "pitch reference missing or wrong size\n");
        if (w0ref && lref) {
            for (i = 0; i < 120; i++) {
                int32_t f0;
                int Lv;
                double d, f0d;
                ambe_pitch_from_b0(i, 7, &f0, &Lv);
                f0d = ldexp((double)f0, -AMBE_Q_F0);
                d = fabs(f0d - w0ref[i]) / w0ref[i];
                if (d > worst) worst = d;
                CHECK(d < 1e-4, "f0[%d]: firmware law %.6f vs mbelib %.6f (%.2e)\n",
                      i, f0d, w0ref[i], d);
                if (Lv != (int)lref[i]) {
                    mism++;
                    CHECK(0, "L[%d]: firmware law %d vs mbelib %d\n",
                          i, Lv, (int)lref[i]);
                }
                CHECK(Lv >= 9 && Lv <= 56, "L[%d] = %d out of range\n", i, Lv);
            }
            CHECK(mism == 0, "expected no L divergence from mbelib, got %d\n",
                  mism);
            printf("\n    pitch law 120 indices, f0 worst %.2e rel, L %d/120 vs mbelib",
                   worst, 120 - mism);
            free(w0ref);
            free(lref);
        }
    }

    printf("\n                         ");
    return t_done("firmware quantiser tables vs mbelib");
}
