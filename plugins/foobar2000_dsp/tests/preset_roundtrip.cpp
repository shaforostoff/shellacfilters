/* ========================================
 *  preset_roundtrip - every parameter survives a save/load cycle.
 *
 *  This exists because `depth` once got written by make() and skipped by
 *  parse(), which silently shifted every field after it. A blind spot like
 *  that does not show up in a smoke test: the dialog looks right, the DSP
 *  runs, and only a saved-and-reloaded chain comes back wrong.
 *
 *  Uses only dsp_preset_builder/parser, so it needs the SDK but not an
 *  installed foobar2000.
 * ======================================== */

#include "stdafx_host.h"

#include "../foo_dsp_declick/declick_preset.h"
#include "../foo_dsp_decrackle/decrackle_preset.h"
#include "../foo_dsp_dehum/dehum_preset.h"
#include "../foo_dsp_paraeq/paraeq_preset.h"

#include <stdio.h>

#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char * what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_failures; }
}

void checkNear(double got, double want, const char * what) {
    // Params are floats; the preset stores them as floats. Exact is expected.
    if (got != want) {
        printf("  FAIL  %s: got %.9g, want %.9g\n", what, got, want);
        ++g_failures;
    }
}

void compare(const declick::Params & got, const declick::Params & want,
             const char * label) {
    printf("%s\n", label);
    checkNear(got.sensitivity, want.sensitivity, "sensitivity");
    checkNear(got.extent,      want.extent,      "extent");
    checkNear(got.maxLengthMs, want.maxLengthMs, "maxLengthMs");
    checkNear(got.depth,       want.depth,       "depth");
    checkNear(got.dryWet,      want.dryWet,      "dryWet");
    check(got.passes == want.passes, "passes");
    check(got.order  == want.order,  "order");
}

declick::Params roundTrip(const declick::Params & in) {
    dsp_preset_impl preset;
    declick_preset::make(in, preset);
    return declick_preset::parse(preset);
}

//! A version-1 preset, as written by the build that shipped before `depth`
//! existed: same field order, minus depth.
void makeLegacyV1(const declick::Params & p, dsp_preset & out) {
    dsp_preset_builder builder;
    builder << (t_uint32)1;
    builder << p.sensitivity << p.extent << p.maxLengthMs << p.dryWet;
    builder << (t_uint32)p.passes << (t_uint32)p.order;
    builder.finish(declick_preset::guid(), out);
}

} // anonymous namespace

