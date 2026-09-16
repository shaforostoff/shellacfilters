/* ========================================
 *  paraeq_au_verify - the MacAU port against the core it shares.
 *
 *  plugins/MacAU/ParaEQ compiles the same paraeq_core.cpp that foo_dsp_paraeq
 *  does. The point of this test is that it stays that way: that the Audio Unit
 *  wrapper publishes sixteen controls, gathers them, and adds no DSP of its
 *  own.
 *
 *  Unlike declick_au_verify and dehum_au_verify, the comparison here is exact
 *  rather than within a couple of ULP. Those two compute in double and dither
 *  on the way down to the host's 32 bit float, which is the Airwindows house
 *  pattern and costs a bit-exact comparison. This wrapper asks the core for
 *  floats instead - process() is instantiated for float and computes each stage
 *  in double regardless - so what it writes is what foo_dsp_paraeq writes, and
 *  anything at all in the last bit is a real difference rather than rounding.
 *
 *  Around that, the things this AU has to get right:
 *
 *    - zero latency and no tail, and therefore no PropertyChanged: nothing
 *      here resizes a pipeline, so a host should never be told to re-read
 *      anything;
 *    - sixteen parameters in the core's own units, with the ranges the core
 *      declares, the clump on each one, and value strings for the one control
 *      whose number means nothing by itself;
 *    - a control move that retunes rather than rebuilds, so the curve glides;
 *    - Reset() that clears the filter state and keeps the settings;
 *    - mono as well as stereo;
 *    - a host may hand the same buffer in and out.
 *
 *  Built against plugins/MacAU/au_shim, which is enough of Apple's AU base
 *  classes to compile the plug-in and drive it, and nothing else. Read that
 *  folder's README for what it does and does not establish: this links the
 *  plug-in in rather than loading a built component, so the bundle, the
 *  Info.plist registration and the entry point are scripts/build.sh --validate
 *  and auval's business, not this file's.
 *
 *  Mirror identity - that MacAU/ParaEQ/paraeq_core.cpp is byte-identical to the
 *  canonical one - is not checked here. That is scripts/sync_cores.sh.
 * ======================================== */

#include "ParaEQ.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

int g_failures = 0;
bool g_silenceCleared = true;

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-56s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
const int kFrames = 30000;

//! Deterministic, so a failure is reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Tones spread across the bands the equaliser is for, over a noise floor, and
//! the two channels deliberately different: the wrapper holds one Channel per
//! side and a test with identical input would not notice them being crossed.
void makeSignal(std::vector<float> & l, std::vector<float> & r, int n) {
    l.assign((size_t)n, 0.0f);
    r.assign((size_t)n, 0.0f);
    Rng rng(2463534242u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / (double)kRate;
        const double two_pi = 6.283185307179586;
        const double bass  = 0.30 * sin(two_pi *   55.0 * t);
        const double low   = 0.20 * sin(two_pi *  220.0 * t);
        const double mid   = 0.15 * sin(two_pi * 1000.0 * t);
        const double high  = 0.10 * sin(two_pi * 5000.0 * t);
        const double hiss  = 0.02 * rng.centred();
        l[(size_t)i] = (float)(bass + mid + hiss);
        r[(size_t)i] = (float)(low + high + hiss * 0.5);
    }
}

//! Something for every band to do. A flat equaliser would pass most of this
//! file whatever the wrapper did with the controls.
paraeq::Params workingParams() {
    paraeq::Params p = paraeq::Params::defaults();
    p.hpFrequency  =   80.0f;
    p.hpSlope      =      2;      // 24 dB/oct, so both high-pass stages are live
    p.lfGain       =    4.5f;
    p.lfFrequency  =  120.0f;
    p.lfBell       =  false;
    p.lmfGain      =   -6.0f;
    p.lmfFrequency =  900.0f;
    p.lmfQ         =    2.5f;
    p.hmfGain      =    3.0f;
    p.hmfFrequency = 4500.0f;
    p.hmfQ         =    1.5f;
    p.hfGain       =  -10.0f;
    p.hfFrequency  = 7000.0f;
    p.hfBell       =   true;      // and the other switch the other way
    p.outputGain   =   -2.0f;
    p.bypass       =  false;
    return p;
}

