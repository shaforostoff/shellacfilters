/* ========================================
 *  dehum_verify - correctness of the dehum core.
 *
 *  The parts where a subtle error yields plausible-but-wrong audio rather than
 *  an obvious failure are the hand-rolled FFT and the frequency tracker, so
 *  those are pinned against independent references: a direct DFT, and a tone of
 *  known frequency.
 * ======================================== */

#include "../foo_dsp_dehum/dehum_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

const double kPi = 3.14159265358979323846;

int g_failures = 0;

void check(bool ok, const char * what) {
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

void checkNear(double got, double want, double tol, const char * what) {
    const bool ok = fabs(got - want) <= tol;
    printf("  %-62s %s (got %.6g, want %.6g +-%.3g)\n",
           what, ok ? "ok" : "FAILED", got, want, tol);
    if (!ok) ++g_failures;
}

double rms(const std::vector<double> & v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < v.size(); ++i) s += v[i] * v[i];
    return sqrt(s / (double)v.size());
}

double db(double x) { return 20.0 * log10(x + 1e-300); }

//! Amplitude of the component of `v` at `f`, over [from, to).
double toneAmplitude(const std::vector<double> & v, double f, double sr,
                     size_t from, size_t to) {
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < to && i < v.size(); ++i) {
        const double a = 2.0 * kPi * f * (double)i / sr;
        re += v[i] * cos(a);
        im -= v[i] * sin(a);
    }
    const double n = (double)(to - from);
    return 2.0 * sqrt(re * re + im * im) / n;
}

//! Deterministic white noise, so a failure is always reproducible.
struct Rng {
    uint64_t s = 0x853c49e6748fea9bULL;
    double next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)((int64_t)(s >> 11)) / (double)(1ULL << 52) - 1.0;
    }
};

dehum::Config makeConfig(const dehum::Params & p, double sr) {
    dehum::Config c;
    c.compute(p, sr);
    return c;
}

void runChannel(dehum::Channel & ch, std::vector<double> & buf, size_t block) {
    dehum::scoped_flush_denormals ftz;
    for (size_t pos = 0; pos < buf.size(); pos += block) {
        const size_t n = (buf.size() - pos < block) ? (buf.size() - pos) : block;
        ch.process(&buf[pos], n, 1);
    }
}

// ---------------------------------------------------------------------------

void testMedian() {
    printf("\nmedian\n");
    double a[5] = { 5.0, 1.0, 4.0, 2.0, 3.0 };
    checkNear(dehum::medianInPlace(a, 5), 3.0, 0.0, "median of 5 values");
    double b[4] = { 10.0, 2.0, 8.0, 4.0 };
    checkNear(dehum::medianInPlace(b, 4), 6.0, 0.0, "median of 4 averages the middle pair");
    double c[1] = { 7.0 };
    checkNear(dehum::medianInPlace(c, 1), 7.0, 0.0, "median of one");
}

