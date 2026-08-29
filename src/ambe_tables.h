/*
 * Declarations for the AMBE+2 quantiser tables.
 *
 * The split is deliberate and is the project's provenance boundary:
 *
 *   ambe_tables_fw.c          extracted verbatim from the DM-32UV firmware
 *                             image by tools/extract_tables.py.  Signed 16-bit
 *                             Q11 - divide by AMBE_Q11 for the real value.
 *
 * There is no longer a second file: every table the decoder reads comes out of
 * the firmware image, and the pitch/harmonic-count pair is computed by the
 * firmware's own closed-form law (see ambe_pitch_from_b0 in ambe_params.c).
 */
#ifndef AMBE_TABLES_H
#define AMBE_TABLES_H

/* Fixed-point scale of every _q11 table below. */
#define AMBE_Q11 2048.0f

/* --- from the firmware image ------------------------------------------- */
extern const short ambe_prba24_q11[1536];   /* [512][3] SRAM 0x18002090 */
extern const short ambe_prba58_q11[512];    /* [128][4] SRAM 0x18002C90 */
extern const short ambe_hoc_b5_q11[128];    /* [32][4]  SRAM 0x18003090 */
extern const short ambe_hoc_b6_q11[64];     /* [16][4]  SRAM 0x18003190 */
extern const short ambe_hoc_b7_q11[64];     /* [16][4]  SRAM 0x18003210 */
extern const short ambe_hoc_b8_q11[32];     /* [8][4]   SRAM 0x18003290 */
extern const short ambe_dg_q11[32];         /* gain     SRAM 0x180038B4 */
extern const short ambe_lmprbl[228];        /* [57][4]  SRAM 0x18002030, unpacked */
extern const unsigned int ambe_vuv_packed[128]; /* voicing SRAM 0x18003628 */

/* --- the fixed-point math primitives' coefficients ----------------------
 *
 * The radio has no FPU: its vocoder runs ITU-T G.191 style basic operators
 * over a block-floating-point package in IRAM (Math_FloatAdd 0x00018DD8 and
 * friends, whose 50 callers are the vocoder).  These are those operators'
 * coefficients, from the same SRAM window as the codebooks above and reached
 * through each function's literal pool.  Signed 16-bit Q15.
 *
 * 0x18001600..0x1800182F is contiguous, and the values identify themselves:
 * log2's leading term is 1/sqrt(2), pow2's fifth is ln2, and the 512-entry
 * table is cosine to within one LSB at every point.
 */
#define AMBE_Q15 32768.0f

extern const short ambe_log2_coeff_q15[6];   /* Math_Log2 0x0001903C     */
extern const short ambe_pow2_coeff_q15[6];   /* Math_Pow2 0x000191C0     */
extern const short ambe_sqrt_coeff_q15[12];  /* Math_Sqrt 0x00019364     */
extern const short ambe_cos512_q15[512];     /* Math_TableInterpLookup   */

/*
 * The speech analyser's window, SRAM 0x180010A8.  Vocoder_ProcessFrame
 * 0x00016E04 - which takes PCM in, and is the encoder front end despite
 * sitting under a task named Vocoder_RxTask - hands this to
 * Dsp_WindowAndComputeFft 0x00019B6C with nInLen = 199 and a 256-point
 * transform.  Only half is stored; the stock code folds it symmetrically.
 * It is a Hamming window (tests/test_tables.c asserts that), peak 29883.
 */
#define AMBE_ANWIN_N    199   /* the window's full length          */
#define AMBE_ANWIN_PEAK 29883 /* its centre tap, the table's scale  */
extern const short ambe_anwin_q15[100];

#endif