//! The controls, in the order ParaEQ.h declares them. The wrapper has its own
//! copy of this correspondence; writing it out again here is the point, because
//! a wrapper that quietly swapped two of them would otherwise still agree with
//! itself.
void apply(ParaEQ & fx, const paraeq::Params & p) {
    fx.SetParameter(kParam_A, p.hpFrequency);
    fx.SetParameter(kParam_B, (float)p.hpSlope);
    fx.SetParameter(kParam_C, p.lfGain);
    fx.SetParameter(kParam_D, p.lfFrequency);
    fx.SetParameter(kParam_E, p.lfBell ? 1.0f : 0.0f);
    fx.SetParameter(kParam_F, p.lmfGain);
    fx.SetParameter(kParam_G, p.lmfFrequency);
    fx.SetParameter(kParam_H, p.lmfQ);
    fx.SetParameter(kParam_I, p.hmfGain);
    fx.SetParameter(kParam_J, p.hmfFrequency);
    fx.SetParameter(kParam_K, p.hmfQ);
    fx.SetParameter(kParam_L, p.hfGain);
    fx.SetParameter(kParam_M, p.hfFrequency);
    fx.SetParameter(kParam_N, p.hfBell ? 1.0f : 0.0f);
    fx.SetParameter(kParam_O, p.outputGain);
    fx.SetParameter(kParam_P, p.bypass ? 1.0f : 0.0f);
}

//! One render call, the way a host makes it. `channels` is 1 or 2; the plug-in
//! offers both.
void render(ParaEQ & fx, float * const * in, float * const * out, int frames, int channels) {
    AudioBufferList inList, outList;
    memset(&inList, 0, sizeof inList);
    memset(&outList, 0, sizeof outList);
    inList.mNumberBuffers = (UInt32)channels;
    outList.mNumberBuffers = (UInt32)channels;
    for (int c = 0; c < channels; ++c) {
        inList.mBuffers[c].mNumberChannels = 1;
        inList.mBuffers[c].mDataByteSize = (UInt32)(sizeof(float) * (size_t)frames);
        inList.mBuffers[c].mData = in[c];
        outList.mBuffers[c].mNumberChannels = 1;
        outList.mBuffers[c].mDataByteSize = (UInt32)(sizeof(float) * (size_t)frames);
        outList.mBuffers[c].mData = out[c];
    }
    //  A plug-in holding filter state that rings has to clear this.
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;
    fx.ProcessBufferLists(flags, inList, outList, (UInt32)frames);
    if (flags & kAudioUnitRenderAction_OutputIsSilence) g_silenceCleared = false;
}

//! Push the whole signal through, cycling round `blockSizes`.
void runBlocks(ParaEQ & fx, const std::vector<float> & inL, const std::vector<float> & inR,
               std::vector<float> & outL, std::vector<float> & outR,
               const std::vector<int> & blockSizes, int channels = 2) {
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
        float * in[2]  = { &bl[0], &br[0] };
        float * out[2] = { &ol[0], &orr[0] };
        render(fx, in, out, want, channels);
        for (int i = 0; i < want; ++i) {
            outL[(size_t)(pos + i)] = ol[(size_t)i];
            if (channels > 1) outR[(size_t)(pos + i)] = orr[(size_t)i];
        }
        pos += want;
    }
}

std::vector<int> oneBlockSize(int n) { return std::vector<int>(1, n); }

//! An initialized plug-in at the rate a host would have told it about, with the
//! controls already where they are going: Initialize() snaps to the curve, so
//! setting them first is what a restored session does and what leaves nothing
//! gliding.
void start(ParaEQ & fx, double rate, const paraeq::Params & p) {
    apply(fx, p);
    fx.AUShimSetSampleRate(rate);
    if (fx.Initialize() != noErr) check(false, "Initialize() returned an error");
    fx.AUShimResetPropertyChangeCount();
}

