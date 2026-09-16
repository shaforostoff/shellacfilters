/* ========================================
 *  paraeq_vst_verify - the WinVST port against the core it shares.
 *
 *  plugins/WinVST/ParaEQ compiles the same paraeq_core.cpp that foo_dsp_paraeq
 *  does. The point of this test is that it stays that way: that the VST wrapper
 *  adds parameter mapping and a dither, and no DSP of its own.
 *
 *  The central check drives paraeq::Channel directly with the same Config and
 *  requires the plug-in's processDoubleReplacing output to be identical to the
 *  bit. processDoubleReplacing is undithered - the standard Airwindows
 *  arrangement - which is what makes an exact comparison possible at all.
 *
 *  It asks the plug-in what Config that is, rather than assuming
 *  Params::defaults(), and the reason is worth knowing. The frequency and Q
 *  sliders are logarithmic, so a default arrives at the core through a powf()
 *  of a logf(), and 5 kHz comes back as 5000.0005 - one float ULP out, a
 *  coefficient or two different in the last bits, and a bit-exact comparison
 *  that fails for a reason with nothing to do with the DSP. So ParaEQ.h makes
 *  paramsFromControls() public and this file uses it. testParameterMapping()
 *  below is where that ULP is held to account, with a tolerance, which is the
 *  right place for it.
 *
 *  Around the central check, the things this wrapper has to get right on its
 *  own:
 *
 *    - it chunks long buffers through a fixed scratch, so a settled equaliser
 *      has to give the identical stream at every block size. Across a glide it
 *      does not quite, and that one is the core's doing rather than the
 *      wrapper's - testBlockSizes() below has the mechanism and the bound;
 *    - a control move must retune rather than reconfigure. The difference is
 *      visible: configure() zeroes the biquad states and snaps the
 *      coefficients, so a wrapper that rebuilds puts a step in the audio and
 *      loses the 20 ms glide the core exists to provide;
 *    - a sample rate change must do the opposite and snap, because the states
 *      describe samples at a rate that is no longer the rate;
 *    - latency is zero and must be declared as such, and nothing may ever
 *      renegotiate it - the same contract Dehum has and the opposite of
 *      Declick's;
 *    - resume() must drop the filter state without disturbing the settings;
 *    - the slider mappings, hostile input, bypass and the preset chunk;
 *    - the editor, in the two directions a drag and a piece of automation have
 *      to travel between the curve and the sixteen parameters underneath it.
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
 *  their value rather than string-compared, except where the display is a word.
 *
 *  Mirror identity - that WinVST/ParaEQ/paraeq_core.cpp is byte-identical to
 *  the canonical one - is not checked here. That is scripts/sync_cores.ps1,
 *  which build_release.ps1 runs before it configures anything.
 * ======================================== */

#include "ParaEQ.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

int g_failures = 0;
int g_ioChanged = 0;
int g_automated = 0;
int g_lastAutomated = -1;

/*  A stand-in host. The shim has no observation hooks of its own - it is the
 *  shim that ships, not a test double - so the only way to see what the plug-in
 *  told the host is to be the host. audioMasterIOChanged is the one callback
 *  anything in this tree ever makes, and for this plug-in the correct number of
 *  times is zero: the latency is zero at every setting and every sample rate,
 *  so there is nothing to renegotiate, ever. */
VstIntPtr VSTCALLBACK hostCallback(AEffect * effect, VstInt32 opcode, VstInt32 index,
                                   VstIntPtr value, void * ptr, float opt) {
    (void)effect; (void)value; (void)ptr; (void)opt;
    if (opcode == audioMasterIOChanged) { ++g_ioChanged; return 1; }
    //What a host recording automation listens for. The editor is the only thing
    //in this plug-in that ever sends it - a slider the host moved itself does
    //not come back - so counting it is how the editor tests below see that a
    //drag would have been recorded.
    if (opcode == audioMasterAutomate) {
        ++g_automated;
        g_lastAutomated = index;
        return 1;
    }
    if (opcode == audioMasterVersion) return 2400;
    return 0;
}

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-56s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
//Three seconds. There is no detector to warm up and no window to fill - the
//longest thing that happens in this core is a glide, and that is measured in
//tens of milliseconds - so the length here is set by wanting a few hundred
//milliseconds either side of a control move, not by the DSP.
const int kFrames = 44100 * 3;
const double kPi = 3.14159265358979323846;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Rumble, a note, a little brightness and surface noise: something in the way
//! of every band, so that a wrong coefficient anywhere has somewhere to show up.
void makeSignal(std::vector<double> & l, std::vector<double> & r) {
    Rng rng(20240813u);
    l.assign((size_t)kFrames, 0.0);
    r.assign((size_t)kFrames, 0.0);
    for (int i = 0; i < kFrames; ++i) {
        const double t = (double)i / (double)kRate;
        const double rumble = 0.05 * sin(2.0 * kPi * 24.0 * t);
        const double note   = 0.10 * sin(2.0 * kPi * 220.0 * t);
        const double boxy   = 0.04 * sin(2.0 * kPi * 1000.0 * t);
        const double bright = 0.02 * sin(2.0 * kPi * 5000.0 * t);
        const double hiss   = 0.004 * rng.centred();
        l[(size_t)i] = rumble + note + boxy + bright + hiss;
        r[(size_t)i] = rumble * 0.9 + note * 1.1 + boxy + bright * 0.8
                     + 0.004 * rng.centred();
    }
}

