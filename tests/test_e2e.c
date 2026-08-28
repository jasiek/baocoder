/*
 * test_e2e.c - the whole chain on a real Baofeng DM-32 transmission.
 *
 *   9 on-air bytes -> deinterleave -> Golay/PRNG FEC -> 49 bits
 *   -> DMRA ARC4 keystream removal -> AMBE+2 decode -> 8 kHz PCM
 *
 * Two levels of ground truth:
 *
 *  1. Always on: the recovered plaintext payload must equal the committed
 *     dm32_arc4_1.ambe49 fixture.  That pins the FEC and the DMRA Enhanced
 *     Privacy keying (RC4 over key||MI, 256 bytes discarded, 49 bits consumed
 *     and 7 skipped per frame, re-seeded every 18-frame superframe) against
 *     vectors produced independently by mbelib plus a Python RC4.
 *
 *  2. When third_party/known-key-mbe-samples is present, the synthesised audio
 *     is compared against that capture's expected.wav, which was produced by
 *     SDRTrunk/JMBE - a different AMBE implementation from both this one and
 *     mbelib.  Phases are randomised by every MBE decoder, so the comparison is
 *     spectral, not sample by sample.
 *
 * That the decrypted stream is speech at all is itself strong evidence the
 * chain is right: 111 of the 360 frames decode to the AMBE silence descriptor
 * (b0 = 124/125), which random bits would produce about 3 times in 360.
 */
#include "ambe.h"
#include "ambe_crypto.h"
#include "testutil.h"

#define WAVPATH "third_party/known-key-mbe-samples/dmr_arc4_1/expected.wav"

static short *load_wav_mono(const char *path, int *nout)
{
    unsigned char hdr[12], ch[8];
    FILE *f = fopen(path, "rb");
    short *out = NULL;
    int channels = 0, bits = 0;
    if (!f)
        return NULL;
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        goto done;
    while (fread(ch, 1, 8, f) == 8) {
        unsigned long sz = (unsigned long)ch[4] | ((unsigned long)ch[5] << 8) |
                           ((unsigned long)ch[6] << 16) | ((unsigned long)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, f) != 16)
                goto done;
            channels = fmt[2] | (fmt[3] << 8);
            bits     = fmt[14] | (fmt[15] << 8);
            if (sz > 16)
                fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            size_t frames;
            short *raw;
            size_t i;
            if (channels < 1 || bits != 16)
                goto done;
            frames = sz / (size_t)(2 * channels);
            raw = (short *)malloc(sz);
            if (!raw || fread(raw, 1, sz, f) != sz) { free(raw); goto done; }
            out = (short *)malloc(frames * sizeof(short));
            for (i = 0; i < frames; i++)
                out[i] = raw[i * (size_t)channels];
            free(raw);
            *nout = (int)frames;
            goto done;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
done:
    fclose(f);
    return out;
}

