/* ========================================
 *  declick_au_verify - the MacAU port against the core it shares.
 *
 *  plugins/MacAU/Declick compiles the same declick_core.cpp that
 *  foo_dsp_declick and plugins/MacVST/Declick do. The point of this test is
 *  that it stays that way: that the Audio Unit wrapper adds a pre-roll, a
 *  latency and tail declaration, and a dither, and no DSP of its own.
 *
 *  Unlike the VST there is no undithered path to compare against - an AU
 *  renders into 32 bit float buffers and the house dither is on the way out -
 *  so the central check is the core driven directly, rounded to float, and the
 *  plug-in required to be within two ULP of it. That is the same relationship
 *  declick_vst_verify establishes for processReplacing; what it costs is the
 *  bit-exact comparison, and what it still catches is any difference that is
 *  not rounding.
 *
 *  Around that, the things an AU has to get right that a VST does not:
 *
 *    - GetLatency() and GetTailTime() are seconds, not samples, and both have
 *      to come back out as cfg.latency at the rate in force;
 *    - PropertyChanged(kAudioUnitProperty_Latency) is what an AU says instead
 *      of ioChanged(), so it must fire for Max repair and Model order and for
 *      nothing else;
 *    - Reset() is where resume() went;
 *    - kAudioUnitRenderAction_OutputIsSilence has to be cleared, because there
 *      is a whole pipeline of audio in here when the input goes quiet;
 *    - a host may hand the same buffer in and out.
 *
 *  Built against plugins/MacAU/au_shim, which is enough of Apple's AU base
 *  classes to compile the plug-in and drive it, and nothing else. Read that
 *  folder's README for what it does and does not establish. In particular this
 *  links the plug-in in rather than loading a built component, so it says
 *  nothing about the Component Manager, the .r resource or the entry point
 *  beyond the fact that they compile.
 *
 *  Mirror identity - that MacAU/Declick/declick_core.cpp is byte-identical to
 *  the canonical one - is not checked here. That is scripts/sync_cores.sh.
 * ======================================== */

#include "Declick.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

//! What the plug-in opens on: the core's defaults, order 128 included. The
//! slider runs 16 .. 256 in eights, and the default position lands on 128.
declick::Params wrapperDefaults() {
    return declick::Params::defaults();
}

int g_failures = 0;
bool g_silenceCleared = true;

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-52s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
const int kFrames = 60000;

//! What "silent" means in a dithered 32 bit float format. The house dither is
//! added to a zero sample too - frexpf(0) reports exponent 0, so the noise
//! lands one bit below the float LSB at unit scale - so nothing this plug-in
//! emits is ever exactly zero and a test that asks for exactly zero is asking
//! the wrong question. Measured peak on a silent stretch is 5.4e-08.
const double kSilenceFloor = 1.2e-7;    // one ULP of 1.0f

//! Deterministic, so a failure is reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Tones over a surface-noise floor, optionally with clicks at known places.
//! The floor is not decoration: the detector thresholds against a robust
//! estimate of its own scale, and on noiseless synthetic material that estimate
//! collapses towards its floor and everything reads as tens of sigmas.
void makeSignal(std::vector<float> & l, std::vector<float> & r,
                std::vector<int> & clickAt, int n, bool withClicks) {
    l.assign((size_t)n, 0.0f);
    r.assign((size_t)n, 0.0f);
    clickAt.clear();
    Rng rng(2463534242u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / (double)kRate;
        const double v = 0.30 * sin(6.283185307179586 * 220.0 * t)
                       + 0.15 * sin(6.283185307179586 * 661.0 * t)
                       + 0.07 * sin(6.283185307179586 * 1237.0 * t);
        l[(size_t)i] = (float)(v + rng.centred() * 2.0e-3);   // about -60 dBFS
        r[(size_t)i] = (float)(v * 0.8 + rng.centred() * 2.0e-3);
    }
    if (!withClicks) return;
    for (int i = 4000; i < n - 4000; i += 3001) {
        clickAt.push_back(i);
        for (int k = 0; k < 4; ++k) {
            const float s = (k & 1) ? -0.55f : 0.75f;
            l[(size_t)(i + k)] += s;
            r[(size_t)(i + k)] += s;
        }
    }
}