//! The detector's two medians slide a sorted window rather than sorting one per
//! position, which is only safe if the step leaves the window sorted and holding
//! the right multiset. Pinned here against the sort it replaced - first the step
//! on its own, then a whole sliding median of the kind baselineMedian() runs,
//! which has to agree bit for bit and not merely closely.
void testSlidingWindow() {
    printf("\nsliding window\n");
    unsigned rng = 20240914u;
    // Few distinct values, so duplicates are the rule and the search has to pick
    // an occurrence of the departing one rather than assume uniqueness.
    const int kLevels[3] = { 3, 12, 1000 };

    int bad = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const int levels = kLevels[trial % 3];
        const int n = 1 + (int)((rng = rng * 1664525u + 1013904223u) >> 24) % 64;
        double win[65], want[65];
        for (int k = 0; k < n; ++k) {
            rng = rng * 1664525u + 1013904223u;
            win[k] = (double)((rng >> 8) % (unsigned)levels);
        }
        dehum::medianInPlace(win, n);
        rng = rng * 1664525u + 1013904223u;
        const int at = (int)((rng >> 8) % (unsigned)n);
        rng = rng * 1664525u + 1013904223u;
        const double v = (double)((rng >> 8) % (unsigned)levels);

        for (int k = 0; k < n; ++k) want[k] = win[k];
        want[at] = v;                       // the naive step: swap, then sort
        dehum::medianInPlace(want, n);

        dehum::sortedReplaceAt(win, n, at, v);
        for (int k = 0; k < n; ++k) if (win[k] != want[k]) { ++bad; break; }
    }
    check(bad == 0, "20000 random window steps match a sort of the same values");

    // And the pass built out of it: a sliding median over an array, edges
    // clamping, exactly as the baseline of the spectrum is taken.
    const int bins = 400, span = 30, w = 2 * span + 1;
    std::vector<double> med((size_t)bins), slid((size_t)bins), naive((size_t)bins);
    for (int i = 0; i < bins; ++i) {
        rng = rng * 1664525u + 1013904223u;
        med[(size_t)i] = -90.0 + (double)((rng >> 8) % 60000u) / 1000.0;
    }
    std::vector<double> buf((size_t)w);
    for (int i = 0; i < bins; ++i) {
        for (int k = 0; k < w; ++k) {
            int j = i + k - span;
            j = j < 0 ? 0 : (j > bins - 1 ? bins - 1 : j);
            buf[(size_t)k] = med[(size_t)j];
        }
        naive[(size_t)i] = dehum::medianInPlace(&buf[0], w);
    }
    for (int k = 0; k < w; ++k) {
        int j = k - span;
        j = j < 0 ? 0 : (j > bins - 1 ? bins - 1 : j);
        buf[(size_t)k] = med[(size_t)j];
    }
    slid[0] = dehum::medianInPlace(&buf[0], w);
    for (int i = 1; i < bins; ++i) {
        int jo = i - 1 - span; jo = jo < 0 ? 0 : (jo > bins - 1 ? bins - 1 : jo);
        int jn = i + span;     jn = jn < 0 ? 0 : (jn > bins - 1 ? bins - 1 : jn);
        const double gone = med[(size_t)jo], came = med[(size_t)jn];
        if (gone != came) {
            int a = 0;
            while (a < w && buf[(size_t)a] != gone) ++a;
            dehum::sortedReplaceAt(&buf[0], w, a, came);
        }
        slid[(size_t)i] = buf[(size_t)span];
    }
    bool same = true;
    for (int i = 0; i < bins; ++i) if (slid[(size_t)i] != naive[(size_t)i]) same = false;
    check(same, "a sliding median over 400 bins is bit-identical to a sorted one");
}

//! The FFT is checked indirectly but decisively: a tone placed exactly on a bin
//! must be detected at that frequency to within a small fraction of a bin. A
//! transposed butterfly, a wrong twiddle sign or a bad unpack all move the peak.
void testDetectorOnBinCentres() {
    printf("\nFFT and detector: tones on known frequencies\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    dehum::Config cfg = makeConfig(p, sr);
    const double binHz = sr / (double)cfg.fftSize;

    // The ends are derived from the nominal search range rather than written out
    // as bin numbers. searchTo is a tuning decision and has already been
    // narrowed once; a hardcoded top bin quietly turned into a test of whether
    // the detector ignores things above its range - a different test, and one it
    // passes by doing nothing.
    //
    // The top target is kBaselineHz below searchTo, not at it. The baseline
    // median clamps at the edge of the analysed band, so within kBaselineHz of
    // the top the window fills with copies of the peak's own shoulder, the
    // baseline rises to meet the line and the prominence collapses to a few dB.
    // The bottom does not suffer from it - there the repeated bin is below the
    // line and clamping only flatters the prominence - which is why binLo is set
    // two bins under kSearchFloor and nothing more was needed.
    //
    // So this is the top of the range the detector can actually resolve, and
    // testing above it would be testing the edge effect rather than the FFT.
    const double targets[3] = { ceil((double)dehum::kSearchFloor / binHz) * binHz,
                                60.0 * binHz,
                                floor(((double)p.searchTo - dehum::kBaselineHz) / binHz) * binHz };
    for (int t = 0; t < 3; ++t) {
        const double f = targets[t];
        std::vector<double> buf((size_t)(sr * 30.0), 0.0);
        Rng rng;
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.05 * sin(2.0 * kPi * f * (double)i / sr)
                   + 0.0005 * rng.next();
        }
        dehum::Channel ch;
        ch.configure(cfg);
        runChannel(ch, buf, 1024);
        dehum::LineReport rep[dehum::kMaxLines];
        int n = 0;
        ch.report(rep, (int)dehum::kMaxLines, &n);
        char msg[160];
        snprintf(msg, sizeof(msg), "tone at %.3f Hz found (%d line(s), first %.3f)",
                 f, n, n ? rep[0].frequency : 0.0);
        check(n >= 1 && fabs(rep[0].frequency - f) < 0.15, msg);
    }
}

