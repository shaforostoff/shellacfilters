/* ========================================
 *  dehum_vst_verify - the WinVST port against the core it shares.
 *
 *  plugins/WinVST/Dehum compiles the same dehum_core.cpp that foo_dsp_dehum
 *  does. The point of this test is that it stays that way: that the VST wrapper
 *  adds parameter mapping and a dither, and no DSP of its own.
 *
 *  The central check drives dehum::Channel directly with the same Config and
 *  requires the plug-in's processDoubleReplacing output to be identical to the
 *  bit. processDoubleReplacing is undithered - the standard Airwindows
 *  arrangement - which is what makes an exact comparison possible at all.
 *
 *  Around that, the things this wrapper has to get right on its own:
 *
 *    - it chunks long buffers through a fixed scratch, so every block size and
 *      every ragged pattern has to give the same stream;
 *    - latency is zero and must be declared as such, and nothing - no parameter,
 *      no sample rate - may ever renegotiate it. That is the opposite of
 *      Declick, where Max repair and Model order must renegotiate, and it is
 *      why this file asserts ioChanged() is never called;
 *    - every parameter move must retune live rather than rebuild, since only
 *      the sample rate sizes anything in this core;
 *    - resume() must keep what the detector has learned while dropping the
 *      stale analysis window;
 *    - the slider mappings, the off positions on Freq and Rumble, hostile
 *      input and the preset chunk, which keeps the retired Dry/Wet's slot.
 *
 *  Built against plugins/WinVST/vst2_shim - the same clean-room VST2 shim the
 *  shipped DLL is built against, not Steinberg's SDK, which is not in this
 *  repository. Read that folder's README for what it does and does not
 *  establish. Two things this file in particular cannot tell you: that the
 *  plug-in compiles against the real SDK, and anything about the ABI, because
 *  the plug-in is linked in here rather than loaded. tests/vst_host_verify.cpp
 *  is the other half - it loads the finished plug-in the way a host does and
 *  talks to it over the C ABI alone.
 *
 *  The shim's parameter formatter is not the SDK's, so displays are parsed for
 *  their value rather than string-compared.
 *
 *  Mirror identity - that WinVST/Dehum/dehum_core.cpp is byte-identical to the
 *  canonical one - is not checked here. That is scripts/sync_cores.ps1, which
 *  build_release.ps1 runs before it configures anything.
 * ======================================== */

#include "Dehum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

int g_failures = 0;
int g_ioChanged = 0;

/*  A stand-in host. The shim has no observation hooks of its own - it is the
 *  shim that ships, not a test double - so the only way to see what the plug-in
 *  told the host is to be the host. audioMasterIOChanged is the one callback
 *  anything in this tree ever makes, and for this plug-in the correct number of
 *  times is zero. */
VstIntPtr VSTCALLBACK hostCallback(AEffect * effect, VstInt32 opcode, VstInt32 index,
                                   VstIntPtr value, void * ptr, float opt) {
    (void)effect; (void)index; (void)value; (void)ptr; (void)opt;
    if (opcode == audioMasterIOChanged) { ++g_ioChanged; return 1; }
    if (opcode == audioMasterVersion) return 2400;
    return 0;
}

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-56s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
//Long enough for the detector to fill its 1.5 s window, accumulate evidence and
//engage - the whole point of the comparison is that both ports do that at the
//same sample.
const int kFrames = 44100 * 25;
const double kPi = 3.14159265358979323846;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Hum over a surface-noise floor, with a little music so the detector has
//! something to reject as well as something to find.
void makeSignal(std::vector<double> & l, std::vector<double> & r, double humHz) {
    Rng rng(20240813u);
    l.assign((size_t)kFrames, 0.0);
    r.assign((size_t)kFrames, 0.0);
    for (int i = 0; i < kFrames; ++i) {
        const double t = (double)i / (double)kRate;
        const double hum = 0.02 * sin(2.0 * kPi * humHz * t);
        const double note = 0.10 * sin(2.0 * kPi * 220.0 * t)
                          * ((fmod(t, 0.5) < 0.35) ? 1.0 : 0.0);
        const double hiss = 0.004 * rng.centred();
        l[(size_t)i] = hum + note + hiss;
        r[(size_t)i] = hum * 0.9 + note * 1.1 + 0.004 * rng.centred();
    }
}

