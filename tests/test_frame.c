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
 *
 * SPDX-License-Identifier: ISC
 */
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

    CHECK(n > 400, "only %d popcount cases\n", n);
    CHECK(partial > 0, "no case has bits above the count, so the fixture cannot "
                       "tell a partial count from a whole one\n");
    CHECK(ok == n, "Math_PopCountBits exact on %d of %d\n", ok, n);

    printf("[Math_PopCountBits %d/%d bit-exact, %d of them with bits above the "
           "count] ", ok, n, partial);
    return t_done("the frame-synthesis layer vs the firmware");
}