//! The core, driven directly with the same Config and the same float buffer.
void reference(const std::vector<float> & in, const paraeq::Params & p, double rate,
               std::vector<float> & out) {
    out = in;
    paraeq::Config cfg;
    cfg.compute(p, rate);
    paraeq::Channel ch;
    ch.configure(cfg);
    paraeq::scoped_flush_denormals ftz;
    if (!out.empty()) ch.process(&out[0], out.size(), 1);
}

//! Worst absolute difference, and how many samples differ at all.
double worst(const std::vector<float> & got, const std::vector<float> & want, int * differing) {
    double w = 0.0;
    int d = 0;
    const size_t n = got.size() < want.size() ? got.size() : want.size();
    for (size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) ++d;
        w = fmax(w, fabs((double)got[i] - (double)want[i]));
    }
    if (differing) *differing = d;
    return w;
}

// ---------------------------------------------------------------------------

//! The wrapper adds no DSP, and it declares no delay.
void testAgainstCore(const std::vector<float> & inL, const std::vector<float> & inR,
                     const paraeq::Params & p) {
    std::vector<float> refL, refR;
    reference(inL, p, (double)kRate, refL);
    reference(inR, p, (double)kRate, refR);

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, p);

    check(fx.GetLatency() == 0.0, "GetLatency() is zero - nothing is held back");
    check(fx.GetTailTime() == 0.0, "GetTailTime() is zero");
    check(!fx.SupportsTail(), "the plug-in claims no tail, as foo_dsp_paraeq reports none");

    std::vector<float> outL, outR;
    runBlocks(fx, inL, inR, outL, outR, oneBlockSize(512));

    char d[96];
    int differing = 0;
    double w = worst(outL, refL, &differing);
    snprintf(d, sizeof d, "%d of %d samples differ, worst %.3e", differing, kFrames, w);
    check(differing == 0, "AU output == the core driven directly, bit for bit", d);
    w = worst(outR, refR, &differing);
    snprintf(d, sizeof d, "%d of %d samples differ, worst %.3e", differing, kFrames, w);
    check(differing == 0, "the right channel agrees too, and is its own channel", d);

    check(g_silenceCleared, "kAudioUnitRenderAction_OutputIsSilence is cleared");
    check(fx.AUShimPropertyChangeCount() == 0,
          "no PropertyChanged - nothing here resizes a pipeline");
}

//! Any block size, including ragged ones, gives the same stream. A settled
//! equaliser takes process()'s no-interpolation path for whole blocks at a
//! time, so this is exact rather than merely close - which it would not be
//! mid-glide, where the settle check is periodic and lands on block boundaries.
void testBlockSizes(const std::vector<float> & inL, const std::vector<float> & inR,
                    const paraeq::Params & p) {
    std::vector<float> refL, refR;
    reference(inL, p, (double)kRate, refL);
    reference(inR, p, (double)kRate, refR);
    static const int patterns[][4] = {
        { 1, 1, 1, 1 }, { 3, 7, 13, 64 }, { 100, 100, 100, 100 },
        { 1024, 64, 4096, 1 }, { 513, 511, 512, 512 }
    };
    for (int q = 0; q < 5; ++q) {
        const std::vector<int> bs(patterns[q], patterns[q] + 4);
        ParaEQ fx((AudioUnit)0);
        start(fx, (double)kRate, p);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, bs);
        int differing = 0;
        const double w = worst(aL, refL, &differing);
        char what[96], d[64];
        snprintf(what, sizeof what, "block pattern %d/%d/%d/%d gives the same stream",
                 patterns[q][0], patterns[q][1], patterns[q][2], patterns[q][3]);
        snprintf(d, sizeof d, "%d differ, worst %.3e", differing, w);
        check(differing == 0, what, d);
    }
}

//! A host is entitled to hand the same buffer in and out, and normally does.
void testInPlace(const std::vector<float> & inL, const std::vector<float> & inR,
                 const paraeq::Params & p) {
    std::vector<float> refL, refR;
    reference(inL, p, (double)kRate, refL);
    reference(inR, p, (double)kRate, refR);

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, p);
    std::vector<float> bl(inL), br(inR);
    int pos = 0;
    while (pos < kFrames) {
        int want = 256;
        if (want > kFrames - pos) want = kFrames - pos;
        float * both[2] = { &bl[(size_t)pos], &br[(size_t)pos] };
        render(fx, both, both, want, 2);
        pos += want;
    }
    int differing = 0;
    const double w = worst(bl, refL, &differing);
    char d[96];
    snprintf(d, sizeof d, "%d differ, worst %.3e", differing, w);
    check(differing == 0, "rendering in place gives the same stream", d);
    check(worst(br, refR, NULL) == 0.0, "and on the right channel");
}

