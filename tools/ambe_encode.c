/*
 * ambe_encode - encode 8 kHz PCM into DMR AMBE+2 voice frames.
 *
 *   ambe_encode [options] <in.wav> <out.frames>
 *
 * The output is the same one-frame-per-line hex format ambe_decode reads, so
 * the two round-trip against each other directly:
 *
 *   ambe_encode speech.wav frames.txt && ambe_decode frames.txt back.wav
 *
 * Options:
 *   -b       write the 49-bit payloads instead of on-air frames
 *
 * Input must be 16-bit PCM at 8 kHz; the first channel is used.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambe.h"

static short *load_wav_mono(const char *path, int *nout, int *rate)
{
    unsigned char hdr[12], ch[8];
    FILE *f = fopen(path, "rb");
    short *out = NULL;
    int channels = 0, bits = 0;
    *nout = 0;
    *rate = 0;
    if (!f)
        return NULL;
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        goto done;
    while (fread(ch, 1, 8, f) == 8) {
        unsigned long sz = (unsigned long)ch[4] | ((unsigned long)ch[5] << 8) |
                           ((unsigned long)ch[6] << 16) | ((unsigned long)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, f) != 16) goto done;
            channels = fmt[2] | (fmt[3] << 8);
            *rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            size_t frames, i;
            short *raw;
            if (channels < 1 || bits != 16) goto done;
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

int main(int argc, char **argv)
{
    const char *inpath = NULL, *outpath = NULL;
    int bitsmode = 0, i, nsamp = 0, rate = 0, nframes = 0;
    long silence = 0;
    short *pcm;
    FILE *out;
    ambe_encoder *enc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b"))      bitsmode = 1;
        else if (!inpath)  inpath = argv[i];
        else if (!outpath) outpath = argv[i];
    }
    if (!inpath || !outpath) {
        fprintf(stderr, "usage: ambe_encode [-b] <in.wav> <out.frames>\n");
        return 2;
    }

    pcm = load_wav_mono(inpath, &nsamp, &rate);
    if (!pcm) { fprintf(stderr, "cannot read %s as 16-bit WAV\n", inpath); return 1; }
    if (rate != 8000)
        fprintf(stderr, "warning: %s is %d Hz, expected 8000\n", inpath, rate);

    out = fopen(outpath, "w");
    if (!out) { perror(outpath); free(pcm); return 1; }
    fprintf(out, "# encoded from %s by ambe_encode\n", inpath);

    enc = ambe_encoder_create();
    for (i = 0; i + AMBE_PCM_SAMPLES <= nsamp; i += AMBE_PCM_SAMPLES) {
        uint8_t frame[AMBE_DMR_BYTES], d[AMBE_BITS];
        ambe_frame_info info;
        int k;
        if (bitsmode) {
            ambe_encode_bits(enc, pcm + i, d, &info);
            for (k = 0; k < AMBE_BITS; k++) fputc('0' + d[k], out);
            fputc('\n', out);
        } else {
            ambe_encode_dmr_frame(enc, pcm + i, frame, &info);
            for (k = 0; k < AMBE_DMR_BYTES; k++) fprintf(out, "%02X", frame[k]);
            fprintf(out, " -\n");
        }
        if (info.type == AMBE_FRAME_SILENCE) silence++;
        nframes++;
    }

    fprintf(stderr, "%d frames, %.2f s: %ld silence\n",
            nframes, (double)nframes * 0.02, silence);
    ambe_encoder_destroy(enc);
    fclose(out);
    free(pcm);
    return 0;
}
