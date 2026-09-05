/*
 * ambe_voiced_int.h - the voiced synthesiser's internal stage boundaries.
 *
 * Not part of the public API.  These exist so each helper can be swept against
 * the firmware on its own rather than only through the 2378 bytes of
 * Vocoder_SynthesizeVoiced above them.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_VOICED_INT_H
#define AMBE_VOICED_INT_H

#include <stdint.h>

/* Vocoder_ComputeHarmonicGains 0x0001D71C */
void ambe_voiced_harmonic_gains(uint16_t *gain, uint16_t *index, int32_t phase,
                                int16_t prev_pitch, int16_t pitch_delta,
                                uint16_t n, int L);

/* Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8.  `fft` is 0x100 shorts seen as
   128 complex bins, with one more short of room after them for the wrap sample
   the stock code writes at pDest[0x100]. */
short ambe_voiced_harmonic_spectrum(int32_t *fft, int16_t *exp_out,
                                    int16_t step, const uint16_t *phase,
                                    const int16_t *voiced,
                                    const uint16_t *amp, int16_t exp_bias,
                                    int end, int start, int16_t mark);

/* Vocoder_InterpolateSpectralEnvelope 0x0001D9F0.  `env` is 0xA8 ints and
   `block` the 0x101 shorts the spectrum builder wrote, wrap sample included -
   the interpolator reads block[i + 1] and i reaches 0xFF. */
void ambe_voiced_interp_envelope(int32_t *env, int32_t mant, int16_t exp,
                                 uint16_t pitch, const uint16_t *block,
                                 int16_t block_exp);

#endif /* AMBE_VOICED_INT_H */