//! One render call, wired the way a host wires one.
void render(Declick & fx, float * inL, float * inR, float * outL, float * outR, int frames) {
    AudioBufferList in, out;
    in.mNumberBuffers = 2;
    in.mBuffers[0].mNumberChannels = 1;
    in.mBuffers[0].mDataByteSize = (UInt32)(frames * sizeof(float));
    in.mBuffers[0].mData = inL;
    in.mBuffers[1] = in.mBuffers[0];
    in.mBuffers[1].mData = inR;
    out = in;
    out.mBuffers[0].mData = outL;
    out.mBuffers[1].mData = outR;

    //  Set on the way in, as a host that pulled silence from upstream would.
    //  A plug-in holding a pipeline has to clear it.
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;
    fx.ProcessBufferLists(flags, in, out, (UInt32)frames);
    if (flags & kAudioUnitRenderAction_OutputIsSilence) g_silenceCleared = false;
}

//! Push the whole signal through, cycling round `blockSizes`.
void runBlocks(Declick & fx, const std::vector<float> & inL, const std::vector<float> & inR,
               std::vector<float> & outL, std::vector<float> & outR,
               const std::vector<int> & blockSizes) {
    const int n = (int)inL.size();
    outL.assign((size_t)n, 0.0f);
    outR.assign((size_t)n, 0.0f);
    int pos = 0, b = 0;
    while (pos < n) {
        int want = blockSizes[(size_t)(b++) % blockSizes.size()];
        if (want > n - pos) want = n - pos;
        std::vector<float> bl(inL.begin() + pos, inL.begin() + pos + want);
        std::vector<float> br(inR.begin() + pos, inR.begin() + pos + want);
        std::vector<float> ol((size_t)want, 0.0f), orr((size_t)want, 0.0f);
        render(fx, &bl[0], &br[0], &ol[0], &orr[0], want);
        for (int i = 0; i < want; ++i) {
            outL[(size_t)(pos + i)] = ol[(size_t)i];
            outR[(size_t)(pos + i)] = orr[(size_t)i];
        }
        pos += want;
    }
}

std::vector<int> oneBlockSize(int n) { return std::vector<int>(1, n); }

//! An initialized plug-in at the rate a host would have told it about.
void start(Declick & fx, double rate) {
    fx.AUShimSetSampleRate(rate);
    if (fx.Initialize() != noErr) check(false, "Initialize() returned an error");
    fx.AUShimResetPropertyChangeCount();
}

//! The core, driven directly with the same Config and the same push/pull.
void reference(const std::vector<float> & inL, const std::vector<float> & inR,
               const declick::Config & cfg,
               std::vector<double> & refL, std::vector<double> & refR) {
    const int n = (int)inL.size();
    refL.assign((size_t)n, 0.0);
    refR.assign((size_t)n, 0.0);
    declick::scoped_flush_denormals ftz;
    declick::Channel a, b;
    a.configure(cfg); b.configure(cfg);
    a.prime();        b.prime();
    for (int i = 0; i < n; ++i) {
        double x = inL[(size_t)i], y = inR[(size_t)i];
        a.push(&x, 1, 1); b.push(&y, 1, 1);
        a.pull(&x, 1, 1); b.pull(&y, 1, 1);
        refL[(size_t)i] = x; refR[(size_t)i] = y;
    }
}

//! Worst deviation, and the same figure in ULP of the loudest sample. The
//! Airwindows float dither is scaled to about one LSB and the float store
//! rounds by up to half of one, so two ULP is the whole budget.
double worstUlp(const std::vector<float> & got, const std::vector<double> & want,
                double * outWorst) {
    double w = 0.0, peak = 0.0;
    const size_t n = got.size() < want.size() ? got.size() : want.size();
    for (size_t i = 0; i < n; ++i) {
        w = fmax(w, fabs((double)got[i] - want[i]));
        peak = fmax(peak, fabs(want[i]));
    }
    *outWorst = w;
    const double ulp = ldexp(1.0, ilogb(peak > 0.0 ? peak : 1.0) - 23);
    return w / ulp;
}

