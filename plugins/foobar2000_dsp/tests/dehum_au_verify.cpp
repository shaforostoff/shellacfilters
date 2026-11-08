/* ========================================
 *  dehum_au_verify - the MacAU port against the core it shares.
 *
 *  plugins/MacAU/Dehum compiles the same dehum_core.cpp that foo_dsp_dehum and
 *  plugins/MacVST/Dehum do. The point of this test is that it stays that way:
 *  that the Audio Unit wrapper adds a scratch buffer, a dither and nothing
 *  else.
 *
 *  Same arrangement as declick_au_verify: an AU renders into 32 bit float and
 *  the house dither is on the way out, so there is no undithered path to
 *  compare and the requirement is two ULP of the core driven directly rather
 *  than the bit-exact match dehum_vst_verify gets.
 *
 *  The AU-specific half is mostly the opposite of Declick's. This core has no
 *  lookahead, so:
 *
 *    - GetLatency() and GetTailTime() are zero and stay zero;
 *    - PropertyChanged() is never called, for any parameter or across a sample
 *      rate change - the exact counterpart of what dehum_vst_verify asserts
 *      about ioChanged();
 *    - Reset() flushes rather than resets, so what the detector learned about
 *      where the hum is has to survive it.
 *
 *  The wrapper's own addition, the fixed 1024-sample scratch, is what the block
 *  size sweep is about: 1025 is deliberate, one past the scratch, so the
 *  chunking loop has to split a buffer and pick up exactly where it left off.
 *
 *  Built against plugins/MacAU/au_shim; read that folder's README for what it
 *  does and does not establish.
 * ======================================== */

#include "Dehum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

int g_failures = 0;
bool g_silenceCleared = true;

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-52s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
//The detector needs seconds of audio before it acts - it is a duty cycle test
//over a 1.5 s window - so this is 25 s, the same as the VST test drives.
const int kFrames = 44100 * 25;
const double kPi = 3.14159265358979323846;

//! What "silent" means in a dithered 32 bit float format: the house dither is
//! added to a zero sample too, so nothing here is ever exactly zero.
const double kSilenceFloor = 1.2e-7;    // one ULP of 1.0f

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Hum over a surface-noise floor, with a little music so the detector has
//! something to reject as well as something to find.
void makeSignal(std::vector<float> & l, std::vector<float> & r, double humHz) {
    Rng rng(20240813u);
    l.assign((size_t)kFrames, 0.0f);
    r.assign((size_t)kFrames, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const double t = (double)i / (double)kRate;
        const double hum = 0.02 * sin(2.0 * kPi * humHz * t);
        const double note = 0.10 * sin(2.0 * kPi * 220.0 * t)
                          * ((fmod(t, 0.5) < 0.35) ? 1.0 : 0.0);
        l[(size_t)i] = (float)(hum + note + 0.004 * rng.centred());
        r[(size_t)i] = (float)(hum * 0.9 + note * 1.1 + 0.004 * rng.centred());
    }
}

//! One render call, wired the way a host wires one.
void render(Dehum & fx, float * inL, float * inR, float * outL, float * outR, int frames) {
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

    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;
    fx.ProcessBufferLists(flags, in, out, (UInt32)frames);
    if (flags & kAudioUnitRenderAction_OutputIsSilence) g_silenceCleared = false;
}

