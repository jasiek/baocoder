/*
 * ambe_subband.h - the analyser's 16-channel filterbank.
 *
 * Transcribed from Vocoder_AnalyzeSubbandSpectrum 0x00023AA8 and
 * Vocoder_SubbandSumDifference 0x0002AFE8; see ambe_subband.c for provenance
 * and docs/fixed-point.md for what the stage is.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_SUBBAND_H
#define AMBE_SUBBAND_H

#include <stdint.h>

/*
 * Window 32 samples with the radio's folded 32-tap window.  `in` and `out` may
 * be the same buffer.
 */
void ambe_subband_window(int16_t out[32], const int16_t in[32]);

/*
 * The 32-point real DFT of x[0..31], in place, as 16 complex bins packed
 * re,im,re,im...  Bin k comes out as X[k]/16 - the factor the window's DC gain
 * of 16 is there to cancel.
 */
void ambe_subband_dft32(int16_t x[32]);

#endif /* AMBE_SUBBAND_H */
