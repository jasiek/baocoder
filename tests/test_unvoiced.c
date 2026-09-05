/*
 * test_unvoiced.c - Vocoder_SynthesizeUnvoiced 0x0001AFE0, against the radio.
 *
 * The fixture is not a capture of the function running inside a frame: it is
 * the function CALLED, one case per record, with its five arguments poked in
 * and its results read out - tools/fw_oracle/gen_uv_jobs.py.  The inputs are
 * still the radio's own, taken from a Vocoder_TxTask run over a real capture,
 * but pairing them with the outputs is now arithmetic rather than alignment.
 * That also settles a suspicion the sequence capture could not: replaying its
 * inputs reproduces its outputs 103 times out of 103, so it had paired state
 * with parameter block correctly all along, and shifting the block by six
 * records drops that to 0 of 97.
 *
 * Five things are checked, in the order a failure is easiest to localise:
 *
 *   the noise generator, one step from the firmware's state and then carrying
 *   its own across the whole run - which is what a stream decoder does, and
 *   what fails if the generator drifts by a single step;
 *
 *   the windowed 2n-sample segment the forward transform is given;
 *
 *   the per-harmonic voicing flags, against the ones
 *   Vocoder_BuildFrameResetPattern 0x00022CD0 built from the same block;
 *
 *   the shaped spectrum handed to the inverse transform, which is where the
 *   three block-float exponents either agree with the radio or do not;
 *
 *   and the 80 samples and 170 shorts of state the call finally produces.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_fft.h"
#include "ambe_unvoiced_int.h"
#include "testutil.h"

#define NS      80
#define BLK     68
#define STLEN   170
#define NVOIC   AMBE_UNVOICED_BANDS
#define NSPEC   256
#define CHECKED 0x55

#define C_PITCH 0
#define C_BLK   1
#define C_ST0   (C_BLK + BLK)
#define C_VOIC  (C_ST0 + STLEN)
#define C_WND   (C_VOIC + NVOIC)
#define C_SPEC  (C_WND + NSPEC)
#define C_ACC   (C_SPEC + NSPEC)
#define C_ST1   (C_ACC + NS)
#define NCOL    (C_ST1 + STLEN)

static int rec[NCOL];

static int read_record(FILE *f, char **line, size_t *cap)
{
    char *p;
    int i;

    while (getline(line, cap, f) > 0) {
        if ((*line)[0] == '#' || (*line)[0] == '\n')
            continue;
        p = *line;
        for (i = 0; i < NCOL; i++)
            rec[i] = (int)strtol(p, &p, 10);
        return 1;
    }
    return 0;
}

int main(void)
{
    FILE *f = fixture_open("dm32_arc4_1.fwunvoiced");
    char *line = NULL;
    size_t cap = 0;
    ambe_unvoiced_state carried;
    int n = 0, given_ok = 0, carried_ok = 0, primed = 0;
    int voic_ok = 0, wnd_ok = 0, spec_ok = 0, acc_ok = 0, st_ok = 0;

    ambe_unvoiced_reset(&carried);

    while (read_record(f, &line, &cap)) {
        ambe_unvoiced_state st;
        int16_t amps[AMBE_UNVOICED_BANDS];
        uint16_t voiced[AMBE_UNVOICED_BANDS];
        uint32_t vuv;
        int cls = rec[C_BLK + 0], L = rec[C_BLK + 2];
        int16_t f0 = (int16_t)rec[C_BLK + 6], pitch = (int16_t)rec[C_PITCH];
        int amp_exp = rec[C_BLK + 0x42];
        int k, bad;

        for (k = 0; k < STLEN; k++)
            st.s[k] = (int16_t)rec[C_ST0 + k];
        for (k = 0; k < AMBE_UNVOICED_BANDS; k++)
            amps[k] = (int16_t)rec[C_BLK + 8 + k];
        vuv = ((uint32_t)(uint16_t)rec[C_BLK + 5] << 16)
            | (uint32_t)(uint16_t)rec[C_BLK + 4];

        /* 1. the generator, one step from the firmware's own state */
        {
            ambe_unvoiced_state u = st;
            ambe_unvoiced_advance_noise(&u, NS);
            for (bad = 0, k = 0; k < CHECKED; k++)
                if (u.s[k] != (int16_t)rec[C_ST1 + k]) {
                    CHECK(0, "call %d state[%d]: %d, firmware %d\n",
                          n, k, (int)u.s[k], rec[C_ST1 + k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                given_ok++;
        }

        /* 2. and carrying its own across the whole run */
        if (!primed) {
            carried = st;
            primed = 1;
        }
        ambe_unvoiced_advance_noise(&carried, NS);
        for (bad = 0, k = 0; k < CHECKED; k++)
            if (carried.s[k] != (int16_t)rec[C_ST1 + k]) {
                CHECK(0, "call %d state[%d] carrying: %d, firmware %d\n",
                      n, k, (int)carried.s[k], rec[C_ST1 + k]);
                bad = 1;
                break;
            }
        if (!bad)
            carried_ok++;

        /* 3. the voicing flags, from the frame's own voicing word */
        ambe_unvoiced_voicing(voiced, vuv, f0, L);
        for (bad = 0, k = 0; k < NVOIC; k++)
            if (voiced[k] != (uint16_t)rec[C_VOIC + k]) {
                CHECK(0, "call %d voicing[%d]: %u, firmware %d\n",
                      n, k, (unsigned)voiced[k], rec[C_VOIC + k]);
                bad = 1;
                break;
            }
        if (!bad)
            voic_ok++;

        /* 4. the windowed segment, and the shaped spectrum it becomes */
        if (cls != 3) {
            ambe_unvoiced_state u = st;
            int32_t fft[128];
            int16_t *spec = (int16_t *)fft;
            short fexp;

            ambe_unvoiced_window(spec, &u, NS, 8);
            for (bad = 0, k = 0; k < NSPEC; k++)
                if (spec[k] != (int16_t)rec[C_WND + k]) {
                    CHECK(0, "call %d windowed[%d]: %d, firmware %d\n",
                          n, k, (int)spec[k], rec[C_WND + k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                wnd_ok++;
            fexp = ambe_fft_forward(fft, 0, 8, 0);
            ambe_unvoiced_shape(fft, NS, cls, L, f0, amps, amp_exp, voiced,
                                pitch, fexp);
            for (bad = 0, k = 0; k < NSPEC; k++)
                if (spec[k] != (int16_t)rec[C_SPEC + k]) {
                    CHECK(0, "call %d spectrum[%d]: %d, firmware %d\n",
                          n, k, (int)spec[k], rec[C_SPEC + k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                spec_ok++;
        } else {
            wnd_ok++;
            spec_ok++;
        }

        /* 5. the whole call: the samples it adds, and the state it leaves */
        {
            ambe_unvoiced_state u = st;
            int32_t acc[NS];

            for (k = 0; k < NS; k++)
                acc[k] = 0;
            ambe_unvoiced_synth(acc, NS, &u, cls, L, f0, amps, amp_exp,
                                voiced, pitch);
            for (bad = 0, k = 0; k < NS; k++)
                if (acc[k] != rec[C_ACC + k]) {
                    CHECK(0, "call %d sample %d: %d, firmware %d\n",
                          n, k, (int)acc[k], rec[C_ACC + k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                acc_ok++;
            for (bad = 0, k = 0; k < STLEN; k++)
                if (u.s[k] != (int16_t)rec[C_ST1 + k]) {
                    CHECK(0, "call %d state[%d] after: %d, firmware %d\n",
                          n, k, (int)u.s[k], rec[C_ST1 + k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                st_ok++;
        }
        n++;
    }
    fclose(f);

    CHECK(n > 80, "only %d unvoiced calls in the fixture\n", n);
    CHECK(given_ok == n,   "generator one-step exact on %d of %d\n", given_ok, n);
    CHECK(carried_ok == n, "generator exact carrying our own state on %d of %d\n",
          carried_ok, n);
    CHECK(voic_ok == n,   "voicing flags exact on %d of %d\n", voic_ok, n);
    CHECK(wnd_ok == n,    "windowed segment exact on %d of %d\n", wnd_ok, n);
    CHECK(spec_ok == n,   "shaped spectrum exact on %d of %d\n", spec_ok, n);
    CHECK(acc_ok == n,    "samples exact on %d of %d\n", acc_ok, n);
    CHECK(st_ok == n,     "state after exact on %d of %d\n", st_ok, n);

    printf("[%d firmware calls: %d generated values, %d voicing flags, %d "
           "windowed segments and %d shaped spectra of 256, and %d samples: "
           "all bit-exact] ", n, n * NS, n * NVOIC, n, n, n * NS);
    free(line);
    return t_done("Vocoder_SynthesizeUnvoiced against the firmware, bit for bit");
}
