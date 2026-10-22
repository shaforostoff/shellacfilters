/* ========================================
 *  declick_vst_verify - the WinVST port against the core it shares.
 *
 *  plugins/WinVST/Declick compiles the same declick_core.cpp that
 *  foo_dsp_declick does. The point of this test is that it stays that way:
 *  that the VST wrapper adds a pre-roll, a latency declaration and a dither,
 *  and no DSP of its own.
 *
 *  The central check drives declick::Channel directly with the same Config and
 *  the same per-sample push/pull, and requires the plug-in's
 *  processDoubleReplacing output to be identical to the bit.
 *  processDoubleReplacing is undithered - that is the standard Airwindows
 *  arrangement - which is what makes an exact comparison possible at all.
 *
 *  Around that:
 *
 *    - every block size, including deliberately ragged patterns, has to give
 *      the same stream. If the pre-roll arithmetic were wrong the core would
 *      zero-fill mid-stream and these would diverge from one another;
 *    - the reported latency has to be the real one, in both directions;
 *    - a live parameter move must not renegotiate latency or punch a hole,
 *      and a structural one must do exactly the opposite;
 *    - hostile input, sample rate changes, dry/wet bypass, resume(), and the
 *      preset chunk.
 *
 *  Built against plugins/WinVST/vst2_shim - the same clean-room VST2 shim the
 *  shipped DLL is built against, not Steinberg's SDK, which is not in this
 *  repository. Read that folder's README for what it does and does not
 *  establish. Two things this file in particular cannot tell you: that the
 *  plug-in compiles against the real SDK, and anything about the ABI, because
 *  the plug-in is linked in here rather than loaded.
 *  tests/vst_host_verify.cpp is the other half - it loads the finished plug-in
 *  the way a host does and talks to it over the C ABI alone.
 *
 *  The shim's parameter formatter is not the SDK's, so displays are parsed for
 *  their value rather than string-compared.
 *
 *  Mirror identity - that WinVST/Declick/declick_core.cpp is byte-identical
 *  to the canonical one - is not checked here. That is scripts/sync_cores.ps1,
 *  which build_release.ps1 runs before it configures anything.
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
int g_ioChanged = 0;

/*  A stand-in host. The shim has no observation hooks of its own - it is the
 *  shim that ships, not a test double - so the only way to see what the plug-in
 *  told the host is to be the host. audioMasterIOChanged is the one callback
 *  anything in this tree ever makes, and for this plug-in it must happen when
 *  Max repair or Model order moves and at no other time. */
VstIntPtr VSTCALLBACK hostCallback(AEffect * effect, VstInt32 opcode, VstInt32 index,
                                   VstIntPtr value, void * ptr, float opt) {
    (void)effect; (void)index; (void)value; (void)ptr; (void)opt;
    if (opcode == audioMasterIOChanged) { ++g_ioChanged; return 1; }
    if (opcode == audioMasterVersion) return 2400;
    return 0;
}

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-52s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
const int kFrames = 60000;

//! Deterministic, so a failure is reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Tones over a surface-noise floor, optionally with clicks at known places.
//!
//! The noise floor is not decoration. The detector thresholds the prediction
//! residual against a robust estimate of its own scale, so on noiseless
//! synthetic material that estimate collapses towards its 1e-12 floor and
//! everything reads as tens of sigmas. Real transfers have a floor.
void makeSignal(std::vector<double> & l, std::vector<double> & r,
                std::vector<int> & clickAt, int n, bool withClicks) {
    l.assign((size_t)n, 0.0);
    r.assign((size_t)n, 0.0);
    clickAt.clear();
    Rng rng(2463534242u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / (double)kRate;
        const double v = 0.30 * sin(6.283185307179586 * 220.0 * t)
                       + 0.15 * sin(6.283185307179586 * 661.0 * t)
                       + 0.07 * sin(6.283185307179586 * 1237.0 * t);
        l[(size_t)i] = v + rng.centred() * 2.0e-3;           // about -60 dBFS
        r[(size_t)i] = v * 0.8 + rng.centred() * 2.0e-3;
    }
    if (!withClicks) return;
    for (int i = 4000; i < n - 4000; i += 3001) {
        clickAt.push_back(i);
        for (int k = 0; k < 4; ++k) {
            const double s = (k & 1) ? -0.55 : 0.75;
            l[(size_t)(i + k)] += s;
            r[(size_t)(i + k)] += s;
        }
    }
}

