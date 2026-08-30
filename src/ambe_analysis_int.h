/*
 * ambe_analysis_int.h - the analyser's envelope-voicing state.
 *
 * Kept out of the public header because it drags in the whole sub-band
 * filterbank, and because ambe_analyse()'s signature is API.  The encoder owns
 * one of these and calls ambe_analyse_v(); ambe_analyse() is the same thing
 * with no voicing state, which keeps the old spectral rule.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef AMBE_ANALYSIS_INT_H
#define AMBE_ANALYSIS_INT_H

#include "ambe.h"
#include "ambe_subband.h"

typedef struct {
    ambe_subband sb;
    int16_t      win[AMBE_SUBBAND_HISTORY];  /* the stage's own 383-sample view */
    int          primed;
} ambe_voicing;

void ambe_analyse_v(ambe_analysis *a, ambe_voicing *v,
                    const int16_t pcm[AMBE_PCM_SAMPLES], ambe_parms *out);

#endif /* AMBE_ANALYSIS_INT_H */
