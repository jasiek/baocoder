/* 49-bit payload lines -> 18-hex on-air frame lines, via baocoder's FEC encoder. */
#include <stdio.h>
#include <string.h>
#include "ambe.h"
int main(int argc, char **argv)
{
    char line[256];
    FILE *in = fopen(argv[1], "r");
    if (!in) return 1;
    while (fgets(line, sizeof line, in)) {
        uint8_t d[AMBE_BITS], fr[AMBE_DMR_BYTES];
        int i;
        if (line[0] == '#' || strlen(line) < AMBE_BITS) continue;
        for (i = 0; i < AMBE_BITS; i++) d[i] = (uint8_t)(line[i] == '1');
        ambe_fec_encode(d, fr);
        for (i = 0; i < AMBE_DMR_BYTES; i++) printf("%02X", fr[i]);
        printf("\n");
    }
    return 0;
}