//! Push the whole signal through the plug-in, cycling round `blockSizes`.
void runBlocks(Declick & fx, const std::vector<double> & inL,
               const std::vector<double> & inR,
               std::vector<double> & outL, std::vector<double> & outR,
               const std::vector<int> & blockSizes) {
    const int n = (int)inL.size();
    outL.assign((size_t)n, 0.0);
    outR.assign((size_t)n, 0.0);
    int pos = 0, b = 0;
    while (pos < n) {
        int want = blockSizes[(size_t)(b++) % blockSizes.size()];
        if (want > n - pos) want = n - pos;
        std::vector<double> bl(inL.begin() + pos, inL.begin() + pos + want);
        std::vector<double> br(inR.begin() + pos, inR.begin() + pos + want);
        std::vector<double> ol((size_t)want, 0.0), orr((size_t)want, 0.0);
        double * ins[2]  = { &bl[0], &br[0] };
        double * outs[2] = { &ol[0], &orr[0] };
        fx.processDoubleReplacing(ins, outs, want);
        for (int i = 0; i < want; ++i) {
            outL[(size_t)(pos + i)] = ol[(size_t)i];
            outR[(size_t)(pos + i)] = orr[(size_t)i];
        }
        pos += want;
    }
}

std::vector<int> oneBlockSize(int n) { return std::vector<int>(1, n); }

double worstDiff(const std::vector<double> & a, const std::vector<double> & b) {
    double w = 0.0;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) w = fmax(w, fabs(a[i] - b[i]));
    return w;
}

// ---------------------------------------------------------------------------

//! The wrapper adds no DSP: same Config, same push/pull, identical output.
void testAgainstCore(const std::vector<double> & inL, const std::vector<double> & inR,
                     const declick::Config & cfg,
                     std::vector<double> & outL, std::vector<double> & outR) {
    std::vector<double> refL, refR;
    refL.assign((size_t)kFrames, 0.0);
    refR.assign((size_t)kFrames, 0.0);
    {
        declick::scoped_flush_denormals ftz;
        declick::Channel a, b;
        a.configure(cfg); b.configure(cfg);
        a.prime();        b.prime();
        for (int i = 0; i < kFrames; ++i) {
            double x = inL[(size_t)i], y = inR[(size_t)i];
            a.push(&x, 1, 1); b.push(&y, 1, 1);
            a.pull(&x, 1, 1); b.pull(&y, 1, 1);
            refL[(size_t)i] = x; refR[(size_t)i] = y;
        }
    }

    Declick fx(hostCallback);
    char d[96];
    snprintf(d, sizeof d, "%d samples", (int)fx.getAeffect()->initialDelay);
    check(fx.getAeffect()->initialDelay == cfg.latency, "latency is declared before any audio flows", d);
    check(fx.getGetTailSize() == cfg.latency, "getGetTailSize matches the declared delay");

    runBlocks(fx, inL, inR, outL, outR, oneBlockSize(512));
    const double w = fmax(worstDiff(outL, refL), worstDiff(outR, refR));
    snprintf(d, sizeof d, "worst deviation %.3e", w);
    check(w == 0.0, "VST double path == the core driven directly", d);
}

