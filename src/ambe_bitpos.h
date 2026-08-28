/*
 * Bit positions of the nine AMBE+2 2450 quantiser indices inside the 49-bit
 * payload.  Defined once in ambe_params.c and shared with the encoder so the
 * two can never drift apart.
 */
#ifndef AMBE_BITPOS_H
#define AMBE_BITPOS_H

extern const int ambe_b0_idx[7];
extern const int ambe_b1_idx[5];
extern const int ambe_b2_idx[5];
extern const int ambe_b3_idx[9];
extern const int ambe_b4_idx[7];
extern const int ambe_b5_idx[5];
extern const int ambe_b6_idx[4];
extern const int ambe_b7_idx[4];
extern const int ambe_b8_idx[3];

#endif