//! Run the plug-in over the signal in blocks, double path.
void runPlugin(Dehum & fx, const std::vector<double> & inL,
               const std::vector<double> & inR,
               std::vector<double> & outL, std::vector<double> & outR,
               int block) {
    outL.assign(inL.size(), 0.0);
    outR.assign(inR.size(), 0.0);
    std::vector<double> bl((size_t)block), br((size_t)block);
    size_t pos = 0;
    while (pos < inL.size()) {
        const size_t n = (inL.size() - pos < (size_t)block) ? inL.size() - pos
                                                            : (size_t)block;
        for (size_t i = 0; i < n; ++i) { bl[i] = inL[pos + i]; br[i] = inR[pos + i]; }
        double * ins[2]  = { &bl[0], &br[0] };
        double * outs[2] = { &bl[0], &br[0] };   //in place, as a host may do
        fx.processDoubleReplacing(ins, outs, (VstInt32)n);
        for (size_t i = 0; i < n; ++i) { outL[pos + i] = bl[i]; outR[pos + i] = br[i]; }
        pos += n;
    }
}

double worstDiff(const std::vector<double> & a, const std::vector<double> & b) {
    double w = 0.0;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const double d = fabs(a[i] - b[i]);
        if (d > w) w = d;
    }
    return w;
}

//! Amplitude of the component at `f`, over the back half.
double toneAmplitude(const std::vector<double> & v, double f) {
    double re = 0.0, im = 0.0;
    const size_t from = v.size() / 2;
    for (size_t i = from; i < v.size(); ++i) {
        const double a = 2.0 * kPi * f * (double)i / (double)kRate;
        re += v[i] * cos(a);
        im -= v[i] * sin(a);
    }
    const double n = (double)(v.size() - from);
    return 2.0 * sqrt(re * re + im * im) / n;
}

// ---------------------------------------------------------------------------

//! The one that matters: the wrapper must add no DSP.
void testAgainstCore(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nagainst the core, driven directly\n");
    Dehum fx(hostCallback);
    fx.setSampleRate((float)kRate);
    g_ioChanged = 0;
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    check(fx.getAeffect()->initialDelay == 0, "latency is declared as zero");
    check(g_ioChanged == 0, "the host is never asked to renegotiate it");

    dehum::Params p = dehum::Params::defaults();
    dehum::Config cfg;
    cfg.compute(p, (double)kRate);
    dehum::Channel cL, cR;
    cL.configure(cfg);
    cR.configure(cfg);
    std::vector<double> refL = inL, refR = inR;
    {
        dehum::scoped_flush_denormals ftz;
        cL.process(&refL[0], refL.size(), 1);
        cR.process(&refR[0], refR.size(), 1);
    }

    char d[96];
    const double w = worstDiff(vL, refL) + worstDiff(vR, refR);
    snprintf(d, sizeof(d), "worst deviation %.3e", w);
    check(w == 0.0, "VST double path == the core driven directly", d);
}

void testBlockSizes(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nblock sizes (the wrapper chunks through a fixed scratch)\n");
    std::vector<double> refL, refR;
    {
        Dehum fx(hostCallback);
        fx.setSampleRate((float)kRate);
        runPlugin(fx, inL, inR, refL, refR, 1);
    }
    const int blocks[5] = { 64, 512, 1024, 1025, 4096 };
    for (int k = 0; k < 5; ++k) {
        Dehum fx(hostCallback);
        fx.setSampleRate((float)kRate);
        std::vector<double> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, blocks[k]);
        const double w = worstDiff(vL, refL) + worstDiff(vR, refR);
        char what[96], d[64];
        snprintf(what, sizeof(what), "block %d gives the same stream as block 1", blocks[k]);
        snprintf(d, sizeof(d), "worst %.3e", w);
        check(w == 0.0, what, d);
    }
    //1025 is deliberate: one past the 1024 scratch, so the chunking loop has to
    //split a buffer and pick up exactly where it left off.
}

