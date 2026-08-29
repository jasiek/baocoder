/*
 * test_encode_sweep.c - exhaustive codebook coverage for the quantiser inverse.
 *
 * The real-capture round trip in test_encode.c is the more meaningful test -
 * those are bits a radio actually transmitted - but it can only exercise the
 * codebook entries that happen to occur in seven seconds of one person talking.
 * Measured, that is 181 of the 512 PRBA24 entries, 16 of the 32 gain levels and
 * 32 of the 48 block-length rows.  No amount of extra speech fixes that: real
 * audio concentrates on common entries and never reaches the tail.
 *
 * So this sweeps the quantiser space directly.  Frames are synthesised by
 * choosing b0..b8, packing them, decoding to parameters and re-quantising, with
 * the indices stepped by coprime strides so that every entry of every codebook
 * is visited many times in different contexts and with a different prediction
 * history.
 *
 * Three classes of index are legitimately not recoverable, and the test knows
 * which is which rather than lowering the bar globally:
 *
 *   b1  when several voicing indices expand to the same 8-band pattern.
 *   b5..b8 for a prediction block whose length leaves it no higher-order
 *       coefficients at all (Ji <= 2): the decoder never reads that index.
 *   b5..b8 where the block uses fewer than four coefficients and another
 *       codebook entry agrees on the ones it does use.
 *
 * Anything else differing is a failure.
 */
#include "ambe.h"
#include "ambe_tables.h"
#include "ambe_bitpos.h"
#include "testutil.h"

static void put_bits(uint8_t *d, const int *idx, int n, int v)
{
    int i;
    for (i = 0; i < n; i++)
        d[idx[i]] = (uint8_t)((v >> (n - 1 - i)) & 1);
}

static void vuv_pattern(int b1, int out[8])
{
    unsigned int w = ambe_vuv_packed[(b1 << 2) & 127];
    int i;
    for (i = 0; i < 8; i++)
        out[i] = (int)((w >> (30 - 2 * i)) & 1u);
}

/* how many higher-order coefficients block i of an L-harmonic frame uses */
static int hoc_dims(int L, int block)
{
    int ji = ambe_lmprbl[L * 4 + block];
    int nb = (ji > 6 ? 6 : ji) - 2;
    return nb < 0 ? 0 : nb;
}

static int hoc_equal(const short *tbl, int a, int b, int dim)
{
    int k;
    for (k = 0; k < dim; k++)
        if (tbl[a * 4 + k] != tbl[b * 4 + k])
            return 0;
    return 1;
}