//! Run the plug-in over the signal in fixed blocks.
void runPlugin(Dehum & fx, const std::vector<float> & inL, const std::vector<float> & inR,
               std::vector<float> & outL, std::vector<float> & outR, int block,
               bool inPlace = false) {
    outL.assign(inL.size(), 0.0f);
    outR.assign(inR.size(), 0.0f);
    std::vector<float> bl((size_t)block), br((size_t)block);
    std::vector<float> ol((size_t)block), orr((size_t)block);
    size_t pos = 0;
    while (pos < inL.size()) {
        const size_t n = (inL.size() - pos < (size_t)block) ? inL.size() - pos
                                                           : (size_t)block;
        for (size_t i = 0; i < n; ++i) { bl[i] = inL[pos + i]; br[i] = inR[pos + i]; }
        if (inPlace) render(fx, &bl[0], &br[0], &bl[0], &br[0], (int)n);
        else         render(fx, &bl[0], &br[0], &ol[0], &orr[0], (int)n);
        for (size_t i = 0; i < n; ++i) {
            outL[pos + i] = inPlace ? bl[i] : ol[i];
            outR[pos + i] = inPlace ? br[i] : orr[i];
        }
        pos += n;
    }
}

//! An initialized plug-in at the rate a host would have told it about.
void start(Dehum & fx, double rate) {
    fx.AUShimSetSampleRate(rate);
    if (fx.Initialize() != noErr) check(false, "Initialize() returned an error");
    fx.AUShimResetPropertyChangeCount();
}

//! The core, driven directly with the default Config. It works in place.
void reference(const std::vector<float> & inL, const std::vector<float> & inR,
               std::vector<double> & refL, std::vector<double> & refR) {
    refL.assign(inL.size(), 0.0);
    refR.assign(inR.size(), 0.0);
    for (size_t i = 0; i < inL.size(); ++i) { refL[i] = inL[i]; refR[i] = inR[i]; }
    dehum::Config cfg;
    cfg.compute(dehum::Params::defaults(), (double)kRate);
    dehum::Channel cL, cR;
    cL.configure(cfg);
    cR.configure(cfg);
    dehum::scoped_flush_denormals ftz;
    cL.process(&refL[0], refL.size(), 1);
    cR.process(&refR[0], refR.size(), 1);
}

//! Worst deviation, in ULP of the loudest sample. The house dither is about one
//! LSB and the float store rounds by up to half of one, so two is the budget.
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

//! Amplitude of the component at `f`, over the back half.
double toneAmplitude(const std::vector<float> & v, double f, size_t from) {
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < v.size(); ++i) {
        const double a = 2.0 * kPi * f * (double)i / (double)kRate;
        re += (double)v[i] * cos(a);
        im -= (double)v[i] * sin(a);
    }
    const double n = (double)(v.size() - from);
    return 2.0 * sqrt(re * re + im * im) / n;
}
double toneAmplitude(const std::vector<float> & v, double f) {
    return toneAmplitude(v, f, v.size() / 2);
}

// ---------------------------------------------------------------------------

//! The one that matters: the wrapper must add no DSP. It also pins the six
//! slider defaults, because the reference is the core at Params::defaults() and
//! nothing here touches a parameter - if a default position mapped anywhere
//! else, this would not be two ULP, it would be a different stream.
void testAgainstCore(const std::vector<float> & inL, const std::vector<float> & inR) {
    printf("\nagainst the core, driven directly\n");
    std::vector<double> refL, refR;
    reference(inL, inR, refL, refR);

    Dehum fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    check(fx.GetLatency() == 0.0, "latency is declared as zero");
    check(fx.GetTailTime() == 0.0, "and so is the tail");
    check(fx.AUShimPropertyChangeCount() == 0, "the host is never asked to renegotiate either");

    char d[96];
    double w = 0.0;
    const double ulpL = worstUlp(vL, refL, &w);
    snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulpL);
    check(ulpL <= 2.0, "AU output == the core driven directly, plus dither", d);
    double wR = 0.0;
    const double ulpR = worstUlp(vR, refR, &wR);
    snprintf(d, sizeof d, "worst %.3e = %.2f ULP", wR, ulpR);
    check(ulpR <= 2.0, "the right channel agrees too", d);
    check(g_silenceCleared, "kAudioUnitRenderAction_OutputIsSilence is cleared");
}