void testRemovesHum(const std::vector<double> & inL, const std::vector<double> & inR,
                    double humHz) {
    printf("\nit actually removes the hum\n");
    Dehum fx(hostCallback);
    fx.setSampleRate((float)kRate);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);
    const double before = toneAmplitude(inL, humHz);
    const double after = toneAmplitude(vL, humHz);
    char d[96];
    snprintf(d, sizeof(d), "%.1f dB down", 20.0 * log10(after / (before + 1e-300) + 1e-300));
    check(after < before * 0.2, "the hum is at least 14 dB down in the back half", d);
    //and the 220 Hz note is not
    const double nBefore = toneAmplitude(inL, 220.0);
    const double nAfter = toneAmplitude(vL, 220.0);
    snprintf(d, sizeof(d), "%.2f dB", 20.0 * log10(nAfter / (nBefore + 1e-300) + 1e-300));
    check(nAfter > nBefore * 0.9, "the 220 Hz note is left alone", d);
}

void testParameterMovesRetune(const std::vector<double> & inL,
                              const std::vector<double> & inR) {
    printf("\nevery parameter retunes live\n");
    Dehum fx(hostCallback);
    fx.setSampleRate((float)kRate);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    //Move all six, one at a time, and require no renegotiation and no hole in
    //the audio. Only a sample rate change may rebuild in this core.
    const char * names[kNumParameters] =
        { "Sensitv", "Bandwid", "SrchTo", "Harmncs", "Freq", "Rumble" };
    for (int k = 0; k < kNumParameters; ++k) {
        const int before = g_ioChanged;
        const float was = fx.getParameter(k);
        fx.setParameter(k, was > 0.5f ? 0.25f : 0.75f);
        std::vector<double> bl(512, 0.0), br(512, 0.0);
        for (int i = 0; i < 512; ++i) {
            bl[(size_t)i] = inL[(size_t)i];
            br[(size_t)i] = inR[(size_t)i];
        }
        double * ins[2] = { &bl[0], &br[0] };
        fx.processDoubleReplacing(ins, ins, 512);
        //a rebuild would zero the notches, not the signal, so look for silence
        int run = 0, worst = 0;
        for (int i = 0; i < 512; ++i) {
            if (bl[(size_t)i] == 0.0) { if (++run > worst) worst = run; } else run = 0;
        }
        char what[112], d[64];
        snprintf(what, sizeof(what), "%-8s moves without renegotiating latency", names[k]);
        snprintf(d, sizeof(d), "longest silent run %d", worst);
        check(g_ioChanged == before && worst < 32, what, d);
        fx.setParameter(k, was);
    }
}

void testRobustness(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nrobustness\n");
    {
        Dehum fx(hostCallback);
        fx.setSampleRate(96000.0f);
        g_ioChanged = 0;
        std::vector<double> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, 512);
        bool finite = true;
        for (size_t i = 0; i < vL.size(); ++i) {
            if (!(vL[i] > -1e30 && vL[i] < 1e30)) { finite = false; break; }
        }
        check(finite, "output stays finite at 96 kHz");
        check(fx.getAeffect()->initialDelay == 0, "latency is still zero at 96 kHz");
        check(g_ioChanged == 0, "a sample rate change renegotiates nothing");
    }
    {
        Dehum fx(hostCallback);
        fx.setSampleRate((float)kRate);
        std::vector<double> bl(2048, 0.0), br(2048, 0.0);
        for (int i = 0; i < 2048; ++i) {
            bl[(size_t)i] = 0.1 * sin(2.0 * kPi * 50.0 * (double)i / kRate);
            br[(size_t)i] = bl[(size_t)i];
        }
        bl[100] = (double)NAN;
        bl[200] = (double)INFINITY;
        bl[300] = -(double)INFINITY;
        bl[400] = 1e300;
        bl[500] = 1e-320;
        double * ins[2] = { &bl[0], &br[0] };
        fx.processDoubleReplacing(ins, ins, 2048);
        bool clean = true;
        for (int i = 0; i < 2048; ++i) {
            if (!(bl[(size_t)i] > -1e30 && bl[(size_t)i] < 1e30)) { clean = false; break; }
        }
        check(clean, "NaN, infinities and denormals produce nothing non-finite");

        for (int i = 0; i < 2048; ++i) {
            bl[(size_t)i] = 0.1 * sin(2.0 * kPi * 400.0 * (double)i / kRate);
            br[(size_t)i] = bl[(size_t)i];
        }
        fx.processDoubleReplacing(ins, ins, 2048);
        double e = 0.0;
        for (int i = 1024; i < 2048; ++i) e += bl[(size_t)i] * bl[(size_t)i];
        check(e > 1.0, "the stream recovers after hostile input");
    }
}