//! Run frames [from, to) of the input through the plug-in, in blocks. Split into
//! a range rather than a whole stream so a caller can stop at an exact frame,
//! move a control and carry on - which is what makes the block size test below
//! able to compare glides that start at the same sample.
void runPlugin(ParaEQ & fx, const std::vector<double> & inL,
               const std::vector<double> & inR,
               std::vector<double> & outL, std::vector<double> & outR,
               int block, size_t from, size_t to) {
    if (outL.size() != inL.size()) outL.assign(inL.size(), 0.0);
    if (outR.size() != inR.size()) outR.assign(inR.size(), 0.0);
    std::vector<double> bl((size_t)block), br((size_t)block);
    size_t pos = from;
    while (pos < to) {
        const size_t n = (to - pos < (size_t)block) ? to - pos : (size_t)block;
        for (size_t i = 0; i < n; ++i) { bl[i] = inL[pos + i]; br[i] = inR[pos + i]; }
        double * ins[2]  = { &bl[0], &br[0] };
        double * outs[2] = { &bl[0], &br[0] };   //in place, as a host may do
        fx.processDoubleReplacing(ins, outs, (VstInt32)n);
        for (size_t i = 0; i < n; ++i) { outL[pos + i] = bl[i]; outR[pos + i] = br[i]; }
        pos += n;
    }
}

void runPlugin(ParaEQ & fx, const std::vector<double> & inL,
               const std::vector<double> & inR,
               std::vector<double> & outL, std::vector<double> & outR, int block) {
    outL.assign(inL.size(), 0.0);
    outR.assign(inR.size(), 0.0);
    runPlugin(fx, inL, inR, outL, outR, block, 0, inL.size());
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

//! Amplitude of the component at `f`, over [from, to).
double toneAmplitude(const std::vector<double> & v, double f, size_t from, size_t to) {
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < to; ++i) {
        const double a = 2.0 * kPi * f * (double)i / (double)kRate;
        re += v[i] * cos(a);
        im -= v[i] * sin(a);
    }
    const double n = (double)(to - from);
    return n > 0.0 ? 2.0 * sqrt(re * re + im * im) / n : 0.0;
}

//! Largest sample-to-sample jump over [from, to). A coefficient discontinuity
//! shows up here and nowhere else: it adds a step of b0*x to one sample and to
//! no other, which no amount of looking at a spectrum will separate from the
//! signal.
double worstStep(const std::vector<double> & v, size_t from, size_t to) {
    double w = 0.0;
    for (size_t i = (from > 0 ? from : 1); i < to; ++i) {
        const double d = fabs(v[i] - v[i - 1]);
        if (d > w) w = d;
    }
    return w;
}

//! The settings used wherever this file wants the equaliser doing something
//! rather than sitting flat. A plausible transfer curve: 24 dB/oct under the
//! rumble, weight back in at the bottom, the boxiness out of the low mids,
//! brilliance up, hiss down.
void setCurve(ParaEQ & fx) {
    fx.setParameter(kParamA, 0.50f);   //HP Freq   ~75 Hz
    fx.setParameter(kParamB, 0.90f);   //HP Slope  24 dB/oct
    fx.setParameter(kParamC, 0.80f);   //LF Gain   +12 dB
    fx.setParameter(kParamF, 0.20f);   //LMF Gain  -12 dB
    fx.setParameter(kParamH, 0.60f);   //LMF Q     ~2.6
    fx.setParameter(kParamI, 0.75f);   //HMF Gain  +10 dB
    fx.setParameter(kParamL, 0.30f);   //HF Gain   -8 dB
    fx.setParameter(kParamO, 0.55f);   //Output    +2 dB
}

// ---------------------------------------------------------------------------

//! The one that matters: the wrapper must add no DSP.
void testAgainstCore(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nagainst the core, driven directly\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    setCurve(fx);
    g_ioChanged = 0;
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    check(fx.getAeffect()->initialDelay == 0, "latency is declared as zero");
    check(g_ioChanged == 0, "the host is never asked to renegotiate it");

    //The plug-in's own reading of its sliders, so the only thing left that can
    //differ is the DSP.
    paraeq::Params p = fx.paramsFromControls();
    paraeq::Config cfg;
    cfg.compute(p, (double)kRate);
    paraeq::Channel cL, cR;
    cL.configure(cfg);
    cR.configure(cfg);
    std::vector<double> refL = inL, refR = inR;
    {
        paraeq::scoped_flush_denormals ftz;
        cL.process(&refL[0], refL.size(), 1);
        cR.process(&refR[0], refR.size(), 1);
    }

    char d[96];
    const double w = worstDiff(vL, refL) + worstDiff(vR, refR);
    snprintf(d, sizeof(d), "worst deviation %.3e", w);
    check(w == 0.0, "VST double path == the core driven directly", d);
}

