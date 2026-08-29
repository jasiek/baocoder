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
 * Fixed-point scales.  The codec this reimplements has no FPU: the radio runs
 * ITU-T G.191 style basic operators over a block-floating-point package in
 * IRAM, so this library is integer-only too and these are the formats it
 * carries model parameters in.
 *
 *   f0      Q19 turns per sample - the firmware's own scale, the one
 *           Vocoder_PitchFromLog2 0x0002AD6C emits and clamps
 *   log2    Q24 - gamma and log2Ml.  Measured over 2052 real frames these
 *           span [-7.6, 11.5], so Q24's +-128 leaves an order of magnitude of
 *           headroom at a resolution ten times finer than float has there
 *   phase   Q32 turns, in a wrapping uint32.  A turn is the whole range, so
 *           the accumulator is exact modulo one turn for ever - unlike a
 *           float, which loses mantissa bits as the phase grows
 */
#define AMBE_Q_F0    19
#define AMBE_Q_LOG   24
#define AMBE_Q_ML    31   /* Ml[l] * 2^(Ml_exp - AMBE_Q_ML) is the amplitude */

/*
 * The MBE speech model parameters for one 20 ms frame.  Index 0 is unused for
 * the per-harmonic arrays; harmonics run 1..L, matching the AMBE literature.
 *
 * The spectral amplitudes are block floating point - one exponent shared by
 * the whole frame, which is how the radio carries spectral data
 * (Vocoder_NormalizeSpectralBlock 0x00022C18 normalises an array to a common
 * exponent).  The amplitude of harmonic l is
 *
 *     Ml[l] * 2^(Ml_exp - AMBE_Q_ML)
 *
 * The radio's own spectral arrays are int16; these mantissas are int32.  That
 * is a deliberate departure, and it is measured rather than assumed: over the
 * 1664 real voice frames in the corpus the loudest-to-quietest amplitude
 * spread within a single frame reaches 13.5 bits, so a 15-bit mantissa would
 * leave the quietest harmonic in a frame under two bits.
 */
typedef struct {
    int32_t  f0;                             /* fundamental, Q19 turns/sample */
    int      L;                              /* number of harmonics          */
    int32_t  gamma;                          /* frame log2 gain, Q24         */
    uint8_t  Vl[AMBE_MAX_HARMONICS + 1];     /* 1 = voiced, 0 = unvoiced     */
    int32_t  Ml[AMBE_MAX_HARMONICS + 1];     /* amplitude mantissas          */
    int      Ml_exp;                         /* their shared exponent        */
    int32_t  log2Ml[AMBE_MAX_HARMONICS + 1]; /* log2 amplitudes, Q24         */
    uint32_t PHIl[AMBE_MAX_HARMONICS + 1];   /* synthesis phase, Q32 turns   */
    uint32_t PSIl[AMBE_MAX_HARMONICS + 1];   /* accumulated phase, Q32 turns */
    int      repeat;                         /* consecutive repeated frames  */
} ambe_parms;

/*
 * The fundamental as an angular frequency, 2*pi*f0 in Q24 radians per sample.
 * The library works in turns throughout - a phase accumulator in turns wraps
 * for free - and only offers this because the MBE literature and every other
 * implementation quote w0.
 */
int32_t ambe_w0_q24(const ambe_parms *p);

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

/*
 * Fundamental frequency and harmonic count for a b0 index, computed by the
 * firmware's own closed-form law rather than a table.  `width` is the width of
 * the b0 field: 7 for the DMR 2450 mode.  f0 comes back in Q19 turns per
 * sample, the scale Vocoder_PitchFromLog2 0x0002AD6C emits.
 */
void ambe_pitch_from_b0(int b0, int width, int32_t *f0_q19, int *L);

void ambe_init_parms(ambe_parms *cur, ambe_parms *prev, ambe_parms *prev_enh);
void ambe_move_parms(const ambe_parms *src, ambe_parms *dst);

/* -------------------------------------------------------------- synthesis */

/* Adaptive spectral amplitude enhancement, applied in place. */
void ambe_enhance_spectrum(ambe_parms *cur);

/*
 * Synthesise 160 samples from the current and previous parameter sets.
 * `rng` carries the decoder's deterministic noise generator state.
 * uvquality selects the number of sinusoids per unvoiced band (1..64).
 *
 * The output is int16 PCM directly: the overlap-add window's taps are all
 * k/50 for integer k, so the 1/50 factors out of the harmonic sum entirely
 * and is applied once, with the codec's x7 output gain, as a single x7/50
 * scaling here.  There is no separate float-to-int16 step to lose anything.
 */
void ambe_synthesize(int16_t out[AMBE_PCM_SAMPLES], ambe_parms *cur,
                     ambe_parms *prev, int uvquality, uint32_t *rng);

/* --------------------------------------------------------------- encoder */

/*
 * Quantise a model parameter set into the nine AMBE indices and pack them into
 * the 49-bit payload.  Exact inverse of ambe_decode_parms: `prev` must carry
 * the same previous-frame state the decoder would have, since the spectral
 * envelope is differentially coded.  `prev` is updated the same way.
 * `info->b[]` receives the chosen indices.  Returns 0.
 */
int ambe_encode_parms(const ambe_parms *cur, ambe_parms *prev,
                      uint8_t ambe_d[AMBE_BITS], ambe_frame_info *info);

/* Emit the silence descriptor (b0 = 124) instead of a voice frame. */
void ambe_encode_silence(uint8_t ambe_d[AMBE_BITS]);

/*
 * Analysis: 160 PCM samples at 8 kHz -> model parameters.  `hist` holds the
 * analyser's overlap state and must be zeroed before the first frame.
 */
#define AMBE_ANALYSIS_HISTORY 256

typedef struct {
    int16_t win[AMBE_ANALYSIS_HISTORY];  /* sliding input window */
    int     primed;
} ambe_analysis;

void ambe_analyse(ambe_analysis *a, const int16_t pcm[AMBE_PCM_SAMPLES],
                  ambe_parms *out);

typedef struct ambe_encoder ambe_encoder;

ambe_encoder *ambe_encoder_create(void);
void          ambe_encoder_destroy(ambe_encoder *e);
void          ambe_encoder_reset(ambe_encoder *e);

/* 160 PCM samples -> 49 payload bits. */
int ambe_encode_bits(ambe_encoder *e, const int16_t pcm[AMBE_PCM_SAMPLES],
                     uint8_t ambe_d[AMBE_BITS], ambe_frame_info *info);

/* 160 PCM samples -> 9 on-air DMR bytes. */
int ambe_encode_dmr_frame(ambe_encoder *e, const int16_t pcm[AMBE_PCM_SAMPLES],
                          uint8_t frame[AMBE_DMR_BYTES], ambe_frame_info *info);

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
