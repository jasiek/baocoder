/*
 * ambe_unvoiced_int.h - the unvoiced synthesiser's internal stage boundary.
 *
 * Not part of the public API: ambe_unvoiced_synth() in include/ambe.h is.
 * This exists so tests/test_unvoiced.c can compare the spectral shaping
 * against the firmware's own intermediate buffer rather than only against the
 * samples that come out the far end.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_UNVOICED_INT_H
#define AMBE_UNVOICED_INT_H

#include <stdint.h>

short ambe_unvoiced_shape(int32_t *fft, int n, int cls, int L, int16_t f0_q19,
                          const int16_t *amps, int amp_exp,
                          const uint16_t *voiced, int16_t pitch, short fexp);

#endif /* AMBE_UNVOICED_INT_H */
