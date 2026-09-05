/*
 * test_blend.c - the two-frame envelope interpolation, against the firmware.
 *
 * Vocoder_ResampleSpectralEnvelope 0x00026A84 is what the radio hands its
 * synthesiser for the first 80-sample half of every frame.  This is not a
 * tolerance test: the fixture is 232 real calls captured by breaking inside
 * the function at 0x00026BB4 - after the mix loop, where the output and both
 * scratch arrays are simultaneously live - so every stage has an exact answer
 * and src/ambe_blend.c has to reproduce it bit for bit.
 *
 * Each record is one instant, so it does not depend on which payloads the
 * probe run happened to feed; the drift that makes the rest of that run
 * non-comparable with the corpus does not reach these triples.
 *
 * The output block's own voicing word is an input, not an output.  The pitch
 * branch reads param_1's voicing BEFORE writing it, so what it sees is
 * whatever the previous call left in that stack slot - which is why
 * ambe_blend_envelope takes it as an argument rather than deriving it.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "testutil.h"

int main(void)
{
    FILE *f = fixture_open("dm32_arc4_1.fwblend");
    char *line = NULL;
    size_t cap = 0;
    int n = 0, okL = 0, okf0 = 0, okenv = 0, gm = 0, copyA = 0, copyB = 0;

    while (getline(&line, &cap, f) > 0) {
        int La, Lb, Lo, k;
        unsigned va, vb, vo;
        int fa, fb, fo;
        int16_t ea[AMBE_MAX_HARMONICS], eb[AMBE_MAX_HARMONICS];
        int16_t eo[AMBE_MAX_HARMONICS], got[AMBE_MAX_HARMONICS];
        int32_t f0got;
        int Lgot, bad = 0;
        char *p = line;

        if (line[0] == '#')
            continue;
        La = (int)strtol(p, &p, 10); fa = (int)strtol(p, &p, 10);
        va = (unsigned)strtoul(p, &p, 16);
        Lb = (int)strtol(p, &p, 10); fb = (int)strtol(p, &p, 10);
        vb = (unsigned)strtoul(p, &p, 16);
        vo = (unsigned)strtoul(p, &p, 16);
        Lo = (int)strtol(p, &p, 10); fo = (int)strtol(p, &p, 10);
        for (k = 0; k < AMBE_MAX_HARMONICS; k++) ea[k] = (int16_t)strtol(p, &p, 10);
        for (k = 0; k < AMBE_MAX_HARMONICS; k++) eb[k] = (int16_t)strtol(p, &p, 10);
        for (k = 0; k < AMBE_MAX_HARMONICS; k++) eo[k] = (int16_t)strtol(p, &p, 10);
        (void)La; (void)Lb;

        Lgot = ambe_blend_envelope(got, &f0got, fa, va, ea, fb, vb, eb, vo);

        if (f0got == fo) okf0++;
        else CHECK(0, "record %d: pitch %d, firmware %d (f0a %d f0b %d)\n",
                   n, (int)f0got, fo, fa, fb);
        if (Lgot == Lo) okL++;
        else CHECK(0, "record %d: L %d, firmware %d\n", n, Lgot, Lo);

        if (Lgot == Lo) {
            for (k = 0; k < Lo; k++)
                if (got[k] != eo[k]) {
                    CHECK(0, "record %d: envelope[%d] %d, firmware %d\n",
                          n, k, got[k], eo[k]);
                    bad = 1;
                    break;
                }
            if (!bad) okenv++;
        }
        /* which branch this record exercises, so the counts prove coverage */
        if (fo != fa && fo != fb) gm++;
        else if (fo == fa) copyA++;
        else copyB++;
        n++;
    }
    free(line);
    fclose(f);

    CHECK(n > 200, "only %d interpolator calls in the fixture\n", n);
    CHECK(gm > 20, "only %d records take the geometric-mean pitch branch\n", gm);
    CHECK(okf0 == n, "pitch exact on %d of %d\n", okf0, n);
    CHECK(okL == n, "harmonic count exact on %d of %d\n", okL, n);
    CHECK(okenv == n, "envelope exact on %d of %d\n", okenv, n);

    printf("[%d firmware interpolator calls, bit-exact: pitch %d, L %d, "
           "envelope %d; branches %d current / %d previous / %d geometric mean] ",
           n, okf0, okL, okenv, copyA, copyB, gm);
    return t_done("two-frame envelope interpolation vs the firmware");
}
