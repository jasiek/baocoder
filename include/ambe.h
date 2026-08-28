/*
 * ambe.h - AMBE+2 (3600x2450) half-rate vocoder decoder, DMR framing.
 *
 * Reimplementation phase of the Baofeng DM-32UV firmware reverse-engineering
 * project.  The framing/FEC layer is transcribed from the stock firmware; every
 * transcribed block cites the stock function and its address (see README.md).
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_H
#define AMBE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMBE_PCM_SAMPLES   160   /* 20 ms @ 8 kHz                            */
#define AMBE_BITS          49    /* payload bits per AMBE+2 2450 frame       */
#define AMBE_DMR_BITS      72    /* on-air bits per DMR AMBE frame           */
#define AMBE_DMR_BYTES     9
#define AMBE_MAX_HARMONICS 56    /* L is bounded by 56 in the 2450 mode      */

/* Frame classification returned by the parameter decoder. */
typedef enum {
    AMBE_FRAME_VOICE   = 0,
    AMBE_FRAME_SILENCE = 1,   /* b0 == 124 or 125                            */
    AMBE_FRAME_ERASURE = 2,   /* b0 in 120..123                              */
    AMBE_FRAME_TONE    = 3    /* b0 == 126 or 127 (not synthesised here)     */
} ambe_frame_type;

/*
 * The MBE speech model parameters for one 20 ms frame.  Index 0 is unused for
 * the per-harmonic arrays; harmonics run 1..L, matching the AMBE literature.
 */
typedef struct {
    float   w0;                              /* fundamental, rad/sample      */
    int     L;                               /* number of harmonics          */
    float   gamma;                           /* frame log2 gain              */
    uint8_t Vl[AMBE_MAX_HARMONICS + 1];      /* 1 = voiced, 0 = unvoiced     */
    float   Ml[AMBE_MAX_HARMONICS + 1];      /* spectral amplitudes          */
    float   log2Ml[AMBE_MAX_HARMONICS + 1];  /* log2 spectral amplitudes     */
    float   PHIl[AMBE_MAX_HARMONICS + 1];    /* synthesis phase              */
    float   PSIl[AMBE_MAX_HARMONICS + 1];    /* accumulated voiced phase     */
    int     repeat;                          /* consecutive repeated frames  */
} ambe_parms;

/* Per-frame diagnostics. */
typedef struct {
    ambe_frame_type type;
    int b[9];        /* the nine quantiser indices b0..b8                    */
    int errs_c0;     /* Golay(23,12) bit errors corrected in codeword 0      */
    int errs_c1;     /* Golay(23,12) bit errors corrected in codeword 1      */
    int uncorrectable;
} ambe_frame_info;

typedef struct ambe_decoder ambe_decoder;

/* ---------------------------------------------------------------- framing */

/*
 * Deinterleave a 9-byte on-air DMR AMBE frame into the four AMBE codewords.
 * fr[0] = c0 (24 bits, Golay24), fr[1] = c1 (23 bits, Golay23, PRNG-scrambled),
 * fr[2] = c2 (11 bits), fr[3] = c3 (14 bits).
 */
void ambe_dmr_deinterleave(const uint8_t frame[AMBE_DMR_BYTES], uint8_t fr[4][24]);

/* Inverse of ambe_dmr_deinterleave. */
void ambe_dmr_interleave(const uint8_t fr[4][24], uint8_t frame[AMBE_DMR_BYTES]);

/*
 * Undo the PRNG scrambling applied to codeword 1.  The generator is seeded from
 * the 12 data bits of the (already corrected) codeword 0.  Self-inverse.
 */
void ambe_descramble_c1(uint8_t fr[4][24]);

/*
 * Run FEC over a deinterleaved frame and produce the 49 payload bits.
 * Returns 0 on success, -1 if either Golay codeword was uncorrectable.
 * `info` may be NULL.
 */
int ambe_fec_decode(uint8_t fr[4][24], uint8_t ambe_d[AMBE_BITS],
                    ambe_frame_info *info);

/* Encode 49 payload bits back into a 9-byte on-air DMR AMBE frame. */
void ambe_fec_encode(const uint8_t ambe_d[AMBE_BITS], uint8_t frame[AMBE_DMR_BYTES]);

/* --------------------------------------------------------------- Golay ECC */

/* Systematic Golay(23,12): 12 data bits in the high positions of a 23-bit word. */
uint32_t ambe_golay23_encode(uint32_t data12);
/* Returns the corrected codeword and the number of corrected bits, or -1. */
int      ambe_golay23_decode(uint32_t codeword23, uint32_t *corrected);
/* Even-parity extension used by codeword 0's 24th bit. */
uint32_t ambe_golay24_encode(uint32_t data12);

/* ------------------------------------------------------------- parameters */

/*
 * Decode the 49 payload bits into MBE model parameters.  `prev` is the previous
 * frame's parameters (the spectral envelope is differentially coded) and is
 * updated in place where the algorithm requires it.
 * Returns the frame classification.
 */
ambe_frame_type ambe_decode_parms(const uint8_t ambe_d[AMBE_BITS],
                                  ambe_parms *cur, ambe_parms *prev,
                                  ambe_frame_info *info);

void ambe_init_parms(ambe_parms *cur, ambe_parms *prev, ambe_parms *prev_enh);
void ambe_move_parms(const ambe_parms *src, ambe_parms *dst);

/* -------------------------------------------------------------- synthesis */

/* Adaptive spectral amplitude enhancement, applied in place. */
void ambe_enhance_spectrum(ambe_parms *cur);

/*
 * Synthesise 160 float samples from the current and previous parameter sets.
 * `rng` carries the decoder's deterministic noise generator state.
 * uvquality selects the number of sinusoids per unvoiced band (1..64).
 */
void ambe_synthesize(float out[AMBE_PCM_SAMPLES], ambe_parms *cur,
                     ambe_parms *prev, int uvquality, uint32_t *rng);

void ambe_float_to_s16(const float in[AMBE_PCM_SAMPLES], int16_t out[AMBE_PCM_SAMPLES]);

/* ------------------------------------------------------------ top level */

ambe_decoder *ambe_decoder_create(void);
void          ambe_decoder_destroy(ambe_decoder *d);
void          ambe_decoder_reset(ambe_decoder *d);
/* uvquality defaults to 3; seed defaults to 0x2450A + frame count. */
void          ambe_decoder_set_uvquality(ambe_decoder *d, int uvquality);
void          ambe_decoder_set_seed(ambe_decoder *d, uint32_t seed);

/* Full path: 9 on-air bytes -> 160 PCM samples. */
int ambe_decode_dmr_frame(ambe_decoder *d, const uint8_t frame[AMBE_DMR_BYTES],
                          int16_t pcm[AMBE_PCM_SAMPLES], ambe_frame_info *info);

/* Payload path: 49 bits (post-decryption) -> 160 PCM samples. */
int ambe_decode_bits(ambe_decoder *d, const uint8_t ambe_d[AMBE_BITS],
                     int16_t pcm[AMBE_PCM_SAMPLES], ambe_frame_info *info);

/* Read-only view of the decoder's current parameter set (for tests). */
const ambe_parms *ambe_decoder_parms(const ambe_decoder *d);

#ifdef __cplusplus
}
#endif
#endif /* AMBE_H */