//! And the same again across a control move, which is the case a wrapper can
//! get wrong without anything above noticing: configure() where retune()
//! belongs zeroes the filter states and snaps the coefficients, and the stream
//! that comes out is a different one.
void testRetunesRatherThanRebuilds(const std::vector<double> & inL,
                                   const std::vector<double> & inR) {
    printf("\na control move retunes, it does not rebuild\n");
    const size_t at = (size_t)kRate;          //one second in

    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    setCurve(fx);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512, 0, at);
    fx.setParameter(kParamC, 0.20f);          //LF Gain +12 dB -> -12 dB
    runPlugin(fx, inL, inR, vL, vR, 512, at, inL.size());

    paraeq::Params p = fx.paramsFromControls();
    paraeq::Channel cL, cR;
    {
        ParaEQ before(hostCallback);
        before.setSampleRate((float)kRate);
        setCurve(before);
        paraeq::Config first;
        first.compute(before.paramsFromControls(), (double)kRate);
        cL.configure(first);
        cR.configure(first);
    }
    std::vector<double> refL = inL, refR = inR;
    {
        paraeq::scoped_flush_denormals ftz;
        cL.process(&refL[0], at, 1);
        cR.process(&refR[0], at, 1);
        paraeq::Config next;
        next.compute(p, (double)kRate);
        cL.retune(next);
        cR.retune(next);
        cL.process(&refL[at], refL.size() - at, 1);
        cR.process(&refR[at], refR.size() - at, 1);
    }

    char d[96];
    const double w = worstDiff(vL, refL) + worstDiff(vR, refR);
    snprintf(d, sizeof(d), "worst deviation %.3e", w);
    check(w == 0.0, "the glide after the move is the core's retune(), to the bit", d);
}

void testBlockSizes(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nblock sizes (the wrapper chunks through a fixed scratch)\n");

    //Part one: a settled equaliser, which is what one is for all but a fraction
    //of a second after a knob stops moving. process() runs the whole remaining
    //block through the loop that does no interpolation and keeps no per-call
    //bookkeeping, so this is exact and there is no excuse for it not to be.
    std::vector<double> refL, refR;
    {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        runPlugin(fx, inL, inR, refL, refR, 1);
    }
    const int blocks[5] = { 64, 512, 1024, 1025, 4096 };
    for (int k = 0; k < 5; ++k) {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        std::vector<double> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, blocks[k]);
        const double w = worstDiff(vL, refL) + worstDiff(vR, refR);
        char what[96], d[64];
        snprintf(what, sizeof(what), "settled, block %d gives the same stream as block 1", blocks[k]);
        snprintf(d, sizeof(d), "worst %.3e", w);
        check(w == 0.0, what, d);
    }
    //1025 is deliberate: one past the 1024 scratch, so the chunking loop has to
    //split a buffer and pick up exactly where it left off.

    /*  Part two: across a glide, where it is NOT exact - and the reason is in
     *  the core rather than in this wrapper. Channel::process() tests every
     *  kSettleCheck = 32 samples whether the glide has arrived, and it counts
     *  those 32 from the start of each call, so the test lands on a different
     *  grid for any block size that is not a multiple of 32, and lands after
     *  every sample at block 1. checkSettled() snaps exactly onto the target
     *  when it fires, so firing a few samples earlier or later leaves a
     *  different residual in the coefficients.
     *
     *  What bounds the difference is the tolerance that test uses: a coefficient
     *  is within kSettleTolerance = 1e-6 of its target when the snap happens,
     *  which is a residual move about 120 dB down, and it lasts only as long as
     *  a glide does. So this is a tolerance rather than a bit comparison, and
     *  the number it prints is the thing to watch: if it climbs out of the
     *  noise, something has changed that is not this.
     *
     *  It is a property of paraeq_core, so foo_dsp_paraeq has it too - a
     *  foobar2000 chunk is not a fixed size either. Neither wrapper can do
     *  anything about it from outside the core.
     */
    const size_t at = (size_t)kRate;
    std::vector<double> glideL, glideR;
    {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        runPlugin(fx, inL, inR, glideL, glideR, 512, 0, at);
        fx.setParameter(kParamC, 0.20f);
        runPlugin(fx, inL, inR, glideL, glideR, 512, at, inL.size());
    }
    const int glideBlocks[2] = { 1024, 1025 };
    for (int k = 0; k < 2; ++k) {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        std::vector<double> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, glideBlocks[k], 0, at);
        fx.setParameter(kParamC, 0.20f);
        runPlugin(fx, inL, inR, vL, vR, glideBlocks[k], at, inL.size());
        const double w = worstDiff(vL, glideL) + worstDiff(vR, glideR);

        /*  And a second later it has gone, which is what says the difference is
         *  the settle test rather than the filtering. Not to exactly zero: both
         *  copies are running identical coefficients on identical input by then,
         *  so what is left is a difference in the biquad states, and that decays
         *  by the pole radius per sample rather than vanishing. A 24 dB/oct
         *  high-pass at 75 Hz has its poles close enough to the unit circle that
         *  a second of decay lands on the double rounding floor and stays there.
         *  1e-12 is that floor with room to spare; the residual during the glide
         *  is six orders of magnitude above it. */
        double tail = 0.0;
        for (size_t i = at + (size_t)kRate; i < inL.size(); ++i) {
            const double a = fabs(vL[i] - glideL[i]);
            const double b = fabs(vR[i] - glideR[i]);
            if (a > tail) tail = a;
            if (b > tail) tail = b;
        }
        char what[112], d[96];
        snprintf(what, sizeof(what), "gliding, block %d tracks block 512", glideBlocks[k]);
        snprintf(d, sizeof(d), "worst %.3e during, %.3e a second later", w, tail);
        check(w < 1.0e-4 && tail < 1.0e-12, what, d);
    }
}

