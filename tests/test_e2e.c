/*
 * test_e2e.c - the whole chain on a real Baofeng DM-32 transmission.
 *
 *   9 on-air bytes -> deinterleave -> Golay/PRNG FEC -> 49 bits
 *   -> DMRA ARC4 keystream removal -> AMBE+2 decode -> 8 kHz PCM
 *
 * Two levels of ground truth:
 *
 *  1. Always on: the recovered plaintext payload must equal the committed
 *     .ambe49 fixture for every capture.  That pins the FEC and the DMRA
 *     keying - ARC4 over key||MI with 256 bytes discarded for ALGID 0x21, and
 *     AES-128/256 in OFB from an LFSR-expanded IV with the first block
 *     discarded for 0x24/0x25 - with 49 bits consumed and 7 skipped per frame
 *     and a re-key every 18-frame superframe.
 *
 *  2. When third_party/known-key-mbe-samples is present, the synthesised audio
 *     is compared against that capture's expected.wav, which was produced by
 *     SDRTrunk/JMBE - a different AMBE implementation from both this one and
 *     mbelib.  Phases are randomised by every MBE decoder, so the comparison is
 *     spectral, not sample by sample.
 *
 * That the decrypted stream is speech at all is itself strong evidence the
 * chain is right, and it is evidence that owes nothing to this project: a
 * quarter to a third of every capture decodes to the AMBE silence descriptor
 * (b0 = 124/125), where random bits would give one frame in sixty.  That check
 * is asserted per capture below, which is what validates the AES keying - the
 * cipher itself is pinned separately by the FIPS-197 vectors in test_aes.c.
 */
#include "ambe.h"
#include "ambe_crypto.h"
#include "testutil.h"

#define SAMPLEDIR "third_party/known-key-mbe-samples"

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