//Dry/Wet is gone: an older chunk that had it at 0, once a bypass, now cleans,
//exactly as the same chunk with it at 1 does.
void testOldDryWetIgnored(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nan older chunk's Dry/Wet\n");
    Dehum ref(hostCallback);
    ref.setSampleRate((float)kRate);
    std::vector<double> rL, rR;
    runPlugin(ref, inL, inR, rL, rR, 512);

    void * data = NULL;
    const VstInt32 size = ref.getChunk(&data, true);
    float old[kChunkFloats];
    memcpy(old, data, sizeof(old));
    free(data);
    old[kChunkDryWet] = 0.0f;

    Dehum fx(hostCallback);
    fx.setChunk(old, (VstInt32)sizeof(old), true);
    fx.setSampleRate((float)kRate);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);
    const double w = worstDiff(vL, rL) + worstDiff(vR, rR);
    const double moved = worstDiff(vL, inL);
    char d[64];
    snprintf(d, sizeof(d), "worst %.3e, %.3e from the input", w, moved);
    check(size == (VstInt32)sizeof(old) && w == 0.0 && moved > 0.0,
          "Dry/Wet 0 in an older chunk is ignored: fully wet", d);
}

void testFloatPathIsDoublePlusDither(const std::vector<double> & inL,
                                     const std::vector<double> & inR) {
    printf("\nthe float path\n");
    std::vector<double> dL, dR;
    {
        Dehum fx(hostCallback);
        fx.setSampleRate((float)kRate);
        runPlugin(fx, inL, inR, dL, dR, 512);
    }
    Dehum fx(hostCallback);
    fx.setSampleRate((float)kRate);
    std::vector<float> fl(512), fr(512);
    double worst = 0.0;
    size_t pos = 0;
    while (pos + 512 <= inL.size()) {
        for (size_t i = 0; i < 512; ++i) {
            fl[i] = (float)inL[pos + i];
            fr[i] = (float)inR[pos + i];
        }
        float * ins[2] = { &fl[0], &fr[0] };
        fx.processReplacing(ins, ins, 512);
        for (size_t i = 0; i < 512; ++i) {
            const double d = fabs((double)fl[i] - dL[pos + i]);
            if (d > worst) worst = d;
        }
        pos += 512;
    }
    //one float ULP at this signal level, plus the dither's own half-ULP
    const double ulp = 2.0e-7;
    char d[80];
    snprintf(d, sizeof(d), "worst %.3e, tolerance %.1e", worst, ulp);
    check(worst <= ulp, "the float path is the double path plus dither", d);
}

void testResumeKeepsLines(const std::vector<double> & inL,
                          const std::vector<double> & inR, double humHz) {
    printf("\nresume()\n");
    Dehum fx(hostCallback);
    fx.setSampleRate((float)kRate);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    //resume() flushes rather than resets, so the hum should still be coming out
    //suppressed almost immediately - not after another 5 s of re-acquisition.
    fx.resume();
    const int n = kRate * 2;
    std::vector<double> bl((size_t)n), br((size_t)n);
    for (int i = 0; i < n; ++i) {
        bl[(size_t)i] = inL[(size_t)i];
        br[(size_t)i] = inR[(size_t)i];
    }
    double * ins[2] = { &bl[0], &br[0] };
    fx.processDoubleReplacing(ins, ins, (VstInt32)n);

    std::vector<double> head(bl.begin() + kRate / 2, bl.end());
    std::vector<double> ref(inL.begin() + kRate / 2, inL.begin() + n);
    const double after = toneAmplitude(head, humHz);
    const double before = toneAmplitude(ref, humHz);
    char d[96];
    snprintf(d, sizeof(d), "%.1f dB down within 2 s of resume",
             20.0 * log10(after / (before + 1e-300) + 1e-300));
    check(after < before * 0.5, "resume() keeps what the detector learned", d);
}

