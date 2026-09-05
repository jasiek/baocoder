/*
 * test_postfilter.c - FUN_00018a2c, against the firmware executed.
 *
 * The fixture is every call the radio made over a real capture, captured on
 * both sides by breaking inside Vocoder_SynthesizeFrame 0x00019DB8 - at
 * 0x00019EF2 after the voiced synthesis and 0x00019EFE after the filter - so
 * this is exactness, not tolerance.
 *
 * Two different things are checked, and a wrong filter can pass the first:
 *
 *   given the firmware's own state, does it produce the firmware's samples;
 *   and carrying its OWN state across calls, does it still - which is the
 *   question that matters, because that is how it would be used.
 *
 * The second is the stronger test: it fails if the state update rule is wrong
 * even where the arithmetic is right.
 *
 * The final scaling to int16 PCM is checked alongside.  Its arguments read as
 * Q formats whose difference would be a shift of two; the machine does a shift
 * of fourteen with rounding, and truncation reproduces none of the samples.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "testutil.h"

#define NS 80

int main(void)
{
    FILE *f = fixture_open("dm32_arc4_1.fwpostfilter");
    char *line = NULL;
    size_t cap = 0;
    ambe_postfilter_state carried;
    int n = 0, given_ok = 0, carried_ok = 0, primed = 0, pcm_ok = 0;

    ambe_postfilter_reset(&carried);

    while (getline(&line, &cap, f) > 0) {
        ambe_postfilter_state st;
        int32_t in[NS], out[NS], work[NS];
        int16_t refpcm[NS], gotpcm[NS];
        char *p = line;
        int k, bad;

        if (line[0] == '#')
            continue;
        for (k = 0; k < 6; k++)  st.s[k] = (int32_t)strtol(p, &p, 10);
        for (k = 0; k < NS; k++) in[k]  = (int32_t)strtol(p, &p, 10);
        for (k = 0; k < NS; k++) out[k] = (int32_t)strtol(p, &p, 10);
        for (k = 0; k < NS; k++) refpcm[k] = (int16_t)strtol(p, &p, 10);

        /* 1. the firmware's own state in, the firmware's samples out */
        memcpy(work, in, sizeof(work));
        {
            ambe_postfilter_state s = st;
            ambe_postfilter(&s, work, NS);
        }
        for (bad = 0, k = 0; k < NS; k++)
            if (work[k] != out[k]) {
                CHECK(0, "call %d sample %d: %d, firmware %d\n", n, k,
                      (int)work[k], (int)out[k]);
                bad = 1;
                break;
            }
        if (!bad)
            given_ok++;

        /* 2. carrying our own state, which is how it would actually be used */
        if (!primed) {
            carried = st;
            primed = 1;
        }
        memcpy(work, in, sizeof(work));
        ambe_postfilter(&carried, work, NS);
        for (bad = 0, k = 0; k < NS; k++)
            if (work[k] != out[k]) {
                CHECK(0, "call %d sample %d carrying state: %d, firmware %d\n",
                      n, k, (int)work[k], (int)out[k]);
                bad = 1;
                break;
            }
        if (!bad)
            carried_ok++;

        /* 3. the accumulator becoming int16 PCM */
        ambe_synth_output(gotpcm, out, NS);
        for (bad = 0, k = 0; k < NS; k++)
            if (gotpcm[k] != refpcm[k]) {
                CHECK(0, "call %d sample %d: pcm %d, firmware %d\n", n, k,
                      (int)gotpcm[k], (int)refpcm[k]);
                bad = 1;
                break;
            }
        if (!bad)
            pcm_ok++;
        n++;
    }
    free(line);
    fclose(f);

    CHECK(n > 100, "only %d filter calls in the fixture\n", n);
    CHECK(given_ok == n, "exact given the firmware's state on %d of %d\n", given_ok, n);
    CHECK(carried_ok == n, "exact carrying our own state on %d of %d\n", carried_ok, n);
    CHECK(pcm_ok == n, "PCM scaling exact on %d of %d\n", pcm_ok, n);

    printf("[%d firmware filter calls, %d samples: bit-exact given the radio's "
           "state, carrying our own, and through the int16 scaling] ", n, n * NS);
    return t_done("the output filter FUN_00018a2c vs the firmware");
}