//! The wrapper chunks through a fixed 1024-sample scratch; the core must not be
//! able to tell. 1025 is one past it, on purpose.
void testBlockSizes(const std::vector<float> & inL, const std::vector<float> & inR) {
    printf("\nblock sizes (the wrapper chunks through a fixed scratch)\n");
    std::vector<double> refL, refR;
    reference(inL, inR, refL, refR);
    const int blocks[6] = { 1, 64, 512, 1024, 1025, 4096 };
    for (int k = 0; k < 6; ++k) {
        Dehum fx((AudioUnit)0);
        start(fx, (double)kRate);
        std::vector<float> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, blocks[k]);
        double w = 0.0;
        const double ulp = worstUlp(vL, refL, &w);
        char what[96], d[64];
        snprintf(what, sizeof what, "block %d gives the same stream", blocks[k]);
        snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulp);
        check(ulp <= 2.0, what, d);
    }
}

//! A host is entitled to hand the same buffer in and out.
void testInPlace(const std::vector<float> & inL, const std::vector<float> & inR) {
    printf("\nin place\n");
    std::vector<double> refL, refR;
    reference(inL, inR, refL, refR);
    Dehum fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512, true);
    double w = 0.0;
    const double ulp = worstUlp(vL, refL, &w);
    char d[64]; snprintf(d, sizeof d, "worst %.3e = %.2f ULP", w, ulp);
    check(ulp <= 2.0, "rendering in place gives the same stream", d);
}

//! It actually removes the hum, and leaves the music where it is.
void testRemovesHum(const std::vector<float> & inL, const std::vector<float> & inR,
                    double humHz) {
    printf("\nit actually removes the hum\n");
    Dehum fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);
    const double before = toneAmplitude(inL, humHz);
    const double after = toneAmplitude(vL, humHz);
    char d[96];
    snprintf(d, sizeof d, "%.1f dB down", 20.0 * log10(after / (before + 1e-300) + 1e-300));
    check(after < before * 0.2, "the hum is at least 14 dB down in the back half", d);

    const double nBefore = toneAmplitude(inL, 220.0);
    const double nAfter = toneAmplitude(vL, 220.0);
    snprintf(d, sizeof d, "%.2f dB", 20.0 * log10(nAfter / (nBefore + 1e-300) + 1e-300));
    check(nAfter > nBefore * 0.9, "the 220 Hz note is left alone", d);
}

//! Every parameter retunes live, and none of them renegotiates anything. Only a
//! sample rate change sizes anything in this core, and even that has no latency
//! to declare.
void testParameterMovesRetune(const std::vector<float> & inL, const std::vector<float> & inR) {
    printf("\nevery parameter retunes live\n");
    Dehum fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    static const char * names[kNumberOfParameters] =
        { "Sensitv", "Bandwid", "SrchTo", "Harmncs", "Freq", "Rumble" };
    for (int k = 0; k < kNumberOfParameters; ++k) {
        fx.AUShimResetPropertyChangeCount();
        const float was = fx.GetParameter((AudioUnitParameterID)k);
        fx.SetParameter((AudioUnitParameterID)k, was > 0.5f ? 0.25f : 0.75f);
        std::vector<float> bl(512), br(512);
        for (int i = 0; i < 512; ++i) { bl[(size_t)i] = inL[(size_t)i]; br[(size_t)i] = inR[(size_t)i]; }
        render(fx, &bl[0], &br[0], &bl[0], &br[0], 512);
        //a rebuild would zero the notches, not the signal, so look for silence
        int run = 0, worst = 0;
        for (int i = 0; i < 512; ++i) {
            if (fabs((double)bl[(size_t)i]) < kSilenceFloor) { if (++run > worst) worst = run; }
            else run = 0;
        }
        char what[112], d[80];
        snprintf(what, sizeof what, "%-8s moves without renegotiating latency", names[k]);
        snprintf(d, sizeof d, "%u changes, longest silent run %d",
                 (unsigned)fx.AUShimPropertyChangeCount(), worst);
        check(fx.AUShimPropertyChangeCount() == 0 && worst < 32, what, d);
        fx.SetParameter((AudioUnitParameterID)k, was);
    }
}

