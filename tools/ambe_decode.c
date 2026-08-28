/*
 * ambe_decode - decode DMR AMBE+2 voice frames to a WAV file.
 *
 *   ambe_decode [options] <frames.txt> <out.wav>
 *
 * The input is one frame per line: 18 hex characters (the 9 on-air bytes),
 * optionally followed by the 8-hex-character DMRA message indicator that starts
 * a new superframe.  tools/mbe_to_frames.py converts an SDRTrunk .mbe export
 * into that format.  Lines starting with '#' are ignored.
 *
 * Options:
 *   -k HEX   10-hex-character DMRA Enhanced Privacy (ARC4) traffic key
 *   -q N     unvoiced quality, 1..64 sinusoids per unvoiced band (default 3)
 *   -s N     PRNG seed for the unvoiced synthesis (default 0x2450A17B)
 *   -b       input is 49-bit payload strings, not on-air frames
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambe.h"
#include "ambe_crypto.h"

static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xFF), f);        fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f);
}

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f);
}

static void write_wav(FILE *f, const short *pcm, long n)
{
    fwrite("RIFF", 1, 4, f); put32(f, (unsigned long)(36 + n * 2));
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, 1);
    put32(f, 8000); put32(f, 16000);
    put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (unsigned long)(n * 2));
    fwrite(pcm, sizeof(short), (size_t)n, f);
}

int main(int argc, char **argv)
{
    const char *inpath = NULL, *outpath = NULL, *keyhex = NULL;
    int uvq = 3, bitsmode = 0, i;
    unsigned long seed = 0x2450A17BuL;
    uint8_t key[5];
    int have_key = 0, keyed = 0;
    ambe_dmra_rc4 rc4;
    ambe_decoder *dec;
    FILE *in, *out;
    char line[512];
    short *pcm = NULL;
    long n = 0, cap = 0;
    long silence = 0, erasure = 0, errbits = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc)      keyhex = argv[++i];
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) uvq = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-b"))                 bitsmode = 1;
        else if (!inpath)  inpath = argv[i];
        else if (!outpath) outpath = argv[i];
    }
    if (!inpath || !outpath) {
        fprintf(stderr,
            "usage: ambe_decode [-k keyhex] [-q 1..64] [-s seed] [-b] <in> <out.wav>\n");
        return 2;
    }
    if (keyhex) {
        if (strlen(keyhex) != 10) {
            fprintf(stderr, "key must be 10 hex characters (40 bits)\n");
            return 2;
        }
        for (i = 0; i < 5; i++)
            key[i] = (uint8_t)((hexnib(keyhex[2*i]) << 4) | hexnib(keyhex[2*i+1]));
        have_key = 1;
    }

    in = fopen(inpath, "r");
    if (!in) { perror(inpath); return 1; }
    out = fopen(outpath, "wb");
    if (!out) { perror(outpath); fclose(in); return 1; }

    dec = ambe_decoder_create();
    ambe_decoder_set_uvquality(dec, uvq);
    ambe_decoder_set_seed(dec, (uint32_t)seed);

    while (fgets(line, sizeof(line), in)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        short frame_pcm[AMBE_PCM_SAMPLES];
        char mi[32];

        if (line[0] == '#' || line[0] == '\n')
            continue;
        memset(&info, 0, sizeof(info));

        if (bitsmode) {
            if (strlen(line) < AMBE_BITS) continue;
            for (i = 0; i < AMBE_BITS; i++) d[i] = (uint8_t)(line[i] - '0');
        } else {
            uint8_t frame[AMBE_DMR_BYTES], fr[4][24];
            if (strlen(line) < 18) continue;
            for (i = 0; i < 9; i++)
                frame[i] = (uint8_t)((hexnib(line[2*i]) << 4) | hexnib(line[2*i+1]));
            if (have_key && sscanf(line + 18, "%31s", mi) == 1 && strcmp(mi, "-")) {
                uint8_t iv[4];
                for (i = 0; i < 4; i++)
                    iv[i] = (uint8_t)((hexnib(mi[2*i]) << 4) | hexnib(mi[2*i+1]));
                ambe_dmra_rc4_init(&rc4, key, iv);
                keyed++;
            }
            ambe_dmr_deinterleave(frame, fr);
            ambe_fec_decode(fr, d, &info);
            errbits += info.errs_c0 + info.errs_c1;
            if (have_key && keyed && ambe_dmra_rc4_apply(&rc4, d) != 0)
                fprintf(stderr, "frame %ld: keystream exhausted\n", n);
        }

        ambe_decode_bits(dec, d, frame_pcm, &info);
        if (info.type == AMBE_FRAME_SILENCE) silence++;
        if (info.type == AMBE_FRAME_ERASURE) erasure++;

        if (n * AMBE_PCM_SAMPLES + AMBE_PCM_SAMPLES > cap) {
            cap = cap ? cap * 2 : 160L * 1024;
            pcm = (short *)realloc(pcm, (size_t)cap * sizeof(short));
        }
        memcpy(pcm + n * AMBE_PCM_SAMPLES, frame_pcm, sizeof(frame_pcm));
        n++;
    }

    write_wav(out, pcm, n * AMBE_PCM_SAMPLES);
    fprintf(stderr, "%ld frames, %.2f s: %ld silence, %ld erasure, "
                    "%.2f corrected bits/frame\n",
            n, (double)n * 0.02, silence, erasure,
            n ? (double)errbits / (double)n : 0.0);

    free(pcm);
    ambe_decoder_destroy(dec);
    fclose(in);
    fclose(out);
    return 0;
}