//! A pinned notch on an exact frequency should annihilate the tone.
void testNotchDepth() {
    printf("\nnotch depth, frequency pinned exactly\n");
    const double sr = 44100.0;
    const double f = 50.0;
    for (int k = 0; k < 3; ++k) {
        const double bw[3] = { 0.3, 1.0, 3.0 };
        dehum::Params p = dehum::Params::defaults();
        p.frequency = (float)f;
        p.harmonics = 1;
        p.bandwidth = (float)bw[k];
        p.rumbleHz = 0.0f;   // measuring the notch, not the high-pass
        dehum::Channel ch;
        ch.configure(makeConfig(p, sr));
        std::vector<double> buf((size_t)(sr * 20.0), 0.0);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.1 * sin(2.0 * kPi * f * (double)i / sr);
        }
        runChannel(ch, buf, 512);
        const size_t from = (size_t)(sr * 10.0);
        const double left = toneAmplitude(buf, f, sr, from, buf.size());
        // The floor is the heterodyne image term, halfWidth/(2*f0) - see the
        // note in the header. Pinning the measured depth to that prediction is
        // a stronger test than an arbitrary threshold: it fails if the notch
        // shape or the lowpass coefficient is wrong in either direction.
        const double predicted = db(bw[k] / (2.0 * f));
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "bandwidth %.1f Hz leaves the tone %.1f dB down (image floor %.1f)",
                 bw[k], db(left / 0.1), predicted);
        check(fabs(db(left / 0.1) - predicted) < 3.0, msg);
    }
}

//! A tone 5 Hz away from the notch must survive it.
void testNotchSelectivity() {
    printf("\nnotch selectivity\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    p.frequency = 50.0f;
    p.harmonics = 1;
    p.bandwidth = 1.0f;
    p.rumbleHz = 0.0f;   // measuring the notch, not the high-pass
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));
    const double f = 55.0;
    std::vector<double> buf((size_t)(sr * 20.0), 0.0);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.1 * sin(2.0 * kPi * f * (double)i / sr);
    }
    runChannel(ch, buf, 512);
    const size_t from = (size_t)(sr * 10.0);
    const double left = toneAmplitude(buf, f, sr, from, buf.size());
    char msg[160];
    snprintf(msg, sizeof(msg), "a tone 5 Hz away loses only %.2f dB",
             -db(left / 0.1));
    check(db(left / 0.1) > -0.5, msg);
}

//! The tracker is the part that matters most: a notch 0.3 Hz off the line leaves
//! the tone only about 14 dB down, and tracking should take it far beyond that.
void testTracker() {
    printf("\nfrequency tracker\n");
    const double sr = 44100.0;
    const double trueF = 41.28;
    const double offsets[4] = { 0.0, 0.3, -0.5, 1.2 };
    for (int k = 0; k < 4; ++k) {
        dehum::Params p = dehum::Params::defaults();
        p.frequency = (float)(trueF + offsets[k]);
        p.harmonics = 1;
        p.bandwidth = 1.0f;
        p.rumbleHz = 0.0f;   // measuring the notch, not the high-pass
        dehum::Channel ch;
        ch.configure(makeConfig(p, sr));
        std::vector<double> buf((size_t)(sr * 40.0), 0.0);
        Rng rng;
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.02 * sin(2.0 * kPi * trueF * (double)i / sr)
                   + 0.01 * rng.next();
        }
        runChannel(ch, buf, 512);
        dehum::LineReport rep[dehum::kMaxLines];
        int n = 0;
        ch.report(rep, (int)dehum::kMaxLines, &n);
        const size_t from = (size_t)(sr * 20.0);
        const double left = toneAmplitude(buf, trueF, sr, from, buf.size());
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "start %+.2f Hz off -> locked %.4f (err %+.4f), tone %.1f dB down",
                 offsets[k], n ? rep[0].frequency : 0.0,
                 n ? rep[0].frequency - trueF : 0.0, -db(left / 0.02));
        check(n == 1 && fabs(rep[0].frequency - trueF) < 0.1
              && db(left / 0.02) < -25.0, msg);
    }
}

