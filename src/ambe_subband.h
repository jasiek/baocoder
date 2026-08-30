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

/* The analysis history the stage reads, and the window overlap it keeps. */
#define AMBE_SUBBAND_HISTORY 383   /* 0x17F, the caller's constant  */
#define AMBE_SUBBAND_OVERLAP 28    /* 32-tap window less the 4-step */
#define AMBE_SUBBAND_MAX_OUT 11    /* the 16 x 11 output buffer     */

/*
 * How many output samples per channel this call produces, and which input it
 * needs.  `acc` is the fractional-resampler state (param_1 + 0x6DC), carried
 * between calls; `nframe` is the caller's frame size, which
 * Vocoder_ProcessFrameFec 0x00016F3C clamps to [76, 84].
 *
 * Writes the number of input samples to take to *nsamp and the offset into the
 * history at which to take them to *offset; either may be NULL.
 */
int ambe_subband_advance(int16_t *acc, int nframe, int *nsamp, int *offset);


/* ---- the eight-band loop in Vocoder_AnalyzeSpectrum 0x000205B8 -----------
 *
 * Eight bands, each built from two adjacent channels of the 16 x 49 ring.
 * Each channel contributes a 58-sample segment, and the two spectra are
 * combined, then written into a 128-entry spectrum at a stride of 16 - so
 * adjacent bands overlap by half.
 */

#define AMBE_BAND_SEG   58   /* samples per channel segment  */
#define AMBE_BAND_NFFT  64   /* zero-padded transform size   */
#define AMBE_BAND_BINS  32   /* squared magnitudes it yields */
#define AMBE_BANDS       8
#define AMBE_BAND_STRIDE 16  /* 8 x 16 = the 128 the voicing rule reads */

/*
 * One channel's segment to 32 squared magnitudes: the 58-tap window folded,
 * zero-padded into a 64-point transform, |X|^2 * 2, with bins 0 and 1 zeroed
 * as the stock code does.  `exp` is the segment's block-float exponent
 * (value = mantissa * 2^exp); the returned exponent is the spectrum's.
 */
short ambe_band_spectrum(int32_t magsq[32], const int16_t seg[58], short exp);

/*
 * Add two 32-entry spectra that carry their own exponents, as the stock code
 * does it: align to the larger exponent, shift the other's mantissas right by
 * the difference, and return the exponent of the result.  `dst` may alias
 * either input.
 */
short ambe_band_add(int32_t dst[32], const int32_t a[32], short ea,
                    const int32_t b[32], short eb);

#endif /* AMBE_SUBBAND_H */