// ---------------------------------------------------------------------------

//! The wrapper adds no DSP, and it declares the delay before any audio flows.
void testAgainstCore(const std::vector<float> & inL, const std::vector<float> & inR,
                     const declick::Config & cfg,
                     std::vector<float> & outL, std::vector<float> & outR) {
    std::vector<double> refL, refR;
    reference(inL, inR, cfg, refL, refR);

    Declick fx((AudioUnit)0);
    start(fx, (double)kRate);

    char d[96];
    const double declared = fx.GetLatency() * (double)kRate;
    snprintf(d, sizeof d, "%.1f samples", declared);
    check(fabs(declared - (double)cfg.latency) < 1e-6,
          "GetLatency() is cfg.latency at the rate in force", d);
    check(fx.GetTailTime() == fx.GetLatency(), "GetTailTime() matches the declared latency");
    check(fx.SupportsTail(), "the plug-in claims a tail at all");

    runBlocks(fx, inL, inR, outL, outR, oneBlockSize(512));
    double w = 0.0;
    const double ulpL = worstUlp(outL, refL, &w);
    snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulpL);
    check(ulpL <= 2.0, "AU output == the core driven directly, plus dither", d);
    double wR = 0.0;
    const double ulpR = worstUlp(outR, refR, &wR);
    snprintf(d, sizeof d, "worst %.3e = %.2f ULP", wR, ulpR);
    check(ulpR <= 2.0, "the right channel agrees too", d);
    check(g_silenceCleared, "kAudioUnitRenderAction_OutputIsSilence is cleared");
}

//! Any block size, including ragged ones, gives the same stream. If the
//! pre-roll arithmetic were wrong the core would zero-fill mid-stream and these
//! would diverge grossly, dither or no dither.
void testBlockSizes(const std::vector<float> & inL, const std::vector<float> & inR,
                    const declick::Config & cfg) {
    std::vector<double> refL, refR;
    reference(inL, inR, cfg, refL, refR);
    static const int patterns[][4] = {
        { 1, 1, 1, 1 }, { 3, 7, 13, 64 }, { 100, 100, 100, 100 },
        { 1024, 64, 4096, 1 }, { 513, 511, 512, 512 }
    };
    for (int p = 0; p < 5; ++p) {
        const std::vector<int> bs(patterns[p], patterns[p] + 4);
        Declick fx((AudioUnit)0);
        start(fx, (double)kRate);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, bs);
        double w = 0.0;
        const double ulp = worstUlp(aL, refL, &w);
        char what[96], d[64];
        snprintf(what, sizeof what, "block pattern %d/%d/%d/%d gives the same stream",
                 patterns[p][0], patterns[p][1], patterns[p][2], patterns[p][3]);
        snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulp);
        check(ulp <= 2.0, what, d);
    }
}

//! A host is entitled to hand the same buffer in and out.
void testInPlace(const std::vector<float> & inL, const std::vector<float> & inR,
                 const declick::Config & cfg) {
    std::vector<double> refL, refR;
    reference(inL, inR, cfg, refL, refR);
    Declick fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> bl(inL), br(inR);
    int pos = 0;
    while (pos < kFrames) {
        const int want = (kFrames - pos < 512) ? (kFrames - pos) : 512;
        render(fx, &bl[(size_t)pos], &br[(size_t)pos],
                   &bl[(size_t)pos], &br[(size_t)pos], want);
        pos += want;
    }
    double w = 0.0;
    const double ulp = worstUlp(bl, refL, &w);
    char d[64]; snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulp);
    check(ulp <= 2.0, "rendering in place gives the same stream", d);
}