//! The tonality ratio has an analytic value for both extremes: 1 for a pure
//! tone, and sqrt(narrow/wide) for noise, because a one-pole's output variance
//! is proportional to its bandwidth. Pinning both ends catches a wrong
//! coefficient, a swapped pair, or an accumulator that is not measuring what it
//! is supposed to.
void testCoherenceScale() {
    printf("\ncoherence, against its analytic values\n");
    const double sr = 44100.0;
    const double f = 50.0;
    const double noiseFloor = sqrt(dehum::kCohNarrowHz / dehum::kCohWideHz);

    struct Case { const char * name; bool tone; bool noise; double want; };
    const Case cases[3] = {
        { "pure tone",             true,  false, 1.0 },
        { "white noise only",      false, true,  noiseFloor },
        { "tone 20 dB over noise", true,  true,  1.0 },
    };
    for (int k = 0; k < 3; ++k) {
        dehum::Params p = dehum::Params::defaults();
        p.frequency = (float)f;      // pin the probe exactly on it
        p.harmonics = 1;
        p.rumbleHz = 0.0f;
        p.dryWet = 0.0f;             // measure, do not subtract
        dehum::Channel ch;
        ch.configure(makeConfig(p, sr));
        std::vector<double> buf((size_t)(sr * 40.0), 0.0);
        Rng rng;
        for (size_t i = 0; i < buf.size(); ++i) {
            double v = 0.0;
            if (cases[k].tone) v += 0.02 * sin(2.0 * kPi * f * (double)i / sr);
            if (cases[k].noise) v += (cases[k].tone ? 0.002 : 0.02) * rng.next();
            buf[i] = v;
        }
        runChannel(ch, buf, 4096);
        dehum::LineReport rep[dehum::kMaxLines];
        int n = 0;
        ch.report(rep, (int)dehum::kMaxLines, &n);
        const double got = n ? rep[0].coherence : -1.0;
        char msg[180];
        snprintf(msg, sizeof(msg), "%-24s coherence %.3f (analytic %.3f)",
                 cases[k].name, got, cases[k].want);
        check(n >= 1 && fabs(got - cases[k].want) < 0.12, msg);
    }
}

//! End to end on synthetic material with a known answer.
void testAutoDetectAndRemove() {
    printf("\nautomatic detection and removal\n");
    const double sr = 44100.0;
    const double f = 41.28;
    dehum::Params p = dehum::Params::defaults();
    // Rumble off, so what is measured is the notch alone. With it on, the
    // high-pass would take a share of the 41 Hz tone too and the "how much of
    // what was removed is the line" figure would no longer mean anything.
    p.rumbleHz = 0.0f;
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));
    std::vector<double> buf((size_t)(sr * 60.0), 0.0);
    Rng rng;
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.02 * sin(2.0 * kPi * f * (double)i / sr)
               + 0.01 * rng.next();
    }
    std::vector<double> orig = buf;
    runChannel(ch, buf, 4096);
    dehum::LineReport rep[dehum::kMaxLines];
    int n = 0;
    ch.report(rep, (int)dehum::kMaxLines, &n);
    char msg[200];
    snprintf(msg, sizeof(msg), "detected %d line(s), first at %.4f Hz",
             n, n ? rep[0].frequency : 0.0);
    check(n >= 1 && fabs(rep[0].frequency - f) < 0.1, msg);
    check(ch.dropouts() == 0, "no dropouts over a minute of steady hum");

    const size_t from = (size_t)(sr * 30.0);
    const double left = toneAmplitude(buf, f, sr, from, buf.size());
    snprintf(msg, sizeof(msg), "hum reduced by %.1f dB in the second half",
             -db(left / 0.02));
    check(db(left / 0.02) < -25.0, msg);

    // What was taken out has to be the tone and almost nothing else. Comparing
    // the total change against the input would prove nothing here - the tone is
    // most of the input, so removing it properly changes the signal a great
    // deal. The question is whether the change is *confined* to the line.
    std::vector<double> diff(buf.size(), 0.0);
    for (size_t i = 0; i < buf.size(); ++i) diff[i] = orig[i] - buf[i];
    std::vector<double> tail(diff.begin() + (ptrdiff_t)from, diff.end());
    const double atLine = toneAmplitude(diff, f, sr, from, diff.size());
    const double all = rms(tail);
    const double confined = (atLine / sqrt(2.0)) / (all + 1e-300);
    snprintf(msg, sizeof(msg),
             "%.1f%% of what was removed is the line itself", 100.0 * confined);
    check(confined > 0.95, msg);
}