//! It is an equaliser: the bands have to land where the controls say they do.
void testItEqualises() {
    printf("\nthe bands do what the knobs say\n");

    //A boost on the low-mid bell at its default 1 kHz, against the 1 kHz tone
    //in the signal, and the 220 Hz note as the control that must not move.
    std::vector<double> l, r;
    makeSignal(l, r);

    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    fx.setParameter(kParamF, 1.0f);       //LMF Gain to +20 dB
    fx.setParameter(kParamH, 0.0f);       //LMF Q to 0.5, wide enough to be clean
    std::vector<double> vL, vR;
    runPlugin(fx, l, r, vL, vR, 512);

    //The back half, so the glide is long over.
    const size_t from = l.size() / 2, to = l.size();
    const double was = toneAmplitude(l, 1000.0, from, to);
    const double now = toneAmplitude(vL, 1000.0, from, to);
    char d[96];
    const double gain = 20.0 * log10(now / (was + 1e-300) + 1e-300);
    snprintf(d, sizeof(d), "%.2f dB, wanted 20.00", gain);
    check(fabs(gain - 20.0) < 0.5, "+20 dB on the low-mid bell is +20 dB at 1 kHz", d);

    //A high-pass at its top corner, against the 24 Hz rumble.
    ParaEQ hp(hostCallback);
    hp.setSampleRate((float)kRate);
    hp.setParameter(kParamA, 1.0f);       //HP Freq to 350 Hz
    hp.setParameter(kParamB, 1.0f);       //24 dB/oct
    std::vector<double> hL, hR;
    runPlugin(hp, l, r, hL, hR, 512);
    const double rumbleWas = toneAmplitude(l, 24.0, from, to);
    const double rumbleNow = toneAmplitude(hL, 24.0, from, to);
    const double down = 20.0 * log10(rumbleNow / (rumbleWas + 1e-300) + 1e-300);
    snprintf(d, sizeof(d), "%.1f dB down", down);
    //Four octaves below 350 Hz at 24 dB/oct is nominally 96 dB; asserting most
    //of it rather than all of it, because the shelf above it is still in circuit.
    check(down < -60.0, "350 Hz at 24 dB/oct takes the 24 Hz rumble out", d);
}

//! The property the core's header is largely about: a control move does not put
//! a step in the audio. Nothing else in this file would notice one - a step of
//! b0*x on a single sample is invisible in a spectrum and invisible in an RMS.
void testTheGlideIsSmooth() {
    printf("\na control move puts no step in the audio\n");

    //A pure tone, so the only thing that can make a jump is the equaliser.
    std::vector<double> l((size_t)kFrames), r((size_t)kFrames);
    for (int i = 0; i < kFrames; ++i) {
        l[(size_t)i] = 0.25 * sin(2.0 * kPi * 1000.0 * (double)i / kRate);
        r[(size_t)i] = l[(size_t)i];
    }

    const size_t at = (size_t)kRate;
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    fx.setParameter(kParamH, 0.0f);       //LMF Q 0.5, centred on the tone
    std::vector<double> vL, vR;
    runPlugin(fx, l, r, vL, vR, 512, 0, at);
    fx.setParameter(kParamF, 1.0f);       //LMF Gain 0 -> +20 dB
    runPlugin(fx, l, r, vL, vR, 512, at, l.size());

    //During the glide the tone is somewhere between the old amplitude and the
    //new one, so its per-sample steps cannot legitimately exceed the settled
    //ones at the end. A coefficient discontinuity would exceed them.
    const double settled    = worstStep(vL, l.size() - (size_t)kRate / 2, l.size());
    const double transition = worstStep(vL, at, at + (size_t)kRate / 2);
    char d[112];
    snprintf(d, sizeof(d), "worst step %.4e in the glide, %.4e settled",
             transition, settled);
    check(transition <= settled * 1.02, "no sample jumps further than the settled signal does", d);

    //and it really did move: +20 dB of it
    const double before = toneAmplitude(vL, 1000.0, at / 2, at);
    const double after  = toneAmplitude(vL, 1000.0, l.size() - (size_t)kRate / 2, l.size());
    snprintf(d, sizeof(d), "%.2f dB", 20.0 * log10(after / (before + 1e-300) + 1e-300));
    check(after > before * 8.0, "and the move it glided to is the one asked for", d);
}