//! The declared delay is the real one, and clean audio survives it untouched.
void testLatency(const std::vector<float> & outL, const declick::Config & cfg) {
    double lead = 0.0;
    for (int i = 0; i < cfg.latency; ++i) lead = fmax(lead, fabs((double)outL[(size_t)i]));
    char d[64]; snprintf(d, sizeof d, "peak %.3e", lead);
    check(lead < kSilenceFloor, "the first `latency` output samples are silent", d);

    // Click-free input, so what is measured is purely the detector's collateral
    // damage - the README puts that at -46.5 dB on real 78s at this setting.
    std::vector<float> clL, clR, aL, aR;
    std::vector<int> none;
    makeSignal(clL, clR, none, kFrames, false);
    Declick fx((AudioUnit)0);
    start(fx, (double)kRate);
    runBlocks(fx, clL, clR, aL, aR, oneBlockSize(512));
    double err = 0.0, ref = 0.0;
    for (int i = cfg.latency; i < kFrames; ++i) {
        const double want = clL[(size_t)(i - cfg.latency)];
        const double e = (double)aL[(size_t)i] - want;
        err += e * e;
        ref += want * want;
    }
    const double db = 10.0 * log10((err + 1e-30) / (ref + 1e-30));
    snprintf(d, sizeof d, "collateral damage %.1f dB", db);
    check(db < -40.0, "clean audio passes through at the declared delay", d);
}

//! Clicks shrink, and Repair depth is the lever the README says it is.
void testRepair(const std::vector<float> & inL, const std::vector<float> & inR,
                const std::vector<int> & clickAt, const declick::Config & cfg) {
    struct Case { float depth; double mustBeat; const char * what; };
    static const Case cases[] = {
        // At the shipping default only the calibrated 0.45 of each click is
        // subtracted, so a modest reduction is the correct expectation and
        // depth 1 is where a large one belongs.
        { -1.0f, 1.00, "clicks come out smaller at the default depth" },
        {  1.0f, 1.33, "Repair depth 1 takes most of the click out" }
    };
    for (int c = 0; c < 2; ++c) {
        Declick fx((AudioUnit)0);
        if (cases[c].depth >= 0.0f) fx.SetParameter(kParam_D, cases[c].depth);
        start(fx, (double)kRate);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
        double before = 0.0, after = 0.0;
        for (size_t k = 0; k < clickAt.size(); ++k) {
            const int p = clickAt[k];
            for (int j = -2; j < 8; ++j) {
                before = fmax(before, fabs((double)inL[(size_t)(p + j)]));
                after  = fmax(after, fabs((double)aL[(size_t)(p + j + cfg.latency)]));
            }
        }
        char d[96]; snprintf(d, sizeof d, "peak %.3f -> %.3f, %.2f dB",
                             before, after, 20.0 * log10(before / after));
        check(after * cases[c].mustBeat < before, cases[c].what, d);
    }
}