int main(void)
{
    FILE *ff = fixture_open("dm32_arc4_1.frames");
    FILE *fk = fixture_open("dm32_arc4_1.key");
    FILE *fa = fixture_open("dm32_arc4_1.ambe49");
    char line[256];
    uint8_t key[5];
    ambe_dmra_rc4 rc4;
    ambe_decoder *dec = ambe_decoder_create();
    short *ours = NULL;
    int n = 0, cap = 0, keyed = 0, silence = 0, have_key = 0;
    int wavn = 0;
    short *wav;

    while (fgets(line, sizeof(line), fk)) {
        if (!strncmp(line, "key_hex=", 8)) {
            int i;
            for (i = 0; i < 5; i++)
                key[i] = (uint8_t)((hexnib(line[8 + 2*i]) << 4) | hexnib(line[9 + 2*i]));
            have_key = 1;
        }
    }
    fclose(fk);
    CHECK(have_key, "no key_hex in the key fixture\n");

    while (fgets(line, sizeof(line), ff)) {
        uint8_t frame[AMBE_DMR_BYTES], fr[4][24], d[AMBE_BITS];
        short pcm[AMBE_PCM_SAMPLES];
        ambe_frame_info info;
        char want[128];
        char mi[16];
        int i;

        if (line[0] == '#' || strlen(line) < 18)
            continue;
        for (i = 0; i < 9; i++)
            frame[i] = (uint8_t)((hexnib(line[2*i]) << 4) | hexnib(line[2*i+1]));

        if (sscanf(line + 18, "%15s", mi) == 1 && strcmp(mi, "-") != 0) {
            uint8_t iv[4];
            for (i = 0; i < 4; i++)
                iv[i] = (uint8_t)((hexnib(mi[2*i]) << 4) | hexnib(mi[2*i+1]));
            ambe_dmra_rc4_init(&rc4, key, iv);
            keyed++;
        }

        memset(&info, 0, sizeof(info));
        ambe_dmr_deinterleave(frame, fr);
        ambe_fec_decode(fr, d, &info);
        CHECK(ambe_dmra_rc4_apply(&rc4, d) == 0,
              "frame %d: keystream exhausted\n", n);

        if (fgets(want, sizeof(want), fa) && strlen(want) >= AMBE_BITS) {
            for (i = 0; i < AMBE_BITS; i++)
                CHECK(d[i] == (uint8_t)(want[i] - '0'),
                      "frame %d: plaintext bit %d\n", n, i);
        }

        ambe_decode_bits(dec, d, pcm, &info);
        if (info.type == AMBE_FRAME_SILENCE)
            silence++;

        if (n == cap) {
            cap = cap ? cap * 2 : 512;
            ours = (short *)realloc(ours, (size_t)cap * AMBE_PCM_SAMPLES * 2);
        }
        memcpy(ours + (size_t)n * AMBE_PCM_SAMPLES, pcm, sizeof(pcm));
        n++;
    }
    fclose(ff);
    fclose(fa);

    CHECK(n == 360, "expected 360 frames, got %d\n", n);
    CHECK(keyed == 20, "expected 20 superframe keystream resets, got %d\n", keyed);
    /* random bits would hit the silence descriptor about 2/128 of the time */
    CHECK(silence > 80, "only %d silence frames - decryption is probably wrong\n",
          silence);

    wav = load_wav_mono(WAVPATH, &wavn);
    if (!wav) {
        printf("[no %s; run 'make fixtures' for the audio comparison] ", WAVPATH);
    } else {
        int cmp = wavn < n * AMBE_PCM_SAMPLES ? wavn : n * AMBE_PCM_SAMPLES;
        int f, ncorr = 0, nlvl = 0;
        double sum = 0.0, worst = 1.0;
        double ra = rms(ours, cmp), rb = rms(wav, cmp);
        double *la = NULL, *lb = NULL;
        int nframes_cmp = cmp / AMBE_PCM_SAMPLES;
        la = (double *)malloc(sizeof(double) * (size_t)nframes_cmp);
        lb = (double *)malloc(sizeof(double) * (size_t)nframes_cmp);

        CHECK(cmp >= 320 * 100, "reference wav too short (%d samples)\n", wavn);
        for (f = 0; f * AMBE_PCM_SAMPLES + AMBE_PCM_SAMPLES <= cmp; f++) {
            const short *a = ours + (size_t)f * AMBE_PCM_SAMPLES;
            const short *b = wav + (size_t)f * AMBE_PCM_SAMPLES;
            double ba[T_BANDS], bb[T_BANDS], c;
            la[nlvl] = log10(rms(a, AMBE_PCM_SAMPLES) + 1.0);
            lb[nlvl] = log10(rms(b, AMBE_PCM_SAMPLES) + 1.0);
            nlvl++;
            if (rms(a, AMBE_PCM_SAMPLES) < 200.0 || rms(b, AMBE_PCM_SAMPLES) < 200.0)
                continue;
            band_energies(a, AMBE_PCM_SAMPLES, ba);
            band_energies(b, AMBE_PCM_SAMPLES, bb);
            c = dcorrelation(ba, bb, T_BANDS);
            sum += c;
            ncorr++;
            if (c < worst) worst = c;
        }
        CHECK(ncorr > 150, "only %d comparable frames\n", ncorr);
        CHECK(sum / ncorr > 0.80,
              "mean band correlation against JMBE %.3f too low\n", sum / ncorr);
        /*
         * SDRTrunk/JMBE applies its own output gain (this capture sits about
         * 14 dB above mbelib's fixed x7), so absolute level says nothing.  What
         * must track is the level envelope: loud frames loud, quiet frames
         * quiet, frame for frame.
         */
        CHECK(dcorrelation(la, lb, nlvl) > 0.90,
              "level-envelope correlation against JMBE %.3f too low\n",
              dcorrelation(la, lb, nlvl));
        printf("[%d silence frames; vs JMBE: mean band corr %.3f, "
               "level envelope corr %.3f, fixed gain offset %.1f dB] ",
               silence, sum / ncorr, dcorrelation(la, lb, nlvl),
               20.0 * log10(rb / ra));
        free(la);
        free(lb);
        free(wav);
    }

    free(ours);
    ambe_decoder_destroy(dec);
    return t_done("end to end: encrypted DM-32 capture -> audio");
}
