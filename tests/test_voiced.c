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
 *   Vocoder_InterpolateSpectralEnvelope 0x0001D9F0   here
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
 * The interpolator was put through the same:
 *
 *   the window on the rising taper           262 of 900 still pass
 *   the wrap sample block[idx + 1]           299
 *   accumulating rather than assigning       346
 *   the falling window running backwards     399
 *   rounding the start position up           470
 *   the end clamp at 0xA6                    crashes - it is what keeps the
 *                                            writes inside the accumulator
 *
 * Replacing `(hi << 17) | (lo >> 15)` with `(int32_t)(prod >> 15)` changes
 * nothing, and that is not a gap either: the two are the same 32 bits.  The
 * stock code reassembles them from a register pair because it has no 64-bit
 * shift, not because it wants a different answer.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_voiced_int.h"
#include "testutil.h"

#define NMAX  0x38
#define NDEST 0x102
#define NENV  (0xA8 + 4)
#define NBLOCK 0x101
#define N_VST 0x22C
#define N_VOI 0x38

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

    {   /* Vocoder_InterpolateSpectralEnvelope: the resample-and-window that
           adds one harmonic into the output.  The fixture's input accumulator
           is a ramp rather than zeros, so a transcription that assigned where
           the radio adds cannot pass. */
        FILE *g = fixture_open("voiced_interp.fw");
        int in = 0, iok = 0, untouched = 0, clipped = 0;

        while (getline(&line, &cap, g) > 0) {
            long mant, exp, pitch, bexp;
            static int32_t env[NENV], ref[NENV];
            static uint16_t blk[NBLOCK];
            char *p = line;
            int k, bad = 0, same = 1;

            if (line[0] == '#')
                continue;
            mant = strtol(p, &p, 10); exp   = strtol(p, &p, 10);
            pitch = strtol(p, &p, 10); bexp = strtol(p, &p, 10);
            for (k = 0; k < NENV; k++)   env[k] = (int32_t)strtol(p, &p, 10);
            for (k = 0; k < NBLOCK; k++) blk[k] = (uint16_t)strtoul(p, &p, 10);
            for (k = 0; k < NENV; k++)   ref[k] = (int32_t)strtol(p, &p, 10);

            for (k = 0; k < NENV; k++)
                if (env[k] != ref[k])
                    same = 0;
            if (same)
                untouched++;
            for (k = 0xA8; k < NENV; k++)
                if (ref[k] != env[k])
                    clipped++;      /* the firmware ran past 0xA8 - it must not */

            ambe_voiced_interp_envelope(env, (int32_t)mant, (int16_t)exp,
                                        (uint16_t)pitch, blk, (int16_t)bexp);
            for (k = 0; k < NENV; k++)
                if (env[k] != ref[k]) {
                    CHECK(0, "interp case %d (mant %ld exp %ld pitch %ld bexp %ld) "
                             "slot %d: %d, firmware %d\n",
                          in, mant, exp, pitch, bexp, k,
                          (int)env[k], (int)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                iok++;
            in++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(in > 800, "only %d interpolate cases in the fixture\n", in);
        CHECK(untouched > 0, "no case returns without writing, which is the "
                             "start-before--0x10 path at 0x0001DA5E\n");
        CHECK(clipped == 0, "the firmware wrote past the 0xA8-int accumulator "
                            "on %d slots\n", clipped);
        CHECK(iok == in, "Vocoder_InterpolateSpectralEnvelope exact on %d of %d\n",
              iok, in);
        printf("[Vocoder_InterpolateSpectralEnvelope: %d cases, %d bit-exact, "
               "%d writing nothing] ", in, iok, untouched);
    }

    {   /* Vocoder_SynthesizeVoiced itself, against the 617 calls the radio
           made over a real capture - the oracle this whole sequence was built
           for.  Everything above is a stage of this. */
        FILE *g = fixture_open("dm32_arc4_1.fwvoiced");
        int vn = 0, vacc = 0, vst = 0;

        while (getline(&line, &cap, g) > 0) {
            static int16_t st[N_VST], ref_st[N_VST], cur[68], prv[68], voi[N_VOI];
            static int32_t acc[80], ref_acc[80];
            long pitch;
            char *p = line;
            int k, bad;

            if (line[0] == '#')
                continue;
            pitch = strtol(p, &p, 10);
            for (k = 0; k < 68; k++)     cur[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 68; k++)     prv[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < N_VST; k++)  st[k]  = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < N_VOI; k++)  voi[k] = (int16_t)strtoul(p, &p, 10);
            for (k = 0; k < 80; k++)     acc[k] = (int32_t)strtol(p, &p, 10);
            for (k = 0; k < 80; k++) ref_acc[k] = (int32_t)strtol(p, &p, 10);
            for (k = 0; k < N_VST; k++) ref_st[k] = (int16_t)strtol(p, &p, 10);

            ambe_voiced_synth(acc, 0x50, st, cur, prv, voi, (int16_t)pitch);

            for (bad = 0, k = 0; k < 80; k++)
                if (acc[k] != ref_acc[k]) {
                    if (vn - vacc < 3)
                        CHECK(0, "call %d sample %d: %d, firmware %d\n",
                              vn, k, (int)acc[k], (int)ref_acc[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                vacc++;
            for (bad = 0, k = 0; k < N_VST; k++)
                if (st[k] != ref_st[k]) {
                    if (vn - vst < 3)
                        CHECK(0, "call %d state %#x: %d, firmware %d\n",
                              vn, k, (int)st[k], (int)ref_st[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                vst++;
            vn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(vn > 600, "only %d calls in the fixture\n", vn);
        {   /* and the two branches 617 frames of real speech never reach */
            FILE *h = fixture_open("voiced_octave.fw");
            int on = 0, oacc = 0, ost = 0;

            while (getline(&line, &cap, h) > 0) {
                static int16_t st[N_VST], ref_st[N_VST], cur[68], prv[68];
                static int16_t voi[N_VOI];
                static int32_t acc[80], ref_acc[80];
                long pitch;
                char *p = line;
                int k, bad;

                if (line[0] == '#')
                    continue;
                pitch = strtol(p, &p, 10);
                for (k = 0; k < 68; k++)     cur[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 68; k++)     prv[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < N_VST; k++)  st[k]  = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < N_VOI; k++)  voi[k] = (int16_t)strtoul(p, &p, 10);
                for (k = 0; k < 80; k++)     acc[k] = (int32_t)strtol(p, &p, 10);
                for (k = 0; k < 80; k++) ref_acc[k] = (int32_t)strtol(p, &p, 10);
                for (k = 0; k < N_VST; k++) ref_st[k] = (int16_t)strtol(p, &p, 10);

                ambe_voiced_synth(acc, 0x50, st, cur, prv, voi, (int16_t)pitch);
                for (bad = 0, k = 0; k < 80; k++)
                    if (acc[k] != ref_acc[k]) {
                        if (on - oacc < 3)
                            CHECK(0, "octave case %d sample %d: %d, firmware %d\n",
                                  on, k, (int)acc[k], (int)ref_acc[k]);
                        bad = 1;
                        break;
                    }
                if (!bad)
                    oacc++;
                for (bad = 0, k = 0; k < N_VST; k++)
                    if (st[k] != ref_st[k]) {
                        if (on - ost < 3)
                            CHECK(0, "octave case %d state %#x: %d, firmware %d\n",
                                  on, k, (int)st[k], (int)ref_st[k]);
                        bad = 1;
                        break;
                    }
                if (!bad)
                    ost++;
                on++;
            }
            free(line);
            line = NULL;
            cap = 0;
            fclose(h);
            CHECK(on > 200, "only %d constructed octave cases\n", on);
            CHECK(oacc == on, "the octave branches: accumulator exact on %d of %d\n",
                  oacc, on);
            CHECK(ost == on, "the octave branches: state exact on %d of %d\n",
                  ost, on);
            printf("[the octave-repair branches, which real speech never reaches: "
                   "%d constructed calls, %d/%d exact] ", on, oacc, ost);
        }
        CHECK(vacc == vn, "the accumulator is exact on %d of %d\n", vacc, vn);
        CHECK(vst == vn, "the channel state is exact on %d of %d\n", vst, vn);
        printf("[Vocoder_SynthesizeVoiced: %d firmware calls, accumulator %d, "
               "state %d] ", vn, vacc, vst);
    }

    CHECK(clamped > 0, "no harmonic reaches the clamp, where the gain would "
                       "exceed the sample count - the comparison there wraps\n");
    CHECK(ok == n, "Vocoder_ComputeHarmonicGains exact on %d of %d\n", ok, n);

    printf("[Vocoder_ComputeHarmonicGains: %d cases, %d bit-exact, up to %d "
           "harmonics, %d of them clamped] ", n, ok, maxL, clamped);
    return t_done("the voiced synthesiser's stages vs the firmware");
}