//! Live moves retune; structural ones rebuild and tell the host so. This is
//! the AU's half of what declick_vst_verify asserts about ioChanged().
void testParameterMoves(const std::vector<float> & inL, const std::vector<float> & inR) {
    Declick fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> aL, aR;
    const std::vector<int> bs = oneBlockSize(512);
    runBlocks(fx, inL, inR, aL, aR, bs);
    check(fx.AUShimPropertyChangeCount() == 0,
          "steady-state rendering renegotiates nothing");

    fx.SetParameter(kParam_A, 0.85f);            // Sensitivity: retune
    runBlocks(fx, inL, inR, aL, aR, bs);
    int run = 0, worstRun = 0;
    for (size_t i = 0; i < aL.size(); ++i) {
        if (fabs((double)aL[i]) < kSilenceFloor) { if (++run > worstRun) worstRun = run; } else run = 0;
    }
    check(fx.AUShimPropertyChangeCount() == 0,
          "a Sensitivity move does not renegotiate latency");
    char d[64]; snprintf(d, sizeof d, "longest silent run %d samples", worstRun);
    check(worstRun < 64, "a Sensitivity move leaves no gap in the audio", d);

    // Down from the default 128 to 32 (16 + 8 * floor(0.08 * 30.999)).
    fx.SetParameter(kParam_F, 0.08f);            // Model order: rebuild
    runBlocks(fx, inL, inR, aL, aR, bs);
    declick::Params p32 = wrapperDefaults();
    p32.sensitivity = 0.85f;
    p32.order = 32;
    declick::Config c32; c32.compute(p32, (double)kRate);
    const double declared = fx.GetLatency() * (double)kRate;
    snprintf(d, sizeof d, "%d changes, id %u", (unsigned)fx.AUShimPropertyChangeCount(),
             (unsigned)fx.AUShimLastPropertyChanged());
    check(fx.AUShimPropertyChangeCount() == 1
          && fx.AUShimLastPropertyChanged() == kAudioUnitProperty_Latency,
          "a Model order move announces the latency property", d);
    snprintf(d, sizeof d, "%d -> %.1f samples", c32.latency, declared);
    check(fabs(declared - (double)c32.latency) < 1e-6,
          "the rebuilt latency is the one order 32 needs", d);
    check(fx.GetTailTime() == fx.GetLatency(),
          "declared latency and tail stay consistent after a rebuild");
}

//! Reset() is where resume() went: the next take must not splice in the last.
void testReset(const std::vector<float> & inL, const std::vector<float> & inR) {
    Declick fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> aL, aR;
    runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
    fx.Reset(kAudioUnitScope_Global, 0);
    const std::vector<float> silence((size_t)4096, 0.0f);
    std::vector<float> zL, zR;
    runBlocks(fx, silence, silence, zL, zR, oneBlockSize(512));
    double peak = 0.0;
    for (size_t i = 0; i < zL.size(); ++i) peak = fmax(peak, fabs((double)zL[i]));
    char d[64]; snprintf(d, sizeof d, "peak %.3e", peak);
    check(peak < kSilenceFloor, "Reset() starts from silence, not the previous take", d);
}

//! Sample rate changes, hostile input, and the dry/wet bypass.
void testRobustness(const std::vector<float> & inL, const std::vector<float> & inR,
                    const declick::Config & cfg) {
    {
        Declick fx((AudioUnit)0);
        start(fx, 96000.0);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(256));
        declick::Config c96; c96.compute(wrapperDefaults(), 96000.0);
        const double declared = fx.GetLatency() * 96000.0;
        char d[64]; snprintf(d, sizeof d, "%d -> %.1f samples", c96.latency, declared);
        check(fabs(declared - (double)c96.latency) < 1e-6,
              "a sample rate change re-derives the latency", d);
        bool finite = true;
        for (size_t i = 0; i < aL.size(); ++i) {
            if (!(fabs((double)aL[i]) < 1e30) || !(fabs((double)aR[i]) < 1e30)) finite = false;
        }
        check(finite, "output stays finite at 96 kHz");
    }
    {
        Declick fx((AudioUnit)0);
        start(fx, (double)kRate);
        std::vector<float> bad((size_t)4096, 0.0f);
        for (size_t i = 0; i < bad.size(); ++i) {
            switch (i % 5) {
                case 0:  bad[i] = 0.0f; break;
                case 1:  bad[i] = (float)INFINITY; break;
                case 2:  bad[i] = -(float)INFINITY; break;
                case 3:  bad[i] = 1e30f; break;
                default: bad[i] = 1.4e-45f; break;      // denormal
            }
        }
        std::vector<float> aL, aR;
        runBlocks(fx, bad, bad, aL, aR, oneBlockSize(333));
        bool clean = true;
        for (size_t i = 0; i < aL.size(); ++i) if (!(fabs((double)aL[i]) < 1e30)) clean = false;
        check(clean, "NaN, infinities and denormals produce nothing non-finite");

        std::vector<float> okL, okR;
        runBlocks(fx, inL, inR, okL, okR, oneBlockSize(333));
        double energy = 0.0;
        for (size_t i = okL.size() / 2; i < okL.size(); ++i) energy += (double)okL[i] * okL[i];
        check(energy > 1.0, "the stream recovers after hostile input");
    }
    {
        Declick fx((AudioUnit)0);
        fx.SetParameter(kParam_G, 0.0f);         // Dry/Wet 0
        start(fx, (double)kRate);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
        double w = 0.0, peak = 0.0;
        for (int i = cfg.latency; i < kFrames; ++i) {
            const double want = inL[(size_t)(i - cfg.latency)];
            w = fmax(w, fabs((double)aL[(size_t)i] - want));
            peak = fmax(peak, fabs(want));
        }
        const double ulp = ldexp(1.0, ilogb(peak) - 23);
        char d[80]; snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, w / ulp);
        check(w <= 2.0 * ulp, "Dry/Wet 0 is a bypass at the declared delay, to the dither", d);
    }
}

