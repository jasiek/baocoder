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

#endif /* AMBE_VOICED_INT_H */