//! The slider defaults must land exactly on the calibrated Params::defaults(),
//! or the two ports ship different tunings while looking identical.
void testParameterMapping() {
    printf("\nslider mappings\n");
    Dehum fx(hostCallback);
    const dehum::Params want = dehum::Params::defaults();

    char text[64];
    fx.getParameterDisplay(kParamB, text);
    const double bw = atof(text);
    fx.getParameterDisplay(kParamC, text);
    const double to = atof(text);
    fx.getParameterDisplay(kParamD, text);
    const int harm = atoi(text);

    char d[96];
    snprintf(d, sizeof(d), "%.3f vs %.3f", bw, (double)want.bandwidth);
    check(fabs(bw - want.bandwidth) < 0.01, "Bandwid default is the core's default", d);
    snprintf(d, sizeof(d), "%.1f vs %.1f", to, (double)want.searchTo);
    check(fabs(to - want.searchTo) < 1.0, "SrchTo default is the core's default", d);
    snprintf(d, sizeof(d), "%d vs %d", harm, want.harmonics);
    check(harm == want.harmonics, "Harmncs default is the core's default", d);

    fx.getParameterDisplay(kParamE, text);
    check(strcmp(text, "auto") == 0, "Freq reads 'auto' at the bottom", text);
    fx.getParameterDisplay(kParamF, text);
    const double rum = atof(text);
    snprintf(d, sizeof(d), "%.1f vs %.1f", rum, (double)want.rumbleHz);
    check(fabs(rum - want.rumbleHz) < 1.0, "Rumble default is the core's default", d);

    //and the off positions really are off
    fx.setParameter(kParamF, 0.0f);
    fx.getParameterDisplay(kParamF, text);
    check(strcmp(text, "off") == 0, "Rumble reads 'off' at the bottom", text);
    fx.setParameter(kParamE, 1.0f);
    fx.getParameterDisplay(kParamE, text);
    check(fabs(atof(text) - 500.0) < 1.0, "Freq reaches 500 Hz at the top", text);
}

void testChunk() {
    printf("\npreset chunk\n");
    Dehum fx(hostCallback);
    const float set[kNumParameters] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
    for (int i = 0; i < kNumParameters; ++i) fx.setParameter(i, set[i]);

    void * data = NULL;
    const VstInt32 size = fx.getChunk(&data, true);
    check(size == (VstInt32)(kChunkFloats * sizeof(float)),
          "the chunk keeps its seven floats, one per parameter and the retired Dry/Wet");
    check(((const float *)data)[kChunkDryWet] == 1.0f,
          "the retired Dry/Wet goes out as 1: an older build opens it fully wet");

    Dehum other(hostCallback);
    other.setChunk(data, size, true);
    bool same = true;
    for (int i = 0; i < kNumParameters; ++i) {
        if (other.getParameter(i) != set[i]) { same = false; break; }
    }
    check(same, "every parameter survives a chunk round trip");
    free(data);

    float wild[kChunkFloats] = { -5.0f, 5.0f, -0.1f, 1.1f, 0.5f, 2.0f, -2.0f };
    Dehum third(hostCallback);
    third.setChunk(wild, (VstInt32)sizeof(wild), true);
    bool pinned = true;
    for (int i = 0; i < kNumParameters; ++i) {
        const float v = third.getParameter(i);
        if (v < 0.0f || v > 1.0f) { pinned = false; break; }
    }
    check(pinned, "a chunk with out-of-range values is pinned to 0..1");
}

} // anonymous namespace

int main() {
    printf("dehum_vst_verify\n");

    const double humHz = 49.58;   //the mains line measured on a real transfer
    std::vector<double> inL, inR;
    makeSignal(inL, inR, humHz);

    testAgainstCore(inL, inR);
    testBlockSizes(inL, inR);
    testRemovesHum(inL, inR, humHz);
    testParameterMovesRetune(inL, inR);
    testRobustness(inL, inR);
    testOldDryWetIgnored(inL, inR);
    testFloatPathIsDoublePlusDither(inL, inR);
    testResumeKeepsLines(inL, inR, humHz);
    testParameterMapping();
    testChunk();

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
