/*
 * test_basop_firmware.c - the block-float pair, against the firmware executed.
 *
 * test_basop.c sweeps log2, pow2, sqrt and cos against libm, which is the right
 * measure for a primitive that approximates a real function: what is being
 * asked there is how accurate the radio is.  These two approximate nothing.
 * Math_FloatAdd 0x00018DD8 and Math_FloatDivExponent 0x00018EF4 are exact
 * integer operations on a 16-bit mantissa and a 16-bit exponent, so there is no
 * tolerance to quote and nothing to compare against but the radio itself.
 *
 * The fixture is 3377 cases run under the p-code emulator, edges first:
 * exponent differences of 31, 32 and 33, the two saturating operands, zero
 * mantissas on either side and both, every sign combination, then a
 * deterministic sweep.  tools/fw_oracle/gen_basop_jobs.py.
 *
 * The edges are not decoration.  Math_FloatAdd aligns to max(expA, expB) + 1
 * and its own guard admits a difference of 31, so the smaller operand is
 * shifted by exactly 32 - which C leaves undefined and the machine renders as
 * everything shifted out.  Transcribing that as a plain C `>>`, which on this
 * host compiles to a shift masked to five bits and so shifts by nothing at all,
 * gives a different answer on 257 of these 3377 cases.  Both readings pass a
 * sweep that never reaches a difference of 31.
 *
 * Why this exists before the code that needs it: the voiced synthesiser calls
 * the pair four times per harmonic, so one wrong bit here is a divergence forty
 * times over in one frame, and it would be hunted through 2378 bytes of
 * Vocoder_SynthesizeVoiced instead of thirty lines of ambe_basop.c.
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe.h"
#include "ambe_basop.h"
#include "ambe_frame_int.h"
#include "testutil.h"

int main(void)
{
    FILE *f = fixture_open("basop_float.fw");
    char *line = NULL;
    size_t cap = 0;
    int n = 0, nadd = 0, ndiv = 0, nsub = 0;
    int add_ok = 0, div_ok = 0, sub_ok = 0;
    int shift32 = 0, sat = 0;

    while (getline(&line, &cap, f) > 0) {
        long ma, ea, mb, eb, am, ax, dm, dx, sm, sx;
        char *p = line;
        uint16_t got;
        int16_t gx;

        if (line[0] == '#')
            continue;
        ma = strtol(p, &p, 10); ea = strtol(p, &p, 10);
        mb = strtol(p, &p, 10); eb = strtol(p, &p, 10);
        am = strtol(p, &p, 10); ax = strtol(p, &p, 10);
        dm = strtol(p, &p, 10); dx = strtol(p, &p, 10);
        sm = strtol(p, &p, 10); sx = strtol(p, &p, 10);

        gx = 0x7BAD;
        got = ambe_float_add((int32_t)ma, (int)ea, (int32_t)mb, (int)eb, &gx);
        nadd++;
        if (got == (uint16_t)am && gx == (int16_t)ax)
            add_ok++;
        else
            CHECK(0, "add(%ld,%ld, %ld,%ld) = %u/%d, firmware %ld/%ld\n",
                  ma, ea, mb, eb, (unsigned)got, (int)gx, am, ax);

        gx = 0x7BAD;
        got = ambe_float_sub((int32_t)ma, (int)ea, (int32_t)mb, (int)eb, &gx);
        nsub++;
        if (got == (uint16_t)sm && gx == (int16_t)sx)
            sub_ok++;
        else
            CHECK(0, "sub(%ld,%ld, %ld,%ld) = %u/%d, firmware %ld/%ld\n",
                  ma, ea, mb, eb, (unsigned)got, (int)gx, sm, sx);

        /* the alignment shift the machine masks to six bits and C leaves
           undefined - counted so a fixture that stopped covering it says so */
        if (labs(ea - eb) == 31 && ma && mb)
            shift32++;
        if (ma == INT32_MIN || mb == -32768)
            sat++;

        if (dm >= 0) {
            gx = 0x7BAD;
            got = ambe_float_div_exp((int32_t)ma, (int)ea, (int32_t)mb,
                                     (int)eb, &gx);
            ndiv++;
            if (got == (uint16_t)dm && gx == (int16_t)dx)
                div_ok++;
            else
                CHECK(0, "div(%ld,%ld, %ld,%ld) = %u/%d, firmware %ld/%ld\n",
                      ma, ea, mb, eb, (unsigned)got, (int)gx, dm, dx);
        }
        n++;
    }
    free(line);
    line = NULL;
    cap = 0;            /* getline reuses both, and the blocks below call it */
    fclose(f);

    {   /* Math_Sqrt, and the demonstration that ambe_sqrt is a different
           function rather than a rougher version of the same one */
        FILE *g = fixture_open("basop_sqrt.fw");
        int sn = 0, sok = 0, agree = 0;

        while (getline(&line, &cap, g) > 0) {
            long m, e, rm, re;
            char *p = line;
            int16_t ex;
            uint32_t got;

            if (line[0] == '#')
                continue;
            m  = strtol(p, &p, 10); e  = strtol(p, &p, 10);
            rm = strtoul(p, &p, 10); re = strtol(p, &p, 10);

            ex = (int16_t)e;
            got = ambe_float_sqrt((int32_t)m, &ex);
            sn++;
            if (got == (uint32_t)rm && ex == (int16_t)re)
                sok++;
            else
                CHECK(0, "sqrt(%ld,%ld) = %u/%d, firmware %lu/%ld\n",
                      m, e, (unsigned)got, (int)ex, (unsigned long)rm, re);
            {   /* the older one, on the same case */
                short oe = (short)e;
                unsigned int old = ambe_sqrt((int)m, &oe);
                if ((uint32_t)old == (uint32_t)rm && oe == (short)re)
                    agree++;
            }
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(sn > 500, "only %d square roots in the fixture\n", sn);
        CHECK(sok == sn, "Math_Sqrt exact on %d of %d\n", sok, sn);
        /* Not a defect in ambe_sqrt: it is measured against libm in
           test_basop.c and used where that is the right measure.  Asserted so
           that a later "tidy-up" merging the two has to argue with a test. */
        CHECK(agree < sn / 2, "ambe_sqrt now matches the firmware on %d of %d - "
              "if that is deliberate the two should be merged, and if it is not, "
              "something has changed underneath both\n", agree, sn);
        printf("[Math_Sqrt %d/%d bit-exact, ambe_sqrt %d/%d - a different "
               "function, deliberately] ", sok, sn, agree, sn);
    }

    {   /* Math_SqrtScaled, which Vocoder_NormalizeSpectralBlock and
           Vocoder_SynthesizeFrame both call - swept before either is written */
        FILE *g = fixture_open("basop_sqrtscaled.fw");
        int qn = 0, qok = 0;

        while (getline(&line, &cap, g) > 0) {
            long m, e, q;
            unsigned long r;
            char *p = line;
            uint32_t got;

            if (line[0] == '#')
                continue;
            m = strtol(p, &p, 10); e = strtol(p, &p, 10);
            q = strtol(p, &p, 10); r = strtoul(p, &p, 10);
            got = ambe_sqrt_scaled((int32_t)m, (uint32_t)e, (int16_t)q);
            qn++;
            if (got == (uint32_t)r)
                qok++;
            else
                CHECK(0, "sqrt_scaled(%ld,%ld,%ld) = %u, firmware %lu\n",
                      m, e, q, (unsigned)got, r);
        }
        free(line);
        line = NULL;
        cap = 0;
        fclose(g);
        CHECK(qn > 600, "only %d scaled square roots\n", qn);
        CHECK(qok == qn, "Math_SqrtScaled exact on %d of %d\n", qok, qn);
        printf("[Math_SqrtScaled %d/%d bit-exact] ", qok, qn);
    }

    CHECK(n > 3000, "only %d cases in the fixture\n", n);
    CHECK(shift32 > 0, "no case aligns across an exponent difference of 31\n");
    CHECK(sat > 0, "no case reaches a saturating operand\n");
    CHECK(add_ok == nadd, "Math_FloatAdd exact on %d of %d\n", add_ok, nadd);
    CHECK(div_ok == ndiv, "Math_FloatDivExponent exact on %d of %d\n",
          div_ok, ndiv);
    CHECK(sub_ok == nsub, "Math_FloatSub exact on %d of %d\n", sub_ok, nsub);

    printf("[%d cases: Math_FloatAdd %d/%d, Math_FloatDivExponent %d/%d and "
           "Math_FloatSub %d/%d bit-exact, including %d alignments by exactly 32 "
           "and %d saturating operands] ", n, add_ok, nadd, div_ok, ndiv,
           sub_ok, nsub, shift32, sat);
    return t_done("the block-float pair vs the firmware, bit for bit");
}
