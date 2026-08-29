/*
 * voicing_diag.c - what the voicing ratio is actually worth, per band.
 *
 * Produces the table in docs/fixed-point.md.  The analyser's voicing decision
 * is pk/tot > VOICE_NUM/100 per band; this asks the prior question of whether
 * that ratio separates voiced bands from unvoiced ones at all, and splits the
 * answer by harmonic spacing because that is where the answer comes from.
 *
 * Needs the diagnostic hook, which is not in the library by default:
 *
 *   cc -O2 -std=c99 -Iinclude -Isrc -D_DEFAULT_SOURCE -DAMBE_VOICING_DIAG \
 *      src/ambe_*.c ... tools/voicing_diag.c -lm -o voicing_diag
 *   ./voicing_diag tests/fixtures/dm32_*.ambe49
 *
 * The threshold sweep that goes with it is just tests/test_encode_voicing
 * rebuilt with -DVOICE_NUM=<n>.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <string.h>
#include "ambe.h"
#include "ambe_tables.h"
double ambe_diag_ratio[8]; int ambe_diag_valid; double ambe_diag_bph;
#define VBAND(b1,j) ((ambe_vuv_packed[((b1)<<2)&127] >> (30-2*(j))) & 1u)
int main(int argc,char**argv){
    /* mean ratio for reference-voiced vs reference-unvoiced bands, split by
       harmonic spacing (bins per harmonic) */
    double sv[3]={0,0,0}, su[3]={0,0,0}; long nv[3]={0,0,0}, nu[3]={0,0,0};
    int fi;
    for(fi=1;fi<argc;fi++){
        FILE*f=fopen(argv[fi],"r"); char bl[128];
        ambe_decoder*dec=ambe_decoder_create(); ambe_encoder*enc=ambe_encoder_create();
        if(!f) continue;
        while(fgets(bl,sizeof bl,f)){
            uint8_t d[AMBE_BITS],e[AMBE_BITS]; ambe_frame_info di,ei;
            short pcm[AMBE_PCM_SAMPLES]; int i,g;
            if(strlen(bl)<AMBE_BITS) continue;
            for(i=0;i<AMBE_BITS;i++) d[i]=(uint8_t)(bl[i]-'0');
            memset(&di,0,sizeof di); ambe_decode_bits(dec,d,pcm,&di);
            if(di.type!=AMBE_FRAME_VOICE||di.b[1]>=16) continue;
            ambe_diag_valid=0;
            memset(&ei,0,sizeof ei); ambe_encode_bits(enc,pcm,e,&ei);
            if(!ambe_diag_valid) continue;
            g = ambe_diag_bph < 4.0 ? 0 : (ambe_diag_bph < 8.0 ? 1 : 2);
            for(i=0;i<8;i++){
                if(ambe_diag_ratio[i]<0) continue;
                if(VBAND(di.b[1],i)){ sv[g]+=ambe_diag_ratio[i]; nv[g]++; }
                else                 { su[g]+=ambe_diag_ratio[i]; nu[g]++; }
            }
        }
        fclose(f); ambe_decoder_destroy(dec); ambe_encoder_destroy(enc);
    }
    printf("%-22s %10s %10s %10s\n","bins per harmonic","voiced","unvoiced","separation");
    for(int g=0;g<3;g++){
        const char*nm = g==0?"< 4 bins":(g==1?"4 - 8 bins":"> 8 bins");
        if(!nv[g]||!nu[g]){ printf("%-22s (too few)\n",nm); continue; }
        printf("%-22s %10.4f %10.4f %+10.4f   (n=%ld/%ld)\n",
               nm, sv[g]/nv[g], su[g]/nu[g], sv[g]/nv[g]-su[g]/nu[g], nv[g], nu[g]);
    }
    return 0;
}
