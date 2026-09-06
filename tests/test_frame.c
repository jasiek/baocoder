/*
 * test_frame.c - the frame-synthesis layer, against the firmware executed.
 *
 * Vocoder_SynthesizeFrame 0x00019DB8 is what stands between src/ambe_voiced.c
 * being exact and the decode path being able to use it: the parameter block the
 * synthesisers consume is one it rewrites on the way down, not the one the
 * decoder produces.  This grows a function at a time, leaves first, each swept
 * before the one above it is written.
 *
 *   Math_PopCountBits         0x000189F4   here
 *   Vocoder_SmoothPitchState  0x00022D7C   here
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
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

    CHECK(n > 400, "only %d popcount cases\n", n);
    CHECK(partial > 0, "no case has bits above the count, so the fixture cannot "
                       "tell a partial count from a whole one\n");
    CHECK(ok == n, "Math_PopCountBits exact on %d of %d\n", ok, n);

    printf("[Math_PopCountBits %d/%d bit-exact, %d of them with bits above the "
           "count] ", ok, n, partial);
    return t_done("the frame-synthesis layer vs the firmware");
}
