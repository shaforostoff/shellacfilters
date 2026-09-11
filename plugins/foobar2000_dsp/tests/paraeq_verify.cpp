/* ========================================
 *  paraeq_verify - the parametric EQ core against its own promises.
 *
 *  Builds paraeq_core.cpp on its own, which is the point: the core sits beside
 *  declick and dehum and takes nothing from a host, so anything it would need
 *  from foobar2000 belongs in the wrapper instead. No SDK.
 *
 *  Carried over from EmbraceNG, where this core was written, with the checks on
 *  the drawing path this port added at the end.
 * ======================================== */
//
// Two of these are worth knowing about.  "measured vs predicted" runs sines
// through a real Channel and compares the gain it actually applies against what
// magnitudeDb() promises, so the curve a host draws is checked against the audio
// rather than against the same formula twice.  "no step larger than the tone's
// own curvature" is what caught the glide stepping per sub-block: a jump in b0
// puts b0*x straight into the output, and 32-sample blocks left a staircase 35
// dB down that the second difference of the output shows plainly.

#include "paraeq_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace paraeq;

static int sFail = 0;
static const double kFs = 44100.0;

// MSVC does not define M_PI without _USE_MATH_DEFINES, and the core does not
// lean on <cmath> for it either.
static const double kPi = 3.14159265358979323846;

static void ck(const char *what, double got, double want, double tol)
{
    bool ok = std::fabs(got - want) <= tol;
    if (!ok) sFail++;
    std::printf("   %-4s %-56s %10.4f (want %.4f +-%.4f)\n",
                ok ? "ok" : "FAIL", what, got, want, tol);
}

