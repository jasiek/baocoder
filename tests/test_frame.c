/*
 * test_frame.c - the frame-synthesis layer, against the firmware executed.
 *
 * Vocoder_SynthesizeFrame 0x00019DB8 is what stands between src/ambe_voiced.c
 * being exact and the decode path being able to use it: the parameter block the
 * synthesisers consume is one it rewrites on the way down, not the one the
 * decoder produces.  This grows a function at a time, leaves first, each swept
 * before the one above it is written.
 *
 *   Math_ArrayShiftCopy       0x0001AB58   here
 *   Math_PopCountBits         0x000189F4   here
 *   Vocoder_SmoothPitchState  0x00022D7C   here
 *   Math_Pow2Scaled           0x00019280   here
 *   Vocoder_NormalizeSpectralBlock 0x00022C18   here
 *   Vocoder_UpdatePitchHistoryBuffer 0x0001A9E8 here
 *   Dsp_HilbertTransform      0x00029D1C   here
 *   Dsp_NormalizeArray        0x0001ADA0   here
 *   Math_ArrayShiftSaturate   0x0001AF5C   here
 *   Vocoder_MatchExcitationEnergy 0x000277F8  here
 *   Vocoder_SynthesizeFrame   0x00019DB8   here, as a sequence
 *   Vocoder_ResetFrameBuffer  0x00019D38   here
 *   Vocoder_DetectFrameErasure 0x0001A4C8  here
 *   Vocoder_BumpErrorCounter  0x000220C0   here
 *   the tone branch of Vocoder_SynthesizeFrame, on constructed frames
 *   Tone_ClassifyCtcssDcsCode 0x0001A434   here
 *   Tone_CtcssDcsCodeToTableIndex 0x0001A478 here
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>

#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_frame_int.h"
#include "testutil.h"

int main(void)
{
    FILE *f = fixture_open("frame_popcount.fw");
    char *line = NULL;
    size_t cap = 0;
    int n = 0, ok = 0, partial = 0;

    while (getline(&line, &cap, f) > 0) {
        unsigned long v;
        long nb, r;
        char *p = line;
        int got;

        if (line[0] == '#')
            continue;
        v  = strtoul(p, &p, 10);
        nb = strtol(p, &p, 10);
        r  = strtol(p, &p, 10);

        got = ambe_popcount_bits((uint32_t)v, (int)nb);
        n++;
        if (got == (int)r)
            ok++;
        else
            CHECK(0, "popcount(%#lx, %ld) = %d, firmware %ld\n", v, nb, got, r);
        /* a case whose answer differs from counting all 32 bits is the one
           that says the bit count is honoured rather than ignored */
        if (nb < 32 && (v >> nb) != 0)
            partial++;
    }
    free(line);
    line = NULL;
    cap = 0;
    fclose(f);

    {   /* Vocoder_SmoothPitchState, whose gate is a count of voiced bands -
           so the cases have to straddle eight of them, or the sweep cannot
           tell the smoother from the identity */
        FILE *g = fixture_open("frame_smooth.fw");
        int sn = 0, sok = 0, ran = 0, held = 0;

        while (getline(&line, &cap, g) > 0) {
            unsigned long st, tg, w, r;
            char *p = line;
            uint32_t got;

            if (line[0] == '#')
                continue;
            st = strtoul(p, &p, 10); tg = strtoul(p, &p, 10);
            w  = strtoul(p, &p, 10); r  = strtoul(p, &p, 10);

            got = ambe_smooth_pitch_state((uint32_t)st, (uint16_t)tg, (uint32_t)w);
            sn++;
            if (got == (uint32_t)r)
                sok++;
            else
                CHECK(0, "smooth(%#lx, %#lx, %#lx) = %u, firmware %lu\n",
                      st, tg, w, (unsigned)got, r);
            if (r == (st & 0xFFFF))
                held++;
            else
                ran++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(sn > 600, "only %d smoother cases\n", sn);
        CHECK(ran > 0 && held > 0, "the fixture is one-sided: %d smoothed, %d "
              "held - the eight-band gate is not being crossed\n", ran, held);
        CHECK(sok == sn, "Vocoder_SmoothPitchState exact on %d of %d\n", sok, sn);
        printf("[Vocoder_SmoothPitchState %d/%d bit-exact, %d smoothed and %d "
               "held at the eight-band gate] ", sok, sn, ran, held);
    }

    {   /* Math_Pow2Scaled - ambe_pow2's chain with a Q-format tail, and a
           second copy of the polynomial in the stock code rather than a
           wrapper, so it is swept on its own */
        FILE *g = fixture_open("frame_pow2scaled.fw");
        int pn = 0, pok = 0;

        while (getline(&line, &cap, g) > 0) {
            long m, e, q;
            unsigned long r;
            char *p = line;
            uint32_t got;

            if (line[0] == '#')
                continue;
            m = strtol(p, &p, 10); e = strtol(p, &p, 10);
            q = strtol(p, &p, 10); r = strtoul(p, &p, 10);
            got = ambe_pow2_scaled((int32_t)m, (int16_t)e, (int16_t)q);
            pn++;
            if (got == (uint32_t)r)
                pok++;
            else
                CHECK(0, "pow2_scaled(%ld,%ld,%ld) = %u, firmware %lu\n",
                      m, e, q, (unsigned)got, r);
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(pn > 600, "only %d scaled powers\n", pn);
        CHECK(pok == pn, "Math_Pow2Scaled exact on %d of %d\n", pok, pn);
        printf("[Math_Pow2Scaled %d/%d bit-exact] ", pok, pn);
    }

    {   /* Vocoder_NormalizeSpectralBlock: clamp, peak, exponentiate.  The
           input is the 56-short array it rewrites in place, so the fixture
           carries it on both sides. */
        FILE *g = fixture_open("frame_normblock.fw");
        int bn = 0, bok = 0, clipped = 0, empty = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t in[56], ref[56], got[56];
            long count, refe;
            int16_t e = 0x7BAD;
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            for (k = 0; k < 56; k++) in[k]  = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 56; k++) ref[k] = (int16_t)strtol(p, &p, 10);
            refe = strtol(p, &p, 10);

            memcpy(got, in, sizeof(got));
            ambe_normalize_spectral_block(got, &e, (int)count);
            if (e != (int16_t)refe) {
                CHECK(0, "normblock case %d (count %ld): exponent %d, firmware %ld\n",
                      bn, count, (int)e, refe);
                bad = 1;
            }
            for (k = 0; k < 56 && !bad; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "normblock case %d (count %ld) slot %d: %d, "
                             "firmware %d\n", bn, count, k, (int)got[k],
                          (int)ref[k]);
                    bad = 1;
                }
            if (!bad)
                bok++;
            for (k = 0; k < 56; k++)
                if (in[k] > 0x77ff || in[k] < -0x77ff)
                    clipped++;
            if (count < 1)
                empty++;
            bn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(bn > 200, "only %d normalise cases\n", bn);
        CHECK(clipped > 0, "no case has a coefficient past the 0x77FF clamp, "
                           "which is where 0x7FFF would look identical\n");
        CHECK(empty > 0, "no case has count < 1, the fixed -0x1E exponent path\n");
        CHECK(bok == bn, "Vocoder_NormalizeSpectralBlock exact on %d of %d\n",
              bok, bn);
        printf("[Vocoder_NormalizeSpectralBlock %d/%d bit-exact, %d values past "
               "the clamp] ", bok, bn, clipped);
    }

    {   /* Vocoder_UpdatePitchHistoryBuffer.  The block's [0x40..0x41] is a
           pointer to the flags in the running firmware, so the fixture's copy
           holds the emulator's address; this library takes the flags as an
           argument and the two columns are compared separately. */
        FILE *g = fixture_open("frame_pitchhist.fw");
        int hn = 0, hok = 0, moved = 0, clamped9 = 0;

        while (getline(&line, &cap, g) > 0) {
            static int16_t b[68], ref[68];
            static uint16_t fl[56], reff[56];
            long cand;
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            cand = strtol(p, &p, 10);
            for (k = 0; k < 68; k++) b[k]    = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 56; k++) fl[k]   = (uint16_t)strtoul(p, &p, 10);
            for (k = 0; k < 68; k++) ref[k]  = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 56; k++) reff[k] = (uint16_t)strtoul(p, &p, 10);

            if (b[3] != (int16_t)cand)
                moved++;
            if (ref[2] == 9)
                clamped9++;

            ambe_update_pitch_history(b, fl, (int16_t)cand);

            for (k = 0; k < 68; k++) {
                if (k == 0x40 || k == 0x41)
                    continue;       /* the firmware's own flags address */
                if (b[k] != ref[k]) {
                    CHECK(0, "pitchhist case %d (cand %ld) slot %d: %d, "
                             "firmware %d\n", hn, cand, k, (int)b[k],
                          (int)ref[k]);
                    bad = 1;
                    break;
                }
            }
            for (k = 0; k < 56 && !bad; k++)
                if (fl[k] != reff[k]) {
                    CHECK(0, "pitchhist case %d (cand %ld) flag %d: %u, "
                             "firmware %u\n", hn, cand, k, (unsigned)fl[k],
                          (unsigned)reff[k]);
                    bad = 1;
                }
            if (!bad)
                hok++;
            hn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(hn > 600, "only %d pitch-history cases\n", hn);
        CHECK(moved > 0, "no case moves the candidate to a different index\n");
        CHECK(clamped9 > 0, "no case lands on the nine-harmonic floor\n");
        CHECK(hok == hn, "Vocoder_UpdatePitchHistoryBuffer exact on %d of %d\n",
              hok, hn);
        printf("[Vocoder_UpdatePitchHistoryBuffer %d/%d bit-exact, %d moving the "
               "candidate] ", hok, hn, moved);
    }

    {   /* Dsp_HilbertTransform, whose answer is mostly decided by the two
           extensions it builds either side of the input */
        FILE *g = fixture_open("frame_hilbert.fw");
        int tn = 0, tok = 0, longblk = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t src[56], ref[56], got[56];
            long count;
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            for (k = 0; k < 56; k++) src[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 56; k++) ref[k] = (int16_t)strtol(p, &p, 10);

            for (k = 0; k < 56; k++)
                got[k] = (int16_t)0xEEEE;
            ambe_hilbert_transform(got, src, (int)count);
            for (k = 0; k < 56; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "hilbert case %d (count %ld) slot %d: %d, "
                             "firmware %d\n", tn, count, k, (int)got[k],
                          (int)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                tok++;
            if (count > 0x2d)
                longblk++;
            tn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(tn > 300, "only %d Hilbert cases\n", tn);
        CHECK(longblk > 0, "no case has count > 0x2D, which is the only way to "
                           "reach the reflection past index 0x40\n");
        CHECK(tok == tn, "Dsp_HilbertTransform exact on %d of %d\n", tok, tn);
        printf("[Dsp_HilbertTransform %d/%d bit-exact, %d long enough to reach "
               "past 0x40] ", tok, tn, longblk);
    }

    {   /* Math_ArrayShiftCopy.  The destination was poked 0xEEEE, so a slot
           the firmware left alone reads as -4370 rather than as a chosen zero,
           which is what catches a transcription copying too many. */
        FILE *g = fixture_open("frame_shiftcopy.fw");
        int cn = 0, cok = 0, lost = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t src[32], ref[32], got[32];
            long count, shift;
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            shift = strtol(p, &p, 10);
            for (k = 0; k < 32; k++) src[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 32; k++) ref[k] = (int16_t)strtol(p, &p, 10);

            for (k = 0; k < 32; k++)
                got[k] = (int16_t)0xEEEE;
            ambe_array_shift_copy(got, src, (int)count, (int)shift);
            for (k = 0; k < 32; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "shiftcopy case %d (count %ld shift %ld) slot %d: "
                             "%d, firmware %d\n", cn, count, shift, k,
                          (int)got[k], (int)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                cok++;
            /* a left shift that pushes bits out of the short: the case where
               truncating and saturating differ */
            if (shift > 0)
                for (k = 0; k < count; k++)
                    if ((int32_t)src[k] << shift != (int32_t)(int16_t)
                        ((uint32_t)src[k] << shift))
                        lost++;
            cn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(cn > 200, "only %d shift-copy cases\n", cn);
        CHECK(lost > 0, "no left shift pushes bits out of the short, where "
                        "truncating and saturating would look the same\n");
        CHECK(cok == cn, "Math_ArrayShiftCopy exact on %d of %d\n", cok, cn);
        printf("[Math_ArrayShiftCopy %d/%d bit-exact, %d values shifted past "
               "the top] ", cok, cn, lost);
    }

    {   /* Dsp_NormalizeArray, whose two special cases are the interesting
           ones: an all-zero block leaves the caller's exponent alone, and a
           block whose only extreme is -0x8000 exposes the true negation */
        FILE *g = fixture_open("frame_normarray.fw");
        int an = 0, aok = 0, zero = 0, extreme = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t src[32], ref[32], got[32];
            long count, ein, refe;
            int16_t e;
            char *p = line;
            int k, bad = 0, allzero = 1;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            ein   = strtol(p, &p, 10);
            for (k = 0; k < 32; k++) src[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 32; k++) ref[k] = (int16_t)strtol(p, &p, 10);
            refe = strtol(p, &p, 10);

            for (k = 0; k < 32; k++)
                got[k] = (int16_t)0xEEEE;
            e = (int16_t)ein;
            ambe_normalize_array(got, src, (int)count, &e);
            if (e != (int16_t)refe) {
                CHECK(0, "normarray case %d (count %ld exp %ld): exponent %d, "
                         "firmware %ld\n", an, count, ein, (int)e, refe);
                bad = 1;
            }
            for (k = 0; k < 32 && !bad; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "normarray case %d (count %ld) slot %d: %d, "
                             "firmware %d\n", an, count, k, (int)got[k],
                          (int)ref[k]);
                    bad = 1;
                }
            if (!bad)
                aok++;
            for (k = 0; k < count; k++) {
                if (src[k] != 0)
                    allzero = 0;
                if (src[k] == -0x8000)
                    extreme++;
            }
            if (count > 0 && allzero)
                zero++;
            an++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(an > 200, "only %d normalise-array cases\n", an);
        CHECK(zero > 0, "no all-zero block, where the exponent is left alone\n");
        CHECK(extreme > 0, "no -0x8000, where a true negation and a saturating "
                           "one part company\n");
        CHECK(aok == an, "Dsp_NormalizeArray exact on %d of %d\n", aok, an);
        printf("[Dsp_NormalizeArray %d/%d bit-exact, %d all-zero blocks and %d "
               "values at -0x8000] ", aok, an, zero, extreme);
    }

    {   /* Math_ArrayShiftSaturate: saturation is per element, against that
           element's own headroom, so a block with one loud sample among quiet
           ones separates this from a whole-array renormalisation */
        FILE *g = fixture_open("frame_shiftsat.fw");
        int qn = 0, qok = 0, sat = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t src[32], ref[32], got[32];
            long count, de, se;
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            de    = strtol(p, &p, 10);
            se    = strtol(p, &p, 10);
            for (k = 0; k < 32; k++) src[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 32; k++) ref[k] = (int16_t)strtol(p, &p, 10);

            for (k = 0; k < 32; k++)
                got[k] = (int16_t)0xEEEE;
            ambe_array_shift_saturate(got, src, (int)count, (int16_t)de,
                                      (int16_t)se);
            for (k = 0; k < 32; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "shiftsat case %d (count %ld %ld<-%ld) slot %d: "
                             "%d, firmware %d\n", qn, count, de, se, k,
                          (int)got[k], (int)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                qok++;
            for (k = 0; k < count; k++)
                if ((ref[k] == 0x7fff || ref[k] == -0x8000) && src[k] != ref[k])
                    sat++;
            qn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(qn > 200, "only %d shift-saturate cases\n", qn);
        CHECK(sat > 0, "nothing saturates, so the headroom check is untested\n");
        CHECK(qok == qn, "Math_ArrayShiftSaturate exact on %d of %d\n", qok, qn);
        printf("[Math_ArrayShiftSaturate %d/%d bit-exact, %d values saturated] ",
               qok, qn, sat);
    }

    {   /* Vocoder_MatchExcitationEnergy: the spectral amplitude enhancement.
           Everything it does is in place - the amplitudes, the block exponent
           and the caller's reference pair - so all three are checked, and the
           reference pair matters most: it is the only state the function
           keeps, and nothing inside it reads what it writes there, so a
           transcription could get it wrong and still sound right. */
        FILE *g = fixture_open("frame_matchenergy.fw");
        int mn = 0, mok = 0, silent = 0, both = 0, moved = 0, built = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t src[0x38], ref[0x38], got[0x38];
            long count, pitch, ein, rmi, rei, eo, rmo, reo;
            int16_t e, rm, re;
            char *p = line;
            int k, bad = 0, allzero = 1;

            if (line[0] == '#')
                continue;
            count = strtol(p, &p, 10);
            pitch = strtol(p, &p, 10);
            ein   = strtol(p, &p, 10);
            rmi   = strtol(p, &p, 10);
            rei   = strtol(p, &p, 10);
            for (k = 0; k < 0x38; k++) src[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 0x38; k++) ref[k] = (int16_t)strtol(p, &p, 10);
            eo  = strtol(p, &p, 10);
            rmo = strtol(p, &p, 10);
            reo = strtol(p, &p, 10);

            memcpy(got, src, sizeof got);
            e  = (int16_t)ein;
            rm = (int16_t)rmi;
            re = (int16_t)rei;
            ambe_match_excitation_energy(got, &e, &rm, &re, (int16_t)pitch,
                                         (int16_t)count);
            if (e != (int16_t)eo) {
                CHECK(0, "matchenergy case %d (count %ld pitch %ld): block "
                         "exponent %d, firmware %ld\n", mn, count, pitch,
                      (int)e, eo);
                bad = 1;
            }
            if (!bad && (rm != (int16_t)rmo || re != (int16_t)reo)) {
                CHECK(0, "matchenergy case %d (count %ld pitch %ld): reference "
                         "%d@%d, firmware %ld@%ld\n", mn, count, pitch,
                      (int)rm, (int)re, rmo, reo);
                bad = 1;
            }
            for (k = 0; k < 0x38 && !bad; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "matchenergy case %d (count %ld pitch %ld) "
                             "harmonic %d: %d, firmware %d\n", mn, count,
                          pitch, k, (int)got[k], (int)ref[k]);
                    bad = 1;
                }
            if (!bad)
                mok++;
            for (k = 0; k < count; k++) {
                if (src[k] != 0)
                    allzero = 0;
                if (ref[k] != src[k])
                    moved = 1;
            }
            if (allzero)
                silent++;              /* R0 is zero: the early return */
            if (count > 8)
                both++;                /* a halved low band AND an enhanced one */
            {   /* the constructed cases: every harmonic at full scale but one,
                   which is what drives R1's mantissa onto 0x8000 exactly */
                int other = 0;

                for (k = 0; k < count; k++)
                    if (src[k] != 0x7fff)
                        other++;
                if (count >= 16 && other == 1)
                    built++;
            }
            mn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(mn > 350, "only %d excitation-match cases\n", mn);
        CHECK(silent > 0, "no silent block, so the early return on a zero R0 "
                          "is untested\n");
        CHECK(both > 20, "only %d cases with a low band and an enhanced band "
                         "both\n", both);
        CHECK(moved, "no case changed an amplitude at all\n");
        CHECK(built >= 10, "only %d constructed cases, so the one saturation "
                           "in the function - R1's mantissa at 0x8000 - is "
                           "untested; a random sweep does not reach it\n",
              built);
        CHECK(mok == mn, "Vocoder_MatchExcitationEnergy exact on %d of %d\n",
              mok, mn);
        printf("[Vocoder_MatchExcitationEnergy %d/%d bit-exact, amplitudes, "
               "block exponent and reference pair, %d silent and %d "
               "constructed for the R1 saturation] ", mok, mn, silent, built);
    }

    {   /* Vocoder_SynthesizeFrame, the whole layer, as a SEQUENCE.

           Nothing here can be checked one call at a time: every number the
           function produces depends on channel state it also writes.  So the
           fixture hands over the state once, at the first call, and then only
           the inputs; this runs the 617 calls in order carrying its own state
           and compares the samples, the block the call left at ctx+0x470 and
           the three scalars it maintains.  A wrong bit anywhere - in this
           layer or in any of the seven exact functions under it - diverges and
           never recovers, which is what makes 49 360 consecutive samples a
           stronger statement than any single call could be. */
        FILE *g = fixture_open("dm32_arc4_1.fwframe");
        static ambe_frame_state fs;
        static int16_t blk[68], ref_pcm[80], ref_prev[68], pcm[80];
        int fn = 0, fok = 0, sok = 0, bok = 0, xok = 0, voice = 0, silence = 0;
        int primed = 0;

        ambe_frame_state_reset(&fs);
        while (getline(&line, &cap, g) > 0) {
            char *p = line;
            int k, bad;

            if (line[0] == '#')
                continue;
            if (line[0] == 'S') {
                p++;
                for (k = 0; k < 6; k++)
                    fs.post.s[k] = (int32_t)strtol(p, &p, 10);
                for (k = 0; k < AMBE_VOICED_STATE; k++)
                    fs.voiced[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 68; k++)
                    fs.prev[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 68; k++)
                    fs.silence[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < AMBE_UNVOICED_STATE; k++)
                    fs.unvoiced.s[k] = (int16_t)strtol(p, &p, 10);
                fs.pitch    = (int16_t)strtol(p, &p, 10);
                fs.ref_mant = (int16_t)strtol(p, &p, 10);
                fs.ref_exp  = (int16_t)strtol(p, &p, 10);
                primed = 1;
                continue;
            }
            if (line[0] != 'C')
                continue;
            CHECK(primed, "a call before the state line\n");
            p++;
            (void)strtol(p, &p, 10);                    /* src */
            {
                long repeat = strtol(p, &p, 10);
                long ns     = strtol(p, &p, 10);

                fs.tone_mode = (int16_t)strtol(p, &p, 10);
                fs.bypass    = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 68; k++)      blk[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 80; k++)  ref_pcm[k] = (int16_t)strtol(p, &p, 10);
                for (k = 0; k < 68; k++) ref_prev[k] = (int16_t)strtol(p, &p, 10);
                {
                    int16_t rp = (int16_t)strtol(p, &p, 10);
                    int16_t rm = (int16_t)strtol(p, &p, 10);
                    int16_t re = (int16_t)strtol(p, &p, 10);

                    if (blk[0] == 1) voice++;
                    if (blk[0] == 2) silence++;
                    ambe_frame_synthesize(blk, pcm, (int)ns, (int)repeat, &fs);

                    for (bad = 0, k = 0; k < 80; k++)
                        if (pcm[k] != ref_pcm[k]) {
                            if (fn - sok < 3)
                                CHECK(0, "frame call %d sample %d: %d, firmware "
                                         "%d\n", fn, k, (int)pcm[k],
                                      (int)ref_pcm[k]);
                            bad = 1;
                            break;
                        }
                    if (!bad) sok++;
                    for (bad = 0, k = 0; k < 68; k++)
                        if (fs.prev[k] != ref_prev[k]) {
                            if (fn - bok < 3)
                                CHECK(0, "frame call %d block[%d] left behind: "
                                         "%d, firmware %d\n", fn, k,
                                      (int)fs.prev[k], (int)ref_prev[k]);
                            bad = 1;
                            break;
                        }
                    if (!bad) bok++;
                    if (fs.pitch == rp && fs.ref_mant == rm && fs.ref_exp == re)
                        xok++;
                    else if (fn - xok < 3)
                        CHECK(0, "frame call %d state: pitch %d ref %d@%d, "
                                 "firmware %d %d@%d\n", fn, (int)fs.pitch,
                              (int)fs.ref_mant, (int)fs.ref_exp,
                              (int)rp, (int)rm, (int)re);
                    fn++;
                }
            }
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        fok = sok < bok ? sok : bok;
        if (xok < fok) fok = xok;
        CHECK(fn > 600, "only %d frame-layer calls\n", fn);
        CHECK(voice > 100 && silence > 10,
              "%d voice and %d silence frames: the two classes take different "
              "paths through the preprocessing\n", voice, silence);
        CHECK(sok == fn, "Vocoder_SynthesizeFrame samples exact on %d of %d\n",
              sok, fn);
        CHECK(bok == fn, "Vocoder_SynthesizeFrame block exact on %d of %d\n",
              bok, fn);
        CHECK(xok == fn, "Vocoder_SynthesizeFrame state exact on %d of %d\n",
              xok, fn);
        printf("[Vocoder_SynthesizeFrame %d/%d calls carrying its own state, "
               "%d samples, block and state; %d voice %d silence] ",
               fok, fn, fn * 80, voice, silence);
    }

    {   /* the two tone classifiers, which were holes in this file until they
           were read and turned out to be 66 and 74 bytes.  Both are pure, so
           they sweep: every code around all four of the classifier's range
           boundaries, and for the index function the whole CTCSS closed form
           plus every DCS code the classifier can hand it. */
        FILE *g = fixture_open("frame_toneclass.fw");
        int cn = 0, cok = 0, bn = 0, bok = 0, dcs = 0, satur = 0;
        int seen[5];
        int fam;

        for (fam = 0; fam < 5; fam++)
            seen[fam] = 0;
        while (getline(&line, &cap, g) > 0) {
            char *p = line;

            if (line[0] == '#')
                continue;
            if (line[0] == 'C') {
                long code, ref;
                int got;

                p++;
                code = strtol(p, &p, 10);
                ref  = strtol(p, &p, 10);
                got  = ambe_tone_class((int16_t)code);
                cn++;
                if (got == (int)ref)
                    cok++;
                else
                    CHECK(0, "tone class(%ld) = %d, firmware %ld\n", code, got,
                          ref);
                if (ref >= 0 && ref < 5)
                    seen[ref]++;
            } else if (line[0] == 'B') {
                long cls, code, flag, ref;
                int16_t got;

                p++;
                cls  = strtol(p, &p, 10);
                code = strtol(p, &p, 10);
                flag = strtol(p, &p, 10);
                ref  = strtol(p, &p, 10);
                got  = ambe_tone_bin((int)cls, (uint16_t)code, (int16_t)flag);
                bn++;
                if (got == (int16_t)ref)
                    bok++;
                else
                    CHECK(0, "tone bin(%ld, %ld, %ld) = %d, firmware %ld\n",
                          cls, code, flag, (int)got, ref);
                if (cls >= 1 && cls < 4)
                    dcs++;
                if (cls == 0 && ref == 0)
                    satur++;
            }
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        for (fam = 0; fam < 5; fam++)
            CHECK(seen[fam] > 0, "no code classified %d, so that range is "
                                 "untested\n", fam);
        CHECK(dcs > 100, "only %d DCS lookups, and the table is 72 entries\n",
              dcs);
        CHECK(satur > 0, "nothing hit the floor at zero, where the CTCSS form "
                         "would otherwise return -1\n");
        CHECK(cok == cn, "Tone_ClassifyCtcssDcsCode exact on %d of %d\n", cok,
              cn);
        CHECK(bok == bn, "Tone_CtcssDcsCodeToTableIndex exact on %d of %d\n",
              bok, bn);
        printf("[the tone classifiers %d + %d cases bit-exact, all five "
               "families and %d DCS lookups] ", cok, bok, dcs);
    }

    {   /* Vocoder_ResetFrameBuffer: the voicing flags, per frame class.  The
           tone branch is the point - the corpus has no tone frame at all, so a
           sweep is the only thing that reaches it, and the destination is
           poked 0xEEEE so a class the function leaves alone is visible as
           having been left alone rather than as a zero it chose. */
        FILE *g = fixture_open("frame_resetbuf.fw");
        int rn = 0, rok = 0, tone = 0, pair = 0, untouched = 0;

        while (getline(&line, &cap, g) > 0) {
            int16_t blk[68];
            uint16_t ref[0x38], got[0x38];
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            for (k = 0; k < 68; k++)   blk[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 0x38; k++) ref[k] = (uint16_t)strtoul(p, &p, 10);

            for (k = 0; k < 0x38; k++)
                got[k] = 0xEEEE;
            ambe_frame_reset_buffer(blk, got);
            for (k = 0; k < 0x38; k++)
                if (got[k] != ref[k]) {
                    CHECK(0, "resetbuf case %d (class %d code %d) flag %d: %u, "
                             "firmware %u\n", rn, (int)blk[0], (int)blk[1], k,
                          (unsigned)got[k], (unsigned)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                rok++;
            if (blk[0] == 3) {
                int set = 0;

                for (k = 0; k < 0x38; k++)
                    if (ref[k] == 1)
                        set++;
                tone++;
                if (set == 2)
                    pair++;          /* a DCS pair, two bins rather than one */
            }
            if (ref[0] == 0xEEEE)
                untouched++;
            rn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(rn > 200, "only %d reset-buffer cases\n", rn);
        CHECK(tone > 50, "only %d tone frames, and they are the branch nothing "
                         "else reaches\n", tone);
        CHECK(pair > 10, "only %d DCS pairs, so the two-bin path is untested\n",
              pair);
        CHECK(untouched > 0, "no case left the array alone, so the classes the "
                             "function does not handle are untested\n");
        CHECK(rok == rn, "Vocoder_ResetFrameBuffer exact on %d of %d\n", rok,
              rn);
        printf("[Vocoder_ResetFrameBuffer %d/%d bit-exact, %d tone frames of "
               "which %d DCS pairs, %d left untouched] ", rok, rn, tone, pair,
               untouched);
    }

    {   /* the tone branch of Vocoder_SynthesizeFrame, on frames built for it.
           pFrameParams[1] is 0xFF on all 617 calls of the capture - every
           frame of the corpus is speech or silence - so this branch has no
           natural coverage at all, and the fixture is 616 constructed calls
           caught either side of it inside the running function. */
        FILE *g = fixture_open("frame_tone.fw");
        static int16_t prv[68], pre[68], ref[68], got[68];
        uint16_t flg[0x38];
        int tn = 0, tok = 0, cont = 0, shape = 0, notch = 0, merged = 0;

        while (getline(&line, &cap, g) > 0) {
            char *p = line;
            long mode;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            mode = strtol(p, &p, 10);
            for (k = 0; k < 68; k++) prv[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 68; k++) pre[k] = (int16_t)strtol(p, &p, 10);
            for (k = 0; k < 68; k++) ref[k] = (int16_t)strtol(p, &p, 10);

            memcpy(got, pre, sizeof got);
            for (k = 0; k < 0x38; k++)
                flg[k] = 0;
            ambe_frame_tone_rewrite(got, prv, (int16_t)mode, flg);
            for (k = 0; k < 68; k++)
                if (got[k] != ref[k]) {
                    if (tn - tok < 3)
                        CHECK(0, "tone case %d (mode %ld code %d prev %d/%d) "
                                 "field %d: %d, firmware %d\n", tn, mode,
                              (int)pre[1], (int)prv[0], (int)prv[1], k,
                              (int)got[k], (int)ref[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                tok++;
            if (mode == 1)
                cont++;
            else
                shape++;
            for (k = 0; k < 0x38; k++)
                if (ref[8 + k] != pre[8 + k])
                    notch++;
            if (mode != 1 && (uint16_t)(pre[1] - 0x80) < 0x20
                && ref[8 + 0x05] == ref[8 + 0x08] && ref[8 + 0x05] != 0)
                merged++;
            tn++;
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(tn > 500, "only %d constructed tone frames\n", tn);
        CHECK(cont > 100 && shape > 100,
              "%d continuation and %d shaping cases: they are two different "
              "branches and both need reaching\n", cont, shape);
        CHECK(notch > 1000, "only %d amplitudes moved, so the notch is barely "
                            "exercised\n", notch);
        CHECK(merged > 10, "only %d DCS pairs merged to their common root\n",
              merged);
        CHECK(tok == tn, "the tone branch exact on %d of %d\n", tok, tn);
        printf("[the tone branch %d/%d constructed frames bit-exact, %d "
               "continuation and %d shaping, %d amplitudes notched] ",
               tok, tn, cont, shape, notch);
    }

    {   /* Vocoder_DetectFrameErasure, which is a sync-pattern detector, and
           the counter that goes with it.  The pattern is seventeen frames of
           tone codes and the window it opens is 32 frames of a bit payload, so
           what has to be swept is the match at exactly its threshold - one
           mismatch accepted, two rejected - and every one of the 32 frames,
           because each plays a different bit. */
        FILE *g = fixture_open("frame_erasure.fw");
        int en = 0, eok = 0, bn = 0, bok = 0, opened = 0, subbed = 0, cleared = 0;

        while (getline(&line, &cap, g) > 0) {
            char *p = line;
            int k, bad = 0;

            if (line[0] == '#')
                continue;
            if (line[0] == 'E') {
                int16_t st[AMBE_ERASURE_STATE], ref[AMBE_ERASURE_STATE];
                long pit, slot, rr, rp;
                int16_t pitch;
                int got;

                p++;
                pit  = strtol(p, &p, 10);
                slot = strtol(p, &p, 10);
                for (k = 0; k < AMBE_ERASURE_STATE; k++)
                    st[k] = (int16_t)strtol(p, &p, 10);
                rr = strtol(p, &p, 10);
                rp = strtol(p, &p, 10);
                for (k = 0; k < AMBE_ERASURE_STATE; k++)
                    ref[k] = (int16_t)strtol(p, &p, 10);

                pitch = (int16_t)pit;
                got = ambe_detect_frame_erasure(&pitch, st, (int16_t)slot);
                if (got != (int)rr || pitch != (int16_t)rp) {
                    CHECK(0, "erasure case %d (pitch %ld slot %ld): returned %d "
                             "pitch %d, firmware %ld %ld\n", en, pit, slot, got,
                          (int)pitch, rr, rp);
                    bad = 1;
                }
                for (k = 0; k < AMBE_ERASURE_STATE && !bad; k++)
                    if (st[k] != ref[k]) {
                        CHECK(0, "erasure case %d (pitch %ld slot %ld) state[%d]"
                                 ": %d, firmware %d\n", en, pit, slot, k,
                              (int)st[k], (int)ref[k]);
                        bad = 1;
                    }
                if (!bad)
                    eok++;
                if (ref[0] == 0x20)
                    opened++;             /* the pattern matched */
                if (rr == 1)
                    subbed++;             /* a code came out of the payload */
                if (pit == 0xff && ref[0] == 0)
                    cleared++;
                en++;
            } else if (line[0] == 'B') {
                long in, ref;
                int16_t c;

                p++;
                in  = strtol(p, &p, 10);
                ref = strtol(p, &p, 10);
                c   = (int16_t)in;
                ambe_bump_error_counter(&c);
                bn++;
                if (c == (int16_t)ref)
                    bok++;
                else
                    CHECK(0, "bump(%ld) = %d, firmware %ld\n", in, (int)c, ref);
            }
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(en > 400, "only %d erasure cases\n", en);
        CHECK(opened > 3, "only %d cases matched the sync pattern, so the "
                          "threshold is untested\n", opened);
        CHECK(subbed > 100, "only %d substitutions, and the payload is 32 bits "
                            "that each need playing\n", subbed);
        CHECK(cleared > 0, "nothing carried the 0xFF every speech frame does\n");
        CHECK(eok == en, "Vocoder_DetectFrameErasure exact on %d of %d\n", eok,
              en);
        CHECK(bok == bn, "Vocoder_BumpErrorCounter exact on %d of %d\n", bok,
              bn);
        printf("[Vocoder_DetectFrameErasure %d/%d bit-exact, %d sync matches "
               "and %d payload substitutions; the counter %d/%d] ",
               eok, en, opened, subbed, bok, bn);
    }

    CHECK(n > 400, "only %d popcount cases\n", n);
    CHECK(partial > 0, "no case has bits above the count, so the fixture cannot "
                       "tell a partial count from a whole one\n");
    CHECK(ok == n, "Math_PopCountBits exact on %d of %d\n", ok, n);

    printf("[Math_PopCountBits %d/%d bit-exact, %d of them with bits above the "
           "count] ", ok, n, partial);
    return t_done("the frame-synthesis layer vs the firmware");
}
