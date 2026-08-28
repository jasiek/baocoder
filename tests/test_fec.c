/*
 * test_fec.c - on-air DMR AMBE frame -> 49-bit payload, against mbelib.
 *
 * The known-good side is mbelib's own FEC (mbe_eccAmbe3600x2450C0,
 * mbe_demodulateAmbe3600x2450Data, mbe_eccAmbe3600x2450Data) run over the same
 * 360 frames; the frames are a real Baofeng DM-32 transmission.  Every payload
 * bit and both error counts must agree exactly.
 *
 * The interleave permutation itself is pinned two further ways: the encoder is
 * checked to be the exact inverse of the decoder, and the residual corrected-
 * error rate over a real capture is required to stay low.  A wrong permutation
 * or a wrong Golay generator drives that rate up by more than an order of
 * magnitude (0.35 bits/frame here vs 5.7 for the wrong generator polynomial),
 * so it is a genuine discriminator and not just a smoke test.
 */
#include "ambe.h"
#include "testutil.h"

/*
 * The interleave schedule exactly as Vocoder_DeinterleaveVoiceBits 0x000230A4
 * writes it: out[4k + j] = in[k + 18j] over the flat MSB-first concatenation
 * c0(24) | c1(23) | c2(11) | c3(14).  Written out again here, from the
 * firmware, so that the library's version is checked against the firmware
 * rather than against itself.
 */
static void air_to_flat_ref(const uint8_t frame[AMBE_DMR_BYTES], uint8_t f[72])
{
    int k, j;
    for (k = 0; k < 18; k++) {
        for (j = 0; j < 4; j++) {
            int p = 4 * k + j;
            f[k + 18 * j] = (uint8_t)((frame[p >> 3] >> (7 - (p & 7))) & 1);
        }
    }
}

int main(void)
{
    FILE *ff = fixture_open("dm32_arc4_1.frames");
    FILE *fr = fixture_open("dm32_arc4_1.fec49");
    char fl[256], rl[256];
    int nframes = 0, identical = 0;
    long total_errs = 0;

    while (fgets(fl, sizeof(fl), ff)) {
        uint8_t frame[AMBE_DMR_BYTES], again[AMBE_DMR_BYTES];
        uint8_t fr8[4][24], d[AMBE_BITS], d2[AMBE_BITS];
        ambe_frame_info info;
        int i, e0, e1, clean;

        if (fl[0] == '#' || strlen(fl) < 18)
            continue;
        if (!fgets(rl, sizeof(rl), fr))
            break;

        for (i = 0; i < 9; i++)
            frame[i] = (uint8_t)((hexnib(fl[2*i]) << 4) | hexnib(fl[2*i+1]));

        /* the library's deinterleave must reproduce the firmware's schedule */
        {
            uint8_t ref[72];
            uint8_t chk[4][24];
            int p;
            air_to_flat_ref(frame, ref);
            ambe_dmr_deinterleave(frame, chk);
            for (p = 0; p < 24; p++)
                CHECK(chk[0][23 - p] == ref[p], "frame %d: c0 bit %d\n", nframes, p);
            for (p = 0; p < 23; p++)
                CHECK(chk[1][22 - p] == ref[24 + p], "frame %d: c1 bit %d\n", nframes, p);
            for (p = 0; p < 11; p++)
                CHECK(chk[2][10 - p] == ref[47 + p], "frame %d: c2 bit %d\n", nframes, p);
            for (p = 0; p < 14; p++)
                CHECK(chk[3][13 - p] == ref[58 + p], "frame %d: c3 bit %d\n", nframes, p);
        }

        memset(&info, 0, sizeof(info));
        ambe_dmr_deinterleave(frame, fr8);
        CHECK(ambe_fec_decode(fr8, d, &info) == 0, "frame %d: fec failed\n", nframes);

        for (i = 0; i < AMBE_BITS; i++)
            CHECK(d[i] == (uint8_t)(rl[i] - '0'),
                  "frame %d bit %d: got %d want %c\n", nframes, i, d[i], rl[i]);

        if (sscanf(rl + AMBE_BITS, " %d %d", &e0, &e1) == 2) {
            CHECK(info.errs_c0 == e0, "frame %d: c0 errs %d want %d\n",
                  nframes, info.errs_c0, e0);
            CHECK(info.errs_c1 == e1, "frame %d: c1 errs %d want %d\n",
                  nframes, info.errs_c1, e1);
            total_errs += e0 + e1;
            clean = (e0 == 0 && e1 == 0);
        } else {
            clean = 0;
        }

        /* the encoder is the exact inverse of the decoder */
        ambe_fec_encode(d, again);
        {
            uint8_t fr2[4][24];
            ambe_frame_info i2;
            memset(&i2, 0, sizeof(i2));
            ambe_dmr_deinterleave(again, fr2);
            ambe_fec_decode(fr2, d2, &i2);
            CHECK(memcmp(d, d2, AMBE_BITS) == 0,
                  "frame %d: encode/decode round trip lost bits\n", nframes);
            CHECK(i2.errs_c0 == 0 && i2.errs_c1 == 0,
                  "frame %d: re-encoded frame is not a clean codeword\n", nframes);
        }
        /*
         * Re-encoding may differ from the received frame in the FEC parity
         * positions (this decoder, like mbelib, repairs parity silently and
         * counts data-bit corrections only).  In the payload positions it must
         * differ in exactly the bits the decoder said it corrected - no more,
         * no fewer.
         */
        {
            uint8_t fa[72], fb[72];
            int p, diff = 0;
            air_to_flat_ref(frame, fa);
            air_to_flat_ref(again, fb);
            for (p = 0; p < 72; p++) {
                int payload = (p < 12) || (p >= 24 && p < 36) || (p >= 47);
                if (payload && fa[p] != fb[p])
                    diff++;
            }
            CHECK(diff == info.errs_c0 + info.errs_c1,
                  "frame %d: %d payload bits changed on re-encode, %d corrected\n",
                  nframes, diff, info.errs_c0 + info.errs_c1);
            if (memcmp(frame, again, AMBE_DMR_BYTES) == 0)
                identical++;
        }
        (void)clean;

        nframes++;
    }
    fclose(ff);
    fclose(fr);

    CHECK(nframes == 360, "expected 360 frames, read %d\n", nframes);
    /*
     * Most frames should re-encode byte for byte.  They do not all, because a
     * bit error landing in the Golay parity is repaired without being counted;
     * 269 of 360 on this capture.  The floor is a sanity check on the capture,
     * not on the codec.
     */
    CHECK(identical * 100 >= nframes * 60,
          "only %d of %d frames re-encoded byte-identically\n", identical, nframes);
    printf("[%d/%d frames re-encode byte-identically, %.2f corrected bits/frame] ",
           identical, nframes, (double)total_errs / nframes);
    CHECK((double)total_errs / nframes < 1.0,
          "corrected-error rate %.3f bits/frame is too high for a real capture\n",
          (double)total_errs / nframes);

    return t_done("dmr fec: 72 on-air bits -> 49 payload bits");
}
