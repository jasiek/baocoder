/*
 * test_firmware.c - this decoder against the radio's own, frame by frame.
 *
 * The fixtures here are not another implementation's opinion.  They are what
 * the stock decoder in the DM-32UV image produces when it is executed: the
 * baofeng-dm32uv-reveng project's p-code emulator runs Vocoder_TxTask
 * 0x0002EC30 out of the firmware over the same 49-bit payloads as
 * <name>.ambe49, and tools/emu exports the parameter block
 * Vocoder_ProcessFrameSignaling 0x00019758 parks at pCtx+1000.  Its layout is
 * Vocoder_CopyFrameParamsWithReset 0x00019CBC's:
 *
 *   +0x00  frame class - 1 voice, 2 for every b0 >= 120
 *   +0x04  L
 *   +0x08  voicing, a uint32 of 16 two-bit crumbs (with +0x0A)
 *   +0x0C  f0, Q19 turns per sample
 *   +0x10  0x38 shorts, the spectral amplitudes, post-enhancement
 *
 * This is the parity test.  Everything above the amplitudes is required to be
 * exact - not bounded, not correlated - because the two are meant to be the
 * same decoder.  The amplitudes are bounded instead, and the bound is the
 * measured residual rather than a round number, so that improving the chain
 * shows up as a failure to tighten it.
 *
 * The band map is the firmware's own and not mbelib's: harmonic l belongs to
 * band int(l * 32 * f0) of sixteen, and the band's voiced flag is the crumb's
 * LOW bit - the high bit selects the pitchless parameterisation instead
 * (Vocoder_DecodePitchlessGainMode 0x00027EF0).  That mapping was identified
 * on four speakers and confirmed held-out on twenty-one others.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "testutil.h"

/*
 * Frames after a non-voice frame before the envelope predictor has forgotten
 * the state difference that frame seeded.  0.65^7 = 0.049 against a seed of
 * 1.12 log2, which is where the measured decay lands.
 */
#define AMBE_STEADY 8

/* The firmware's voicing band for harmonic l: sixteen bands, not eight. */
static int fw_band(int l, int32_t f0_q19)
{
    return (int)(((int64_t)l * 32 * f0_q19) >> AMBE_Q_F0);
}

static int cmp_capture(const char *name, int *frames_out, int *harm_out,
                       int *voice_out)
{
    char pb[128], fb[256];
    FILE *fp, *ff;
    ambe_parms cur, prev, prev_enh;
    int n = 0;

    snprintf(pb, sizeof(pb), "%s.ambe49", name);
    fp = fixture_open(pb);
    snprintf(pb, sizeof(pb), "%s.fwparms", name);
    ff = fixture_open(pb);

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(pb, sizeof(pb), fp)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        unsigned int word;
        int cls, L, f0, i, l;

        if (strlen(pb) < AMBE_BITS)
            continue;
        do {
            if (!fgets(fb, sizeof(fb), ff))
                goto done;
        } while (fb[0] == '#');
        if (sscanf(fb, "%d %d %d %x", &cls, &L, &f0, &word) != 4)
            break;

        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(pb[i] == '1');
        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);

        /* the radio does not distinguish silence from erasure from tone */
        CHECK((cls == 1) == (type == AMBE_FRAME_VOICE),
              "%s frame %d: class %d vs firmware %d (b0=%d)\n",
              name, n, (int)type, cls, info.b[0]);
        CHECK(cur.L == L, "%s frame %d: L %d vs firmware %d (b0=%d)\n",
              name, n, cur.L, L, info.b[0]);
        CHECK((int)cur.f0 == f0, "%s frame %d: f0 %d vs firmware %d (b0=%d)\n",
              name, n, (int)cur.f0, f0, info.b[0]);

        if (type == AMBE_FRAME_VOICE) {
            for (l = 1; l <= cur.L; l++) {
                int j = fw_band(l, cur.f0);
                int v = j < 16 ? (int)((word >> (2 * j)) & 1u) : 0;
                CHECK((int)cur.Vl[l] == v,
                      "%s frame %d: Vl[%d] %d vs firmware %d (band %d, b1=%d)\n",
                      name, n, l, (int)cur.Vl[l], v, j, info.b[1]);
                (*harm_out)++;
            }
            (*voice_out)++;
        }

        /* only a voice frame advances the predictor - see ambe.h */
        if (type == AMBE_FRAME_VOICE)
            ambe_move_parms(&cur, &prev);
        n++;
    }
done:
    fclose(fp);
    fclose(ff);
    *frames_out += n;
    return n;
}

/*
 * The amplitudes, on the one capture whose firmware array is carried.  They
 * are block floating point on both sides with different exponents, so what is
 * compared is the shape: the per-frame RMS of log2(firmware) - log2(ours)
 * after the constant offset between them is removed.
 */