int main(void)
{
    ambe_parms cur, prev, prev_enh;
    const short *hoc[5];
    int hoc_n[5];
    unsigned char seen0[128], seen1[32], seen2[32], seen3[512], seen4[128];
    unsigned char seen5[32], seen6[16], seen7[16], seen8[8], seenL[57];
    int trial, i, cov, tot;
    long compared = 0, ambiguous_b1 = 0, unused_hoc = 0, tied_hoc = 0;

    hoc[1] = ambe_hoc_b5_q11; hoc_n[1] = 32;
    hoc[2] = ambe_hoc_b6_q11; hoc_n[2] = 16;
    hoc[3] = ambe_hoc_b7_q11; hoc_n[3] = 16;
    hoc[4] = ambe_hoc_b8_q11; hoc_n[4] = 8;

    memset(seen0, 0, sizeof(seen0)); memset(seen1, 0, sizeof(seen1));
    memset(seen2, 0, sizeof(seen2)); memset(seen3, 0, sizeof(seen3));
    memset(seen4, 0, sizeof(seen4)); memset(seen5, 0, sizeof(seen5));
    memset(seen6, 0, sizeof(seen6)); memset(seen7, 0, sizeof(seen7));
    memset(seen8, 0, sizeof(seen8)); memset(seenL, 0, sizeof(seenL));

    ambe_init_parms(&cur, &prev, &prev_enh);

    for (trial = 0; trial < 4096; trial++) {
        uint8_t d[AMBE_BITS], e[AMBE_BITS];
        ambe_frame_info dinfo, einfo;
        ambe_frame_type t;
        int b[9];

        /* coprime strides so every codebook is swept independently */
        b[0] = (trial * 17) % 120;          /* voice frames only */
        b[1] = (trial * 7)  % 32;
        b[2] = (trial * 11) % 32;
        b[3] = (trial * 5)  % 512;
        b[4] = (trial * 3)  % 128;
        b[5] = (trial * 13) % 32;
        b[6] = (trial * 9)  % 16;
        b[7] = (trial * 15) % 16;
        b[8] = (trial * 3)  % 8;

        memset(d, 0, sizeof(d));
        put_bits(d, ambe_b0_idx, 7, b[0]);
        put_bits(d, ambe_b1_idx, 5, b[1]);
        put_bits(d, ambe_b2_idx, 5, b[2]);
        put_bits(d, ambe_b3_idx, 9, b[3]);
        put_bits(d, ambe_b4_idx, 7, b[4]);
        put_bits(d, ambe_b5_idx, 5, b[5]);
        put_bits(d, ambe_b6_idx, 4, b[6]);
        put_bits(d, ambe_b7_idx, 4, b[7]);
        put_bits(d, ambe_b8_idx, 3, b[8]);

        memset(&dinfo, 0, sizeof(dinfo));
        t = ambe_decode_parms(d, &cur, &prev, &dinfo);
        CHECK(t == AMBE_FRAME_VOICE, "trial %d: b0=%d did not decode as voice\n",
              trial, b[0]);
        if (t != AMBE_FRAME_VOICE)
            continue;

        memset(&einfo, 0, sizeof(einfo));
        {
            ambe_parms prev_copy = prev, chk;
            ambe_frame_info cinfo;
            int l;
            ambe_encode_parms(&cur, &prev, e, &einfo);
            /*
             * The criterion that actually matters: whatever indices come back,
             * the re-encoded payload must decode to the same model.  Index
             * equality is then checked on top of this, so a difference is only
             * excused when it is genuinely unobservable.
             */
            memset(&cinfo, 0, sizeof(cinfo));
            ambe_decode_parms(e, &chk, &prev_copy, &cinfo);
            CHECK(chk.L == cur.L, "trial %d: L %d -> %d\n", trial, cur.L, chk.L);
            for (l = 1; l <= cur.L; l++) {
                CHECK(chk.Vl[l] == cur.Vl[l],
                      "trial %d: Vl[%d] %d -> %d\n", trial, l, cur.Vl[l], chk.Vl[l]);
                CHECK(fabs(t_log2ml(&chk, l) - t_log2ml(&cur, l)) < 5e-3,
                      "trial %d: log2Ml[%d] %.6f -> %.6f\n",
                      trial, l, t_log2ml(&cur, l), t_log2ml(&chk, l));
            }
        }

        seen0[b[0]] = 1; seen1[b[1]] = 1; seen2[b[2]] = 1; seen3[b[3]] = 1;
        seen4[b[4]] = 1; seen5[b[5]] = 1; seen6[b[6]] = 1; seen7[b[7]] = 1;
        seen8[b[8]] = 1; seenL[cur.L] = 1;

        for (i = 0; i < 9; i++) {
            if (i >= 5) {
                int dim = hoc_dims(cur.L, i - 5);
                if (dim == 0) { unused_hoc++; continue; }
                if (einfo.b[i] != b[i]) {
                    CHECK(hoc_equal(hoc[i - 4], einfo.b[i], b[i], dim),
                          "trial %d: b%d %d -> %d, differs within %d used dims\n",
                          trial, i, b[i], einfo.b[i], dim);
                    tied_hoc++;
                    continue;
                }
                compared++;
                continue;
            }
            if (einfo.b[i] == b[i]) { compared++; continue; }
            if (i == 1) {
                /*
                 * Accepted only when the two indices agree on every band some
                 * harmonic actually maps to; bands outside that set are never
                 * read at this L, so they cannot be recovered and do not matter.
                 */
                int pa[8], pb[8], l, used[8];
                int32_t f0c;
                int Lc;
                memset(used, 0, sizeof(used));
                ambe_pitch_from_b0(b[0], 7, &f0c, &Lc);
                for (l = 1; l <= Lc; l++)
                    used[t_band(l, f0c)] = 1;
                vuv_pattern(b[1], pa);
                vuv_pattern(einfo.b[1], pb);
                for (l = 0; l < 8; l++)
                    CHECK(!used[l] || pa[l] == pb[l],
                          "trial %d: b1 %d -> %d differs in band %d, which L=%d uses\n",
                          trial, b[1], einfo.b[1], l, Lc);
                ambiguous_b1++;
                continue;
            }
            CHECK(0, "trial %d: b%d %d -> %d (L=%d)\n",
                  trial, i, b[i], einfo.b[i], cur.L);
        }
        ambe_move_parms(&cur, &prev);
    }

    /* every codebook must have been swept end to end */
#define COVER(arr, n, name)                                                   \
    do {                                                                      \
        cov = 0;                                                              \
        for (i = 0; i < (n); i++) cov += arr[i] ? 1 : 0;                      \
        CHECK(cov == (n), "%s: only %d of %d entries swept\n", name, cov, n);  \
    } while (0)
    COVER(seen0, 120, "b0 pitch");
    COVER(seen1, 32,  "b1 voicing");
    COVER(seen2, 32,  "b2 gain");
    COVER(seen3, 512, "b3 PRBA24");
    COVER(seen4, 128, "b4 PRBA58");
    COVER(seen5, 32,  "b5 HOC");
    COVER(seen6, 16,  "b6 HOC");
    COVER(seen7, 16,  "b7 HOC");
    COVER(seen8, 8,   "b8 HOC");
    tot = 0;
    for (i = 9; i <= 56; i++) tot += seenL[i] ? 1 : 0;
    CHECK(tot >= 40, "only %d of 48 harmonic counts reached\n", tot);

    printf("[4096 frames: every entry of all 9 codebooks swept, %d/48 L values; "
           "%ld indices exact, %ld ambiguous b1, %ld unused HOC, %ld tied HOC] ",
           tot, compared, ambiguous_b1, unused_hoc, tied_hoc);

    return t_done("encoder: exhaustive codebook sweep");
}
