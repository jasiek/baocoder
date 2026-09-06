/*
 * ambe_frame_int.h - the frame-synthesis layer's internal stage boundaries.
 *
 * Not part of the public API.  These exist so each stage of
 * Vocoder_SynthesizeFrame 0x00019DB8 can be swept against the firmware on its
 * own rather than only through the whole of it.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_FRAME_INT_H
#define AMBE_FRAME_INT_H

#include <stdint.h>

/* Math_PopCountBits 0x000189F4: the population count of the low `n` bits. */
int ambe_popcount_bits(uint32_t x, int n);

/* Math_SqrtScaled 0x000193E0 */
uint32_t ambe_sqrt_scaled(int32_t mant, uint32_t exp, int16_t q);

/* Vocoder_CopyFrameParamsWithReset 0x00019CBC */
void ambe_frame_params_copy(int16_t *dst, const int16_t *src);

/* Vocoder_ResetFrameBuffer 0x00019D38.  `flags` is the 0x38-short array the
   block's [0x40] points at in the running firmware. */
void ambe_frame_reset_buffer(int16_t *params, uint16_t *flags);

#endif /* AMBE_FRAME_INT_H */
