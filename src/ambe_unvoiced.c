/*
 * ambe_unvoiced.c - the unvoiced excitation's noise source.
 *
 * The first verified piece of Vocoder_SynthesizeUnvoiced 0x0001AFE0.  That
 * function builds a windowed noise segment, transforms it, shapes it against
 * the frame's unvoiced bands and transforms back; this is the part that
 * produces and carries the noise itself, and everything downstream of it
 * depends on getting it bit-exact.
 *
 * The generator is a 16-bit LCG, x = 173x + 13849 (0xAD and 0x3619), which is
 * the same one mbelib uses - so the two decoders' noise is the same sequence
 * given the same seed, which is not true of anything that calls rand().
 *
 * The state is pChannelState+0x648, and its first 0x55 shorts are what this
 * touches:
 *
 *   [0]        the LCG's carry - seeded from here and written back
 *   [1..0x54]  the noise history, 84 entries
 *
 * Per frame the history is shifted down by nCount and the vacated tail is
 * refilled from the generator.  With nCount 0x50, which is what this radio
 * always uses, that leaves four entries to carry over and eighty to generate.
 * The rest of the array - [0x55..] the previous segment's overlap-add tail and
 * [0xa9] its exponent - belongs to the parts not yet transcribed.
 *
 * Verified bit-exact against 103 firmware calls: tests/test_unvoiced.c.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"

/*
 * The unvoiced synthesis window, 81 entries at SRAM 0x18003584 (file 0x66c44).
 * It is 32767*sqrt(k/80) to within one LSB, but the exact int16s are what the
 * firmware multiplies by, so those are what ship.
 */
const int16_t ambe_uv_window_q15[81] = {
         0,   3664,   5181,   6345,   7327,   8192,   8974,   9693,  10362,  10991,
     11585,  12151,  12691,  13209,  13708,  14189,  14654,  15105,  15543,  15969,
     16384,  16789,  17184,  17570,  17948,  18318,  18681,  19037,  19386,  19729,
     20066,  20398,  20724,  21046,  21362,  21674,  21981,  22285,  22584,  22879,
     23170,  23458,  23743,  24024,  24301,  24576,  24848,  25116,  25382,  25645,
     25905,  26163,  26418,  26671,  26922,  27170,  27416,  27659,  27901,  28140,
     28378,  28613,  28847,  29079,  29309,  29537,  29763,  29988,  30211,  30432,
     30652,  30870,  31086,  31302,  31515,  31727,  31938,  32148,  32356,  32563,
     32767,
};

#define LCG_MUL  0xAD
#define LCG_ADD  0x3619
#define HIST_END 0x54

void ambe_unvoiced_reset(ambe_unvoiced_state *u)
{
    int i;
    for (i = 0; i < AMBE_UNVOICED_STATE; i++)
        u->s[i] = 0;
}

void ambe_unvoiced_advance_noise(ambe_unvoiced_state *u, int n)
{
    int carry = HIST_END - n;
    int i;
    int16_t x;

    /* what survives of the previous frame's history */
    for (i = 0; i < carry; i++)
        u->s[1 + i] = u->s[n + i + 1];

    /* and the rest generated afresh, continuing the same sequence */
    x = u->s[0];
    for (i = carry; i < HIST_END; i++) {
        x = (int16_t)(x * LCG_MUL + LCG_ADD);
        u->s[i + 1] = x;
    }
    u->s[0] = x;
}

/*
 * The 2n-sample windowed noise segment, zero-padded to the transform length.
 *
 * The rising half is windowed against the history BEFORE the generator is
 * advanced and the falling half against it after, with the window index
 * reversed - so one square-root window is used in both directions and the two
 * halves overlap-add cleanly across the frame boundary.  The order matters:
 * calling this after advancing the noise gives a buffer that is wrong in its
 * first half only, which is easy to miss.
 *
 * `buf` receives `1 << size_bits` shorts; the tail beyond 2n is zeroed.
 */
void ambe_unvoiced_window(int16_t *buf, ambe_unvoiced_state *u, int n,
                          int size_bits)
{
    int len = 1 << size_bits;
    int i;

    for (i = 0; i < n; i++)
        buf[i] = (int16_t)(((int32_t)ambe_uv_window_q15[i] * u->s[i + 1]) >> 15);

    ambe_unvoiced_advance_noise(u, n);

    for (i = 0; i < n; i++)
        buf[n + i] = (int16_t)(((int32_t)ambe_uv_window_q15[n - i] * u->s[i + 1]) >> 15);

    for (i = 2 * n; i < len; i++)
        buf[i] = 0;
}