//! Reset() is where resume() went, and it flushes rather than resets: the hum
//! should still be coming out suppressed almost immediately, not after another
//! five seconds of re-acquisition.
void testResetKeepsLines(const std::vector<float> & inL, const std::vector<float> & inR,
                         double humHz) {
    printf("\nReset()\n");
    Dehum fx((AudioUnit)0);
    start(fx, (double)kRate);
    std::vector<float> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    fx.Reset(kAudioUnitScope_Global, 0);
    check(fx.AUShimPropertyChangeCount() == 0, "Reset() renegotiates nothing either");

    const int n = kRate * 2;
    std::vector<float> bl((size_t)n), br((size_t)n);
    for (int i = 0; i < n; ++i) { bl[(size_t)i] = inL[(size_t)i]; br[(size_t)i] = inR[(size_t)i]; }
    render(fx, &bl[0], &br[0], &bl[0], &br[0], n);

    std::vector<float> head(bl.begin() + kRate / 2, bl.end());
    std::vector<float> ref(inL.begin() + kRate / 2, inL.begin() + n);
    const double after = toneAmplitude(head, humHz, 0);
    const double before = toneAmplitude(ref, humHz, 0);
    char d[96];
    snprintf(d, sizeof d, "%.1f dB down within 2 s of Reset",
             20.0 * log10(after / (before + 1e-300) + 1e-300));
    check(after < before * 0.5, "Reset() keeps what the detector learned", d);
}

//! Sample rate changes, hostile input, and an older state's Dry/Wet.
void testRobustness(const std::vector<float> & inL, const std::vector<float> & inR) {
    printf("\nrobustness\n");
    {
        Dehum fx((AudioUnit)0);
        start(fx, 96000.0);
        std::vector<float> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, 256);
        check(fx.GetLatency() == 0.0 && fx.AUShimPropertyChangeCount() == 0,
              "a sample rate change still declares nothing");
        bool finite = true;
        for (size_t i = 0; i < vL.size(); ++i) {
            if (!(fabs((double)vL[i]) < 1e30) || !(fabs((double)vR[i]) < 1e30)) finite = false;
        }
        check(finite, "output stays finite at 96 kHz");
    }
    {
        Dehum fx((AudioUnit)0);
        start(fx, (double)kRate);
        std::vector<float> bad((size_t)8192, 0.0f);
        for (size_t i = 0; i < bad.size(); ++i) {
            switch (i % 5) {
                case 0:  bad[i] = 0.0f; break;
                case 1:  bad[i] = (float)INFINITY; break;
                case 2:  bad[i] = -(float)INFINITY; break;
                case 3:  bad[i] = 1e30f; break;
                default: bad[i] = 1.4e-45f; break;      // denormal
            }
        }
        std::vector<float> vL, vR;
        runPlugin(fx, bad, bad, vL, vR, 333);
        bool clean = true;
        for (size_t i = 0; i < vL.size(); ++i) if (!(fabs((double)vL[i]) < 1e30)) clean = false;
        check(clean, "NaN, infinities and denormals produce nothing non-finite");

        std::vector<float> okL, okR;
        runPlugin(fx, inL, inR, okL, okR, 333);
        double energy = 0.0;
        for (size_t i = okL.size() / 2; i < okL.size(); ++i) energy += (double)okL[i] * okL[i];
        check(energy > 1.0, "the stream recovers after hostile input");
    }
    {
        // Dry/Wet is retired: a state from an older build that had it at 0,
        // once a bypass, now cleans like any other.
        Dehum ref((AudioUnit)0);
        start(ref, (double)kRate);
        std::vector<float> rL, rR;
        runPlugin(ref, inL, inR, rL, rR, 512);

        Dehum fx((AudioUnit)0);
        fx.SetParameter(kParam_G, 0.0f);          // an older state's Dry/Wet 0
        start(fx, (double)kRate);
        std::vector<float> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, 512);
        double w = 0.0, moved = 0.0, peak = 0.0;
        for (size_t i = 0; i < vL.size(); ++i) {
            w = fmax(w, fabs((double)vL[i] - (double)rL[i]));
            moved = fmax(moved, fabs((double)vL[i] - (double)inL[i]));
            peak = fmax(peak, fabs((double)rL[i]));
        }
        // Each instance seeds its own dither, so the two agree to the dither.
        const double ulp = ldexp(1.0, ilogb(peak) - 23);
        char d[96]; snprintf(d, sizeof d, "worst %.2f ULP, %.3e from the input", w / ulp, moved);
        check(w <= 2.0 * ulp && moved > 1e-3,
              "an older state's Dry/Wet 0 is ignored: fully wet, to the dither", d);
    }
}

