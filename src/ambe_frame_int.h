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

/* FUN_0001ABDC: twice the dot product of two int16 arrays, renormalised, as a
   32-bit mantissa with the shift that produced it. */
int32_t ambe_dot_norm(int16_t *exp_out, const int16_t *a, const int16_t *b,
                      int n);

/* Math_ArrayShiftCopy 0x0001AB58: copy n shorts, shifting each by `shift` -
   left when positive, arithmetic right when negative. */
void ambe_array_shift_copy(int16_t *dst, const int16_t *src, int n, int shift);

/* Dsp_NormalizeArray 0x0001ADA0: shift an array up by its headroom and take
   the same amount off the exponent the caller carries. */
void ambe_normalize_array(int16_t *dst, const int16_t *src, int count,
                          int16_t *exp_io);

/* Math_ArrayShiftSaturate 0x0001AF5C: rescale an array between two block-float
   exponents, saturating per element.  Not Math_ArrayShiftSaturateInt
   0x0001AE14, which is ambe_synth_output. */
void ambe_array_shift_saturate(int16_t *dst, const int16_t *src, int count,
                               int16_t dst_exp, int16_t src_exp);

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

/* Dsp_HilbertTransform 0x00029D1C: a ten-tap antisymmetric FIR along the
   harmonic axis.  `dst` is 0x38 shorts; the tail past `count` is zeroed. */
void ambe_hilbert_transform(int16_t *dst, const int16_t *src, int count);

/* Vocoder_MatchExcitationEnergy 0x000277F8: the IMBE spectral amplitude
   enhancement.  `amps` is 0x38 shorts at the block exponent `exp_io`, both
   rewritten in place; `ref_mant`/`ref_exp` are the caller's running reference
   energy, read and rewritten and not used here. */
void ambe_match_excitation_energy(int16_t *amps, int16_t *exp_io,
                                  int16_t *ref_mant, int16_t *ref_exp,
                                  int16_t pitch, int16_t count);

/* Math_SqrtScaled 0x000193E0 */
uint32_t ambe_sqrt_scaled(int32_t mant, uint32_t exp, int16_t q);

/* Vocoder_CopyFrameParamsWithReset 0x00019CBC */
void ambe_frame_params_copy(int16_t *dst, const int16_t *src);

/* Vocoder_ResetFrameBuffer 0x00019D38.  `flags` is the 0x38-short array the
   block's [0x40] points at in the running firmware. */
void ambe_frame_reset_buffer(int16_t *params, uint16_t *flags);

#endif /* AMBE_FRAME_INT_H */
