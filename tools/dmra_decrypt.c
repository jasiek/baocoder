/*
 * dmra_decrypt - strip DMRA voice privacy from a .frames capture.
 *
 *   dmra_decrypt <frames.txt> <algid-hex> <key-hex>
 *
 * Writes one 49-bit plaintext payload per line to stdout, and a summary of the
 * frame classification to stderr.  That summary is the check that the keying is
 * right: correctly decrypted speech is full of AMBE silence descriptors
 * (b0 = 124 or 125), which random bits produce about 2 times in 128.
 *
 * ALGID 0x21 is ARC4, 0x24 is AES-128 and 0x25 is AES-256.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambe.h"
#include "ambe_crypto.h"

static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    char line[512];
    uint8_t key[32];
    int keybytes, algid, i;
    long nframes = 0, silence = 0, erasure = 0, tone = 0, keyed = 0;
    ambe_dmra_rc4 ks;
    FILE *f;
    int have_ks = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: dmra_decrypt <frames.txt> <algid-hex> <key-hex>\n");
        return 2;
    }
    algid = (int)strtol(argv[2], NULL, 16);
    keybytes = (int)strlen(argv[3]) / 2;
    if (keybytes < 1 || keybytes > 32) {
        fprintf(stderr, "key must be 1..32 bytes of hex\n");
        return 2;
    }
    for (i = 0; i < keybytes; i++)
        key[i] = (uint8_t)((hexnib(argv[3][2*i]) << 4) | hexnib(argv[3][2*i+1]));

    f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }

    while (fgets(line, sizeof(line), f)) {
        uint8_t frame[AMBE_DMR_BYTES], fr[4][24], d[AMBE_BITS];
        ambe_frame_info info;
        char mi[32];
        int b0;

        if (line[0] == '#' || strlen(line) < 18)
            continue;
        for (i = 0; i < 9; i++)
            frame[i] = (uint8_t)((hexnib(line[2*i]) << 4) | hexnib(line[2*i+1]));

        if (sscanf(line + 18, "%31s", mi) == 1 && strcmp(mi, "-") != 0) {
            uint8_t iv[4];
            for (i = 0; i < 4; i++)
                iv[i] = (uint8_t)((hexnib(mi[2*i]) << 4) | hexnib(mi[2*i+1]));
            if (algid == 0x21)
                ambe_dmra_rc4_init(&ks, key, iv);
            else
                ambe_dmra_aes_init(&ks, key, keybytes, iv);
            have_ks = 1;
            keyed++;
        }
        if (!have_ks)
            continue;

        memset(&info, 0, sizeof(info));
        ambe_dmr_deinterleave(frame, fr);
        ambe_fec_decode(fr, d, &info);
        if (ambe_dmra_rc4_apply(&ks, d) != 0) {
            fprintf(stderr, "frame %ld: keystream exhausted\n", nframes);
            break;
        }
        for (i = 0; i < AMBE_BITS; i++)
            putchar('0' + d[i]);
        putchar('\n');

        b0 = (d[0]<<6)|(d[1]<<5)|(d[2]<<4)|(d[3]<<3)|(d[37]<<2)|(d[38]<<1)|d[39];
        if (b0 >= 120 && b0 <= 123) erasure++;
        else if (b0 == 124 || b0 == 125) silence++;
        else if (b0 >= 126) tone++;
        nframes++;
    }
    fclose(f);
    fprintf(stderr, "%ld frames, %ld keystream resets: %ld silence (%.1f%%), "
                    "%ld erasure, %ld tone\n",
            nframes, keyed, silence, 100.0 * (double)silence / (double)(nframes ? nframes : 1),
            erasure, tone);
    return 0;
}
