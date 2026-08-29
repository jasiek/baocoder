/*
 * ambe_dump.c - dump every decoded frame's model parameters and PCM.
 *
 *   ambe_dump <in.ambe49> <out.parms> <out.pcm>
 *
 * The input side of the precision comparison between the fixed-point library
 * and the float one on main.  One source file compiles against either, because
 * the two have different public types: AMBE_FLOAT_API selects the float build.
 *
 *   cc -Iinclude tools/ambe_dump.c libbaocoder.a -o ambe_dump_fixed
 *   cc -DAMBE_FLOAT_API -I../baocoder/include tools/ambe_dump.c \
 *      ../baocoder/libbaocoder.a -lm -o ambe_dump_float
 *
 * The RNG seed is fixed so the two runs draw the same noise phases: the
 * generator is integer in both and its state update is identical, so as long
 * as L and the voicing decisions agree the two synthesisers stay in lockstep
 * and their PCM can be compared sample by sample rather than spectrally.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ambe.h"

#define SEED 0x2450A17Bu

int main(int argc, char **argv)
{
    FILE *fb, *fp, *fw;
    char line[256];
    ambe_decoder *dec;
    int n = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.ambe49> <out.parms> <out.pcm>\n", argv[0]);
        return 2;
    }
    fb = fopen(argv[1], "r");
    if (!fb) { perror(argv[1]); return 2; }
    fp = fopen(argv[2], "w");
    fw = fopen(argv[3], "wb");
    if (!fp || !fw) { perror("open output"); return 2; }

    dec = ambe_decoder_create();
    ambe_decoder_set_seed(dec, SEED);

    while (fgets(line, sizeof(line), fb)) {
        uint8_t d[AMBE_BITS];
        int16_t pcm[AMBE_PCM_SAMPLES];
        ambe_frame_info info;
        const ambe_parms *p;
        int i;
        double w0, gamma;

        if (strlen(line) < AMBE_BITS)
            continue;
        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(line[i] - '0');

        memset(&info, 0, sizeof(info));
        ambe_decode_bits(dec, d, pcm, &info);
        p = ambe_decoder_parms(dec);

#ifdef AMBE_FLOAT_API
        w0    = p->w0;
        gamma = p->gamma;
#else
        w0    = ldexp((double)ambe_w0_q24(p), -AMBE_Q_LOG);
        gamma = ldexp((double)p->gamma, -AMBE_Q_LOG);
#endif
        fprintf(fp, "%d %d %d %.10g %.10g", n, (int)info.type, p->L, w0, gamma);
        for (i = 1; i <= p->L; i++)
            fprintf(fp, " %d", (int)p->Vl[i]);
        for (i = 1; i <= p->L; i++) {
#ifdef AMBE_FLOAT_API
            fprintf(fp, " %.10g", (double)p->log2Ml[i]);
#else
            fprintf(fp, " %.10g", ldexp((double)p->log2Ml[i], -AMBE_Q_LOG));
#endif
        }
        for (i = 1; i <= p->L; i++) {
#ifdef AMBE_FLOAT_API
            fprintf(fp, " %.10g", (double)p->Ml[i]);
#else
            fprintf(fp, " %.10g", ldexp((double)p->Ml[i], p->Ml_exp - AMBE_Q_ML));
#endif
        }
        fputc('\n', fp);
        fwrite(pcm, sizeof(int16_t), AMBE_PCM_SAMPLES, fw);
        n++;
    }

    ambe_decoder_destroy(dec);
    fclose(fb);
    fclose(fp);
    fclose(fw);
    fprintf(stderr, "%s: %d frames\n", argv[1], n);
    return 0;
}
