/*
 * test_fft_firmware.c - the transform against the radio's, bit for bit.
 *
 * test_fft.c compares this FFT with an independently computed DFT and allows a
 * fraction of a dB.  That is the right test for "is this a transform", and it
 * is not enough: it passed for as long as the stage loop scaled one bit too
 * hard on every rescanned stage, because a spectrum that is uniformly half as
 * large still correlates perfectly with the reference and only shifts the dB
 * offset the test already tolerates.
 *
 * This is the test that would have caught it.  The fixture is Dsp_FftForward
 * 0x000256D0 executed over 93 real buffers, captured either side of the call,
 * so there is nothing to tolerate: same input, same output, same returned
 * block-float exponent.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_fft.h"
#include "testutil.h"

int main(void)
{
    FILE *f = fixture_open("dm32_arc4_1.fwfft");
    char *line = NULL;
    size_t cap = 0;
    int n = 0, buf_ok = 0, exp_ok = 0;

    while (getline(&line, &cap, f) > 0) {
        int16_t in[256], want[256];
        int32_t buf[128];
        char *p = line;
        int k, e, bad;
        short r;

        if (line[0] == '#')
            continue;
        e = (int)strtol(p, &p, 10);
        for (k = 0; k < 256; k++) in[k]   = (int16_t)strtol(p, &p, 10);
        for (k = 0; k < 256; k++) want[k] = (int16_t)strtol(p, &p, 10);

        memcpy(buf, in, sizeof(buf));
        r = ambe_fft_forward(buf, 0, 8, 0);

        if (r == (short)e)
            exp_ok++;
        else
            CHECK(0, "buffer %d: exponent %d, firmware %d\n", n, (int)r, e);

        {
            const int16_t *got = (const int16_t *)buf;
            for (bad = 0, k = 0; k < 256; k++)
                if (got[k] != want[k]) {
                    CHECK(0, "buffer %d bin %d: %d, firmware %d\n",
                          n, k, (int)got[k], (int)want[k]);
                    bad = 1;
                    break;
                }
            if (!bad)
                buf_ok++;
        }
        n++;
    }
    free(line);
    fclose(f);

    CHECK(n > 80, "only %d transforms in the fixture\n", n);
    CHECK(buf_ok == n, "spectrum exact on %d of %d\n", buf_ok, n);
    CHECK(exp_ok == n, "exponent exact on %d of %d\n", exp_ok, n);

    printf("[%d transforms of 256 points: every bin and every block-float "
           "exponent identical to the radio's] ", n);
    return t_done("Dsp_FftForward vs the firmware, bit for bit");
}
