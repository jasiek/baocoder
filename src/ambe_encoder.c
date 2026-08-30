/*
 * ambe_encoder.c - PCM -> on-air DMR AMBE frames.
 *
 * Mirrors the stock TX pipeline: Vocoder_TxTask 0x0002EC30 runs the analyser,
 * then Vocoder_DescrambleVoiceFrame 0x0001893C assembles the FEC and
 * interleaves.  Vocoder_ComputeBitAllocation 0x000188D4 is where the frame
 * geometry this uses is stated in the firmware itself: its defaults are 0x31
 * payload bits and 0x17 FEC bits totalling 0x48 - 49 + 23 = 72.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdlib.h>
#include <string.h>
#include "ambe.h"
#include "ambe_analysis_int.h"

struct ambe_encoder {
    ambe_analysis an;
    ambe_voicing  vc;
    ambe_parms    cur;
    ambe_parms    prev;
    ambe_parms    prev_enh;
};

ambe_encoder *ambe_encoder_create(void)
{
    ambe_encoder *e = (ambe_encoder *)calloc(1, sizeof(*e));
    if (e)
        ambe_encoder_reset(e);
    return e;
}

void ambe_encoder_destroy(ambe_encoder *e) { free(e); }

void ambe_encoder_reset(ambe_encoder *e)
{
    memset(&e->an, 0, sizeof(e->an));
    memset(&e->vc, 0, sizeof(e->vc));
    ambe_subband_init(&e->vc.sb);
    ambe_init_parms(&e->cur, &e->prev, &e->prev_enh);
}

int ambe_encode_bits(ambe_encoder *e, const int16_t pcm[AMBE_PCM_SAMPLES],
                     uint8_t ambe_d[AMBE_BITS], ambe_frame_info *info)
{
    ambe_frame_info local;
    long i, energy = 0;

    if (!info)
        info = &local;
    memset(info, 0, sizeof(*info));

    for (i = 0; i < AMBE_PCM_SAMPLES; i++)
        energy += (long)pcm[i] * pcm[i] / AMBE_PCM_SAMPLES;
    if (energy < 400) {                 /* below ~20 LSB rms: send silence */
        ambe_encode_silence(ambe_d);
        info->type = AMBE_FRAME_SILENCE;
        info->b[0] = 124;
        return 0;
    }

    ambe_analyse_v(&e->an, &e->vc, pcm, &e->cur);
    ambe_encode_parms(&e->cur, &e->prev, ambe_d, info);
    ambe_move_parms(&e->cur, &e->prev);
    return 0;
}

int ambe_encode_dmr_frame(ambe_encoder *e, const int16_t pcm[AMBE_PCM_SAMPLES],
                          uint8_t frame[AMBE_DMR_BYTES], ambe_frame_info *info)
{
    uint8_t d[AMBE_BITS];
    int r = ambe_encode_bits(e, pcm, d, info);
    ambe_fec_encode(d, frame);
    return r;
}