static void cmp_amplitudes(const char *name, double *worst, double *sum,
                           int *count, double *tsum, int *tcount)
{
    char pb[128];
    char *fb = NULL;
    size_t fcap = 0;
    FILE *fp, *fa;
    ambe_parms cur, prev, prev_enh;

    snprintf(pb, sizeof(pb), "%s.ambe49", name);
    fp = fixture_open(pb);
    snprintf(pb, sizeof(pb), "%s.fwamps", name);
    fa = fixture_open(pb);

    int since = 99;

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(pb, sizeof(pb), fp)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        ambe_parms enh;
        double dl[AMBE_MAX_HARMONICS + 1], mean = 0, acc = 0, r;
        int i, l, m = 0;
        char *p;

        if (strlen(pb) < AMBE_BITS)
            continue;
        do {
            if (getline(&fb, &fcap, fa) < 0)
                goto done;
        } while (fb[0] == '#');

        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(pb[i] == '1');
        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);

        if (type == AMBE_FRAME_VOICE)
            ambe_move_parms(&cur, &prev);

        /* what the firmware parks is the enhanced spectrum, before the
           unvoiced gain - the same point ambe_decoder.c reaches */
        enh = cur;
        ambe_enhance_spectrum(&enh);

        p = fb;
        if (type != AMBE_FRAME_VOICE) { since = 0; continue; }
        since++;
        if (cur.L < 8)
            continue;
        for (l = 1; l <= cur.L; l++) {
            long v = strtol(p, &p, 10);
            double ours = t_ml(&enh, l);
            if (v <= 0 || ours <= 0.0) { m = 0; break; }
            dl[l] = log2((double)v) - log2(ours);
            mean += dl[l];
            m++;
        }
        if (m == cur.L) {
            mean /= m;
            for (l = 1; l <= cur.L; l++)
                acc += (dl[l] - mean) * (dl[l] - mean);
            r = sqrt(acc / m);
            /*
             * Split, and both bounded the same, because they used to be two
             * populations and are not any more.  Propagating a b0 >= 120 frame
             * into the predictor put the frame after a silence run at 1.12
             * log2, decaying at the codec's own 0.65 over the next seven;
             * holding it puts that frame at 0.20, which is the settled figure.
             * Keeping the split is what makes a regression visible.
             */
            if (since >= AMBE_STEADY) {
                if (r > *worst) *worst = r;
                *sum += r;
                (*count)++;
            } else {
                *tsum += r;
                (*tcount)++;
            }
        }
    }
done:
    free(fb);
    fclose(fp);
    fclose(fa);
}

int main(void)
{
    t_capture cap[16];
    int ncap = load_captures(cap, 16), i;
    int frames = 0, harmonics = 0, voice = 0, count = 0, tcount = 0;
    double worst = 0.0, sum = 0.0, tsum = 0.0;

    CHECK(ncap >= 6, "expected at least 6 captures in the manifest, got %d\n", ncap);
    for (i = 0; i < ncap; i++) {
        int n = 0;
        cmp_capture(cap[i].name, &n, &harmonics, &voice);
        CHECK(n == cap[i].frames, "%s: decoded %d frames, manifest says %d\n",
              cap[i].name, n, cap[i].frames);
        frames += n;
    }
    CHECK(frames == 2052, "expected 2052 frames, compared %d\n", frames);

    cmp_amplitudes("dm32_arc4_1", &worst, &sum, &count, &tsum, &tcount);
    CHECK(count > 100, "only %d settled frames' amplitudes were comparable\n", count);
    CHECK(tcount > 20, "only %d transient frames; the split is not exercised\n", tcount);
    /*
     * The amplitudes are the one layer not at parity, and the bound is the
     * measured figure rather than a round number, so that closing the gap has
     * to come back here and tighten it.  1.1 dB.  The residual is flat with
     * depth below the frame's peak until the last few bits, so it is not the
     * fixture's int16 precision - it is a real difference in the envelope
     * chain, and it is the same on both sides of the split.
     */
    CHECK(sum / count < 0.21,
          "settled amplitude residual %.4f log2 (%.2f dB) above the measured 0.197\n",
          sum / count, 6.02 * sum / count);
    CHECK(worst < 1.0, "worst settled amplitude residual %.4f log2 too large\n", worst);
    /*
     * The transient is gone, and this is where that is asserted: holding the
     * predictor across a b0 >= 120 frame brought the frames right after one
     * from 0.65 log2 to the settled figure, so the two populations now get the
     * same bound.  Loosening this one is the regression to catch.
     */
    CHECK(tsum / tcount < 0.21,
          "the frames after a non-voice frame are at %.4f log2 (%.2f dB), "
          "above the settled bound - the predictor hold has regressed\n",
          tsum / tcount, 6.02 * tsum / tcount);

    printf("[%d frames, %d voice: L, f0, class and all %d voicing decisions "
           "exact; amplitudes %.2f dB settled (%d frames), %.2f dB in the "
           "%d frames after a non-voice frame] ",
           frames, voice, harmonics, 6.02 * sum / count, count,
           6.02 * tsum / tcount, tcount);

    return t_done("against the firmware's own decoder, p-code emulated");
}
