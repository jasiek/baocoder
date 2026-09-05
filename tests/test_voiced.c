/*
 * test_voiced.c - Vocoder_SynthesizeVoiced's stages against the firmware.
 *
 * Grows a stage at a time, innermost outwards, each one swept on its own before
 * the next is written.  The whole-function oracle is
 * tests/fixtures/dm32_arc4_1.fwvoiced, and it is deliberately the last question
 * asked: a mismatch there is 2378 bytes wide, and a mismatch here is thirty
 * lines wide.
 *
 *   Vocoder_ComputeHarmonicGains 0x0001D71C   here
 *
 * The fixture's output buffers were poked 0xEEEE before each call, so a short
 * the firmware did not write reads as 61166 rather than as a plausible zero -
 * which is how a transcription that writes the right values into the wrong
 * slots is caught.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_voiced_int.h"
#include "testutil.h"

#define NMAX 0x38

int main(void)
{
    FILE *f = fixture_open("voiced_gains.fw");
    char *line = NULL;
    size_t cap = 0;
    int n = 0, ok = 0, clamped = 0, zero_phase = 0, maxL = 0;

    while (getline(&line, &cap, f) > 0) {
        long phase, pitch, delta, cnt, L;
        uint16_t g[NMAX], x[NMAX], refg[NMAX], refx[NMAX];
        char *p = line;
        int k, bad = 0;

        if (line[0] == '#')
            continue;
        phase = strtol(p, &p, 10); pitch = strtol(p, &p, 10);
        delta = strtol(p, &p, 10); cnt   = strtol(p, &p, 10);
        L     = strtol(p, &p, 10);
        CHECK(L >= 1 && L <= NMAX, "case %d: L = %ld\n", n, L);
        for (k = 0; k < L; k++) refg[k] = (uint16_t)strtoul(p, &p, 10);
        for (k = 0; k < L; k++) refx[k] = (uint16_t)strtoul(p, &p, 10);

        for (k = 0; k < NMAX; k++) g[k] = x[k] = 0xEEEE;
        ambe_voiced_harmonic_gains(g, x, (int32_t)phase, (int16_t)pitch,
                                   (int16_t)delta, (uint16_t)cnt, (int)L);

        for (k = 0; k < L; k++) {
            if (g[k] != refg[k] || x[k] != refx[k]) {
                CHECK(0, "case %d (phase %ld pitch %ld delta %ld n %ld L %ld) "
                         "harmonic %d: %u/%u, firmware %u/%u\n",
                      n, phase, pitch, delta, cnt, L, k,
                      (unsigned)g[k], (unsigned)x[k],
                      (unsigned)refg[k], (unsigned)refx[k]);
                bad = 1;
                break;
            }
            if (refg[k] == 0x5000 && refx[k] == 7)
                clamped++;    /* the sample count normalised: the clamp fired */
        }
        /* nothing past L may be touched */
        for (k = (int)L; k < NMAX && !bad; k++)
            if (g[k] != 0xEEEE || x[k] != 0xEEEE) {
                CHECK(0, "case %d wrote past harmonic %ld\n", n, L);
                bad = 1;
            }
        if (!bad)
            ok++;
        if (phase == 0)
            zero_phase++;
        if (L > maxL)
            maxL = (int)L;
        n++;
    }
    free(line);
    fclose(f);

    CHECK(n > 1500, "only %d cases in the fixture\n", n);
    CHECK(zero_phase > 0, "no case has a zero phase error, which is the only "
                          "way to the 0x1f exponent path at 0x0001D95A\n");
    CHECK(maxL == NMAX, "the sweep reaches only %d harmonics, not %d\n",
          maxL, NMAX);
    CHECK(clamped > 0, "no harmonic reaches the clamp, where the gain would "
                       "exceed the sample count - the comparison there wraps\n");
    CHECK(ok == n, "Vocoder_ComputeHarmonicGains exact on %d of %d\n", ok, n);

    printf("[Vocoder_ComputeHarmonicGains: %d cases, %d bit-exact, up to %d "
           "harmonics, %d of them clamped] ", n, ok, maxL, clamped);
    return t_done("the voiced synthesiser's stages vs the firmware");
}