//! Clean material must come out untouched: nothing detected, nothing removed.
void testCleanMaterialUntouched() {
    printf("\nclean material\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    // Rumble off: this test is about the *detector* not firing on music, and
    // the high-pass legitimately changes the signal whether it fires or not.
    p.rumbleHz = 0.0f;
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));
    std::vector<double> buf((size_t)(sr * 40.0), 0.0);
    Rng rng;
    // Pink-ish noise plus notes that come and go, none of them continuous.
    double lp = 0.0;
    for (size_t i = 0; i < buf.size(); ++i) {
        lp += 0.02 * (rng.next() - lp);
        const double t = (double)i / sr;
        const int slot = (int)(t * 2.0);
        const double note = 60.0 + 10.0 * (double)(slot % 5);
        const double env = (fmod(t * 2.0, 1.0) < 0.6) ? 1.0 : 0.0;
        buf[i] = 0.3 * lp + 0.05 * env * sin(2.0 * kPi * note * t + (double)slot);
    }
    std::vector<double> orig = buf;
    runChannel(ch, buf, 4096);
    char msg[160];
    snprintf(msg, sizeof(msg), "no line confirmed (%u confirmations)",
             ch.confirmations());
    check(ch.confirmations() == 0, msg);
    bool same = true;
    for (size_t i = 0; i < buf.size(); ++i) if (buf[i] != orig[i]) { same = false; break; }
    check(same, "output is bit-identical to the input");
}

void testBypassIsExact() {
    printf("\ndry/wet 0\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    p.frequency = 50.0f;
    p.dryWet = 0.0f;
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));
    std::vector<double> buf((size_t)(sr * 5.0), 0.0);
    Rng rng;
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.2 * sin(2.0 * kPi * 50.0 * (double)i / sr) + 0.01 * rng.next();
    }
    std::vector<double> orig = buf;
    runChannel(ch, buf, 777);
    bool same = true;
    for (size_t i = 0; i < buf.size(); ++i) if (buf[i] != orig[i]) { same = false; break; }
    check(same, "bit-exact bypass");
}

void testBlockSizeInvariance() {
    printf("\nblock size invariance\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    p.frequency = 50.0f;
    const size_t blocks[4] = { 1, 64, 1024, 65536 };
    std::vector<double> ref;
    bool ok = true;
    for (int k = 0; k < 4; ++k) {
        dehum::Channel ch;
        ch.configure(makeConfig(p, sr));
        std::vector<double> buf((size_t)(sr * 6.0), 0.0);
        Rng rng;
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.2 * sin(2.0 * kPi * 50.0 * (double)i / sr) + 0.01 * rng.next();
        }
        runChannel(ch, buf, blocks[k]);
        if (k == 0) ref = buf;
        else {
            double worst = 0.0;
            for (size_t i = 0; i < buf.size(); ++i) {
                const double d = fabs(buf[i] - ref[i]);
                if (d > worst) worst = d;
            }
            char msg[160];
            snprintf(msg, sizeof(msg), "block %zu matches block 1 (worst %.3g)",
                     blocks[k], worst);
            check(worst == 0.0, msg);
            if (worst != 0.0) ok = false;
        }
    }
    (void)ok;
}

void testHostileInput() {
    printf("\nhostile input\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    p.frequency = 50.0f;
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));

    std::vector<double> buf((size_t)(sr * 3.0), 0.0);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.1 * sin(2.0 * kPi * 50.0 * (double)i / sr);
    }
    // A NaN, an infinity, a huge value and a denormal.
    buf[1000] = (double)NAN;
    buf[2000] = (double)INFINITY;
    buf[3000] = -(double)INFINITY;
    buf[4000] = 1e300;
    buf[5000] = 1e-320;
    runChannel(ch, buf, 512);
    bool finite = true;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (!(buf[i] > -1e30 && buf[i] < 1e30)) { finite = false; break; }
    }
    check(finite, "output stays finite");

    // And clean audio afterwards comes back clean.
    std::vector<double> after((size_t)(sr * 3.0), 0.0);
    for (size_t i = 0; i < after.size(); ++i) {
        after[i] = 0.1 * sin(2.0 * kPi * 400.0 * (double)i / sr);
    }
    runChannel(ch, after, 512);
    const double amp = toneAmplitude(after, 400.0, sr, after.size() / 2, after.size());
    checkNear(amp, 0.1, 0.002, "a 400 Hz tone afterwards passes at full level");
}

