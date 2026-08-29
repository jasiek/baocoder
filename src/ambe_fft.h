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

/*
 * The transform on its own, Dsp_FftForward 0x000256D0.  `buf` holds 2^size_bits
 * real samples packed even/odd into the re/im halves of 2^(size_bits-1) int32
 * words, and is transformed in place; the real spectrum is unpacked afterwards.
 * Returns the block-float exponent.
 *
 * ambe_fft_window is this with the 199-tap analysis window in front of it.  The
 * analyser's eight-band loop needs the transform alone, at 64 points and with
 * its own window, which is why this is a separate entry point.
 */
short ambe_fft_forward(int32_t *buf, short scale_exp, int size_bits, int shift);

#endif /* AMBE_FFT_H */
