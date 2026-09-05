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