void testSampleRates() {
    printf("\nsample rates\n");
    const double rates[6] = { 8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 };
    for (int k = 0; k < 6; ++k) {
        const double sr = rates[k];
        dehum::Params p = dehum::Params::defaults();
        p.frequency = 50.0f;
        p.harmonics = 4;
        p.rumbleHz = 0.0f;   // measuring the notch, not the high-pass
        dehum::Channel ch;
        dehum::Config cfg = makeConfig(p, sr);
        ch.configure(cfg);
        std::vector<double> buf((size_t)(sr * 12.0), 0.0);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.1 * sin(2.0 * kPi * 50.0 * (double)i / sr);
        }
        runChannel(ch, buf, 1000);
        const double left = toneAmplitude(buf, 50.0, sr, buf.size() / 2, buf.size());
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "%7.0f Hz: window %d (%.2f s, %.3f Hz bins), 50 Hz tone %.1f dB down",
                 sr, cfg.fftSize, cfg.fftSize / sr, sr / cfg.fftSize,
                 -db(left / 0.1));
        check(db(left / 0.1) < -30.0, msg);
    }
}

void testRumble() {
    printf("\nrumble high-pass, 4th order Butterworth at 60 Hz\n");
    const double sr = 44100.0;
    const double fc = 60.0;
    dehum::Params p = dehum::Params::defaults();
    p.rumbleHz = (float)fc;
    // Pin a line at 500 Hz purely to keep the detector out of this: a continuous
    // pure tone is exactly what it is built to find, and left on automatic it
    // would notch every probe below searchTo and the measurement would be of the
    // notch and the high-pass together.
    p.frequency = 500.0f;
    p.harmonics = 1;

    const double probes[5] = { 20.0, 40.0, 60.0, 120.0, 300.0 };
    for (int k = 0; k < 5; ++k) {
        dehum::Channel ch;
        ch.configure(makeConfig(p, sr));
        std::vector<double> buf((size_t)(sr * 4.0), 0.0);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.1 * sin(2.0 * kPi * probes[k] * (double)i / sr);
        }
        runChannel(ch, buf, 512);
        const double amp = toneAmplitude(buf, probes[k], sr, buf.size() / 2, buf.size());
        const double got = db(amp / 0.1);
        // |H|^2 = x/(1+x) with x = (f/fc)^(2n), n = 4
        const double x = pow(probes[k] / fc, 8.0);
        const double want = 10.0 * log10(x / (1.0 + x));
        char msg[160];
        snprintf(msg, sizeof(msg), "%5.0f Hz: %7.2f dB (Butterworth says %7.2f)",
                 probes[k], got, want);
        check(fabs(got - want) < 1.5, msg);
    }
}

void testRetuneKeepsState() {
    printf("\nretune\n");
    const double sr = 44100.0;
    dehum::Params p = dehum::Params::defaults();
    p.frequency = 50.0f;
    dehum::Channel ch;
    ch.configure(makeConfig(p, sr));

    dehum::Params q = p;
    q.bandwidth = 2.0f;
    check(ch.retune(makeConfig(q, sr)), "a bandwidth change retunes");
    q.sensitivity = 0.9f;
    check(ch.retune(makeConfig(q, sr)), "a sensitivity change retunes");
    q.searchTo = 400.0f;
    check(ch.retune(makeConfig(q, sr)), "a searchTo change retunes");
    q.harmonics = 8;
    check(ch.retune(makeConfig(q, sr)), "a harmonics change retunes");
    q.rumbleHz = 40.0f;
    check(ch.retune(makeConfig(q, sr)), "a rumble change retunes");
    q.dryWet = 0.5f;
    check(ch.retune(makeConfig(q, sr)), "a dry/wet change retunes");
    q.frequency = 0.0f;
    check(ch.retune(makeConfig(q, sr)), "switching to automatic retunes");
    check(!ch.retune(makeConfig(q, 96000.0)), "a sample rate change does not");
}

void testFootprint() {
    printf("\nfootprint\n");
    const double rates[3] = { 44100.0, 96000.0, 192000.0 };
    for (int k = 0; k < 3; ++k) {
        dehum::Channel ch;
        ch.configure(makeConfig(dehum::Params::defaults(), rates[k]));
        char msg[160];
        snprintf(msg, sizeof(msg), "%7.0f Hz: %zu kB per channel",
                 rates[k], ch.heapBytes() / 1024);
        check(ch.heapBytes() < 6u * 1024u * 1024u, msg);
    }
}

} // anonymous namespace

int main() {
    printf("dehum_verify\n");
    testMedian();
    testSlidingWindow();
    testDetectorOnBinCentres();
    testNotchDepth();
    testNotchSelectivity();
    testTracker();
    testCoherenceScale();
    testAutoDetectAndRemove();
    testCleanMaterialUntouched();
    testBypassIsExact();
    testBlockSizeInvariance();
    testHostileInput();
    testSampleRates();
    testRumble();
    testRetuneKeepsState();
    testFootprint();

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