//! Any block size, including ragged ones, gives the identical stream.
void testBlockSizes(const std::vector<double> & inL, const std::vector<double> & inR,
                    const std::vector<double> & refL, const std::vector<double> & refR) {
    static const int patterns[][4] = {
        { 1, 1, 1, 1 }, { 3, 7, 13, 64 }, { 100, 100, 100, 100 },
        { 1024, 64, 4096, 1 }, { 513, 511, 512, 512 }
    };
    for (int p = 0; p < 5; ++p) {
        const std::vector<int> bs(patterns[p], patterns[p] + 4);
        Declick fx(hostCallback);
        std::vector<double> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, bs);
        const double w = fmax(worstDiff(aL, refL), worstDiff(aR, refR));
        char what[96], d[64];
        snprintf(what, sizeof what, "block pattern %d/%d/%d/%d gives the same stream",
                 patterns[p][0], patterns[p][1], patterns[p][2], patterns[p][3]);
        snprintf(d, sizeof d, "worst %.3e", w);
        check(w == 0.0, what, d);
    }
}

//! The declared delay is the real one, and clean audio survives it untouched.
void testLatency(const std::vector<double> & refL, const declick::Config & cfg) {
    bool leadIsZero = true;
    for (int i = 0; i < cfg.latency; ++i) if (refL[(size_t)i] != 0.0) leadIsZero = false;
    check(leadIsZero, "the first `latency` output samples are exactly zero");

    // Click-free input, so the number is purely the detector's collateral
    // damage - the README puts that at -46.5 dB on real 78s at this setting.
    std::vector<double> clL, clR, aL, aR;
    std::vector<int> none;
    makeSignal(clL, clR, none, kFrames, false);
    Declick fx(hostCallback);
    runBlocks(fx, clL, clR, aL, aR, oneBlockSize(512));
    double err = 0.0, ref = 0.0;
    for (int i = cfg.latency; i < kFrames; ++i) {
        const double want = clL[(size_t)(i - cfg.latency)];
        const double e = aL[(size_t)i] - want;
        err += e * e;
        ref += want * want;
    }
    const double db = 10.0 * log10((err + 1e-30) / (ref + 1e-30));
    char d[64]; snprintf(d, sizeof d, "collateral damage %.1f dB", db);
    check(db < -40.0, "clean audio passes through at the declared delay", d);
}

//! Clicks shrink, and Repair depth is the lever the README says it is.
void testRepair(const std::vector<double> & inL, const std::vector<double> & inR,
                const std::vector<double> & refL, const std::vector<int> & clickAt,
                const declick::Config & cfg) {
    struct Case { float depth; double mustBeat; const char * what; };
    static const Case cases[] = {
        // At the shipping default only the calibrated 0.45 of each click is
        // subtracted, which the README measures at about 1.8 dB whole-file - so
        // a modest reduction is the correct expectation, and depth 1 is where a
        // large one belongs.
        { -1.0f, 1.00, "clicks come out smaller at the default depth" },
        {  1.0f, 1.33, "Repair depth 1 takes most of the click out" }
    };
    for (int c = 0; c < 2; ++c) {
        std::vector<double> aL, aR;
        const std::vector<double> * use = &refL;
        Declick fx(hostCallback);
        if (cases[c].depth >= 0.0f) {
            fx.setParameter(kParamD, cases[c].depth);
            runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
            use = &aL;
        }
        double before = 0.0, after = 0.0;
        for (size_t k = 0; k < clickAt.size(); ++k) {
            const int p = clickAt[k];
            // The last clicks come out after the end of the stream once the
            // latency outgrows makeSignal()'s 4000-frame margin, which it has:
            // reading them ran past the vector, a crash on x86.
            if ((size_t)(p + 8 + cfg.latency) > use->size()) continue;
            for (int j = -2; j < 8; ++j) {
                before = fmax(before, fabs(inL[(size_t)(p + j)]));
                after  = fmax(after, fabs((*use)[(size_t)(p + j + cfg.latency)]));
            }
        }
        char d[96]; snprintf(d, sizeof d, "peak %.3f -> %.3f, %.2f dB",
                             before, after, 20.0 * log10(before / after));
        check(after * cases[c].mustBeat < before, cases[c].what, d);
    }
}

