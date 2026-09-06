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

/* Vocoder_SmoothPitchState 0x00022D7C: a one-pole smoother applied only when
   more than seven bands are voiced. */
uint32_t ambe_smooth_pitch_state(uint32_t state, uint16_t target, uint32_t vuv);

/* Vocoder_NormalizeSpectralBlock 0x00022C18: clamp, peak, exponentiate, and
   write the block float's common exponent.  `coeffs` is 0x38 shorts; the tail
   past `count` is zeroed. */
void ambe_normalize_spectral_block(int16_t *coeffs, int16_t *exp_out, int count);

/* Vocoder_UpdatePitchHistoryBuffer 0x0001A9E8: moves the frame's pitch
   candidate to a new index and recomputes the pitch and harmonic count that
   follow from it.  `flags` is what params[0x40] points at. */
void ambe_update_pitch_history(int16_t *params, uint16_t *flags, int16_t cand);

/* Math_SqrtScaled 0x000193E0 */
uint32_t ambe_sqrt_scaled(int32_t mant, uint32_t exp, int16_t q);

/* Vocoder_CopyFrameParamsWithReset 0x00019CBC */
void ambe_frame_params_copy(int16_t *dst, const int16_t *src);

/* Vocoder_ResetFrameBuffer 0x00019D38.  `flags` is the 0x38-short array the
   block's [0x40] points at in the running firmware. */
void ambe_frame_reset_buffer(int16_t *params, uint16_t *flags);

#endif /* AMBE_FRAME_INT_H */
