/*
 * ambe_decoder.c - frame-to-PCM driver and inter-frame state.
 *
 * Mirrors the stock RX pipeline: Vocoder_RxTask 0x0002E5A0 runs
 * Vocoder_ProcessFrameFec 0x00016F3C -> Vocoder_DecodeFrameParameters
 * 0x0001994C -> Vocoder_SynthesizeFrame 0x00019DB8, and falls back to the
 * previous frame's parameters when the FEC reports too many corrections
 * (Vocoder_DetectFrameErasure 0x0001A4C8, Vocoder_BumpErrorCounter 0x000220C0).
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdlib.h>
#include <string.h>
#include "ambe.h"

struct ambe_decoder {
    ambe_parms cur;
    ambe_parms prev;
    ambe_parms prev_enh;
    int        uvquality;
    uint32_t   rng;
};

ambe_decoder *ambe_decoder_create(void)
{
    ambe_decoder *d = (ambe_decoder *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->uvquality = 3;
    ambe_decoder_reset(d);
    return d;
}

void ambe_decoder_destroy(ambe_decoder *d)
{
    free(d);
}

void ambe_decoder_reset(ambe_decoder *d)
{
    ambe_init_parms(&d->cur, &d->prev, &d->prev_enh);
    d->rng = 0x2450A17Bu;
}

void ambe_decoder_set_uvquality(ambe_decoder *d, int uvquality)
{
    d->uvquality = (uvquality < 1 || uvquality > 64) ? 3 : uvquality;
}

void ambe_decoder_set_seed(ambe_decoder *d, uint32_t seed)
{
    d->rng = seed ? seed : 0x2450A17Bu;
}

const ambe_parms *ambe_decoder_parms(const ambe_decoder *d)
{
    return &d->cur;
}

static void use_last_parms(ambe_parms *cur, const ambe_parms *prev)
{
    ambe_move_parms(prev, cur);
}

int ambe_decode_bits(ambe_decoder *d, const uint8_t ambe_d[AMBE_BITS],
                     int16_t pcm[AMBE_PCM_SAMPLES], ambe_frame_info *info)
{
    ambe_frame_info local;
    ambe_frame_type type;
    float buf[AMBE_PCM_SAMPLES];
    int errs2;

    if (!info) {
        memset(&local, 0, sizeof(local));
        info = &local;
    }
    errs2 = info->errs_c0 + info->errs_c1;

    type = ambe_decode_parms(ambe_d, &d->cur, &d->prev, info);

    if (type == AMBE_FRAME_ERASURE || type == AMBE_FRAME_TONE) {
        d->cur.repeat = 0;
    } else if (errs2 > 3) {
        use_last_parms(&d->cur, &d->prev);
        d->cur.repeat++;
    } else {
        d->cur.repeat = 0;
    }

    if (type == AMBE_FRAME_VOICE || type == AMBE_FRAME_SILENCE) {
        if (d->cur.repeat <= 3) {
            ambe_move_parms(&d->cur, &d->prev);
            ambe_enhance_spectrum(&d->cur);
            ambe_synthesize(buf, &d->cur, &d->prev_enh, d->uvquality, &d->rng);
            ambe_move_parms(&d->cur, &d->prev_enh);
            ambe_float_to_s16(buf, pcm);
            return 0;
        }
        /* too many repeats: mute and restart the model */
        memset(pcm, 0, sizeof(int16_t) * AMBE_PCM_SAMPLES);
        ambe_init_parms(&d->cur, &d->prev, &d->prev_enh);
        return 1;
    }

    memset(pcm, 0, sizeof(int16_t) * AMBE_PCM_SAMPLES);
    ambe_init_parms(&d->cur, &d->prev, &d->prev_enh);
    return 1;
}

int ambe_decode_dmr_frame(ambe_decoder *d, const uint8_t frame[AMBE_DMR_BYTES],
                          int16_t pcm[AMBE_PCM_SAMPLES], ambe_frame_info *info)
{
    ambe_frame_info local;
    uint8_t fr[4][24];
    uint8_t ambe_d[AMBE_BITS];

    if (!info)
        info = &local;
    memset(info, 0, sizeof(*info));

    ambe_dmr_deinterleave(frame, fr);
    if (ambe_fec_decode(fr, ambe_d, info) != 0) {
        memset(pcm, 0, sizeof(int16_t) * AMBE_PCM_SAMPLES);
        info->uncorrectable = 1;
        return -1;
    }
    return ambe_decode_bits(d, ambe_d, pcm, info);
}
