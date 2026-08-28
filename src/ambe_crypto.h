/* DMRA voice privacy helpers used by the fixture/e2e path.  See rc4.c. */
#ifndef AMBE_CRYPTO_H
#define AMBE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "ambe.h"

/* 49 payload bits are XORed, then 7 bits are skipped to stay byte aligned. */
#define AMBE_DMRA_FRAME_BITS 56
/* One superframe: 18 voice frames per message indicator. */
#define AMBE_DMRA_SUPERFRAME 18

typedef struct {
    uint8_t ks[AMBE_DMRA_FRAME_BITS * AMBE_DMRA_SUPERFRAME / 8 + 8];
    uint8_t bits[AMBE_DMRA_FRAME_BITS * AMBE_DMRA_SUPERFRAME + 64];
    int     bitpos;
} ambe_dmra_rc4;

/* AES-128/192/256, forward direction only (OFB needs nothing else). */
typedef struct {
    uint8_t rk[240];
    int     rounds;
} ambe_aes;

int  ambe_aes_init(ambe_aes *a, const uint8_t *key, int keybytes);
void ambe_aes_encrypt_block(const ambe_aes *a, uint8_t blk[16]);

/* DMRA expands a 32-bit MI into the 128-bit OFB input register. */
void ambe_dmra_expand_iv(const uint8_t mi4[4], uint8_t iv[16]);
void ambe_dmra_aes_init(ambe_dmra_rc4 *ctx, const uint8_t *key, int keybytes,
                        const uint8_t mi4[4]);

void ambe_rc4_keystream(const uint8_t *key, size_t keylen, size_t drop,
                        uint8_t *out, size_t outlen);
void ambe_dmra_rc4_init(ambe_dmra_rc4 *ctx, const uint8_t key5[5],
                        const uint8_t mi4[4]);
int  ambe_dmra_rc4_apply(ambe_dmra_rc4 *ctx, uint8_t ambe_d[AMBE_BITS]);

#endif
