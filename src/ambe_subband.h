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

/*
 * One 32-sample frame: window, transform, and accumulate the sixteen band
 * energies.  `bins` receives the transform (16 complex pairs, re,im) and
 * `energy` is added to, not overwritten - the stock code sweeps many frames
 * into one set of sixteen accumulators.
 */
void ambe_subband_frame(int32_t energy[16], int16_t bins[32],
                        const int16_t in[32]);

/*
 * The decimator.  `sets` is interleaved sample-sets of 16 channels each; this
 * produces `nout` samples for one channel, reading seven consecutive sets per
 * output and advancing two sets between them - so it needs 2*nout + 5 sets.
 */
void ambe_subband_decimate(int16_t *out, const int16_t *sets, int chan,
                           int nout);

/*
 * The sixteen per-band magnitudes of one transformed frame - the channel
 * samples the decimator filters.  `bins` is the output of ambe_subband_dft32
 * and `e_in` the block-float exponent the input samples were normalised to;
 * the magnitudes come back aligned to that same exponent, so a channel sample
 * carries the units the PCM did.
 */
void ambe_subband_magnitudes(int16_t out[16], const int16_t bins[32],
                             int e_in);

#endif /* AMBE_SUBBAND_H */
