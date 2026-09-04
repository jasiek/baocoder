/*
 * test_synth.c - synthesis, against the radio's own audio for the same frames.
 *
 * The reference is dm32_arc4_1.fwpcm: the PCM the stock decoder produces when
 * the firmware is executed under the p-code emulator over these 360 payloads
 * (Vocoder_TxTask 0x0002EC30 into its 160-sample ring at TX_CTX+0x116E).
 * mbelib's PCM is compared alongside, as a measurement of the reference
 * decoder rather than as a second oracle.
 *
 * Not sample by sample, and this is not a weakness of either implementation:
 * MBE synthesis is deliberately stochastic - every harmonic above L/4 gets a
 * random phase offset and unvoiced bands are filled with randomly phased
 * multi-sine noise - so no two implementations produce the same waveform.  The
 * radio's synthesiser is an inverse FFT, this one is a time-domain sinusoid
 * sum, and mbelib is a third.  What has to agree is the spectrum.
 *
 * The level does not, and the difference is a convention rather than an error:
 * the ring the emulator reads is upstream of the output gain this decoder
 * applies, so its samples are consistently smaller.  What is asserted is that
 * the ratio is stable, not that it is one.
 *
 * The last check is a regression guard on this decoder's own determinism.
 */
#include "ambe.h"
#include "testutil.h"

/* measured: this decoder's output over the firmware ring's, energy weighted */
#define LEVEL_RATIO 3.53

int main(void)
{
    FILE *fb = fixture_open("dm32_arc4_1.ambe49");
    FILE *ff = fixture_open("dm32_arc4_1.fwpcm");
    FILE *fp = fixture_open("dm32_arc4_1.pcm");
    char bl[128];
    ambe_decoder *dec = ambe_decoder_create();
    short ref[AMBE_PCM_SAMPLES], mbe[AMBE_PCM_SAMPLES];
    short *all_ours = NULL;
    int n = 0, cap = 0, ncorr = 0, nmbe = 0;
    double sum_corr = 0.0, worst = 1.0, sum_mbe = 0.0, worst_mbe = 1.0;
    double e_ours = 0.0, e_ref = 0.0;

    while (fgets(bl, sizeof(bl), fb)) {
        uint8_t d[AMBE_BITS];
        short pcm[AMBE_PCM_SAMPLES];
        ambe_frame_info info;
        double ba[T_BANDS], bb[T_BANDS], c, ra, rb, rm;
        int i;

        if (strlen(bl) < AMBE_BITS)
            continue;
        if (fread(ref, sizeof(short), AMBE_PCM_SAMPLES, ff) != AMBE_PCM_SAMPLES)
            break;
        if (fread(mbe, sizeof(short), AMBE_PCM_SAMPLES, fp) != AMBE_PCM_SAMPLES)
            memset(mbe, 0, sizeof(mbe));
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(bl[i] - '0');

        memset(&info, 0, sizeof(info));
        ambe_decode_bits(dec, d, pcm, &info);

        if (n == cap) {
            cap = cap ? cap * 2 : 512;
            all_ours = (short *)realloc(all_ours, (size_t)cap * AMBE_PCM_SAMPLES * 2);
        }
        memcpy(all_ours + (size_t)n * AMBE_PCM_SAMPLES, pcm, sizeof(pcm));

        ra = rms(pcm, AMBE_PCM_SAMPLES);
        rb = rms(ref, AMBE_PCM_SAMPLES);
        rm = rms(mbe, AMBE_PCM_SAMPLES);
        e_ours += ra * ra;
        e_ref  += rb * rb;

        /* skip near-silent frames: their band energies are pure noise floor */
        if (rb > 50.0 && ra > 50.0) {
            band_energies(pcm, AMBE_PCM_SAMPLES, ba);
            band_energies(ref, AMBE_PCM_SAMPLES, bb);
            c = dcorrelation(ba, bb, T_BANDS);
            sum_corr += c;
            ncorr++;
            if (c < worst)
                worst = c;
            CHECK(c > 0.60, "frame %d: band-energy correlation %.3f against the "
                  "firmware too low\n", n, c);
        }
        if (rb > 50.0 && rm > 50.0) {
            band_energies(mbe, AMBE_PCM_SAMPLES, ba);
            band_energies(ref, AMBE_PCM_SAMPLES, bb);
            c = dcorrelation(ba, bb, T_BANDS);
            sum_mbe += c;
            nmbe++;
            if (c < worst_mbe)
                worst_mbe = c;
        }
        n++;
    }
    fclose(fb);
    fclose(ff);
    fclose(fp);

    CHECK(n == 360, "expected 360 frames, decoded %d\n", n);
    CHECK(ncorr > 150, "only %d frames were loud enough to compare\n", ncorr);
    CHECK(sum_corr / ncorr > 0.96,
          "mean band-energy correlation against the firmware %.3f too low\n",
          sum_corr / ncorr);
    {
        double lr = sqrt(e_ours / e_ref);
        CHECK(lr > LEVEL_RATIO * 0.8 && lr < LEVEL_RATIO * 1.25,
              "level ratio %.3f is not the measured %.2f convention offset\n",
              lr, LEVEL_RATIO);
    }

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

    printf("[vs the firmware's own audio: mean band corr %.3f, worst %.3f, "
           "level x%.2f; mbelib on the same reference: %.3f mean, %.3f worst] ",
           sum_corr / ncorr, worst, sqrt(e_ours / e_ref),
           sum_mbe / nmbe, worst_mbe);
    free(all_ours);
    ambe_decoder_destroy(dec);
    return t_done("synthesis vs the firmware's own audio (spectral)");
}