//! Mono is offered as well as stereo, and it is the same filter with one
//! channel rather than a different path.
void testMono(const std::vector<float> & inL, const paraeq::Params & p) {
    std::vector<float> ref;
    reference(inL, p, (double)kRate, ref);

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, p);
    std::vector<float> outL, outR;
    std::vector<float> silentR((size_t)kFrames, 0.0f);
    runBlocks(fx, inL, silentR, outL, outR, oneBlockSize(256), 1);

    int differing = 0;
    const double w = worst(outL, ref, &differing);
    char d[96];
    snprintf(d, sizeof d, "%d differ, worst %.3e", differing, w);
    check(differing == 0, "a one-channel render is the core on that channel", d);
}

//! Bypass is a target, not a branch: the whole equaliser glides to flat. Set
//! before Initialize() there is nothing to glide, so it is an exact pass-through
//! from the first sample - which is the strongest statement available that the
//! wrapper is not adding anything of its own on the way past.
void testBypass(const std::vector<float> & inL, const std::vector<float> & inR) {
    paraeq::Params p = workingParams();
    p.bypass = true;

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, p);
    std::vector<float> outL, outR;
    runBlocks(fx, inL, inR, outL, outR, oneBlockSize(333));

    int differing = 0;
    const double w = worst(outL, inL, &differing);
    char d[96];
    snprintf(d, sizeof d, "%d of %d samples differ, worst %.3e", differing, kFrames, w);
    check(differing == 0, "Bypass passes the input through unchanged", d);
    check(worst(outR, inR, NULL) == 0.0, "and on the right channel");
}

//! A control move retunes rather than rebuilds, so the curve arrives over
//! kGlideSec instead of switching. Two claims: the sample after the move is not
//! already the new curve, and a second later it is.
void testGlide(const std::vector<float> & inL, const std::vector<float> & inR) {
    const paraeq::Params flat = paraeq::Params::defaults();
    paraeq::Params moved = flat;
    moved.lfGain = 15.0f;          // a large, obviously audible move

    std::vector<float> settled;
    reference(inL, moved, (double)kRate, settled);

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, flat);

    // Half the stream flat, then the move, then the rest.
    const int half = kFrames / 2;
    std::vector<float> aL, aR;
    {
        std::vector<float> preL(inL.begin(), inL.begin() + half);
        std::vector<float> preR(inR.begin(), inR.begin() + half);
        runBlocks(fx, preL, preR, aL, aR, oneBlockSize(128));
    }
    apply(fx, moved);
    std::vector<float> postL(inL.begin() + half, inL.end());
    std::vector<float> postR(inR.begin() + half, inR.end());
    std::vector<float> bL, bR;
    runBlocks(fx, postL, postR, bL, bR, oneBlockSize(128));

    // The curve has to be somewhere other than its destination immediately
    // after the move. Compared against the same input run through a settled
    // equaliser at the new setting: an equaliser that snapped would match it.
    const int early = 64;
    double earlyGap = 0.0, lateGap = 0.0, lateScale = 0.0;
    for (int i = 0; i < early && i < (int)bL.size(); ++i) {
        earlyGap = fmax(earlyGap, fabs((double)bL[(size_t)i] - (double)settled[(size_t)(half + i)]));
    }
    // 200 ms later - ten times kGlideSec - it has arrived. The reference has
    // been running from sample 0 at the new setting and the plug-in has not, so
    // what is compared is two settled filters on the same input rather than two
    // identical histories: the residue is the difference between their states,
    // which at these corner frequencies has decayed long before this point.
    const size_t lateFrom = (size_t)(kRate / 5);
    for (size_t i = lateFrom; i < bL.size(); ++i) {
        lateGap = fmax(lateGap, fabs((double)bL[i] - (double)settled[(size_t)half + i]));
        lateScale = fmax(lateScale, fabs((double)settled[(size_t)half + i]));
    }
    char d[96];
    snprintf(d, sizeof d, "gap right after the move %.4f", earlyGap);
    check(earlyGap > 1e-3, "a control move glides rather than snapping", d);
    snprintf(d, sizeof d, "gap after 200 ms %.2e of %.2f", lateGap, lateScale);
    check(lateScale > 0.0 && lateGap < lateScale * 1e-3,
          "and it arrives at the new curve", d);
    check(fx.AUShimPropertyChangeCount() == 0,
          "a control move tells the host nothing - no latency to re-read");
}

