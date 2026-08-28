/*
 * test_aes.c - AES against the FIPS-197 known-answer vectors.
 *
 * The DMRA AES modes are only useful here if the cipher underneath them is
 * right, and that has to be established against the standard rather than
 * against anything else in this project.  These are the Appendix B worked
 * example and the Appendix C vectors for all three key lengths, verbatim.
 *
 * The IV expansion is checked separately for its structural property: the MI
 * occupies the first four bytes unchanged, and the LFSR that fills the other
 * twelve must not be degenerate.
 */
#include "ambe.h"
#include "ambe_crypto.h"
#include "testutil.h"

static void hexbytes(const char *h, uint8_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)((hexnib(h[2 * i]) << 4) | hexnib(h[2 * i + 1]));
}

static void kat(const char *keyh, int keybytes, const char *pth, const char *cth,
                const char *label)
{
    uint8_t key[32], blk[16], want[16];
    ambe_aes a;
    int i;
    hexbytes(keyh, key, keybytes);
    hexbytes(pth, blk, 16);
    hexbytes(cth, want, 16);
    CHECK(ambe_aes_init(&a, key, keybytes) == 0, "%s: key setup failed\n", label);
    ambe_aes_encrypt_block(&a, blk);
    for (i = 0; i < 16; i++)
        CHECK(blk[i] == want[i], "%s: byte %d = %02X, want %02X\n",
              label, i, blk[i], want[i]);
}

int main(void)
{
    /* FIPS-197 Appendix B */
    kat("2b7e151628aed2a6abf7158809cf4f3c", 16,
        "3243f6a8885a308d313198a2e0370734",
        "3925841d02dc09fbdc118597196a0b32", "FIPS-197 B (AES-128)");
    /* FIPS-197 Appendix C.1 / C.2 / C.3 */
    kat("000102030405060708090a0b0c0d0e0f", 16,
        "00112233445566778899aabbccddeeff",
        "69c4e0d86a7b0430d8cdb78070b4c55a", "FIPS-197 C.1 (AES-128)");
    kat("000102030405060708090a0b0c0d0e0f1011121314151617", 24,
        "00112233445566778899aabbccddeeff",
        "dda97ca4864cdfe06eaf70a0ec0d7191", "FIPS-197 C.2 (AES-192)");
    kat("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 32,
        "00112233445566778899aabbccddeeff",
        "8ea2b7ca516745bfeafc49904b496089", "FIPS-197 C.3 (AES-256)");

    {
        ambe_aes a;
        uint8_t key[16];
        CHECK(ambe_aes_init(&a, key, 17) != 0, "17-byte key was accepted\n");
        CHECK(ambe_aes_init(&a, key, 0) != 0, "empty key was accepted\n");
    }

    /* the DMRA IV expansion: MI in front, LFSR behind, and not degenerate */
    {
        uint8_t mi[4] = { 0xAA, 0x6A, 0x4E, 0x7F };
        uint8_t iv[16], iv2[16];
        uint8_t zero[4] = { 0, 0, 0, 0 };
        int i, ones = 0, differ = 0;
        ambe_dmra_expand_iv(mi, iv);
        for (i = 0; i < 4; i++)
            CHECK(iv[i] == mi[i], "iv[%d] = %02X, expected the MI byte %02X\n",
                  i, iv[i], mi[i]);
        for (i = 4; i < 16; i++) {
            int b = iv[i], k;
            for (k = 0; k < 8; k++) ones += (b >> k) & 1;
        }
        CHECK(ones > 20 && ones < 76,
              "expanded IV has %d set bits of 96 - the LFSR looks degenerate\n", ones);
        /* an all-zero MI is a fixed point of this LFSR; anything else must move */
        ambe_dmra_expand_iv(zero, iv2);
        for (i = 4; i < 16; i++)
            if (iv[i] != iv2[i]) differ++;
        CHECK(differ > 6, "two different MIs produced %d differing IV bytes\n", differ);
        for (i = 0; i < 16; i++)
            CHECK(iv2[i] == 0, "an all-zero MI should stay zero, iv2[%d]=%02X\n",
                  i, iv2[i]);
    }

    return t_done("aes: FIPS-197 vectors and the DMRA IV expansion");
}
