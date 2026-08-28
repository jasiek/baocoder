/*
 * test_synth.c - synthesis, against mbelib's PCM for the same 360 frames.
 *
 * MBE synthesis is deliberately stochastic: every harmonic above L/4 gets a
 * random phase offset, and unvoiced bands are filled with randomly phased
 * multi-sine noise.  Two correct implementations therefore never produce the
 * same waveform, and this decoder uses a seeded xorshift where mbelib calls
 * rand().  What must agree is the spectrum and the level, so that is what is
 * compared: per-frame 16-band log energy correlation, per-frame level, and
 * overall level.
 *
 * The last check is a regression guard on this decoder's own determinism.
 */
#include "ambe.h"
#include "testutil.h"

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    FILE *fp = fixture_open("dm32_arc4_1.pcm");
    char bl[128];
    ambe_decoder *dec = ambe_decoder_create();
    short ref[AMBE_PCM_SAMPLES];
    short *all_ours = NULL, *all_ref = NULL;
    int n = 0, cap = 0, ncorr = 0;
    double sum_corr = 0.0, worst = 1.0;
    double e_ours = 0.0, e_ref = 0.0;

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS];
        short pcm[AMBE_PCM_SAMPLES];
        ambe_frame_info info;
        double ba[T_BANDS], bb[T_BANDS], c, ra, rb;
        int i;

        if (strlen(bl) < AMBE_BITS)
            continue;
        if (fread(ref, sizeof(short), AMBE_PCM_SAMPLES, fp) != AMBE_PCM_SAMPLES)
            break;
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] - '0');

        memset(&info, 0, sizeof(info));
        ambe_decode_bits(dec, d, pcm, &info);

        if (n == cap) {
            cap = cap ? cap * 2 : 512;
            all_ours = (short *)realloc(all_ours, (size_t)cap * AMBE_PCM_SAMPLES * 2);
            all_ref  = (short *)realloc(all_ref,  (size_t)cap * AMBE_PCM_SAMPLES * 2);
        }
        memcpy(all_ours + (size_t)n * AMBE_PCM_SAMPLES, pcm, sizeof(pcm));
        memcpy(all_ref  + (size_t)n * AMBE_PCM_SAMPLES, ref, sizeof(ref));

        ra = rms(pcm, AMBE_PCM_SAMPLES);
        rb = rms(ref, AMBE_PCM_SAMPLES);
        e_ours += ra * ra;
        e_ref  += rb * rb;

        /* skip near-silent frames: their band energies are pure noise floor */
        if (rb > 200.0 && ra > 200.0) {
            band_energies(pcm, AMBE_PCM_SAMPLES, ba);
            band_energies(ref, AMBE_PCM_SAMPLES, bb);
            c = dcorrelation(ba, bb, T_BANDS);
            sum_corr += c;
            ncorr++;
            if (c < worst)
                worst = c;
            CHECK(c > 0.55, "frame %d: band-energy correlation %.3f too low\n", n, c);
            CHECK(ra / rb > 0.4 && ra / rb < 2.5,
                  "frame %d: level ratio %.3f out of range\n", n, ra / rb);
        }
        n++;
    }
    fclose(fb);
    fclose(fp);

    CHECK(n == 360, "expected 360 frames, decoded %d\n", n);
    CHECK(ncorr > 150, "only %d frames were loud enough to compare\n", ncorr);
    CHECK(sum_corr / ncorr > 0.85,
          "mean band-energy correlation %.3f too low\n", sum_corr / ncorr);
    CHECK(sqrt(e_ours / e_ref) > 0.8 && sqrt(e_ours / e_ref) < 1.25,
          "overall level ratio %.3f out of range\n", sqrt(e_ours / e_ref));

    /* determinism: the same input and seed must give the same bytes */
    {
        ambe_decoder *d2 = ambe_decoder_create();
        FILE *f2 = fixture_open("dm32_arc4_1.ambe49");
        uint32_t h1 = fnv1a(all_ours, (size_t)n * AMBE_PCM_SAMPLES * 2), h2;
        short *again = (short *)malloc((size_t)n * AMBE_PCM_SAMPLES * 2);
        int m = 0;
        while (fgets(bl, sizeof(bl), f2) && m < n) {
            uint8_t d[AMBE_BITS];
            ambe_frame_info info;
            int i;
            if (strlen(bl) < AMBE_BITS) continue;
            for (i = 0; i < AMBE_BITS; i++) d[i] = (uint8_t)(bl[i] - '0');
            memset(&info, 0, sizeof(info));
            ambe_decode_bits(d2, d, again + (size_t)m * AMBE_PCM_SAMPLES, &info);
            m++;
        }
        h2 = fnv1a(again, (size_t)n * AMBE_PCM_SAMPLES * 2);
        CHECK(h1 == h2, "decoder is not deterministic: %08x vs %08x\n", h1, h2);
        free(again);
        fclose(f2);
        ambe_decoder_destroy(d2);
    }

    printf("[mean band corr %.3f, worst %.3f, level ratio %.3f] ",
           sum_corr / ncorr, worst, sqrt(e_ours / e_ref));
    free(all_ours);
    free(all_ref);
    ambe_decoder_destroy(dec);
    return t_done("synthesis vs mbelib (spectral)");
}
