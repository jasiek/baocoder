/*
 * test_voiced.c - Vocoder_SynthesizeVoiced's stages against the firmware.
 *
 * Grows a stage at a time, innermost outwards, each one swept on its own before
 * the next is written.  The whole-function oracle is
 * tests/fixtures/dm32_arc4_1.fwvoiced, and it is deliberately the last question
 * asked: a mismatch there is 2378 bytes wide, and a mismatch here is thirty
 * lines wide.
 *
 *   Vocoder_ComputeHarmonicGains        0x0001D71C   here
 *   Vocoder_SynthesizeHarmonicSpectrum  0x0001D4C8   here
 *
 * The fixture's output buffers were poked 0xEEEE before each call, so a short
 * the firmware did not write reads as 61166 rather than as a plausible zero -
 * which is how a transcription that writes the right values into the wrong
 * slots is caught.
 *
 * The spectrum sweep was mutation-tested rather than trusted, because 417 of
 * 417 first time is as consistent with a weak fixture as with a right
 * transcription.  Breaking one decision at a time in ambe_voiced.c:
 *
 *   bins numbered p+1 rather than p        12 of 417 still pass
 *   the +0x17 exponent bias                12
 *   folding the phase to its magnitude     195
 *   the peak taken over voiced harmonics   410
 *   the wrap sample after a silent frame   415
 *
 * Two mutations survive intact, and both are dead code rather than a gap in the
 * sweep: the two saturating phase branches cannot be reached by any 16-bit
 * phase, and the block-float shift cannot reach 32.  ambe_voiced.c carries the
 * range arguments.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_voiced_int.h"
#include "testutil.h"

#define NMAX  0x38
#define NDEST 0x102

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
    line = NULL;
    cap = 0;            /* getline reuses both, and the next block calls it */
    fclose(f);

    CHECK(n > 1500, "only %d cases in the fixture\n", n);
    CHECK(zero_phase > 0, "no case has a zero phase error, which is the only "
                          "way to the 0x1f exponent path at 0x0001D95A\n");
    CHECK(maxL == NMAX, "the sweep reaches only %d harmonics, not %d\n",
          maxL, NMAX);
    {   /* Vocoder_SynthesizeHarmonicSpectrum, the one that runs the inverse
           transform - and so the first stage whose answer depends on the FFT
           this library already holds bit-exact against the radio */
        FILE *g = fixture_open("voiced_spectrum.fw");
        int sn = 0, sok = 0, silent = 0, wrapped = 0;

        while (getline(&line, &cap, g) > 0) {
            long step, bias, end, start, mark, refe, refr;
            uint16_t ph[NMAX], am[NMAX];
            int16_t vo[NMAX], ref[NDEST];
            int32_t fft[NDEST / 2 + 1];
            int16_t *dest = (int16_t *)fft;
            int16_t e = 0x7BAD;
            char *p = line;
            short got;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            step  = strtol(p, &p, 10); bias = strtol(p, &p, 10);
            end   = strtol(p, &p, 10); start = strtol(p, &p, 10);
            mark  = strtol(p, &p, 10);
            for (k = 0; k < NMAX; k++)  ph[k] = (uint16_t)strtoul(p, &p, 10);
            for (k = 0; k < NMAX; k++)  vo[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < NMAX; k++)  am[k] = (uint16_t)strtoul(p, &p, 10);
            for (k = 0; k < NDEST; k++) ref[k] = (int16_t)strtol(p, &p, 10);
            refe = strtol(p, &p, 10);
            refr = strtol(p, &p, 10);

            for (k = 0; k < NDEST; k++)
                dest[k] = (int16_t)0xEEEE;
            got = ambe_voiced_harmonic_spectrum(fft, &e, (int16_t)step, ph, vo,
                                                am, (int16_t)bias, (int)end,
                                                (int)start, (int16_t)mark);
            if (got != (short)refr || e != (int16_t)refe) {
                CHECK(0, "spectrum case %d (step %ld bias %ld end %ld start %ld): "
                         "%d bins exp %d, firmware %ld bins exp %ld\n",
                      sn, step, bias, end, start, (int)got, (int)e, refr, refe);
                bad = 1;
            }
            for (k = 0; k < NDEST && !bad; k++)
                if (dest[k] != ref[k]) {
                    CHECK(0, "spectrum case %d short %d: %d, firmware %d\n",
                          sn, k, (int)dest[k], (int)ref[k]);
                    bad = 1;
                }
            if (!bad)
                sok++;
            if (refr == 0)
                silent++;
            if (ref[0x100] == ref[0])
                wrapped++;
            sn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(sn > 400, "only %d spectrum cases in the fixture\n", sn);
        CHECK(silent > 0, "no case writes zero bins, which is the -0x20 "
                          "exponent path at 0x0001D67E\n");
        CHECK(wrapped == sn, "the wrap sample at [0x100] is not the first on "
                             "%d cases\n", sn - wrapped);
        CHECK(sok == sn, "Vocoder_SynthesizeHarmonicSpectrum exact on %d of %d\n",
              sok, sn);
        printf("[Vocoder_SynthesizeHarmonicSpectrum: %d cases, %d bit-exact "
               "through the inverse transform, %d of them silent] ",
               sn, sok, silent);
    }

    CHECK(clamped > 0, "no harmonic reaches the clamp, where the gain would "
                       "exceed the sample count - the comparison there wraps\n");
    CHECK(ok == n, "Vocoder_ComputeHarmonicGains exact on %d of %d\n", ok, n);

    printf("[Vocoder_ComputeHarmonicGains: %d cases, %d bit-exact, up to %d "
           "harmonics, %d of them clamped] ", n, ok, maxL, clamped);
    return t_done("the voiced synthesiser's stages vs the firmware");
}
