/*
 * test_frame.c - the frame-synthesis layer, against the firmware executed.
 *
 * Vocoder_SynthesizeFrame 0x00019DB8 is what stands between src/ambe_voiced.c
 * being exact and the decode path being able to use it: the parameter block the
 * synthesisers consume is one it rewrites on the way down, not the one the
 * decoder produces.  This grows a function at a time, leaves first, each swept
 * before the one above it is written.
 *
 *   Math_PopCountBits 0x000189F4   here
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

    CHECK(n > 400, "only %d popcount cases\n", n);
    CHECK(partial > 0, "no case has bits above the count, so the fixture cannot "
                       "tell a partial count from a whole one\n");
    CHECK(ok == n, "Math_PopCountBits exact on %d of %d\n", ok, n);

    printf("[Math_PopCountBits %d/%d bit-exact, %d of them with bits above the "
           "count] ", ok, n, partial);
    return t_done("the frame-synthesis layer vs the firmware");
}
