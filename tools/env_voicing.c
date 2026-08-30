/*
 * env_voicing.c - the firmware's voicing feature, measured on the real corpus.
 *
 * docs/fixed-point.md records a sweep concluding that the analyser's voicing
 * cannot be fixed by a better spectrum.  That sweep used a longer window on the
 * ordinary speech spectrum as a proxy for the radio's filterbank, and the proxy
 * was wrong: the filterbank's channel signals are magnitudes, so the eight-band
 * loop transforms *envelopes*, and what it measures is envelope periodicity at
 * 1000/64 = 15.625 Hz per bin - a different quantity in a different domain.
 *
 * This runs the real thing.  For each frame of a capture it drives the
 * assembled stage (ambe_subband_process twice, as the radio does, then
 * ambe_band_analyse's per-band spectra) and prints, per voicing band, how much
 * of that band's envelope-modulation energy sits on the pitch's harmonic grid.
 * Score the output with tools/proto_voicing.py's machinery.
 *
 *   cc -O2 -std=c99 -Iinclude -Isrc tools/env_voicing.c libbaocoder.a -lm \
 *      -o env_voicing
 *   ./env_voicing <base>.pcm <base>.meta        # from tools/dump_corpus.c
 *
 * One line per frame:  <index> <b1> <fundamental in bins> <8 ratios> <pattern>
 *
 * The bin scale is the measured one rather than the firmware's Q-format
 * constant: tests/test_subband.c confirms the voiced peak lands at
 * f0 / 15.625 Hz, and deriving the grid from the stock code's
 * `step = pitch * 128` would mean trusting a conversion chain this project has
 * not verified.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ambe.h"
#include "ambe_subband.h"

#define BINHZ (1000.0/64.0)     /* channel rate 1000 Hz, 64-point transform */

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <base.pcm> <base.meta>\n",argv[0]);return 2;}
    FILE*fp=fopen(argv[1],"rb"); FILE*fm=fopen(argv[2],"r");
    if(!fp||!fm){fprintf(stderr,"open failed\n");return 1;}
    fseek(fp,0,SEEK_END); long ns=ftell(fp)/2; fseek(fp,0,SEEK_SET);
    int16_t*pcm=malloc(ns*2); fread(pcm,2,ns,fp); fclose(fp);

    ambe_subband st; ambe_subband_init(&st);
    int16_t hist[AMBE_SUBBAND_HISTORY]; memset(hist,0,sizeof hist);
    int32_t en[16]; short bexp[8];
    char line[256]; long pos=0;

    while(fgets(line,sizeof line,fm)){
        int idx,L,b1; long f0q; char vp[16];
        if(sscanf(line,"%d %ld %d %d %15s",&idx,&f0q,&L,&b1,vp)!=5) continue;
        /* two 80-sample calls per 160-sample frame, as the radio does */
        int ok=0;
        for(int half=0; half<2; half++){
            memmove(hist,hist+80,(AMBE_SUBBAND_HISTORY-80)*sizeof(int16_t));
            for(int i=0;i<80;i++){
                long k=pos+i;
                hist[AMBE_SUBBAND_HISTORY-80+i] = (k<ns)?pcm[k]:0;
            }
            pos+=80;
            ambe_subband_process(&st,hist,80,en);
            if(half==1){
                /* per-band 32-bin spectra, before the overlap-add */
                int16_t seg[AMBE_BAND_SEG];
                int32_t a[32],b[32],band[32];
                if(b1>=0){
                    double f0=(double)f0q/(double)(1L<<19);       /* turns/sample */
                    double fund = f0*8000.0/BINHZ;                /* bins */
                    printf("%d %d %.4f",idx,b1,fund);
                    for(int bd=0;bd<8;bd++){
                        short ea,eb;
                        if(!ambe_subband_segment(&st,2*bd,seg)){printf(" -1");continue;}
                        ea=ambe_band_spectrum(a,seg,0);
                        ambe_subband_segment(&st,2*bd+1,seg);
                        eb=ambe_band_spectrum(b,seg,0);
                        ambe_band_add(band,a,ea,b,eb);
                        double tot=0,harm=0;
                        for(int k=2;k<32;k++) tot+=(double)band[k];
                        for(int h=1;h*fund<32.0;h++){
                            double c=h*fund;
                            for(int k=2;k<32;k++)
                                if(fabs(k-c)<=fund/4.0) harm+=(double)band[k];
                        }
                        printf(" %.5f", tot>0?harm/tot:-1.0);
                    }
                    printf(" %s\n",vp);
                    ok=1;
                }
            }
            ambe_subband_commit(&st);
        }
        (void)ok; (void)L;
    }
    fclose(fm); free(pcm);
    return 0;
}