int main() {
    // 1. Defaults survive.
    {
        declick::Params d = declick::Params::defaults();
        compare(roundTrip(d), d, "defaults round-trip");
    }

    // 2. Every field distinct, so a shifted read cannot accidentally pass.
    {
        declick::Params p;
        p.sensitivity = 0.8125f;   // exact in binary, no rounding to argue about
        p.extent      = 0.375f;
        p.maxLengthMs = 6.5f;
        p.depth       = 0.25f;
        p.dryWet      = 0.75f;
        p.passes      = 3;
        p.order       = 48;
        compare(roundTrip(p), p, "non-default values round-trip");
    }

    // 3. Extremes, after sanitize() has had its say.
    {
        declick::Params p;
        p.sensitivity = 1.0f; p.extent = 0.0f; p.maxLengthMs = 20.0f;
        p.depth = 1.0f; p.dryWet = 0.0f; p.passes = 1; p.order = 8;
        p.sanitize();
        compare(roundTrip(p), p, "extremes round-trip");
    }

    // 4. A version-1 preset still loads, with depth at its default.
    {
        declick::Params p = declick::Params::defaults();
        p.sensitivity = 0.8125f;
        p.extent      = 0.375f;
        p.maxLengthMs = 6.5f;
        p.dryWet      = 0.75f;
        p.passes      = 3;
        p.order       = 48;
        p.depth       = 123.0f;   // never written; must not come back

        dsp_preset_impl preset;
        makeLegacyV1(p, preset);
        const declick::Params got = declick_preset::parse(preset);

        declick::Params want = p;
        want.depth = declick::Params::defaults().depth;
        compare(got, want, "version-1 preset upgrades");
    }

    // 5. A preset belonging to somebody else is ignored, not misread.
    {
        dsp_preset_impl preset;
        dsp_preset_builder builder;
        builder << (t_uint32)0xDEADBEEF << 1.0f << 2.0f;
        GUID other = declick_preset::guid();
        other.Data1 ^= 1;
        builder.finish(other, preset);

        compare(declick_preset::parse(preset), declick::Params::defaults(),
                "foreign preset falls back to defaults");
    }

    // 6. Truncated payload: must fall back, must not throw out of parse().
    {
        dsp_preset_impl preset;
        dsp_preset_builder builder;
        builder << (t_uint32)declick_preset::version << 0.5f;   // and nothing else
        builder.finish(declick_preset::guid(), preset);

        compare(declick_preset::parse(preset), declick::Params::defaults(),
                "truncated preset falls back to defaults");
    }

    // 7. Same guarantee for the other component in the pack.
    {
        using airwindows::DeCrackleParams;
        DeCrackleParams p;
        p.filter = 0.8125f; p.window = 0.375f; p.threshold = 0.625f;
        p.surface = 0.125f; p.dryWet = 0.75f;
        p.sanitize();

        dsp_preset_impl preset;
        decrackle_preset::make(p, preset);
        const DeCrackleParams got = decrackle_preset::parse(preset);

        printf("decrackle round-trip\n");
        checkNear(got.filter,    p.filter,    "filter");
        checkNear(got.window,    p.window,    "window");
        checkNear(got.threshold, p.threshold, "threshold");
        checkNear(got.surface,   p.surface,   "surface");
        checkNear(got.dryWet,    p.dryWet,    "dryWet");
    }

    // 8. And for dehum. Values are deliberately all different from each other
    //    and from the defaults, so a field read out of position cannot pass.
    {
        using dehum::Params;
        Params p;
        p.sensitivity = 0.3125f;
        p.bandwidth   = 2.375f;
        p.searchTo    = 275.0f;
        p.harmonics   = 6;
        p.frequency   = 123.5f;
        p.rumbleHz    = 47.0f;
        p.dryWet      = 0.8125f;
        p.sanitize();

        dsp_preset_impl preset;
        dehum_preset::make(p, preset);
        const Params got = dehum_preset::parse(preset);

        printf("dehum round-trip\n");
        checkNear(got.sensitivity, p.sensitivity, "sensitivity");
        checkNear(got.bandwidth,   p.bandwidth,   "bandwidth");
        checkNear(got.searchTo,    p.searchTo,    "searchTo");
        checkNear(got.frequency,   p.frequency,   "frequency");
        checkNear(got.rumbleHz,    p.rumbleHz,    "rumbleHz");
        checkNear(got.dryWet,      p.dryWet,      "dryWet");
        check(got.harmonics == p.harmonics, "harmonics");

        // Frequency and Rumble both have an off value of 0 that sits outside
        // their live ranges; sanitize() must leave it alone rather than clamping
        // it up to the bottom of the range, or "auto" would become "pinned at
        // 10 Hz" on every save.
        Params off = Params::defaults();
        off.frequency = 0.0f;
        off.rumbleHz = 0.0f;
        dsp_preset_impl p2;
        dehum_preset::make(off, p2);
        const Params back = dehum_preset::parse(p2);
        checkNear(back.frequency, 0.0f, "frequency 0 survives as automatic");
        checkNear(back.rumbleHz,  0.0f, "rumble 0 survives as off");

        // A preset owned by something else, and a truncated one.
        dsp_preset_impl foreign;
        GUID other = dehum_preset::guid();
        other.Data1 ^= 1;
        foreign.set_owner(other);
        foreign.set_data("junk", 4);
        check(dehum_preset::parse(foreign) == Params::defaults(),
              "a foreign preset falls back to the defaults");

        dsp_preset_impl short_;
        dsp_preset_builder b;
        b << (t_uint32)dehum_preset::version << 0.5f;   // and nothing else
        b.finish(dehum_preset::guid(), short_);
        check(dehum_preset::parse(short_) == Params::defaults(),
              "a truncated preset falls back to the defaults");
    }

    // 9. And for the equaliser, which has the most fields of the four and four
    //    switches among them, which a builder writes as t_uint32 and a careless
    //    parser could read back as something else.
    {
        using paraeq::Params;
        Params p;
        p.hpFrequency  =   137.5f;
        p.hpSlope      =       2;
        p.lfGain       =    -7.25f;
        p.lfFrequency  =   213.0f;
        p.lfBell       =    true;
        p.lmfGain      =    11.5f;
        p.lmfFrequency =  1750.0f;
        p.lmfQ         =     3.25f;
        p.hmfGain      =    -4.75f;
        p.hmfFrequency =  3300.0f;
        p.hmfQ         =     0.75f;
        p.hfGain       =    17.0f;
        p.hfFrequency  = 11000.0f;
        p.hfBell       =    true;
        p.outputGain   =    -2.5f;
        p.bypass       =    true;
        p.sanitize();

        dsp_preset_impl preset;
        paraeq_preset::make(p, preset);
        const Params got = paraeq_preset::parse(preset);

        printf("paraeq round-trip\n");
        checkNear(got.hpFrequency,  p.hpFrequency,  "hpFrequency");
        checkNear(got.lfGain,       p.lfGain,       "lfGain");
        checkNear(got.lfFrequency,  p.lfFrequency,  "lfFrequency");
        checkNear(got.lmfGain,      p.lmfGain,      "lmfGain");
        checkNear(got.lmfFrequency, p.lmfFrequency, "lmfFrequency");
        checkNear(got.lmfQ,         p.lmfQ,         "lmfQ");
        checkNear(got.hmfGain,      p.hmfGain,      "hmfGain");
        checkNear(got.hmfFrequency, p.hmfFrequency, "hmfFrequency");
        checkNear(got.hmfQ,         p.hmfQ,         "hmfQ");
        checkNear(got.hfGain,       p.hfGain,       "hfGain");
        checkNear(got.hfFrequency,  p.hfFrequency,  "hfFrequency");
        checkNear(got.outputGain,   p.outputGain,   "outputGain");
        check(got.hpSlope == p.hpSlope, "hpSlope");
        check(got.lfBell  == p.lfBell,  "lfBell");
        check(got.hfBell  == p.hfBell,  "hfBell");
        check(got.bypass  == p.bypass,  "bypass");
        check(got == p, "every field together");

        // The switches are stored as integers, so what matters is that false
        // comes back false rather than as whatever byte happened to follow.
        Params allOff = Params::defaults();
        allOff.lfBell = false; allOff.hfBell = false; allOff.bypass = false;
        allOff.hpSlope = 0;
        dsp_preset_impl p2;
        paraeq_preset::make(allOff, p2);
        check(paraeq_preset::parse(p2) == allOff, "the switches survive being off");

        dsp_preset_impl foreign;
        GUID other = paraeq_preset::guid();
        other.Data1 ^= 1;
        foreign.set_owner(other);
        foreign.set_data("junk", 4);
        check(paraeq_preset::parse(foreign) == Params::defaults(),
              "a foreign preset falls back to the defaults");

        dsp_preset_impl short_;
        dsp_preset_builder b;
        b << (t_uint32)paraeq_preset::version << 0.5f;   // and nothing else
        b.finish(paraeq_preset::guid(), short_);
        check(paraeq_preset::parse(short_) == Params::defaults(),
              "a truncated preset falls back to the defaults");
    }

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