//! Bypass retargets every stage to unity and the trim to 0 dB rather than
//! blending a dry path in, so once the glide has arrived the cascade is six
//! unity biquads and the output is the input - exactly, not nearly.
void testBypassSettlesToPassthrough(const std::vector<double> & inL,
                                    const std::vector<double> & inR) {
    printf("\nbypass\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    setCurve(fx);
    fx.setParameter(kParamP, 1.0f);       //Bypass on
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    //The back half: the first second is the glide to flat, which is meant to be
    //audible as a transition and is not a bypass yet.
    const size_t from = inL.size() / 2;
    double w = 0.0;
    for (size_t i = from; i < inL.size(); ++i) {
        const double a = fabs(vL[i] - inL[i]);
        const double b = fabs(vR[i] - inR[i]);
        if (a > w) w = a;
        if (b > w) w = b;
    }
    char d[64];
    snprintf(d, sizeof(d), "worst %.3e", w);
    check(w == 0.0, "a settled bypass is a bit-exact passthrough", d);
}

//! Sixteen sliders, one at a time. None of them may renegotiate latency and
//! none of them may put a hole in the audio.
void testEveryParameterMovesLive(const std::vector<double> & inL,
                                 const std::vector<double> & inR) {
    printf("\nevery parameter moves live\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    const char * names[kNumParameters] =
        { "Low cut", "Slope", "Bass", "Bass Hz", "Bass Shp",
          "Revrb", "Revrb Hz", "Revrb Q",
          "Brill", "Brill Hz", "Brill Q",
          "Hiss", "Hiss Hz", "Hiss Shp", "Output", "Bypass" };

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
        //a rebuild zeroes the filter states, not the signal, so what it leaves
        //is a gap rather than silence - but silence is what a dropped block
        //looks like, and this is the cheap way to see either
        int run = 0, worst = 0;
        bool finite = true;
        for (int i = 0; i < 512; ++i) {
            const double s = bl[(size_t)i];
            if (!(s > -1e30 && s < 1e30)) finite = false;
            if (s == 0.0) { if (++run > worst) worst = run; } else run = 0;
        }
        char what[112], d[64];
        snprintf(what, sizeof(what), "%-8s moves without renegotiating latency", names[k]);
        snprintf(d, sizeof(d), "longest silent run %d", worst);
        check(g_ioChanged == before && worst < 32 && finite, what, d);
        fx.setParameter(k, was);
    }
}

void testRobustness(const std::vector<double> & inL, const std::vector<double> & inR) {
    printf("\nrobustness\n");
    {
        ParaEQ fx(hostCallback);
        fx.setSampleRate(96000.0f);
        setCurve(fx);
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
        //8 kHz is a shelf corner at 44.1 kHz and comfortably past Nyquist at
        //11.025 kHz, where kMaxFreqFraction is what stands between the design
        //and a shelf that is not one.
        ParaEQ fx(hostCallback);
        fx.setSampleRate(11025.0f);
        fx.setParameter(kParamM, 1.0f);   //HF Freq to 16 kHz, above Nyquist
        fx.setParameter(kParamL, 1.0f);   //and boosted as far as it goes
        std::vector<double> vL, vR;
        runPlugin(fx, inL, inR, vL, vR, 512);
        bool finite = true;
        double peak = 0.0;
        for (size_t i = 0; i < vL.size(); ++i) {
            if (!(vL[i] > -1e30 && vL[i] < 1e30)) { finite = false; break; }
            if (fabs(vL[i]) > peak) peak = fabs(vL[i]);
        }
        char d[64];
        snprintf(d, sizeof(d), "peak %.3f", peak);
        check(finite && peak < 100.0, "a corner above Nyquist is held down, not designed badly", d);
    }
    {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        std::vector<double> bl(2048, 0.0), br(2048, 0.0);
        for (int i = 0; i < 2048; ++i) {
            bl[(size_t)i] = 0.1 * sin(2.0 * kPi * 1000.0 * (double)i / kRate);
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
            bl[(size_t)i] = 0.1 * sin(2.0 * kPi * 1000.0 * (double)i / kRate);
            br[(size_t)i] = bl[(size_t)i];
        }
        fx.processDoubleReplacing(ins, ins, 2048);
        double e = 0.0;
        for (int i = 1024; i < 2048; ++i) e += bl[(size_t)i] * bl[(size_t)i];

        //Against a copy that was never fed anything hostile, rather than against
        //a constant: setCurve() puts a 12 dB cut on the low mids and this tone
        //is at 1 kHz, so what a healthy stream carries here is a property of the
        //curve rather than a number worth writing down.
        ParaEQ healthy(hostCallback);
        healthy.setSampleRate((float)kRate);
        setCurve(healthy);
        std::vector<double> cl(2048), cr(2048);
        for (int i = 0; i < 2048; ++i) {
            cl[(size_t)i] = 0.1 * sin(2.0 * kPi * 1000.0 * (double)i / kRate);
            cr[(size_t)i] = cl[(size_t)i];
        }
        double * cins[2] = { &cl[0], &cr[0] };
        healthy.processDoubleReplacing(cins, cins, 2048);
        healthy.processDoubleReplacing(cins, cins, 2048);
        double want = 0.0;
        for (int i = 1024; i < 2048; ++i) want += cl[(size_t)i] * cl[(size_t)i];

        char d2[96];
        snprintf(d2, sizeof(d2), "%.4f of an undisturbed %.4f", e, want);
        check(e > want * 0.5, "the stream recovers after hostile input", d2);
    }
}

void testFloatPathIsDoublePlusDither(const std::vector<double> & inL,
                                     const std::vector<double> & inR) {
    printf("\nthe float path\n");
    std::vector<double> dL, dR;
    {
        ParaEQ fx(hostCallback);
        fx.setSampleRate((float)kRate);
        setCurve(fx);
        runPlugin(fx, inL, inR, dL, dR, 512);
    }

    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    setCurve(fx);
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
    char d[96];
    snprintf(d, sizeof(d), "worst %.3e, tolerance 2.0e-07", worst);
    //A tolerance rather than an identity: the 32 bit path is the same DSP plus
    //a dither seeded from rand(), which is not repeatable between instances.
    check(worst <= 2.0e-7, "processReplacing is processDoubleReplacing plus dither", d);
}

//! resume() is a transport start. The biquad states describe samples that are
//! no longer adjacent to what comes next, so they go; the settings do not.
void testResumeKeepsTheSettings(const std::vector<double> & inL,
                                const std::vector<double> & inR) {
    printf("\nresume()\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);
    setCurve(fx);
    std::vector<double> vL, vR;
    runPlugin(fx, inL, inR, vL, vR, 512);

    const paraeq::Params was = fx.paramsFromControls();
    fx.resume();
    const paraeq::Params now = fx.paramsFromControls();
    check(was == now, "the settings survive it");

    //And the equaliser is still equalising on the far side: a reset() would
    //have snapped a glide to its target, but a flush() leaves a curve running.
    std::vector<double> aL, aR;
    runPlugin(fx, inL, inR, aL, aR, 512);
    const size_t from = inL.size() / 2;
    const double moved = toneAmplitude(aL, 24.0, from, inL.size());
    const double raw   = toneAmplitude(inL, 24.0, from, inL.size());
    char d[96];
    snprintf(d, sizeof(d), "%.1f dB of rumble left", 20.0 * log10(moved / (raw + 1e-300) + 1e-300));
    check(moved < raw * 0.5, "and it is still equalising on the far side", d);
}

/*  The editor. Not what it draws - nothing here can see a curve - but the two
 *  things the join between it and the plug-in has to get right, in both
 *  directions:
 *
 *    a drag has to reach the parameters AND the host, because a VST's state is
 *    its parameters and a host that is recording has to be told a knob moved;
 *
 *    a parameter moved by anything else - automation, a preset, the host's own
 *    generic slider - has to reach the editor, or the curve on screen stops
 *    describing the audio.
 *
 *  The window itself is exercised here only far enough to have one: opening it
 *  is what makes the editor hold a paraeq_editor::Editor to talk to. The
 *  lifecycle - open, close, reopen, close twice - is vst_host_verify's, because
 *  there it goes over the ABI, which is where a host would break it.
 */
HWND makeParentWindow() {
    return CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
                           0, 0, 700, 460, NULL, NULL, NULL, NULL);
}

void testEditorIsOffered() {
    printf("\nthe editor is offered to the host\n");
    ParaEQ fx(hostCallback);

    char d[64];
    snprintf(d, sizeof(d), "0x%04X", (unsigned)fx.getAeffect()->flags);
    check((fx.getAeffect()->flags & effFlagsHasEditor) != 0,
          "effFlagsHasEditor is set", d);
    check(fx.getEditor() != NULL, "and there is an editor behind it");

    //Before any window exists, because that is when a host asks.
    ERect * r = NULL;
    const bool got = fx.getEditor()->getRect(&r);
    if (r) snprintf(d, sizeof(d), "%d x %d", (int)(r->right - r->left),
                    (int)(r->bottom - r->top));
    check(got && r != NULL && r->right > r->left && r->bottom > r->top,
          "it has a size before it has a window", d);
}

void testADragReachesTheParametersAndTheHost() {
    printf("\na drag on the curve moves the sliders, and says so\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);

    HWND parent = makeParentWindow();
    if (!parent) { check(false, "a parent window is created"); return; }
    ParaEQEditor * ed = static_cast<ParaEQEditor *>(fx.getEditor());
    if (!ed->open(parent)) {
        check(false, "the editor opens");
        DestroyWindow(parent);
        return;
    }
    check(true, "the editor opens");

    //What the editor hands over when the user lets go of a handle: a whole
    //Params, already sanitised, not a single control.
    paraeq::Params p = fx.paramsFromControls();
    p.lmfGain      = -12.0f;
    p.lmfFrequency = 1200.0f;
    p.lmfQ         = 2.5f;

    g_automated = 0;
    ed->editorParamsChanged(p);

    const paraeq::Params back = fx.paramsFromControls();
    char d[112];
    snprintf(d, sizeof(d), "%.2f dB at %.1f Hz, Q %.2f",
             (double)back.lmfGain, (double)back.lmfFrequency, (double)back.lmfQ);
    check(fabs(back.lmfGain + 12.0) < 0.05
          && fabs(back.lmfFrequency - 1200.0) < 1.0
          && fabs(back.lmfQ - 2.5) < 0.02,
          "the move arrives in the plug-in's parameters", d);

    //Three controls moved, so three lanes should have been told and the other
    //thirteen left alone: a drag on one band must not write automation for the
    //whole equaliser.
    snprintf(d, sizeof(d), "audioMasterAutomate called %d times", g_automated);
    check(g_automated == 3, "and the host is told about exactly those three", d);

    //And a second, identical push says nothing at all, which is what stops a
    //drag that lands back where it started from filling an automation lane.
    g_automated = 0;
    ed->editorParamsChanged(p);
    snprintf(d, sizeof(d), "%d further calls", g_automated);
    check(g_automated == 0, "a push that changes nothing tells the host nothing", d);

    ed->close();
    DestroyWindow(parent);
}

void testTheEditorFollowsTheHost() {
    printf("\nand it follows a parameter moved anywhere else\n");
    ParaEQ fx(hostCallback);
    fx.setSampleRate((float)kRate);

    HWND parent = makeParentWindow();
    if (!parent) { check(false, "a parent window is created"); return; }
    ParaEQEditor * ed = static_cast<ParaEQEditor *>(fx.getEditor());
    if (!ed->open(parent)) {
        check(false, "the editor opens");
        DestroyWindow(parent);
        return;
    }

    const paraeq::Params * shown = ed->shownParams();
    check(shown != NULL, "an open editor is showing something");
    if (!shown) { ed->close(); DestroyWindow(parent); return; }

    char d[112];
    snprintf(d, sizeof(d), "%.2f dB", (double)shown->hfGain);
    check(fabs(shown->hfGain - fx.paramsFromControls().hfGain) < 1e-6,
          "and it opened on the plug-in's current settings", d);

    //The host moves a slider - automation, a preset, its own generic UI. VST2
    //tells the plug-in and nobody tells the editor, so idle() is where it finds
    //out, and a host that never sends effEditIdle is the documented gap.
    fx.setParameter(kParamL, 0.1f);          //Hiss gain hard down
    ed->idle();

    shown = ed->shownParams();
    snprintf(d, sizeof(d), "%.2f dB, wanted %.2f",
             (double)shown->hfGain, (double)fx.paramsFromControls().hfGain);
    check(fabs(shown->hfGain - fx.paramsFromControls().hfGain) < 1e-6,
          "idle() pulls the change into the editor", d);

    //An idle() with nothing to do must not push anything back at the host: that
    //is the loop this join could have, and m_synced is what stops it.
    g_automated = 0;
    ed->idle();
    ed->idle();
    snprintf(d, sizeof(d), "%d calls", g_automated);
    check(g_automated == 0, "and a quiet idle() writes no automation", d);

    ed->close();
    check(ed->shownParams() == NULL, "a closed editor is showing nothing");
    DestroyWindow(parent);
}

//! The sliders must land on the core's own defaults, or the two ports ship
//! different tunings while looking identical.
void testParameterMapping() {
    printf("\nslider mappings\n");
    ParaEQ fx(hostCallback);
    const paraeq::Params want = paraeq::Params::defaults();
    const paraeq::Params got = fx.paramsFromControls();

    char d[112];
    //A relative tolerance, and a tight one. The frequency sliders are
    //logarithmic, so a default makes a round trip through logf() and powf() to
    //get here: five of the six land back on the float they started as, and
    //5 kHz comes back as 5000.0005, which is one ULP and about four
    //thousandths of a cent.
    struct { const char * name; double got; double want; } freqs[] = {
        { "Low cut",  got.hpFrequency,  want.hpFrequency  },
        { "Bass Hz",  got.lfFrequency,  want.lfFrequency  },
        { "Revrb Hz", got.lmfFrequency, want.lmfFrequency },
        { "Revrb Q",  got.lmfQ,         want.lmfQ         },
        { "Brill Hz", got.hmfFrequency, want.hmfFrequency },
        { "Brill Q",  got.hmfQ,         want.hmfQ         },
        { "Hiss Hz",  got.hfFrequency,  want.hfFrequency  }
    };
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; ++i) {
        const double rel = fabs(freqs[i].got - freqs[i].want) / freqs[i].want;
        char what[96];
        snprintf(what, sizeof(what), "%-8s default is the core's default", freqs[i].name);
        snprintf(d, sizeof(d), "%.4f vs %.4f, rel %.1e", freqs[i].got, freqs[i].want, rel);
        check(rel < 1.0e-6, what, d);
    }

    //The gains are linear in dB, so unity is exactly the centre of the slider
    //and there is nothing to round.
    check(got.lfGain == want.lfGain && got.lmfGain == want.lmfGain
          && got.hmfGain == want.hmfGain && got.hfGain == want.hfGain
          && got.outputGain == want.outputGain,
          "every gain default is exactly 0 dB");
    check(got.hpSlope == want.hpSlope && got.lfBell == want.lfBell
          && got.hfBell == want.hfBell && got.bypass == want.bypass,
          "the switches default where the core does");

    //The ends of the travel really are the ends of the range.
    fx.setParameter(kParamA, 1.0f);
    fx.setParameter(kParamD, 0.0f);
    fx.setParameter(kParamM, 1.0f);
    fx.setParameter(kParamH, 1.0f);
    const paraeq::Params ends = fx.paramsFromControls();
    snprintf(d, sizeof(d), "%.1f Hz", ends.hpFrequency);
    check(fabs(ends.hpFrequency - paraeq::kHpFreqMax) < 0.1, "Low cut reaches the top of its range", d);
    snprintf(d, sizeof(d), "%.1f Hz", ends.lfFrequency);
    check(fabs(ends.lfFrequency - paraeq::kLfFreqMin) < 0.1, "Bass Hz reaches the bottom of its range", d);
    snprintf(d, sizeof(d), "%.1f Hz", ends.hfFrequency);
    check(fabs(ends.hfFrequency - paraeq::kHfFreqMax) < 1.0, "Hiss Hz reaches the top of its range", d);
    snprintf(d, sizeof(d), "%.3f", ends.lmfQ);
    check(fabs(ends.lmfQ - paraeq::kQMax) < 0.01, "Revrb Q reaches the top of its range", d);

    //A log slider is the point of the exercise: the geometric middle of the
    //range sits at the middle of the travel, where a linear one would put it
    //far up the top octave.
    fx.setParameter(kParamM, 0.5f);
    const double mid = fx.paramsFromControls().hfFrequency;
    const double geometric = sqrt((double)paraeq::kHfFreqMin * (double)paraeq::kHfFreqMax);
    snprintf(d, sizeof(d), "%.1f Hz, geometric mean %.1f Hz", mid, geometric);
    check(fabs(mid - geometric) < 1.0, "the Hiss Hz slider is logarithmic", d);

    //The discrete ones, through the displays a host shows.
    char text[64];
    const float slopeAt[3] = { 0.0f, 0.5f, 1.0f };
    const char * slopeIs[3] = { "off", "12", "24" };
    bool slopes = true;
    for (int i = 0; i < 3; ++i) {
        fx.setParameter(kParamB, slopeAt[i]);
        fx.getParameterDisplay(kParamB, text);
        if (strcmp(text, slopeIs[i]) != 0) slopes = false;
    }
    check(slopes, "Slope reads off, 12 and 24 across its three buckets");

    fx.setParameter(kParamE, 0.0f);
    fx.getParameterDisplay(kParamE, text);
    bool shapes = (strcmp(text, "shelf") == 0);
    fx.setParameter(kParamE, 1.0f);
    fx.getParameterDisplay(kParamE, text);
    shapes = shapes && (strcmp(text, "bell") == 0);
    check(shapes, "Bass Shp reads shelf and bell at the two ends");

    fx.setParameter(kParamP, 1.0f);
    fx.getParameterDisplay(kParamP, text);
    check(strcmp(text, "on") == 0, "Bypass reads on at the top", text);

    //And every bucket's own resting position reads back as the bucket it came
    //from, which is what a stored preset has to survive.
    ParaEQ fresh(hostCallback);
    paraeq::Params round = paraeq::Params::defaults();
    round.hpSlope = 2;
    round.lfBell = true;
    round.hfBell = true;
    round.bypass = true;
    fresh.setParameter(kParamB, 5.0f / 6.0f);   //the centre of bucket 2 of 3
    fresh.setParameter(kParamE, 0.75f);
    fresh.setParameter(kParamN, 0.75f);
    fresh.setParameter(kParamP, 0.75f);
    const paraeq::Params back = fresh.paramsFromControls();
    check(back.hpSlope == 2 && back.lfBell && back.hfBell && back.bypass,
          "a bucket's centre reads back as that bucket");
}

void testChunk() {
    printf("\npreset chunk\n");
    ParaEQ fx(hostCallback);
    float set[kNumParameters];
    for (int i = 0; i < kNumParameters; ++i) {
        set[i] = (float)i / (float)(kNumParameters - 1);
        fx.setParameter(i, set[i]);
    }

    void * data = NULL;
    const VstInt32 size = fx.getChunk(&data, true);
    check(size == (VstInt32)(kNumParameters * sizeof(float)),
          "the chunk is one float per parameter");

    ParaEQ other(hostCallback);
    other.setChunk(data, size, true);
    bool same = true;
    for (int i = 0; i < kNumParameters; ++i) {
        if (other.getParameter(i) != set[i]) { same = false; break; }
    }
    check(same, "every parameter survives a chunk round trip");
    free(data);

    float wild[kNumParameters];
    for (int i = 0; i < kNumParameters; ++i) wild[i] = (i & 1) ? -5.0f : 5.0f;
    ParaEQ third(hostCallback);
    third.setChunk(wild, (VstInt32)sizeof(wild), true);
    bool pinned = true;
    for (int i = 0; i < kNumParameters; ++i) {
        const float v = third.getParameter(i);
        if (!(v >= 0.0f && v <= 1.0f)) { pinned = false; break; }
    }
    check(pinned, "a chunk with out-of-range values is pinned to 0..1");

    /*  A NaN is the case pinning does not cover, and every Airwindows plug-in in
     *  this tree behaves the same way: the house pinParameter() is two
     *  comparisons and both are false against a NaN, so it hands the NaN back
     *  unchanged. What stops it is the core's sanitize(), which replaces a value
     *  that is not a plausible control value at all with its default rather than
     *  clamping it - clamping a NaN leaves a NaN, and NaN coefficients stay in a
     *  biquad for the rest of the stream. So the slider keeps it and the audio
     *  never sees it. */
    float poisoned[kNumParameters];
    for (int i = 0; i < kNumParameters; ++i) poisoned[i] = 0.5f;
    poisoned[kParamC] = (float)NAN;
    ParaEQ fourth(hostCallback);
    fourth.setChunk(poisoned, (VstInt32)sizeof(poisoned), true);
    const paraeq::Params p = fourth.paramsFromControls();
    char d[96];
    snprintf(d, sizeof(d), "LF Gain came out %.3f dB", (double)p.lfGain);
    check(p.lfGain >= -paraeq::kGainMaxDb && p.lfGain <= paraeq::kGainMaxDb,
          "a NaN in the chunk does not reach the coefficients", d);
}

} // anonymous namespace

int main() {
    printf("paraeq_vst_verify\n");

    std::vector<double> inL, inR;
    makeSignal(inL, inR);

    testAgainstCore(inL, inR);
    testRetunesRatherThanRebuilds(inL, inR);
    testBlockSizes(inL, inR);
    testItEqualises();
    testTheGlideIsSmooth();
    testBypassSettlesToPassthrough(inL, inR);
    testEveryParameterMovesLive(inL, inR);
    testRobustness(inL, inR);
    testFloatPathIsDoublePlusDither(inL, inR);
    testResumeKeepsTheSettings(inL, inR);
    testEditorIsOffered();
    testADragReachesTheParametersAndTheHost();
    testTheEditorFollowsTheHost();
    testParameterMapping();
    testChunk();

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