//! Live moves retune; structural ones rebuild and say so.
void testParameterMoves(const std::vector<double> & inL, const std::vector<double> & inR) {
    Declick fx(hostCallback);
    std::vector<double> aL, aR;
    const std::vector<int> bs = oneBlockSize(512);
    runBlocks(fx, inL, inR, aL, aR, bs);
    const int baseline = g_ioChanged;

    fx.setParameter(kParamA, 0.85f);            // Sensitivity: retune
    runBlocks(fx, inL, inR, aL, aR, bs);
    int run = 0, worstRun = 0;
    for (size_t i = 0; i < aL.size(); ++i) {
        if (aL[i] == 0.0) { if (++run > worstRun) worstRun = run; } else run = 0;
    }
    check(g_ioChanged == baseline, "a Sensitivity move does not renegotiate latency");
    char d[64]; snprintf(d, sizeof d, "longest silent run %d samples", worstRun);
    check(worstRun < 64, "a Sensitivity move leaves no gap in the audio", d);

    // Down from the default 128 to 32 (16 + 8 * floor(0.08 * 30.999)).
    fx.setParameter(kParamF, 0.08f);            // Model order: rebuild
    runBlocks(fx, inL, inR, aL, aR, bs);
    declick::Params p32 = wrapperDefaults();
    p32.sensitivity = 0.85f;
    p32.order = 32;
    declick::Config c32; c32.compute(p32, (double)kRate);
    snprintf(d, sizeof d, "%d -> %d samples", (int)c32.pad, (int)fx.getAeffect()->initialDelay);
    check(g_ioChanged > baseline, "a Model order move renegotiates latency");
    check(fx.getAeffect()->initialDelay == c32.latency, "the rebuilt latency is the one order 32 needs", d);
    check(fx.getGetTailSize() == fx.getAeffect()->initialDelay,
          "declared delay and tail stay consistent after a rebuild");
}

