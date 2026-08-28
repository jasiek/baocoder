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

#endif
