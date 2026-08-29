/*
 * ambe_fft.h - the radio's spectral front end.
 *
 * A 128-point complex FFT over 256 real samples packed even/odd, with the
 * real spectrum unpacked afterwards and block floating point carried through.
 * Transcribed from Dsp_WindowAndComputeFft 0x00019B6C and the Dsp_Fft*
 * cluster; see ambe_fft.c for the full provenance list.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_FFT_H
#define AMBE_FFT_H

#include <stdint.h>
#include <stddef.h>

#define AMBE_FFT_MAX 320   /* the largest input the analyser hands it */

/*
 * Window `in_len` samples with the radio's analysis window, transform them,
 * and optionally write |X|^2.  Complex samples are packed one to an int32,
 * real in the low half.  Returns the block-float exponent: the spectrum is
 * the transform of the input scaled by 2^-exponent.
 */
short ambe_fft_window(int32_t *magsq_out, const int16_t *in, int in_len,
                      short scale_bias, int32_t *fft_buf, int size_bits,
                      int shift);

#endif /* AMBE_FFT_H */