//! Sample rate changes, hostile input, bypass, the float path, resume().
void testRobustness(const std::vector<double> & inL, const std::vector<double> & inR,
                    const std::vector<double> & refL, const declick::Config & cfg) {
    {
        Declick fx(hostCallback);
        fx.setSampleRate(96000.0f);
        std::vector<double> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(256));
        declick::Config c96; c96.compute(wrapperDefaults(), 96000.0);
        check(fx.getAeffect()->initialDelay == c96.latency, "a sample rate change re-derives the latency");
        bool finite = true;
        for (size_t i = 0; i < aL.size(); ++i) {
            if (!(fabs(aL[i]) < 1e30) || !(fabs(aR[i]) < 1e30)) finite = false;
        }
        check(finite, "output stays finite at 96 kHz");
    }
    {
        // What a host does: rate, then resume, then audio. The delay has to be
        // right before the first block, or the host compensates the wrong amount.
        Declick fx(hostCallback);
        fx.setSampleRate(96000.0f);
        fx.resume();
        declick::Config c96; c96.compute(wrapperDefaults(), 96000.0);
        char d[64];
        snprintf(d, sizeof d, "%d, want %d", (int)fx.getAeffect()->initialDelay, c96.latency);
        check(fx.getAeffect()->initialDelay == c96.latency,
              "resume() declares the latency at the host's rate", d);
    }
    {
        Declick fx(hostCallback);
        std::vector<double> bad((size_t)4096, 0.0);
        for (size_t i = 0; i < bad.size(); ++i) {
            switch (i % 5) {
                case 0:  bad[i] = 0.0; break;
                case 1:  bad[i] = (double)INFINITY; break;
                case 2:  bad[i] = -(double)INFINITY; break;
                case 3:  bad[i] = 1e300; break;
                default: bad[i] = 4.9e-324; break;      // denormal
            }
        }
        std::vector<double> aL, aR;
        runBlocks(fx, bad, bad, aL, aR, oneBlockSize(333));
        bool clean = true;
        for (size_t i = 0; i < aL.size(); ++i) if (!(fabs(aL[i]) < 1e30)) clean = false;
        check(clean, "NaN, infinities and denormals produce nothing non-finite");

        std::vector<double> okL, okR;
        runBlocks(fx, inL, inR, okL, okR, oneBlockSize(333));
        double energy = 0.0;
        for (size_t i = okL.size() / 2; i < okL.size(); ++i) energy += okL[i] * okL[i];
        check(energy > 1.0, "the stream recovers after hostile input");
    }
    {
        Declick fx(hostCallback);
        fx.setParameter(kParamG, 0.0f);         // Dry/Wet 0
        std::vector<double> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
        double w = 0.0;
        for (int i = cfg.latency; i < kFrames; ++i) {
            w = fmax(w, fabs(aL[(size_t)i] - inL[(size_t)(i - cfg.latency)]));
        }
        char d[64]; snprintf(d, sizeof d, "worst %.3e", w);
        check(w == 0.0, "Dry/Wet 0 is a bit-exact bypass at the declared delay", d);
    }
    {
        // The float path is the double path plus the house dither and a float
        // store, so it has to be fed float-quantized input to isolate that -
        // otherwise input quantization alone perturbs a detection and the
        // difference stops being about rounding.
        std::vector<double> qL((size_t)kFrames), qR((size_t)kFrames);
        std::vector<float> fl((size_t)kFrames), fr((size_t)kFrames);
        std::vector<float> ol((size_t)kFrames, 0.0f), orr((size_t)kFrames, 0.0f);
        for (int i = 0; i < kFrames; ++i) {
            fl[(size_t)i] = (float)inL[(size_t)i];
            fr[(size_t)i] = (float)inR[(size_t)i];
            qL[(size_t)i] = (double)fl[(size_t)i];
            qR[(size_t)i] = (double)fr[(size_t)i];
        }
        Declick dbl(hostCallback);
        std::vector<double> dL, dR;
        runBlocks(dbl, qL, qR, dL, dR, oneBlockSize(512));

        Declick flt(hostCallback);
        int pos = 0;
        while (pos < kFrames) {
            const int want = (kFrames - pos < 512) ? (kFrames - pos) : 512;
            float * ins[2]  = { &fl[(size_t)pos], &fr[(size_t)pos] };
            float * outs[2] = { &ol[(size_t)pos], &orr[(size_t)pos] };
            flt.processReplacing(ins, outs, want);
            pos += want;
        }
        double w = 0.0, peak = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            w = fmax(w, fabs((double)ol[(size_t)i] - dL[(size_t)i]));
            peak = fmax(peak, fabs(dL[(size_t)i]));
        }
        // The Airwindows float dither is scaled to about one LSB and the store
        // rounds by up to half of one, so two ULP at the loudest sample covers it.
        const double ulp = ldexp(1.0, ilogb(peak) - 23);
        char d[96]; snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, w / ulp);
        check(w <= 2.0 * ulp, "the float path is the double path plus dither", d);
    }
    {
        Declick fx(hostCallback);
        std::vector<double> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));
        fx.resume();
        const std::vector<double> silence((size_t)4096, 0.0);
        std::vector<double> zL, zR;
        runBlocks(fx, silence, silence, zL, zR, oneBlockSize(512));
        double peak = 0.0;
        for (size_t i = 0; i < zL.size(); ++i) peak = fmax(peak, fabs(zL[i]));
        char d[64]; snprintf(d, sizeof d, "peak %.3e", peak);
        check(peak == 0.0, "resume() starts from silence, not the previous take", d);
    }
    (void)refL;
}