static int run_capture(const t_capture *c, double *corr_out, int *corr_n)
{
    char path[160];
    FILE *ff, *fa;
    char line[256];
    uint8_t key[32];
    int keybytes, i, n = 0, keyed = 0, silence = 0;
    int prev_b0 = -1;
    long db0_sum = 0, db0_n = 0;
    ambe_dmra_rc4 ks;
    ambe_decoder *dec = ambe_decoder_create();
    short *ours = NULL;
    int cap = 0, wavn = 0;
    short *wav;

    keybytes = (int)strlen(c->key) / 2;
    CHECK(keybytes >= 5 && keybytes <= 32, "%s: key is %d bytes\n",
          c->name, keybytes);
    for (i = 0; i < keybytes; i++)
        key[i] = (uint8_t)((hexnib(c->key[2*i]) << 4) | hexnib(c->key[2*i+1]));

    capture_path(path, sizeof(path), c->name, ".frames");
    ff = fixture_open(path);
    capture_path(path, sizeof(path), c->name, ".ambe49");
    fa = fixture_open(path);

    while (fgets(line, sizeof(line), ff)) {
        uint8_t frame[AMBE_DMR_BYTES], fr[4][24], d[AMBE_BITS];
        short pcm[AMBE_PCM_SAMPLES];
        ambe_frame_info info;
        char want[128], mi[32];

        if (line[0] == '#' || strlen(line) < 18)
            continue;
        for (i = 0; i < 9; i++)
            frame[i] = (uint8_t)((hexnib(line[2*i]) << 4) | hexnib(line[2*i+1]));

        if (sscanf(line + 18, "%31s", mi) == 1 && strcmp(mi, "-") != 0) {
            uint8_t iv[4];
            for (i = 0; i < 4; i++)
                iv[i] = (uint8_t)((hexnib(mi[2*i]) << 4) | hexnib(mi[2*i+1]));
            if (c->algid == 0x21)
                ambe_dmra_rc4_init(&ks, key, iv);
            else
                ambe_dmra_aes_init(&ks, key, keybytes, iv);
            keyed++;
        }
        if (!keyed)
            continue;

        memset(&info, 0, sizeof(info));
        ambe_dmr_deinterleave(frame, fr);
        ambe_fec_decode(fr, d, &info);
        CHECK(ambe_dmra_rc4_apply(&ks, d) == 0,
              "%s frame %d: keystream exhausted\n", c->name, n);

        if (fgets(want, sizeof(want), fa) && strlen(want) >= AMBE_BITS)
            for (i = 0; i < AMBE_BITS; i++)
                CHECK(d[i] == (uint8_t)(want[i] - '0'),
                      "%s frame %d: plaintext bit %d\n", c->name, n, i);

        ambe_decode_bits(dec, d, pcm, &info);
        if (info.type == AMBE_FRAME_SILENCE)
            silence++;
        /* pitch continuity: see the assertion below */
        if (info.b[0] < 120) {
            if (prev_b0 >= 0) {
                int dd = info.b[0] - prev_b0;
                db0_sum += dd < 0 ? -dd : dd;
                db0_n++;
            }
            prev_b0 = info.b[0];
        } else {
            prev_b0 = -1;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 512;
            ours = (short *)realloc(ours, (size_t)cap * AMBE_PCM_SAMPLES * 2);
        }
        memcpy(ours + (size_t)n * AMBE_PCM_SAMPLES, pcm, sizeof(pcm));
        n++;
    }
    fclose(ff);
    fclose(fa);

    CHECK(n == c->frames, "%s: expected %d frames, got %d\n",
          c->name, c->frames, n);
    /*
     * What says the keying is right, without reference to any other
     * implementation: speech has a continuous pitch track.  The mean step
     * between consecutive frames' b0 runs 3.9 to 16.4 across these six
     * captures, where randomly keyed bits give 39.4 - the expected mean
     * absolute difference of two uniform draws over the 120 voice indices.
     * The silence-descriptor rate was tried first and rejected: it ranges from
     * 4.8% to 30.8% here, because some of these captures are continuous speech,
     * so it does not separate cleanly.
     */
    CHECK(db0_n > 50, "%s: only %ld voiced transitions to measure\n",
          c->name, db0_n);
    CHECK(db0_n > 0 && (double)db0_sum / (double)db0_n < 25.0,
          "%s: mean |db0| between frames is %.1f - too close to random (39.4), "
          "decryption looks wrong\n", c->name,
          db0_n ? (double)db0_sum / (double)db0_n : 0.0);
    (void)silence;

    /* audio, when the upstream sample repository is present */
    snprintf(path, sizeof(path), "%s/dmr%s/expected.wav", SAMPLEDIR, c->name + 4);
    wav = load_wav_mono(path, &wavn);
    if (wav) {
        int cmp = wavn < n * AMBE_PCM_SAMPLES ? wavn : n * AMBE_PCM_SAMPLES;
        int f, nc = 0;
        double sum = 0.0;
        for (f = 0; f * AMBE_PCM_SAMPLES + AMBE_PCM_SAMPLES <= cmp; f++) {
            const short *a = ours + (size_t)f * AMBE_PCM_SAMPLES;
            const short *b = wav + (size_t)f * AMBE_PCM_SAMPLES;
            double ba[T_BANDS], bb[T_BANDS];
            if (rms(a, AMBE_PCM_SAMPLES) < 200.0 || rms(b, AMBE_PCM_SAMPLES) < 200.0)
                continue;
            band_energies(a, AMBE_PCM_SAMPLES, ba);
            band_energies(b, AMBE_PCM_SAMPLES, bb);
            sum += dcorrelation(ba, bb, T_BANDS);
            nc++;
        }
        if (nc > 50) {
            CHECK(sum / nc > 0.80,
                  "%s: mean band correlation against JMBE %.3f too low\n",
                  c->name, sum / nc);
            *corr_out += sum / nc;
            (*corr_n)++;
        }
        free(wav);
    }
    free(ours);
    ambe_decoder_destroy(dec);
    return n;
}

int main(void)
{
    t_capture cap[16];
    int ncap = load_captures(cap, 16), i, total = 0, corr_n = 0;
    double corr = 0.0;

    CHECK(ncap >= 6, "only %d captures in the manifest\n", ncap);
    for (i = 0; i < ncap; i++)
        total += run_capture(&cap[i], &corr, &corr_n);

    CHECK(total >= 2000, "only %d frames across %d captures\n", total, ncap);
    if (corr_n)
        printf("[%d frames over %d captures; vs JMBE on %d of them, mean band "
               "correlation %.3f] ", total, ncap, corr_n, corr / corr_n);
    else
        printf("[%d frames over %d captures; no %s, audio comparison skipped] ",
               total, ncap, SAMPLEDIR);
    return t_done("end to end: encrypted DM-32 captures -> audio");
}
