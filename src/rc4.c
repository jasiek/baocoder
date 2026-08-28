/*
 * rc4.c - RC4 keystream, and the DMRA "Enhanced Privacy" (ALGID 0x21) voice
 * keying built on top of it.
 *
 * The DM-32UV implements voice privacy itself; the stock RC4 path lives around
 * PrivacyKey_DecodeAndVerifyId 0x00015CD4 and the AES tables at file 0x06485C /
 * 0x06495C.  This module exists so that the encrypted DM-32 capture used as a
 * test fixture can be decoded end to end; it is not on the plain-voice path.
 *
 * DMRA EP keying: RC4 is keyed with the 5-byte traffic key followed by the
 * 4-byte message indicator, the first 256 keystream bytes are discarded, and
 * each 20 ms voice frame consumes 56 bits (49 payload bits XORed onto the AMBE
 * payload, then 7 bits skipped to stay byte-aligned).  The MI is re-seeded at
 * every superframe boundary, i.e. every 18 voice frames.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe_crypto.h"

void ambe_rc4_keystream(const uint8_t *key, size_t keylen, size_t drop,
                        uint8_t *out, size_t outlen)
{
    uint8_t S[256];
    size_t n;
    int i = 0, j = 0, t;

    for (n = 0; n < 256; n++)
        S[n] = (uint8_t)n;
    for (n = 0; n < 256; n++) {
        j = (j + S[n] + key[n % keylen]) & 0xFF;
        t = S[n]; S[n] = S[j]; S[j] = (uint8_t)t;
    }
    j = 0;
    for (n = 0; n < outlen + drop; n++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        t = S[i]; S[i] = S[j]; S[j] = (uint8_t)t;
        if (n >= drop)
            out[n - drop] = S[(S[i] + S[j]) & 0xFF];
    }
}

void ambe_dmra_rc4_init(ambe_dmra_rc4 *ctx, const uint8_t key5[5],
                        const uint8_t mi4[4])
{
    uint8_t kiv[9];
    size_t i;

    memcpy(kiv, key5, 5);
    memcpy(kiv + 5, mi4, 4);
    ambe_rc4_keystream(kiv, sizeof(kiv), 256, ctx->ks, sizeof(ctx->ks));
    ctx->bitpos = 0;
    for (i = 0; i < sizeof(ctx->bits); i++)
        ctx->bits[i] = (ctx->ks[i >> 3] >> (7 - (i & 7))) & 1u;
}

int ambe_dmra_rc4_apply(ambe_dmra_rc4 *ctx, uint8_t ambe_d[AMBE_BITS])
{
    int i;
    if (ctx->bitpos + AMBE_DMRA_FRAME_BITS > (int)sizeof(ctx->bits))
        return -1;
    for (i = 0; i < AMBE_BITS; i++)
        ambe_d[i] ^= ctx->bits[ctx->bitpos + i];
    ctx->bitpos += AMBE_DMRA_FRAME_BITS;
    return 0;
}