static void ckTrue(const char *what, bool ok)
{
    if (!ok) sFail++;
    std::printf("   %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// Gain a Channel actually applies at `hz`, measured rather than derived.
static double measuredDb(Params p, double hz)
{
    Config cfg;
    cfg.compute(p, kFs);

    Channel ch;
    ch.configure(cfg);

    size_t warm = 40000, meas = 40000;
    double sumIn = 0, sumOut = 0;
    std::vector<float> buf(1024);

    for (size_t n = 0; n < warm + meas; n += buf.size()) {
        for (size_t i = 0; i < buf.size(); i++) {
            buf[i] = (float)std::sin(2.0 * kPi * hz * (double)(n + i) / kFs);
            if (n >= warm) sumIn += (double)buf[i] * buf[i];
        }

        ch.process(buf.data(), buf.size(), 1);

        if (n >= warm) {
            for (size_t i = 0; i < buf.size(); i++) sumOut += (double)buf[i] * buf[i];
        }
    }

    return 10.0 * std::log10(sumOut / sumIn);
}

int main()
{
    std::printf("paraeq core checks, Fs = %.0f\n\n", kFs);

    std::printf("== cookbook sections land where the formulas say\n");
    {
        Biquad pk = design(kPeaking, 1000, 1.0, 6.0, kFs);
        ck("peaking +6dB @1k, at 1k",       magnitudeDb(pk, 1000, kFs),  6.0,  0.02);
        ck("peaking +6dB @1k, at 20Hz",     magnitudeDb(pk,   20, kFs),  0.0,  0.05);
        ck("peaking +6dB @1k, at 20kHz",    magnitudeDb(pk, 20000, kFs), 0.0,  0.10);

        Biquad cut = design(kPeaking, 1000, 4.0, -12.0, kFs);
        ck("peaking -12dB Q4 @1k, at 1k",   magnitudeDb(cut, 1000, kFs), -12.0, 0.02);

        Biquad ls = design(kLowShelf, 100, kShelfQ, 6.0, kFs);
        ck("low shelf +6dB @100, at 5Hz",   magnitudeDb(ls,    5, kFs),  6.0,  0.05);
        ck("low shelf +6dB @100, at 100",   magnitudeDb(ls,  100, kFs),  3.0,  0.05);
        ck("low shelf +6dB @100, at 10kHz", magnitudeDb(ls, 10000, kFs), 0.0,  0.02);

        Biquad hs = design(kHighShelf, 8000, kShelfQ, -9.0, kFs);
        ck("high shelf -9dB @8k, at 20kHz", magnitudeDb(hs, 20000, kFs), -9.0, 0.30);
        ck("high shelf -9dB @8k, at 8k",    magnitudeDb(hs,  8000, kFs), -4.5, 0.05);
        ck("high shelf -9dB @8k, at 100",   magnitudeDb(hs,   100, kFs),  0.0, 0.02);
    }

    std::printf("\n== the high-pass is Butterworth at both slopes\n");
    {
        Biquad h2 = design(kHighPass, 100, kButterworthQ2, 0, kFs);
        ck("12dB/oct @100, at 100",  magnitudeDb(h2, 100, kFs), -3.01,  0.02);
        ck("12dB/oct @100, at 50",   magnitudeDb(h2,  50, kFs), -12.30, 0.05);
        ck("12dB/oct @100, at 1k",   magnitudeDb(h2, 1000, kFs), 0.0,   0.01);

        Biquad lo = design(kHighPass, 100, kButterworthQ4Lo, 0, kFs);
        Biquad hi = design(kHighPass, 100, kButterworthQ4Hi, 0, kFs);
        double at100 = magnitudeDb(lo, 100, kFs) + magnitudeDb(hi, 100, kFs);
        double at50  = magnitudeDb(lo,  50, kFs) + magnitudeDb(hi,  50, kFs);
        double at1k  = magnitudeDb(lo, 1000, kFs) + magnitudeDb(hi, 1000, kFs);
        ck("24dB/oct @100, at 100",  at100, -3.01,  0.02);
        ck("24dB/oct @100, at 50",   at50,  -24.10, 0.10);
        ck("24dB/oct @100, at 1k",   at1k,    0.0,  0.01);
    }

    std::printf("\n== what a Channel does matches what magnitudeDb promises\n");
    {
        Params p = Params::defaults();
        p.lfGain = 5;  p.lfFrequency = 100;
        p.lmfGain = -8; p.lmfFrequency = 1000; p.lmfQ = 2;
        p.hmfGain = 4; p.hmfFrequency = 5000; p.hmfQ = 1;
        p.hfGain = -10; p.hfFrequency = 8000;
        p.hpSlope = 2; p.hpFrequency = 60;
        p.outputGain = 2;

        Config cfg;
        cfg.compute(p, kFs);

        const double probes[] = { 40, 60, 100, 250, 1000, 2000, 5000, 8000, 12000 };

        for (int i = 0; i < 9; i++) {
            char label[96];
            std::snprintf(label, sizeof(label), "measured vs predicted at %.0f Hz", probes[i]);
            ck(label, measuredDb(p, probes[i]), magnitudeDb(cfg, probes[i]), 0.06);
        }
    }

    std::printf("\n== flat by default, and flat again when bypassed\n");
    {
        Params d = Params::defaults();
        Config cfg; cfg.compute(d, kFs);
        double worst = 0;
        for (double hz = 10; hz < 20000; hz *= 1.05) {
            worst = std::fmax(worst, std::fabs(magnitudeDb(cfg, hz)));
        }
        ck("defaults are flat everywhere", worst, 0.0, 1e-9);

        Params p = d;
        p.lfGain = 20; p.hfGain = -20; p.hpSlope = 2; p.outputGain = -6;
        p.bypass = true;
        Config bcfg; bcfg.compute(p, kFs);
        worst = 0;
        for (double hz = 10; hz < 20000; hz *= 1.05) {
            worst = std::fmax(worst, std::fabs(magnitudeDb(bcfg, hz)));
        }
        ck("bypass targets a flat curve", worst, 0.0, 1e-9);
    }

    std::printf("\n== interpolated coefficients stay inside the stability triangle\n");
    {
        // The two most distant settings the controls allow, in both directions.
        struct { Kind kind; double f, q, g; } ends[] = {
            { kHighPass,  kHpFreqMin,  kButterworthQ4Hi,   0 },
            { kHighPass,  kHpFreqMax,  kButterworthQ4Lo,   0 },
            { kLowShelf,  kLfFreqMin,  kShelfQ,          20 },
            { kHighShelf, kHfFreqMax,  kShelfQ,         -20 },
            { kPeaking,   kLmfFreqMin, kQMax,            20 },
            { kPeaking,   kHmfFreqMax, kQMin,           -20 },
            { kUnity,     1000,        1,                 0 }
        };

        int n = (int)(sizeof(ends) / sizeof(ends[0]));
        bool allStable = true, allEndsStable = true;

        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                Biquad a = design(ends[i].kind, ends[i].f, ends[i].q, ends[i].g, kFs);
                Biquad b = design(ends[j].kind, ends[j].f, ends[j].q, ends[j].g, kFs);

                if (!(std::fabs(a.a2) < 1.0 && std::fabs(a.a1) < 1.0 + a.a2)) allEndsStable = false;

                for (int k = 0; k <= 1000; k++) {
                    double t = k / 1000.0;
                    double a1 = a.a1 + (b.a1 - a.a1) * t;
                    double a2 = a.a2 + (b.a2 - a.a2) * t;

                    if (!(std::fabs(a2) < 1.0 && std::fabs(a1) < 1.0 + a2)) allStable = false;
                }
            }
        }

        ckTrue("every endpoint design is itself stable", allEndsStable);
        ckTrue("49 x 1001 interpolants all stable", allStable);
    }

    std::printf("\n== a control move glides, settles, and does not click\n");
    {
        Params from = Params::defaults();
        Params to   = Params::defaults();
        to.lfGain = 18; to.hfGain = -18; to.hpSlope = 2; to.hpFrequency = 300;

        Config c0, c1;
        c0.compute(from, kFs);
        c1.compute(to, kFs);

        Channel ch;
        ch.configure(c0);

        // A steady 200 Hz tone across the move: the biggest step between two
        // adjacent output samples is what a click is.
        std::vector<float> buf(32768);   // longer than the glide takes to settle
        for (size_t i = 0; i < buf.size(); i++) {
            buf[i] = (float)(0.5 * std::sin(2.0 * kPi * 200.0 * (double)i / kFs));
        }

        std::vector<float> before = buf;
        ch.process(buf.data(), 2048, 1);
        ckTrue("settled before the move", ch.settled());

        ch.retune(c1);
        ckTrue("retune unsettles", !ch.settled());

        ch.process(buf.data() + 2048, buf.size() - 2048, 1);
        ckTrue("settled again after the move", ch.settled());

        // Second difference of the output, against the same measure on the input.
        double worstOut = 0, worstIn = 0;
        for (size_t i = 2050; i < buf.size(); i++) {
            worstOut = std::fmax(worstOut, std::fabs((double)buf[i] - 2.0 * buf[i-1] + buf[i-2]));
            worstIn  = std::fmax(worstIn,  std::fabs((double)before[i] - 2.0 * before[i-1] + before[i-2]));
        }
        std::printf("        worst 2nd difference: out %.3e, input %.3e\n", worstOut, worstIn);
        ckTrue("no step larger than the tone's own curvature", worstOut < worstIn * 4.0);

        size_t settleFrames = 0;
        Channel ch2;
        ch2.configure(c0);
        ch2.retune(c1);
        std::vector<float> z(64, 0.0f);
        while (!ch2.settled() && settleFrames < (size_t)kFs) {
            ch2.process(z.data(), z.size(), 1);
            settleFrames += z.size();
        }
        std::printf("        glide settled in %.1f ms\n", 1000.0 * settleFrames / kFs);
        ckTrue("settles within 350 ms", settleFrames < (size_t)(0.35 * kFs));
    }

    std::printf("\n== a NaN does not stay in the filter\n");
    {
        Params p = Params::defaults();
        p.lfGain = 12; p.hpSlope = 2;
        Config cfg; cfg.compute(p, kFs);
        Channel ch; ch.configure(cfg);

        std::vector<float> buf(512, 0.25f);
        buf[10] = NAN;
        buf[11] = INFINITY;
        ch.process(buf.data(), buf.size(), 1);

        bool allFinite = true;
        for (size_t i = 0; i < buf.size(); i++) {
            if (!std::isfinite(buf[i])) allFinite = false;
        }
        ckTrue("output is finite throughout", allFinite);

        std::vector<float> after(512, 0.25f);
        ch.process(after.data(), after.size(), 1);
        bool recovered = std::isfinite(after[511]) && std::fabs(after[511]) > 0.01;
        ckTrue("filter still passes signal afterwards", recovered);
    }

    std::printf("\n== sanitize refuses nonsense\n");
    {
        Params d = Params::defaults();
        Params p = d;
        p.lmfFrequency = NAN;
        p.hmfFrequency = INFINITY;
        p.hfGain       = 1e30f;    // not a control value at all
        p.lfGain       = 60;       // merely out of range
        p.lmfQ         = -5;
        p.hpSlope      = 99;
        p.sanitize();

        ck("NaN frequency replaced by default",  p.lmfFrequency, d.lmfFrequency, 0.001);
        ck("infinite frequency replaced",        p.hmfFrequency, d.hmfFrequency, 0.001);
        ck("absurd gain replaced by default",    p.hfGain,       d.hfGain,       0.001);
        ck("out-of-range gain clamped",          p.lfGain,       kGainMaxDb,     0.001);
        ck("negative Q clamped",                 p.lmfQ,         kQMin,          0.001);
        ck("slope clamped",                      p.hpSlope,      2,              0.001);
    }

    std::printf("\n== interleaved stride is honoured\n");
    {
        Params p = Params::defaults();
        p.hfGain = -20; p.hfFrequency = 8000;
        Config cfg; cfg.compute(p, kFs);

        Channel l, r;
        l.configure(cfg);
        r.configure(cfg);

        std::vector<float> stereo(2048 * 2);
        for (size_t i = 0; i < 2048; i++) {
            stereo[i * 2 + 0] = (float)std::sin(2.0 * kPi * 10000.0 * (double)i / kFs);
            stereo[i * 2 + 1] = 0.0f;
        }

        l.process(stereo.data() + 0, 2048, 2);
        r.process(stereo.data() + 1, 2048, 2);

        double energyR = 0;
        for (size_t i = 0; i < 2048; i++) energyR += (double)stereo[i*2+1] * stereo[i*2+1];

        double energyL = 0;
        for (size_t i = 1024; i < 2048; i++) energyL += (double)stereo[i*2+0] * stereo[i*2+0];

        ckTrue("silent channel stays silent", energyR == 0.0);
        ckTrue("signal channel carries signal", energyL > 1.0);
    }

    // -----------------------------------------------------------------------
    // The fast drawing path has to agree with the slow one it replaces, at
    // every point and for every shape of section - otherwise the picture and
    // the audio are two different equalisers.

    std::printf("\n== the drawing path agrees with magnitudeDb\n");
    {
        Params p = Params::defaults();
        p.hpSlope      =      2; p.hpFrequency  =    80.0f;
        p.lfGain       =   6.0f; p.lfFrequency  =   120.0f;
        p.lmfGain      =  -8.0f; p.lmfFrequency =  1000.0f; p.lmfQ = 4.0f;
        p.hmfGain      =  12.0f; p.hmfFrequency =  5000.0f; p.hmfQ = 0.7f;
        p.hfGain       = -20.0f; p.hfFrequency  =  8000.0f;
        p.outputGain   =  -3.0f;

        Config cfg;
        cfg.compute(p, kFs);

        // A log sweep, which is what a display asks for, running past the
        // audible band at both ends so curveTrig's clamps are exercised too.
        const size_t kPoints = 601;
        std::vector<double> hz(kPoints), trig(kPoints * kCurveTrigStride);
        for (size_t i = 0; i < kPoints; i++) {
            hz[i] = 10.0 * std::pow(4000.0, (double)i / (double)(kPoints - 1));
        }
        curveTrig(hz.data(), kPoints, kFs, trig.data());

        std::vector<float> fast(kPoints);
        curveDb(cfg, trig.data(), kPoints, fast.data());

        double worst = 0.0, worstAt = 0.0;
        for (size_t i = 0; i < kPoints; i++) {
            // Past Nyquist curveTrig holds the point at Nyquist and magnitudeDb
            // does not, so compare where both are asked the same question.
            if (hz[i] > kFs * 0.5) continue;
            double d = std::fabs(magnitudeDb(cfg, hz[i]) - (double)fast[i]);
            if (d > worst) { worst = d; worstAt = hz[i]; }
        }
        std::printf("        worst disagreement at %.0f Hz\n", worstAt);
        // float output against double arithmetic, on a curve reaching -100 dB.
        ck("cascade: fast curve matches magnitudeDb", worst, 0.0, 2e-3);

        // And one section at a time, which is the overlay a host draws for the
        // band under the pointer.
        double worstOne = 0.0;
        std::vector<float> one(kPoints);
        for (int s = 0; s < kStages; s++) {
            curveDb(cfg.stage[s], trig.data(), kPoints, one.data());
            for (size_t i = 0; i < kPoints; i++) {
                if (hz[i] > kFs * 0.5) continue;
                double d = std::fabs(magnitudeDb(cfg.stage[s], hz[i], kFs)
                                     - (double)one[i]);
                if (d > worstOne) worstOne = d;
            }
        }
        ck("one section: fast curve matches magnitudeDb", worstOne, 0.0, 2e-3);

        // The table is the expensive half, and the whole design rests on it
        // depending on nothing a control can move - otherwise a drag would have
        // to rebuild it every frame.
        Params moved = p;
        moved.lmfGain = 3.0f; moved.hpFrequency = 300.0f; moved.hfBell = true;
        Config movedCfg;
        movedCfg.compute(moved, kFs);

        std::vector<float> kept(kPoints), remade(kPoints);
        curveDb(movedCfg, trig.data(), kPoints, kept.data());

        std::vector<double> rebuilt(kPoints * kCurveTrigStride);
        curveTrig(hz.data(), kPoints, kFs, rebuilt.data());
        curveDb(movedCfg, rebuilt.data(), kPoints, remade.data());

        bool same = true;
        for (size_t i = 0; i < kPoints; i++) if (kept[i] != remade[i]) same = false;
        ckTrue("the table survives a control move", same);
    }

    std::printf("\n== a flat equaliser draws a flat line\n");
    {
        const size_t kPoints = 256;
        std::vector<double> hz(kPoints), trig(kPoints * kCurveTrigStride);
        for (size_t i = 0; i < kPoints; i++) {
            hz[i] = 20.0 * std::pow(1000.0, (double)i / (double)(kPoints - 1));
        }
        curveTrig(hz.data(), kPoints, kFs, trig.data());

        Config cfg;
        cfg.compute(Params::defaults(), kFs);

        std::vector<float> db(kPoints);
        curveDb(cfg, trig.data(), kPoints, db.data());

        double worst = 0.0;
        for (size_t i = 0; i < kPoints; i++) {
            worst = std::fmax(worst, std::fabs((double)db[i]));
        }
        ck("defaults draw 0 dB everywhere", worst, 0.0, 1e-5);

        // A high-pass really does reach zero at DC, and the floor is what keeps
        // that a number rather than letting -inf into a polyline.
        Params hp = Params::defaults();
        hp.hpSlope = 2; hp.hpFrequency = 350.0f;
        cfg.compute(hp, kFs);
        curveDb(cfg, trig.data(), kPoints, db.data());

        bool allFinite = true;
        for (size_t i = 0; i < kPoints; i++) {
            if (!(db[i] > -1e30f && db[i] < 1e30f)) allFinite = false;
        }
        ckTrue("24 dB/oct at the bottom of the sweep stays finite", allFinite);
        ckTrue("and is well below 0 dB there", db[0] < -40.0f);
    }

    std::printf("\n%s  (%d failures)\n", sFail ? "FAILED" : "all checks passed", sFail);
    return sFail ? 1 : 0;
}