//! What the plug-in tells the host about its own controls.
void testParameterInfo() {
    Declick fx((AudioUnit)0);
    static const char * names[kNumberOfParameters] = {
        "Sensitv", "Extent", "MaxLen", "Depth", "Passes", "Order", "Dry/Wet"
    };
    bool named = true, ranged = true, flagged = true, defaulted = true;
    for (int i = 0; i < kNumberOfParameters; ++i) {
        AudioUnitParameterInfo info;
        memset(&info, 0, sizeof info);
        if (fx.GetParameterInfo(kAudioUnitScope_Global, (AudioUnitParameterID)i, info) != noErr) {
            named = false;
            continue;
        }
        if (strcmp(info.name, names[i]) != 0) named = false;
        if (info.minValue != 0.0f || info.maxValue != 1.0f) ranged = false;
        if (!(info.flags & kAudioUnitParameterFlag_IsReadable)
            || !(info.flags & kAudioUnitParameterFlag_IsWritable)) flagged = false;
        // the classic template slip: an advertised default that is not the one
        // the constructor actually set
        if (info.defaultValue != fx.GetParameter((AudioUnitParameterID)i)) defaulted = false;
    }
    check(named, "all seven parameters are named, in the VST's order");
    check(ranged, "every parameter is 0..1");
    check(flagged, "every parameter is readable and writable");
    check(defaulted, "the advertised default is the one the constructor set");

    AudioUnitParameterInfo info;
    memset(&info, 0, sizeof info);
    check(fx.GetParameterInfo(kAudioUnitScope_Global,
                              (AudioUnitParameterID)kNumberOfParameters, info)
          == kAudioUnitErr_InvalidParameter, "an out-of-range parameter is refused");
    check(fx.GetParameterInfo(1 /* input scope */, 0, info)
          == kAudioUnitErr_InvalidParameter, "a non-global scope is refused");

    const AUChannelInfo * ch = NULL;
    const UInt32 n = fx.SupportedNumChannels(&ch);
    check(n == 1 && ch != NULL && ch->inChannels == 2 && ch->outChannels == 2,
          "stereo in, stereo out, and nothing else");
}

} // anonymous namespace

int main() {
    std::vector<float> inL, inR;
    std::vector<int> clickAt;
    makeSignal(inL, inR, clickAt, kFrames, true);

    declick::Config cfg;
    cfg.compute(wrapperDefaults(), (double)kRate);
    printf("declick_au_verify: order %d, maxRun %d, madWindow %d, pad %d, "
           "latency %d, passes %d\n\n",
           cfg.order, cfg.maxRun, cfg.madWindow, cfg.pad, cfg.latency, cfg.passes);

    std::vector<float> outL, outR;
    testAgainstCore(inL, inR, cfg, outL, outR);
    testBlockSizes(inL, inR, cfg);
    testInPlace(inL, inR, cfg);
    testLatency(outL, cfg);
    testRepair(inL, inR, clickAt, cfg);
    testParameterMoves(inL, inR);
    testReset(inL, inR);
    testRobustness(inL, inR, cfg);
    testParameterInfo();

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
