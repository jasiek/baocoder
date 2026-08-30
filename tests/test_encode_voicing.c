/*
 * test_encode_voicing.c - what the analyser's voicing decision is actually worth.
 *
 * The round trip is a fair known-good pair, the same one test_encode_pcm uses
 * for pitch: decoding a real DM-32 frame gives audio whose voicing pattern is
 * known exactly, because it came out of the radio's bitstream.  Re-analysing
 * that audio and comparing the recovered b1 says how well the estimator does.
 *
 * The number that matters is not the hit rate on its own.  67% of the corpus's
 * bands are voiced, so "always say voiced" is a strong trivial answer, and any
 * estimator has to be measured against that rather than against 50%.  This
 * test therefore computes the baseline from the same frames it scores, and
 * asserts the estimator beats it.
 *
 * It does now.  The estimator scores 72% against the trivial answer's 67%, and
 * recovers the exact eight-bit pattern on 22% of frames against 5.5% before -
 * so this asserts the property one wants rather than pinning a defect.  It was
 * a characterisation test for as long as the decision was made on the 256-point
 * speech spectrum; moving it to the sub-band filterbank's envelope spectrum is
 * what changed it.
 *
 * How it got here is worth keeping, because two plausible fixes were measured
 * and neither worked.  Retuning the old spectral threshold is worth about 12
 * points and still lands below always-voiced (the sweep is on VOICE_NUM in
 * ambe_analysis.c); soft scoring over the 16 codebook patterns, and per-frame
 * normalisation, both came out worse still.  tools/proto_voicing.py then showed
 * why: on the 256-point speech spectrum the measure is at chance in exactly the
 * bands where a decision would pay, at any window length, so no rule built on
 * it could beat the trivial answer.
 *
 * What worked was changing the quantity.  The radio's filterbank transforms
 * channel *magnitudes*, so it measures envelope periodicity rather than
 * spectral harmonic concentration - a different thing in a different domain.
 * src/ambe_subband.c computes it, tools/env_voicing.c scores it at AUC 0.762
 * against the spectral rule's 0.615, and ambe_analysis.c now decides on it.
 * docs/fixed-point.md, "Envelope periodicity, measured", has the tables.
 *
 * One coupling to know about: voicing and amplitude are linked, because an
 * unvoiced harmonic is rescaled at the amplitude step.  Changing this decision
 * cost 3.6 dB of round-trip level until AMBE_ML_SCALE_Q16 was re-swept, which
 * is why tests/test_encode_pcm.c must be re-run alongside this one.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "testutil.h"
#include "ambe_tables.h"

#define VBAND(b1, j) ((ambe_vuv_packed[((b1) << 2) & 127] >> (30 - 2 * (j))) & 1u)

int main(void)
{
    t_capture cap[16];
    int ncap = load_captures(cap, 16), c;
    int frames = 0, exact = 0, bandhit = 0, bandtot = 0, refvoiced = 0;

    CHECK(ncap >= 6, "only %d captures in the manifest\n", ncap);

    for (c = 0; c < ncap; c++) {
        char path[128], bl[128];
        FILE *fb;
        ambe_decoder *dec = ambe_decoder_create();
        ambe_encoder *enc = ambe_encoder_create();

        capture_path(path, sizeof(path), cap[c].name, ".ambe49");
        fb = fixture_open(path);
        while (fgets(bl, sizeof(bl), fb)) {
            uint8_t d[AMBE_BITS], e[AMBE_BITS];
            ambe_frame_info di, ei;
            short pcm[AMBE_PCM_SAMPLES];
            int i;

            if (strlen(bl) < AMBE_BITS)
                continue;
            for (i = 0; i < AMBE_BITS; i++)
                d[i] = (uint8_t)(bl[i] - '0');

            memset(&di, 0, sizeof(di));
            ambe_decode_bits(dec, d, pcm, &di);
            /*
             * Voicing indices 16..31 are all-unvoiced and their audio is
             * noise, so the pattern is not a fair question there - the same
             * exclusion test_encode_pcm makes for pitch.
             */
            if (di.type != AMBE_FRAME_VOICE || di.b[1] >= 16)
                continue;

            memset(&ei, 0, sizeof(ei));
            ambe_encode_bits(enc, pcm, e, &ei);
            frames++;
            if (ei.b[1] == di.b[1])
                exact++;
            for (i = 0; i < 8; i++) {
                bandtot++;
                if (VBAND(di.b[1], i) == VBAND(ei.b[1], i))
                    bandhit++;
                if (VBAND(di.b[1], i))
                    refvoiced++;
            }
        }
        fclose(fb);
        ambe_decoder_destroy(dec);
        ambe_encoder_destroy(enc);
    }

    CHECK(frames > 1000, "only %d comparable frames\n", frames);

    /*
     * The bar that matters, and it now holds: the estimator must beat the
     * trivial answer on the same bands it is scored over.  This was a
     * characterisation test pinning a known defect until the voicing decision
     * moved to the envelope spectrum; it is a pass/fail test now.
     *
     * The comparison is against the corpus's own voiced fraction rather than a
     * constant, so it stays honest if the corpus changes.
     */
    CHECK(bandhit > refvoiced,
          "voicing scores %d/%d bands (%.2f%%) against %.2f%% for answering "
          "\"voiced\" every time - the estimator carries no information\n",
          bandhit, bandtot, 100.0 * bandhit / bandtot,
          100.0 * refvoiced / bandtot);

    /* and a floor under the whole-pattern rate, which moved 5.5% -> 22.3% */
    CHECK(exact * 100 > frames * 15,
          "only %d of %d frames recover the exact pattern\n", exact, frames);

    printf("[%d frames; b1 exact %d (%.1f%%); bands %.2f%% vs %.2f%% for "
           "always-voiced] ",
           frames, exact, 100.0 * exact / frames,
           100.0 * bandhit / bandtot, 100.0 * refvoiced / bandtot);
    return t_done("encoder: voicing, against the transmitted pattern");
}
