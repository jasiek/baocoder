/*
 * mbe_ref.c - reference-vector generator.  Links against mbelib and emits the
 * known-good side of every test in tests/.
 *
 * mbelib is an independent, widely deployed open reimplementation of the same
 * codec (used by DSD, OP25 and SDRangel).  It is the oracle here; it is not
 * vendored and this tool is not part of the library build.
 *
 *   mbe_ref fec    <frames.txt>                 hex on-air frames -> 49 bits
 *   mbe_ref decode <ambe49.txt> <out.pcm>       49 bits -> params + PCM
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambe.h"
#include "mbelib.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int mode_fec(const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    while (fgets(line, sizeof(line), f)) {
        uint8_t frame[AMBE_DMR_BYTES];
        uint8_t fr8[4][24];
        char fr[4][24];
        char ambe_d[49];
        int i, errs, errs2;
        if (strlen(line) < 18) continue;
        for (i = 0; i < 9; i++)
            frame[i] = (uint8_t)((hexval(line[2*i]) << 4) | hexval(line[2*i+1]));
        /* our deinterleave, mbelib's ECC: the permutation is validated
         * separately (see tests/test_fec.c) */
        ambe_dmr_deinterleave(frame, fr8);
        for (i = 0; i < 4; i++) {
            int j;
            for (j = 0; j < 24; j++) fr[i][j] = (char)fr8[i][j];
        }
        errs  = mbe_eccAmbe3600x2450C0(fr);
        mbe_demodulateAmbe3600x2450Data(fr);
        errs2 = errs + mbe_eccAmbe3600x2450Data(fr, ambe_d);
        for (i = 0; i < 49; i++) putchar('0' + (ambe_d[i] & 1));
        printf(" %d %d\n", errs, errs2 - errs);
    }
    fclose(f);
    return 0;
}

static int mode_decode(const char *path, const char *pcmpath)
{
    char line[256];
    mbe_parms cur, prev, prev_enh;
    FILE *f = fopen(path, "r");
    FILE *p = fopen(pcmpath, "wb");
    if (!f || !p) { perror("open"); return 1; }

    srand(1);
    mbe_initMbeParms(&cur, &prev, &prev_enh);

    while (fgets(line, sizeof(line), f)) {
        char ambe_d[49];
        float fbuf[160];
        short pcm[160];
        int i, bad;
        if (strlen(line) < 49) continue;
        for (i = 0; i < 49; i++) ambe_d[i] = (char)(line[i] - '0');

        bad = mbe_decodeAmbe2450Parms(ambe_d, &cur, &prev);

        /* dump the pure decode result, before enhancement */
        printf("%d %.9g %d %.9g", bad, cur.w0, cur.L, cur.gamma);
        for (i = 1; i <= cur.L; i++) printf(" %d", cur.Vl[i]);
        for (i = 1; i <= cur.L; i++) printf(" %.9g", cur.Ml[i]);
        for (i = 1; i <= cur.L; i++) printf(" %.9g", cur.log2Ml[i]);
        printf("\n");

        if (bad == 0) {
            cur.repeat = 0;
            mbe_moveMbeParms(&cur, &prev);
            mbe_spectralAmpEnhance(&cur);
            mbe_synthesizeSpeechf(fbuf, &cur, &prev_enh, 3);
            mbe_moveMbeParms(&cur, &prev_enh);
            mbe_floattoshort(fbuf, pcm);
        } else {
            memset(pcm, 0, sizeof(pcm));
            mbe_initMbeParms(&cur, &prev, &prev_enh);
        }
        fwrite(pcm, sizeof(short), 160, p);
    }
    fclose(f);
    fclose(p);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "fec") == 0)
        return mode_fec(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "decode") == 0)
        return mode_decode(argv[2], argv[3]);
    fprintf(stderr, "usage: mbe_ref fec <frames.txt>\n"
                    "       mbe_ref decode <ambe49.txt> <out.pcm>\n");
    return 2;
}