//! What the plug-in tells the host about its own controls.
void testParameterInfo() {
    printf("\nwhat the host is told\n");
    Dehum fx((AudioUnit)0);
    static const char * names[kNumberOfParameters] = {
        "Sensitv", "Bandwid", "SrchTo", "Harmncs", "Freq", "Rumble"
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
        if (info.defaultValue != fx.GetParameter((AudioUnitParameterID)i)) defaulted = false;
    }
    check(named, "all six parameters are named, in the VST's order");
    check(ranged, "every parameter is 0..1");
    check(flagged, "every parameter is readable and writable");
    check(defaulted, "the advertised default is the one the constructor set");

    AudioUnitParameterInfo info;
    memset(&info, 0, sizeof info);
    check(fx.GetParameterInfo(kAudioUnitScope_Global,
                              (AudioUnitParameterID)kNumberOfParameters, info)
          == kAudioUnitErr_InvalidParameter, "the retired Dry/Wet (6) has no info");

    AudioUnitParameterID list[kParameterSlots];
    UInt32 listed = 0;
    fx.GetParameterList(kAudioUnitScope_Global, NULL, listed);
    bool listOk = (listed == (UInt32)kNumberOfParameters);
    if (listOk) {
        fx.GetParameterList(kAudioUnitScope_Global, list, listed);
        for (UInt32 i = 0; i < listed; ++i) if (list[i] != i) listOk = false;
    }
    check(listOk, "the host is listed IDs 0..5, and not the retired Dry/Wet");
    check(fx.GetParameter(kParam_G) == 1.0f,
          "the retired Dry/Wet's slot holds 1: an older build opens a state fully wet");
    check(fx.GetParameterInfo(1 /* input scope */, 0, info)
          == kAudioUnitErr_InvalidParameter, "a non-global scope is refused");

    const AUChannelInfo * ch = NULL;
    const UInt32 n = fx.SupportedNumChannels(&ch);
    check(n == 1 && ch != NULL && ch->inChannels == 2 && ch->outChannels == 2,
          "stereo in, stereo out, and nothing else");
}

} // anonymous namespace

int main() {
    const double humHz = 49.58;   //the mains line measured on a real transfer
    std::vector<float> inL, inR;
    makeSignal(inL, inR, humHz);

    dehum::Config cfg;
    cfg.compute(dehum::Params::defaults(), (double)kRate);
    printf("dehum_au_verify: %d frames at %d Hz, %.2f Hz hum\n",
           kFrames, kRate, humHz);

    testAgainstCore(inL, inR);
    testBlockSizes(inL, inR);
    testInPlace(inL, inR);
    testRemovesHum(inL, inR, humHz);
    testParameterMovesRetune(inL, inR);
    testResetKeepsLines(inL, inR, humHz);
    testRobustness(inL, inR);
    testParameterInfo();

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