//! Reset() is the transport stopping. It clears the filter state and keeps the
//! settings, so the next take starts where a fresh instance at the same
//! settings would - not flat, and not with the tail of the last one spliced in.
void testReset(const std::vector<float> & inL, const std::vector<float> & inR,
               const paraeq::Params & p) {
    std::vector<float> refL, refR;
    reference(inL, p, (double)kRate, refL);
    reference(inR, p, (double)kRate, refR);

    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, p);

    // Something else entirely, to leave state behind worth clearing.
    std::vector<float> loudL((size_t)4096, 0.8f), loudR((size_t)4096, -0.8f);
    std::vector<float> junkL, junkR;
    runBlocks(fx, loudL, loudR, junkL, junkR, oneBlockSize(512));

    fx.Reset(kAudioUnitScope_Global, 0);

    std::vector<float> aL, aR;
    runBlocks(fx, inL, inR, aL, aR, oneBlockSize(512));

    int differing = 0;
    const double w = worst(aL, refL, &differing);
    char d[96];
    snprintf(d, sizeof d, "%d differ, worst %.3e", differing, w);
    check(differing == 0, "after Reset() the stream is a fresh instance's", d);
    check(worst(aR, refR, NULL) == 0.0, "and on the right channel");
}

//! A host that sends something outside the range it was given, or not a number
//! at all, must not be able to put a NaN in a biquad - it would stay there for
//! the rest of the stream.
void testRobustness(const std::vector<float> & inL, const std::vector<float> & inR) {
    ParaEQ fx((AudioUnit)0);
    start(fx, (double)kRate, workingParams());

    fx.SetParameter(kParam_A, 1.0e9f);                    // far past kHpFreqMax
    fx.SetParameter(kParam_H, -5.0f);                     // below kQMin
    fx.SetParameter(kParam_L, (float)NAN);
    fx.SetParameter(kParam_O, (float)INFINITY);
    fx.SetParameter(kParam_B, 7.0f);                      // no such slope

    std::vector<float> outL, outR;
    runBlocks(fx, inL, inR, outL, outR, oneBlockSize(256));

    bool finite = true;
    for (size_t i = 0; i < outL.size(); ++i) {
        if (!isfinite(outL[i]) || !isfinite(outR[i])) { finite = false; break; }
    }
    check(finite, "out-of-range and non-finite control values stay out of the filter");
}

