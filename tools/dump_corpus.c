/*
 * dump_corpus.c - decode the capture corpus to continuous PCM plus per-frame
 * truth, for the float filterbank prototype in tools/proto_filterbank.py.
 *
 * The known-good pair is the same one tests/test_encode_voicing.c uses:
 * decoding a real DM-32 frame gives audio whose voicing pattern, pitch and
 * harmonic count are known exactly, because they came out of the radio's own
 * bitstream.  This writes that pair out so a prototype can be built in a
 * language where changing the filterbank is cheap, before anything is
 * transcribed exactly.
 *
 *   cc -O2 -std=c99 -Iinclude -Isrc tools/dump_corpus.c libbaocoder.a -lm \
 *      -o dump_corpus
 *   ./dump_corpus <outdir> tests/fixtures/dm32_*.ambe49
 *
 * Per capture it writes <name>.pcm (raw int16, frames concatenated in order,
 * which is what makes it continuous audio a filterbank can run over) and
 * <name>.meta, one line per frame:
 *
 *   <frame index> <f0 Q19> <L> <b1> <v0><v1>..<v7>
 *
 * Frames that are not voice, or whose b1 is out of range, are written with
 * b1 = -1 so the prototype can skip them without losing the sample alignment
 * that makes the PCM continuous.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <string.h>
#include "ambe.h"
#include "ambe_tables.h"

#define VBAND(b1, j) ((ambe_vuv_packed[((b1) << 2) & 127] >> (30 - 2 * (j))) & 1u)

int main(int argc, char **argv)
{
    int fi;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <outdir> <capture.ambe49>...\n", argv[0]);
        return 2;
    }

    for (fi = 2; fi < argc; fi++) {
        char bl[128], out[512], *base, *dot;
        FILE *f = fopen(argv[fi], "r"), *fp, *fm;
        ambe_decoder *dec;
        int idx = 0;

        if (!f) { fprintf(stderr, "cannot open %s\n", argv[fi]); continue; }

        base = strrchr(argv[fi], '/');
        base = base ? base + 1 : argv[fi];
        snprintf(out, sizeof(out), "%s/%s", argv[1], base);
        dot = strrchr(out, '.');
        if (dot) *dot = '\0';
        {
            char p1[600], p2[600];
            snprintf(p1, sizeof(p1), "%s.pcm", out);
            snprintf(p2, sizeof(p2), "%s.meta", out);
            fp = fopen(p1, "wb");
            fm = fopen(p2, "w");
            if (!fp || !fm) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        }

        dec = ambe_decoder_create();
        while (fgets(bl, sizeof(bl), f)) {
            uint8_t d[AMBE_BITS];
            ambe_frame_info di;
            const ambe_parms *pp;
            short pcm[AMBE_PCM_SAMPLES];
            int i;

            if (strlen(bl) < AMBE_BITS) continue;
            for (i = 0; i < AMBE_BITS; i++) d[i] = (uint8_t)(bl[i] - '0');
            memset(&di, 0, sizeof di);
            ambe_decode_bits(dec, d, pcm, &di);
            fwrite(pcm, sizeof(short), AMBE_PCM_SAMPLES, fp);

            pp = ambe_decoder_parms(dec);
            if (di.type != AMBE_FRAME_VOICE || di.b[1] < 0 || di.b[1] >= 16) {
                fprintf(fm, "%d 0 0 -1 00000000\n", idx);
            } else {
                fprintf(fm, "%d %d %d %d ", idx, (int)pp->f0, pp->L, di.b[1]);
                for (i = 0; i < 8; i++)
                    fputc(VBAND(di.b[1], i) ? '1' : '0', fm);
                fputc('\n', fm);
            }
            idx++;
        }
        ambe_decoder_destroy(dec);
        fclose(f); fclose(fp); fclose(fm);
        fprintf(stderr, "%s: %d frames\n", base, idx);
    }
    return 0;
}
