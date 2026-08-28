/*
 * golay.c - Golay(23,12) and Golay(24,12) as used by AMBE+2 frame protection.
 *
 * Firmware provenance (Ghidra program DM32UV_L01_048.bin):
 *   Golay23_Decode                 0x00015230  syndrome decoder, two-stage
 *                                              weight test against 0x366/0x6cc
 *   Vocoder_ComputeParityCode      0x00022DF4  generator-matrix encoder
 *   Vocoder_ComputeParityCodeWord  0x00022E5C  wrapper: (data, nparity)
 *
 * The firmware encoder calls the wrapper with nparity = 12 for codeword 0
 * (Golay(24,12)) and nparity = 11 for codeword 1 (Golay(23,12)); see
 * Vocoder_DescrambleVoiceFrame 0x0001893C.  That fixes the code lengths without
 * having to guess them.
 *
 * The generator polynomial is g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1 = 0xC75.  It is
 * not assumed: it is the only degree-11 Golay generator that drives the residual
 * error count on real DM-32 captures to ~0.35 corrected bits/frame (the other
 * choice, 0xAE3, gives ~5.7).  See tests/test_golay.c.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"

#define GOLAY_POLY 0xC75u

/*
 * The binary Golay(23,12) code is perfect: the 2048 syndromes are in bijection
 * with the 2048 error patterns of weight <= 3 (1 + 23 + 253 + 1771 = 2048).
 * So the decoder is a single table lookup and never fails.
 */
static uint32_t golay_err[2048];
static int      golay_ready;

static uint32_t syndrome(uint32_t cw23)
{
    uint32_t r = cw23 & 0x7FFFFFu;
    int i;
    for (i = 22; i >= 11; i--) {
        if (r & (1u << i))
            r ^= GOLAY_POLY << (i - 11);
    }
    return r & 0x7FFu;
}

static void golay_init(void)
{
    int a, b, c;
    if (golay_ready)
        return;
    for (a = 0; a < 2048; a++)
        golay_err[a] = 0;
    /* weight 1..3; weight 0 leaves syndrome 0 -> pattern 0, already set */
    for (a = 0; a < 23; a++) {
        uint32_t e1 = 1u << a;
        golay_err[syndrome(e1)] = e1;
        for (b = a + 1; b < 23; b++) {
            uint32_t e2 = e1 | (1u << b);
            golay_err[syndrome(e2)] = e2;
            for (c = b + 1; c < 23; c++) {
                uint32_t e3 = e2 | (1u << c);
                golay_err[syndrome(e3)] = e3;
            }
        }
    }
    golay_ready = 1;
}

uint32_t ambe_golay23_encode(uint32_t data12)
{
    uint32_t cw = (data12 & 0xFFFu) << 11;
    int i;
    for (i = 22; i >= 11; i--) {
        if (cw & (1u << i))
            cw ^= GOLAY_POLY << (i - 11);
    }
    return ((data12 & 0xFFFu) << 11) | (cw & 0x7FFu);
}

/*
 * The extended code as the firmware builds it: Vocoder_ComputeParityCode is
 * called with nBitCount = 12, and returns (data << 12) | parity12.  Comparing
 * that against the on-air bit order shows parity12 = (golay11 << 1) | overall,
 * i.e. the eleven Golay parity bits followed by one overall-parity bit.
 */
uint32_t ambe_golay24_encode(uint32_t data12)
{
    uint32_t cw = ambe_golay23_encode(data12);
    uint32_t p  = cw;
    p ^= p >> 16;
    p ^= p >> 8;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (cw << 1) | (p & 1u);
}

int ambe_golay23_decode(uint32_t codeword23, uint32_t *corrected)
{
    uint32_t e, cw = codeword23 & 0x7FFFFFu;
    int w = 0;
    golay_init();
    e = golay_err[syndrome(cw)];
    cw ^= e;
    while (e) {
        w += (int)(e & 1u);
        e >>= 1;
    }
    if (corrected)
        *corrected = cw;
    return w;
}
