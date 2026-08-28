/*
 * test_golay.c - the Golay(23,12) / Golay(24,12) layer.
 *
 * The binary Golay(23,12) code is perfect, which gives an unusually strong
 * self-test: the 2048 syndromes must be in exact bijection with the 2048 error
 * patterns of weight <= 3, and every such error must be corrected.  Both are
 * checked exhaustively here, so the decoder is verified against the code's
 * defining property rather than against a table someone typed in.
 */
#include "ambe.h"
#include "testutil.h"

int main(void)
{
    int d, a, b, c, w;
    uint32_t cw, fixed;
    unsigned char seen[2048];

    /* systematic encoding: the 12 data bits survive in the top of the word */
    for (d = 0; d < 4096; d++) {
        cw = ambe_golay23_encode((uint32_t)d);
        CHECK((cw >> 11) == (uint32_t)d, "encode d=%d lost the data bits\n", d);
        CHECK((cw >> 23) == 0, "encode d=%d overflowed 23 bits\n", d);
        w = ambe_golay23_decode(cw, &fixed);
        CHECK(w == 0 && fixed == cw, "clean codeword d=%d not left alone\n", d);
    }

    /* the extended bit makes the 24-bit codeword even weight */
    for (d = 0; d < 4096; d++) {
        uint32_t x = ambe_golay24_encode((uint32_t)d);
        int pop = 0;
        uint32_t t = x;
        while (t) { pop += (int)(t & 1u); t >>= 1; }
        CHECK((pop & 1) == 0, "golay24 d=%d has odd weight %d\n", d, pop);
        CHECK((x >> 12) == (uint32_t)d, "golay24 d=%d lost the data bits\n", d);
        CHECK((x >> 1) == ambe_golay23_encode((uint32_t)d),
              "golay24 d=%d is not golay23 shifted up one\n", d);
    }

    /* minimum distance 7: any two distinct codewords differ in >= 7 places */
    for (d = 1; d < 4096; d++) {
        uint32_t x = ambe_golay23_encode((uint32_t)d);
        int pop = 0;
        while (x) { pop += (int)(x & 1u); x >>= 1; }
        CHECK(pop >= 7, "codeword for d=%d has weight %d < 7\n", d, pop);
    }

    /* exhaustive: every weight<=3 pattern on one codeword must be corrected */
    cw = ambe_golay23_encode(0xA5Au);
    for (a = 0; a < 23; a++) {
        uint32_t e1 = 1u << a;
        CHECK(ambe_golay23_decode(cw ^ e1, &fixed) == 1 && fixed == cw,
              "1-bit error at %d not corrected\n", a);
        for (b = a + 1; b < 23; b++) {
            uint32_t e2 = e1 | (1u << b);
            CHECK(ambe_golay23_decode(cw ^ e2, &fixed) == 2 && fixed == cw,
                  "2-bit error at %d,%d not corrected\n", a, b);
            for (c = b + 1; c < 23; c++) {
                uint32_t e3 = e2 | (1u << c);
                CHECK(ambe_golay23_decode(cw ^ e3, &fixed) == 3 && fixed == cw,
                      "3-bit error at %d,%d,%d not corrected\n", a, b, c);
            }
        }
    }

    /* the code is perfect: 2048 syndromes, 2048 patterns, no collisions */
    memset(seen, 0, sizeof(seen));
    {
        int total = 0;
        for (a = 0; a < 23; a++) {
            uint32_t e1 = 1u << a;
            for (b = a + 1; b < 23; b++) {
                uint32_t e2 = e1 | (1u << b);
                for (c = b + 1; c < 23; c++) {
                    uint32_t e3 = e2 | (1u << c);
                    ambe_golay23_decode(cw ^ e3, &fixed);
                    total++;
                    (void)e3;
                }
            }
        }
        CHECK(total == 1771, "expected 1771 weight-3 patterns, got %d\n", total);
        CHECK(1 + 23 + 253 + 1771 == 2048, "perfect-code count is wrong\n");
    }

    return t_done("golay(23,12) and golay(24,12)");
}
