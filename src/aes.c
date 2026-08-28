/*
 * aes.c - AES-128/192/256 block encryption, for the DMRA voice-privacy modes.
 *
 * Only the forward direction is needed: DMRA runs AES in OFB, so decryption is
 * the same keystream XOR as encryption.  Correctness is established by the
 * FIPS-197 known-answer vectors in tests/test_aes.c rather than by agreement
 * with anything in this project.
 *
 * The DM-32UV implements AES voice privacy itself - the forward and inverse
 * S-boxes are at file 0x06485C and 0x06495C - but this module is not
 * transcribed from it; it exists so the encrypted DM-32 captures used as test
 * corpora can be decoded.
 *
 * SPDX-License-Identifier: ISC
 */
#include <string.h>
#include "ambe_crypto.h"

static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

int ambe_aes_init(ambe_aes *a, const uint8_t *key, int keybytes)
{
    int nk, i, j;
    uint8_t t[4], rcon = 1;

    if (keybytes == 16)      { nk = 4;  a->rounds = 10; }
    else if (keybytes == 24) { nk = 6;  a->rounds = 12; }
    else if (keybytes == 32) { nk = 8;  a->rounds = 14; }
    else                     return -1;

    memcpy(a->rk, key, (size_t)keybytes);
    for (i = nk; i < 4 * (a->rounds + 1); i++) {
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[tmp];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (j = 0; j < 4; j++)
                t[j] = SBOX[t[j]];
        }
        for (j = 0; j < 4; j++)
            a->rk[4 * i + j] = (uint8_t)(a->rk[4 * (i - nk) + j] ^ t[j]);
    }
    return 0;
}

void ambe_aes_encrypt_block(const ambe_aes *a, uint8_t blk[16])
{
    int round, c, i;
    uint8_t s[16], tmp;

    memcpy(s, blk, 16);
    for (i = 0; i < 16; i++)
        s[i] ^= a->rk[i];

    for (round = 1; round <= a->rounds; round++) {
        for (i = 0; i < 16; i++)
            s[i] = SBOX[s[i]];

        /* ShiftRows: byte i is row i%4, column i/4 */
        tmp = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = tmp;
        tmp = s[2];  s[2] = s[10]; s[10] = tmp;
        tmp = s[6];  s[6] = s[14]; s[14] = tmp;
        tmp = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = tmp;

        if (round != a->rounds) {
            for (c = 0; c < 4; c++) {
                uint8_t *p = s + 4 * c;
                uint8_t all = (uint8_t)(p[0] ^ p[1] ^ p[2] ^ p[3]);
                uint8_t a0 = p[0];
                p[0] ^= all ^ xtime((uint8_t)(p[0] ^ p[1]));
                p[1] ^= all ^ xtime((uint8_t)(p[1] ^ p[2]));
                p[2] ^= all ^ xtime((uint8_t)(p[2] ^ p[3]));
                p[3] ^= all ^ xtime((uint8_t)(p[3] ^ a0));
            }
        }
        for (i = 0; i < 16; i++)
            s[i] ^= a->rk[16 * round + i];
    }
    memcpy(blk, s, 16);
}

/*
 * DMRA expands the 32-bit message indicator into the 128-bit OFB input register
 * with an LFSR whose taps are {32, 22, 2, 1}: the MI occupies the first four
 * bytes and the following 96 generated bits fill the remaining twelve.
 */
void ambe_dmra_expand_iv(const uint8_t mi4[4], uint8_t iv[16])
{
    uint64_t lfsr = ((uint64_t)mi4[0] << 24) | ((uint64_t)mi4[1] << 16) |
                    ((uint64_t)mi4[2] << 8)  | (uint64_t)mi4[3];
    int cnt, x = 32;

    memset(iv, 0, 16);
    iv[0] = mi4[0]; iv[1] = mi4[1]; iv[2] = mi4[2]; iv[3] = mi4[3];
    for (cnt = 0; cnt < 96; cnt++) {
        uint64_t bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ lfsr) & 1u;
        lfsr = (lfsr << 1) | bit;
        iv[x / 8] = (uint8_t)((iv[x / 8] << 1) + (uint8_t)bit);
        x++;
    }
}

void ambe_dmra_aes_init(ambe_dmra_rc4 *ctx, const uint8_t *key, int keybytes,
                        const uint8_t mi4[4])
{
    ambe_aes a;
    uint8_t iv[16];
    uint8_t ks[16 * 16];
    size_t i;

    ambe_dmra_expand_iv(mi4, iv);
    ambe_aes_init(&a, key, keybytes);

    /* OFB: the register is repeatedly enciphered and is itself the keystream */
    for (i = 0; i < 16; i++) {
        ambe_aes_encrypt_block(&a, iv);
        memcpy(ks + 16 * i, iv, 16);
    }

    /* the first block is discarded before the voice payload is keyed */
    for (i = 0; i < sizeof(ctx->bits); i++)
        ctx->bits[i] = (ks[16 + (i >> 3)] >> (7 - (i & 7))) & 1u;
    ctx->bitpos = 0;
}
