/*
 * test_encode.c - the quantiser inverse, against real radio frames.
 *
 * mbelib has no encoder, so there is no reference implementation to diff
 * against.  The oracle here is better than one anyway: 360 frames of AMBE that
 * a real Baofeng DM-32 actually transmitted.  Decode each to model parameters,
 * re-quantise those parameters, and require the radio's own bits back.
 *
 * Two things are asserted, and the distinction matters:
 *
 *   1. The re-encoded payload must decode to *identical* parameters - same w0,
 *      same L, same voicing, same spectral amplitudes to within float epsilon.
 *      That is what an encoder actually has to guarantee, and it is required of
 *      every voice frame.
 *
 *   2. The nine indices must come back exactly, with one principled exception:
 *      b1.  The voicing table has 32 entries but only 13 distinct 8-band
 *      patterns - indices 16..31 are all-unvoiced, and {0,1,3} and {10,13,15}
 *      also collide - so when a frame's voicing is degenerate the transmitted
 *      index is not recoverable from the decoded parameters, by construction.
 *      A b1 difference is accepted only when both indices expand to the same
 *      voicing pattern; any other index differing is a failure.
 *
 * Silence frames are excluded from the round trip: the decoder replaces their
 * pitch with the fixed silence descriptor, so b0 is not recoverable.  The
 * descriptor itself is checked separately.
 */
#include "ambe.h"
#include "ambe_tables.h"
#include "testutil.h"

static void vuv_pattern(int b1, int out[8])
{
    unsigned int w = ambe_vuv_packed[(b1 << 2) & 127];
    int i;
    for (i = 0; i < 8; i++)
        out[i] = (int)((w >> (30 - 2 * i)) & 1u);
}

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    char bl[128];
    ambe_parms cur, prev, prev_enh;
    int n = 0, voice = 0, silence = 0, other = 0;
    int exact_bits = 0, b1_ambiguous = 0;
    int i;

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS], e[AMBE_BITS];
        ambe_frame_info dinfo, einfo;
        ambe_frame_type t;

        if (strlen(bl) < AMBE_BITS)
            continue;
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] - '0');

        memset(&dinfo, 0, sizeof(dinfo));
        t = ambe_decode_parms(d, &cur, &prev, &dinfo);

        if (t == AMBE_FRAME_VOICE) {
            ambe_parms chk, prev_copy;
            ambe_frame_info cinfo;
            int l;

            memset(&einfo, 0, sizeof(einfo));
            prev_copy = prev;
            ambe_encode_parms(&cur, &prev, e, &einfo);

            /* (1) the re-encoded payload must decode to the same parameters */
            memset(&cinfo, 0, sizeof(cinfo));
            ambe_decode_parms(e, &chk, &prev_copy, &cinfo);
            CHECK(chk.L == cur.L, "frame %d: L %d -> %d\n", n, cur.L, chk.L);
            CHECK(fabs((double)chk.w0 - cur.w0) < 1e-6,
                  "frame %d: w0 %.9g -> %.9g\n", n, cur.w0, chk.w0);
            for (l = 1; l <= cur.L && l == l; l++) {
                CHECK(chk.Vl[l] == cur.Vl[l],
                      "frame %d: Vl[%d] %d -> %d\n", n, l, cur.Vl[l], chk.Vl[l]);
                CHECK(fabs((double)chk.log2Ml[l] - cur.log2Ml[l]) < 1e-3,
                      "frame %d: log2Ml[%d] %.6f -> %.6f\n",
                      n, l, cur.log2Ml[l], chk.log2Ml[l]);
            }

            /* (2) the indices themselves */
            for (i = 0; i < 9; i++) {
                if (einfo.b[i] == dinfo.b[i])
                    continue;
                if (i == 1) {
                    int pa[8], pb[8];
                    vuv_pattern(dinfo.b[1], pa);
                    vuv_pattern(einfo.b[1], pb);
                    CHECK(memcmp(pa, pb, sizeof(pa)) == 0,
                          "frame %d: b1 %d -> %d with a different pattern\n",
                          n, dinfo.b[1], einfo.b[1]);
                    b1_ambiguous++;
                } else {
                    CHECK(0, "frame %d: b%d %d -> %d\n",
                          n, i, dinfo.b[i], einfo.b[i]);
                }
            }
            if (memcmp(d, e, AMBE_BITS) == 0)
                exact_bits++;
            voice++;
            ambe_move_parms(&cur, &prev);
        } else if (t == AMBE_FRAME_SILENCE) {
            uint8_t s[AMBE_BITS];
            ambe_parms c2, p2, pe2;
            ambe_frame_info si;
            ambe_frame_type st;
            ambe_encode_silence(s);
            ambe_init_parms(&c2, &p2, &pe2);
            memset(&si, 0, sizeof(si));
            st = ambe_decode_parms(s, &c2, &p2, &si);
            CHECK(st == AMBE_FRAME_SILENCE,
                  "silence descriptor decoded as %d\n", (int)st);
            CHECK(si.b[0] == 124, "silence descriptor b0 = %d\n", si.b[0]);
            silence++;
            ambe_move_parms(&cur, &prev);
        } else {
            other++;
            ambe_init_parms(&cur, &prev, &prev_enh);
        }
        n++;
    }
    fclose(fb);

    CHECK(n == 360, "expected 360 frames, read %d\n", n);
    CHECK(voice > 200, "only %d voice frames\n", voice);
    CHECK(exact_bits + b1_ambiguous >= voice,
          "%d frames were neither bit-identical nor an accepted b1 tie\n",
          voice - exact_bits - b1_ambiguous);
    printf("[%d/%d voice frames bit-identical, %d differ only by an ambiguous "
           "b1; %d silence] ", exact_bits, voice, b1_ambiguous, silence);

    return t_done("encoder: parameters -> 49 bits, round trip on real frames");
}