//! The slider-to-core mappings, read back through the parameter displays.
//! Parsed rather than string-compared: the stub's formatter is not the SDK's,
//! and it is the value that is being asserted.
void testParameterMapping() {
    Declick fx(hostCallback);
    char t[64];
    struct Case { int index; float set; double want; const char * what; };
    static const Case cases[] = {
        { kParamC, 0.2f, 4.0,  "Max repair at the default slider reads 4 ms" },
        { kParamC, 0.0f, 0.2,  "Max repair at 0.0 clamps to 0.2 ms" },
        { kParamC, 1.0f, 20.0, "Max repair at 1.0 reads 20 ms" },
        { kParamE, 0.5f, 2.0,  "Passes at the default slider reads 2" },
        { kParamE, 0.0f, 1.0,  "Passes at 0.0 reads 1" },
        { kParamE, 1.0f, 3.0,  "Passes at 1.0 reads 3" },
        { kParamF, 0.4677f, 128.0, "Model order at the default slider reads 128" },
        { kParamF, 0.5f, 136.0, "Model order at mid slider reads 136" },
        { kParamF, 0.0f, 16.0,  "Model order at 0.0 reads 16" },
        { kParamF, 1.0f, 256.0, "Model order at 1.0 reads 256" }
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        fx.setParameter(cases[c].index, cases[c].set);
        memset(t, 0, sizeof t);
        fx.getParameterDisplay(cases[c].index, t);
        const double got = atof(t);
        char d[64]; snprintf(d, sizeof d, "\"%s\"", t);
        check(fabs(got - cases[c].want) < 1e-6, cases[c].what, d);
    }
}

//! Presets: a chunk has to carry every parameter, in the right slot.
void testChunk() {
    Declick a(hostCallback), b(hostCallback);
    // Distinct values, so a parameter read out of position cannot pass.
    const float vals[kNumParameters] = { 0.11f, 0.22f, 0.33f, 0.44f, 0.55f, 0.66f, 0.77f };
    for (int i = 0; i < kNumParameters; ++i) a.setParameter(i, vals[i]);

    void * blob = NULL;
    const VstInt32 size = a.getChunk(&blob, true);
    char d[64]; snprintf(d, sizeof d, "%d bytes", (int)size);
    check(size == (VstInt32)(kNumParameters * sizeof(float)),
          "a chunk is one float per parameter", d);

    b.setChunk(blob, size, true);
    bool same = true;
    for (int i = 0; i < kNumParameters; ++i) if (b.getParameter(i) != vals[i]) same = false;
    check(same, "every parameter survives a chunk round trip");
    free(blob);

    // Out-of-range values in a stored preset must be pinned, not trusted.
    Declick c(hostCallback);
    float wild[kNumParameters] = { -3.0f, 4.0f, -0.5f, 2.0f, -1.0f, 9.0f, -9.0f };
    c.setChunk(wild, (VstInt32)sizeof wild, true);
    bool pinned = true;
    for (int i = 0; i < kNumParameters; ++i) {
        const float v = c.getParameter(i);
        if (!(v >= 0.0f && v <= 1.0f)) pinned = false;
    }
    check(pinned, "a chunk with out-of-range values is pinned to 0..1");
}

} // anonymous namespace

int main() {
    std::vector<double> inL, inR;
    std::vector<int> clickAt;
    makeSignal(inL, inR, clickAt, kFrames, true);

    declick::Config cfg;
    cfg.compute(wrapperDefaults(), (double)kRate);
    printf("declick_vst_verify: order %d, maxRun %d, madWindow %d, pad %d, "
           "latency %d, passes %d\n\n",
           cfg.order, cfg.maxRun, cfg.madWindow, cfg.pad, cfg.latency, cfg.passes);

    std::vector<double> refL, refR;
    testAgainstCore(inL, inR, cfg, refL, refR);
    testBlockSizes(inL, inR, refL, refR);
    testLatency(refL, cfg);
    testRepair(inL, inR, refL, clickAt, cfg);
    testParameterMoves(inL, inR);
    testRobustness(inL, inR, refL, cfg);
    testParameterMapping();
    testChunk();

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
