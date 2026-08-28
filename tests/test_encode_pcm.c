/*
 * test_encode_pcm.c - the analysis front end, on audio with known parameters.
 *
 * There is no reference encoder to diff against, but there is something almost
 * as good: decoding the 360 real DM-32 frames produces audio whose true pitch,
 * harmonic count and gain are known exactly, because they came out of the
 * radio's own bitstream.  Running that audio back through the analyser and
 * comparing what it recovers is a real known-good pair for pitch estimation.
 *
 * The bar is set where a correlation pitch detector belongs: most voiced frames
 * within one or two quantiser steps of the transmitted b0, and octave errors
 * rare.  It is deliberately not "exact" - the analyser here is a conventional
 * MBE estimator, not a transcription of the firmware's multi-candidate search.
 */
#include "ambe.h"
#include "testutil.h"

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    char bl[128];
    ambe_decoder *dec = ambe_decoder_create();
    ambe_encoder *enc = ambe_encoder_create();
    short ref[AMBE_PCM_SAMPLES];
    int n = 0, cmp = 0, close = 0, octave = 0, i;
    double sum_rel = 0.0, lvl_ratio = 0.0;
    int lvl_n = 0;

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS], e[AMBE_BITS];
        ambe_frame_info dinfo, einfo;
        short again[AMBE_PCM_SAMPLES];
        ambe_decoder *d2;
        double r0, r1;

        if (strlen(bl) < AMBE_BITS)
            continue;
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] - '0');

        memset(&dinfo, 0, sizeof(dinfo));
        ambe_decode_bits(dec, d, ref, &dinfo);

        memset(&einfo, 0, sizeof(einfo));
        ambe_encode_bits(enc, ref, e, &einfo);

        /*
         * Only frames with actual voiced bands carry a meaningful pitch:
         * voicing indices 16..31 are all-unvoiced, and the decoded audio for
         * those is noise, so asking what pitch it has is not a fair question.
         */
        if (dinfo.type == AMBE_FRAME_VOICE && einfo.type == AMBE_FRAME_VOICE &&
            dinfo.b[1] < 16) {
            int db = einfo.b[0] - dinfo.b[0];
            if (db < 0) db = -db;
            cmp++;
            if (db <= 4) close++;
            /* one octave is 45.37 quantiser steps: f0 = 2^(-(b0+C)/45.3683) */
            if (db > 30) octave++;
            sum_rel += db;
        }

        /* level: re-decode and compare frame energy with the reference */
        d2 = ambe_decoder_create();
        ambe_decode_bits(d2, e, again, NULL);
        r0 = rms(ref, AMBE_PCM_SAMPLES);
        r1 = rms(again, AMBE_PCM_SAMPLES);
        if (r0 > 500.0 && r1 > 1.0) { lvl_ratio += r1 / r0; lvl_n++; }
        ambe_decoder_destroy(d2);
        n++;
    }
    fclose(fb);

    CHECK(n == 360, "expected 360 frames, got %d\n", n);
    CHECK(cmp > 150, "only %d frames comparable\n", cmp);
    CHECK(close * 100 >= cmp * 70,
          "only %d of %d voiced frames within 4 quantiser steps of the true pitch\n",
          close, cmp);
    CHECK(octave * 100 <= cmp * 10,
          "%d of %d frames are octave errors\n", octave, cmp);
    CHECK(lvl_n > 100, "only %d frames loud enough for a level check\n", lvl_n);
    CHECK(lvl_ratio / lvl_n > 0.79 && lvl_ratio / lvl_n < 1.26,
          "mean level ratio %.3f is more than 2 dB off\n", lvl_ratio / lvl_n);

    printf("[pitch within 4 steps on %d/%d, %d octave errors, mean |db0| %.1f; "
           "level x%.2f] ", close, cmp, octave, sum_rel / cmp, lvl_ratio / lvl_n);
    ambe_decoder_destroy(dec);
    ambe_encoder_destroy(enc);
    return t_done("encoder: PCM -> parameters, against known-pitch audio");
}
