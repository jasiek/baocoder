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
 *   +0x10  0x38 shorts.  At the interpolator this is the envelope, log2 at
 *          Q11; one wake later it has been converted in place to linear
 *          block-float amplitudes, which is what .fwamps carries
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
 * The spectral envelope, over every capture.
 *
 * `<name>.fwenv` is the frame's OWN decoded envelope - the parameter block at
 * `+0x198`, log2 at Q11 - which is exactly what `ambe_parms.log2Ml` is.  This
 * is the comparison that matters, and it is nearly exact.
 *
 * It is not the array `.fwamps` carries.  That one is `+0x10`, and it is a
 * different variable in different units: by the time the corpus peeks it, the
 * block has been converted in place from log2 Q11 to LINEAR block-float
 * amplitudes.  Comparing `log2Ml` against it, which is what this test used to
 * do, charged the envelope decode 1.15 dB for a units and stage mismatch.
 * cmp_synth_amplitudes below keeps that number as what it actually is - a
 * comparison of `Ml`, not of the envelope.
 */
static void cmp_envelope(const char *name, double *worst, double *sum, int *count)
{
    char pb[128];
    char *fb = NULL;
    size_t fcap = 0;
    FILE *fp, *fe;
    ambe_parms cur, prev, prev_enh;

    snprintf(pb, sizeof(pb), "%s.ambe49", name);
    fp = fixture_open(pb);
    snprintf(pb, sizeof(pb), "%s.fwenv", name);
    fe = fixture_open(pb);

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(pb, sizeof(pb), fp)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        double dl[AMBE_MAX_HARMONICS + 1], mean = 0, acc = 0, r;
        int i, l, m = 0;
        char *p;

        if (strlen(pb) < AMBE_BITS)
            continue;
        do {
            if (getline(&fb, &fcap, fe) < 0)
                goto done;
        } while (fb[0] == '#');

        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(pb[i] == '1');
        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);
        if (type == AMBE_FRAME_VOICE)
            ambe_move_parms(&cur, &prev);

        p = fb;
        if (type != AMBE_FRAME_VOICE || cur.L < 8)
            continue;
        for (l = 1; l <= cur.L; l++) {
            /* the fixture is Q11 log2; t_log2ml is the same thing as a double */
            dl[l] = (double)strtol(p, &p, 10) / 2048.0 - t_log2ml(&cur, l);
            mean += dl[l];
            m++;
        }
        if (m != cur.L)
            continue;
        mean /= m;
        for (l = 1; l <= cur.L; l++)
            acc += (dl[l] - mean) * (dl[l] - mean);
        r = sqrt(acc / m);
        if (r > *worst) *worst = r;
        *sum += r;
        (*count)++;
    }
done:
    free(fb);
    fclose(fp);
    fclose(fe);
}

/*
 * The linear spectral amplitudes at `params+0x10`, on the one capture whose
 * array is carried.  Not the envelope and not interpolated - breaking inside
 * Vocoder_ResampleSpectralEnvelope 0x00026A84 shows it writes to a caller stack
 * local and never touches the parameter context at all (docs/amplitude-gap.md,
 * tools/fw_oracle/resample_probe.py).  What this block holds is `Ml` after the
 * firmware's enhancement, exponentiated and block-float packed, so the mean is
 * removed below and only the shape is compared.
 *
 * Reported, not required.  The residual is 1.12 dB with `ambe_enhance_spectrum`
 * alone; adding `ambe_apply_unvoiced_gain` makes it 1.54 dB, so whatever else
 * the firmware folds in here, it is not this decoder's unvoiced scaling.
 */
static void cmp_synth_amplitudes(const char *name, double *sum, int *count)
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

    ambe_init_parms(&cur, &prev, &prev_enh);

    while (fgets(pb, sizeof(pb), fp)) {
        uint8_t d[AMBE_BITS];
        ambe_frame_info info;
        ambe_frame_type type;
        ambe_parms enh;
        double dl[AMBE_MAX_HARMONICS + 1], mean = 0, acc = 0;
        int i, l, m = 0;
        char *p;

        if (strlen(pb) < AMBE_BITS)
            continue;
        do {
            if (getline(&fb, &fcap, fa) < 0)
                goto done2;
        } while (fb[0] == '#');

        for (i = 0; i < AMBE_BITS; i++)
            d[i] = (uint8_t)(pb[i] == '1');
        memset(&info, 0, sizeof(info));
        type = ambe_decode_parms(d, &cur, &prev, &info);
        if (type == AMBE_FRAME_VOICE)
            ambe_move_parms(&cur, &prev);

        enh = cur;
        ambe_enhance_spectrum(&enh);

        p = fb;
        if (type != AMBE_FRAME_VOICE || cur.L < 8)
            continue;
        for (l = 1; l <= cur.L; l++) {
            long v = strtol(p, &p, 10);
            double ours = t_ml(&enh, l);
            if (v <= 0 || ours <= 0.0) { m = 0; break; }
            dl[l] = log2((double)v) - log2(ours);
            mean += dl[l];
            m++;
        }
        if (m != cur.L)
            continue;
        mean /= m;
        for (l = 1; l <= cur.L; l++)
            acc += (dl[l] - mean) * (dl[l] - mean);
        *sum += sqrt(acc / m);
        (*count)++;
    }
done2:
    free(fb);
    fclose(fp);
    fclose(fa);
}

int main(void)
{
    t_capture cap[16];
    int ncap = load_captures(cap, 16), i;
    int frames = 0, harmonics = 0, voice = 0, count = 0, icount = 0;
    double worst = 0.0, sum = 0.0, isum = 0.0;

    CHECK(ncap >= 6, "expected at least 6 captures in the manifest, got %d\n", ncap);
    for (i = 0; i < ncap; i++) {
        int n = 0;
        cmp_capture(cap[i].name, &n, &harmonics, &voice);
        CHECK(n == cap[i].frames, "%s: decoded %d frames, manifest says %d\n",
              cap[i].name, n, cap[i].frames);
        frames += n;
    }
    CHECK(frames == 2052, "expected 2052 frames, compared %d\n", frames);

    for (i = 0; i < ncap; i++)
        cmp_envelope(cap[i].name, &worst, &sum, &count);
    CHECK(count > 1000, "only %d frames' envelopes were comparable\n", count);
    /*
     * 0.020 dB, measured over every voice frame of all six captures.  This is
     * the spectral envelope against the radio's own, and it is the bound to
     * tighten if the envelope chain is ever improved - not the amplitude
     * figure below, which is a different quantity in different units.
     */
    CHECK(sum / count < 0.004,
          "envelope residual %.5f log2 (%.3f dB) above the measured 0.0034\n",
          sum / count, 6.02 * sum / count);
    CHECK(worst < 0.10, "worst envelope residual %.4f log2 too large\n", worst);

    cmp_synth_amplitudes("dm32_arc4_1", &isum, &icount);
    CHECK(icount > 100, "only %d frames for the amplitude comparison\n", icount);

    printf("[%d frames, %d voice: L, f0, class and all %d voicing decisions "
           "exact; spectral envelope %.3f dB mean over %d frames, %.3f dB worst; "
           "%.2f dB of shape against the linear amplitudes at params+0x10, "
           "which is a later stage in different units] ",
           frames, voice, harmonics, 6.02 * sum / count, count, 6.02 * worst,
           6.02 * isum / icount);

    return t_done("against the firmware's own decoder, p-code emulated");
}
