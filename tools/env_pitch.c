/*
 * env_pitch.c - can the filterbank's envelope spectrum estimate pitch?
 *
 * It cannot, and this is the tool that settled it.  The envelope spectrum's
 * peak *is* the fundamental - tests/test_subband.c confirms that on synthetic
 * signals - so replacing the analyser's normalised-cross-correlation search
 * with it looked like a way to kill the octave errors that search makes.
 *
 * Measured against the transmitted pitch over the corpus it is much worse:
 * 68.8% within four quantiser steps against the NCC's 86.3%, and 34.0% within
 * one against 60.6%.  Worse, the two are not complementary - on the 153 frames
 * where the NCC is off by more than four steps, this is within four on 3.9% of
 * them.  An oracle picking the better of the two every frame reaches 86.8%
 * against 86.3%, so even a perfect combiner is worth half a point.
 *
 * Kept so the result is reproducible rather than a claim.
 *
 *   cc -O2 -std=c99 -Iinclude -Isrc tools/env_pitch.c libbaocoder.a -lm \
 *      -o env_pitch
 *   ./env_pitch <base>.pcm <base>.meta     # from tools/dump_corpus.c
 *
 * One line per frame: <index> <true Hz> <envelope Hz> <true b0> <envelope b0>
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ambe.h"
#include "ambe_subband.h"
#define BINHZ (1000.0/64.0)

/* nearest b0 whose f0 matches a target frequency in Hz */
static int b0_of_hz(double hz){
    int best=0; double bd=1e18;
    for(int b=0;b<120;b++){
        int32_t cf; int cl; ambe_pitch_from_b0(b,7,&cf,&cl);
        double f=(double)cf/(double)(1L<<19)*8000.0;
        if(fabs(f-hz)<bd){bd=fabs(f-hz);best=b;}
    }
    return best;
}

int main(int argc,char**argv){
    FILE*fp=fopen(argv[1],"rb"); FILE*fm=fopen(argv[2],"r");
    fseek(fp,0,SEEK_END); long ns=ftell(fp)/2; fseek(fp,0,SEEK_SET);
    int16_t*pcm=malloc(ns*2); fread(pcm,2,ns,fp); fclose(fp);
    ambe_subband st; ambe_subband_init(&st);
    int16_t hist[AMBE_SUBBAND_HISTORY]; memset(hist,0,sizeof hist);
    int32_t en[16],spec[128]; short bexp[8];
    char line[256]; long pos=0;
    while(fgets(line,sizeof line,fm)){
        int idx,L,b1; long f0q; char vp[16];
        if(sscanf(line,"%d %ld %d %d %15s",&idx,&f0q,&L,&b1,vp)!=5) continue;
        int ok=0;
        for(int half=0;half<2;half++){
            memmove(hist,hist+80,(AMBE_SUBBAND_HISTORY-80)*sizeof(int16_t));
            for(int i=0;i<80;i++){long k=pos+i; hist[AMBE_SUBBAND_HISTORY-80+i]=(k<ns)?pcm[k]:0;}
            pos+=80;
            ambe_subband_process(&st,hist,80,en);
            if(half==1) ok=ambe_band_analyse(&st,spec,bexp);
            ambe_subband_commit(&st);
        }
        if(!ok||b1<0) continue;
        /* sum the eight bands' own 32-bin spectra, then take the peak */
        double acc[32]={0};
        for(int bd=0;bd<8;bd++)
            for(int k=0;k<32;k++){
                int at=bd*16+k; if(at<128) acc[k]+=(double)spec[at];
            }
        int pk=4; for(int k=4;k<26;k++) if(acc[k]>acc[pk]) pk=k;
        /* parabolic interpolation on the peak, for sub-bin resolution */
        double y0=acc[pk-1],y1=acc[pk],y2=acc[pk+1];
        double d=(y0-y2)/(2*(y0-2*y1+y2)); if(!(d>-1&&d<1)) d=0;
        double hz=(pk+d)*BINHZ;
        double truehz=(double)f0q/(double)(1L<<19)*8000.0;
        printf("%d %.2f %.2f %d %d\n",idx,truehz,hz,b0_of_hz(truehz),b0_of_hz(hz));
    }
    return 0;
}