//! What the plug-in tells the host about its own controls.
void testParameterInfo() {
    ParaEQ fx((AudioUnit)0);

    static const char * names[kNumberOfParameters] = {
        "HP Freq", "HP Slope", "Low Gain", "Low Freq", "Low Bell",
        "LoMid Gain", "LoMid Freq", "LoMid Q",
        "HiMid Gain", "HiMid Freq", "HiMid Q",
        "High Gain", "High Freq", "High Bell",
        "Output", "Bypass"
    };
    // The ranges are the core's constants, and that is the assertion: a control
    // whose travel disagreed with the core would be asking for filters the core
    // clamps behind the host's back.
    static const float mins[kNumberOfParameters] = {
        paraeq::kHpFreqMin, 0.0f, -paraeq::kGainMaxDb, paraeq::kLfFreqMin, 0.0f,
        -paraeq::kGainMaxDb, paraeq::kLmfFreqMin, paraeq::kQMin,
        -paraeq::kGainMaxDb, paraeq::kHmfFreqMin, paraeq::kQMin,
        -paraeq::kGainMaxDb, paraeq::kHfFreqMin, 0.0f,
        -paraeq::kOutputMaxDb, 0.0f
    };
    static const float maxs[kNumberOfParameters] = {
        paraeq::kHpFreqMax, 2.0f, paraeq::kGainMaxDb, paraeq::kLfFreqMax, 1.0f,
        paraeq::kGainMaxDb, paraeq::kLmfFreqMax, paraeq::kQMax,
        paraeq::kGainMaxDb, paraeq::kHmfFreqMax, paraeq::kQMax,
        paraeq::kGainMaxDb, paraeq::kHfFreqMax, 1.0f,
        paraeq::kOutputMaxDb, 1.0f
    };

    bool named = true, ranged = true, flagged = true, defaulted = true, clumped = true;
    for (int i = 0; i < kNumberOfParameters; ++i) {
        AudioUnitParameterInfo info;
        memset(&info, 0, sizeof info);
        if (fx.GetParameterInfo(kAudioUnitScope_Global, (AudioUnitParameterID)i, info) != noErr) {
            named = false;
            continue;
        }
        if (strcmp(info.name, names[i]) != 0) named = false;
        if (info.minValue != mins[i] || info.maxValue != maxs[i]) ranged = false;
        if (!(info.flags & kAudioUnitParameterFlag_IsReadable)
            || !(info.flags & kAudioUnitParameterFlag_IsWritable)) flagged = false;
        if (!(info.flags & kAudioUnitParameterFlag_HasClump)
            || info.clumpID == 0 || info.clumpID > (UInt32)kNumberOfClumps) clumped = false;
        // the classic template slip: an advertised default that is not the one
        // the constructor actually set
        if (info.defaultValue != fx.GetParameter((AudioUnitParameterID)i)) defaulted = false;
    }
    check(named, "all sixteen parameters are named, in the header's order");
    check(ranged, "every range is the core's own constant");
    check(flagged, "every parameter is readable and writable");
    check(clumped, "every parameter is in one of the six clumps");
    check(defaulted, "the advertised default is the one the constructor set");

    // The constructor's defaults are Params::defaults(), so reading the
    // controls back has to reproduce it exactly.
    const paraeq::Params defaults = paraeq::Params::defaults();
    ParaEQ fresh((AudioUnit)0);
    bool roundTrip =
        fresh.GetParameter(kParam_A) == defaults.hpFrequency
        && (int)(fresh.GetParameter(kParam_B) + 0.5f) == defaults.hpSlope
        && fresh.GetParameter(kParam_C) == defaults.lfGain
        && fresh.GetParameter(kParam_D) == defaults.lfFrequency
        && (fresh.GetParameter(kParam_E) > 0.5f) == defaults.lfBell
        && fresh.GetParameter(kParam_F) == defaults.lmfGain
        && fresh.GetParameter(kParam_G) == defaults.lmfFrequency
        && fresh.GetParameter(kParam_H) == defaults.lmfQ
        && fresh.GetParameter(kParam_I) == defaults.hmfGain
        && fresh.GetParameter(kParam_J) == defaults.hmfFrequency
        && fresh.GetParameter(kParam_K) == defaults.hmfQ
        && fresh.GetParameter(kParam_L) == defaults.hfGain
        && fresh.GetParameter(kParam_M) == defaults.hfFrequency
        && (fresh.GetParameter(kParam_N) > 0.5f) == defaults.hfBell
        && fresh.GetParameter(kParam_O) == defaults.outputGain
        && (fresh.GetParameter(kParam_P) > 0.5f) == defaults.bypass;
    check(roundTrip, "the controls start at paraeq::Params::defaults()");

    AudioUnitParameterInfo info;
    memset(&info, 0, sizeof info);
    check(fx.GetParameterInfo(kAudioUnitScope_Global,
                              (AudioUnitParameterID)kNumberOfParameters, info)
          == kAudioUnitErr_InvalidParameter, "an out-of-range parameter is refused");
    check(fx.GetParameterInfo(1 /* input scope */, 0, info)
          == kAudioUnitErr_InvalidParameter, "a non-global scope is refused");

    const AUChannelInfo * ch = NULL;
    const UInt32 n = fx.SupportedNumChannels(&ch);
    check(n == 2 && ch != NULL
          && ch[0].inChannels == 1 && ch[0].outChannels == 1
          && ch[1].inChannels == 2 && ch[1].outChannels == 2,
          "mono in mono out and stereo in stereo out, and nothing else");
}

