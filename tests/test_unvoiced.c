/*
 * test_unvoiced.c - the unvoiced excitation's noise source, against the firmware.
 *
 * The fixture is every Vocoder_SynthesizeUnvoiced 0x0001AFE0 call the radio
 * made over a real capture, with the state peeked either side, so this is
 * exactness rather than tolerance.
 *
 * Two things are checked, and the second is the one that matters:
 *
 *   given the firmware's own state, does one frame's update land where the
 *   firmware's did; and carrying its OWN state across all 103 calls, does it
 *   stay there - which is what a stream decoder actually does, and what fails
 *   if the generator drifts by a single step.
 *
 * Only the first 0x55 shorts are compared.  The rest of the array is the
 * previous segment's overlap-add tail and its exponent, which belong to the
 * parts of the synthesiser not yet transcribed; asserting on them would be
 * asserting on something this file does not claim to produce.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "testutil.h"

#define NS      80
#define BLK     68
#define STLEN   170
#define CHECKED 0x55

int main(void)
{
    FILE *f = fixture_open("dm32_arc4_1.fwunvoiced");
    char *line = NULL;
    size_t cap = 0;
    ambe_unvoiced_state carried;
    int n = 0, given_ok = 0, carried_ok = 0, primed = 0;

    ambe_unvoiced_reset(&carried);

    while (getline(&line, &cap, f) > 0) {
        ambe_unvoiced_state st;
        int16_t after[STLEN];
        char *p = line;
        int k, bad;

        if (line[0] == '#')
            continue;
        (void)strtol(p, &p, 10);                       /* pitch */
        for (k = 0; k < BLK; k++) (void)strtol(p, &p, 10);
        for (k = 0; k < STLEN; k++) st.s[k] = (int16_t)strtol(p, &p, 10);
        for (k = 0; k < NS; k++)   (void)strtol(p, &p, 10);
        for (k = 0; k < STLEN; k++) after[k] = (int16_t)strtol(p, &p, 10);

        /* 1. one step from the firmware's own state */
        {
            ambe_unvoiced_state u = st;
            ambe_unvoiced_advance_noise(&u, NS);
            for (bad = 0, k = 0; k < CHECKED; k++)
                if (u.s[k] != after[k]) {
                    CHECK(0, "call %d state[%d]: %d, firmware %d\n",
                          n, k, (int)u.s[k], (int)after[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                given_ok++;
        }

        /* 2. carrying our own across the whole run */
        if (!primed) {
            carried = st;
            primed = 1;
        }
        ambe_unvoiced_advance_noise(&carried, NS);
        for (bad = 0, k = 0; k < CHECKED; k++)
            if (carried.s[k] != after[k]) {
                CHECK(0, "call %d state[%d] carrying: %d, firmware %d\n",
                      n, k, (int)carried.s[k], (int)after[k]);
                bad = 1;
                break;
            }
        if (!bad)
            carried_ok++;
        n++;
    }
    free(line);
    fclose(f);

    CHECK(n > 80, "only %d unvoiced calls in the fixture\n", n);
    CHECK(given_ok == n, "one-step exact on %d of %d\n", given_ok, n);
    CHECK(carried_ok == n, "exact carrying our own state on %d of %d\n", carried_ok, n);

    printf("[%d firmware calls, %d generated values: the noise source is exact "
           "one step at a time and across the whole run] ", n, n * NS);
    return t_done("the unvoiced noise source vs the firmware");
}