//! Value strings for the one control whose number means nothing on its own,
//! and for nothing else.
void testValueStrings() {
    ParaEQ fx((AudioUnit)0);
    CFArrayRef strings = NULL;
    check(fx.GetParameterValueStrings(kAudioUnitScope_Global, kParam_B, &strings) == noErr
          && strings != NULL,
          "HP Slope offers names for its three positions");

    bool othersRefuse = true;
    for (int i = 0; i < kNumberOfParameters; ++i) {
        if (i == kParam_B) continue;
        CFArrayRef s = NULL;
        if (fx.GetParameterValueStrings(kAudioUnitScope_Global,
                                        (AudioUnitParameterID)i, &s) == noErr) {
            othersRefuse = false;
        }
    }
    check(othersRefuse, "no other parameter claims to have value strings");
}

//! The six clumps are named, and nothing else is.
void testClumpNames() {
    ParaEQ fx((AudioUnit)0);
    bool allNamed = true;
    for (UInt32 c = 1; c <= (UInt32)kNumberOfClumps; ++c) {
        CFStringRef name = NULL;
        if (fx.CopyClumpName(kAudioUnitScope_Global, c, 0, &name) != noErr || name == NULL) {
            allNamed = false;
        }
    }
    check(allNamed, "all six clumps have a name");

    CFStringRef name = NULL;
    check(fx.CopyClumpName(kAudioUnitScope_Global, 0, 0, &name)
          == kAudioUnitErr_InvalidPropertyValue, "clump 0 - the API's 'none' - is refused");
    check(fx.CopyClumpName(kAudioUnitScope_Global, (UInt32)kNumberOfClumps + 1, 0, &name)
          == kAudioUnitErr_InvalidPropertyValue, "a clump past the last one is refused");
    check(fx.CopyClumpName(1 /* input scope */, 1, 0, &name)
          == kAudioUnitErr_InvalidProperty, "a non-global scope is refused");
}

//! The sample rate reaches the design, so the same controls at a different rate
//! are a different set of coefficients and the same curve.
void testSampleRates(const std::vector<float> & inL, const std::vector<float> & inR,
                     const paraeq::Params & p) {
    static const double rates[] = { 22050.0, 48000.0, 96000.0, 192000.0 };
    for (int i = 0; i < 4; ++i) {
        std::vector<float> ref;
        reference(inL, p, rates[i], ref);
        ParaEQ fx((AudioUnit)0);
        start(fx, rates[i], p);
        std::vector<float> aL, aR;
        runBlocks(fx, inL, inR, aL, aR, oneBlockSize(480));
        int differing = 0;
        const double w = worst(aL, ref, &differing);
        char what[80], d[64];
        snprintf(what, sizeof what, "at %g Hz the AU is the core at %g Hz", rates[i], rates[i]);
        snprintf(d, sizeof d, "%d differ, worst %.3e", differing, w);
        check(differing == 0, what, d);
    }
}

} // anonymous namespace

int main() {
    std::vector<float> inL, inR;
    makeSignal(inL, inR, kFrames);

    const paraeq::Params p = workingParams();
    paraeq::Config cfg;
    cfg.compute(p, (double)kRate);
    printf("paraeq_au_verify: %d stages, glide %.6f per sample at %d Hz, "
           "trim %.4f, heap %zu\n\n",
           (int)paraeq::kStages, cfg.glide, kRate, cfg.outputGain,
           paraeq::Channel().heapBytes());

    testAgainstCore(inL, inR, p);
    testBlockSizes(inL, inR, p);
    testInPlace(inL, inR, p);
    testMono(inL, p);
    testBypass(inL, inR);
    testGlide(inL, inR);
    testReset(inL, inR, p);
    testSampleRates(inL, inR, p);
    testRobustness(inL, inR);
    testParameterInfo();
    testValueStrings();
    testClumpNames();

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
