/* ========================================
 *  declick_cli - run the declick core over a WAV file offline.
 *
 *    declick_cli <in.wav> <out.wav> [options]
 *
 *  Development utility, used to check the C++ core against the Python
 *  reference implementation and to batch-process material for listening.
 * ======================================== */

#include "../foo_dsp_declick/declick_core.h"
#include "wav_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <string>
#include <vector>

namespace {

void usage() {
    printf(
        "declick_cli - AR detect-and-interpolate declicker\n"
        "\n"
        "  declick_cli <in.wav> <out.wav> [options]\n"
        "\n"
        "  -s <0..1>    Sensitivity (default 0.9, higher catches more)\n"
        "  -e <0..1>    Extent, how far a detection spreads (default 0.5)\n"
        "  -l <ms>      Longest single repair, ms (default 4)\n"
        "  -p <1..3>    Passes (default 2)\n"
        "  -o <8..256>  AR model order (default 128)\n"
        "  -d <0..1>    Repair depth: 0 = 45%% subtraction, 1 = full replacement (default)\n"
        "  -w <0..1>    Dry/wet (default 1)\n"
        "  --delta      Write what was removed instead of the repaired audio\n"
        "  --quiet\n"
        "  --no-avx2    Use the baseline kernels even where AVX2 or NEON is available\n"
        "  --no-neon    The same\n"
        "\n"
        "  Calibration only, not exposed in the component:\n"
        "  --alpha <x>  Over-subtraction on the estimate variance\n"
        "  --wmax <x>   Ceiling on the subtracted fraction\n"
        "  --rerepair <0..2>  Repair runs again once the model is refitted (default 1)\n"
        "  --dip <n>    Samples below the floor a run may step over (default 1)\n"
        "  --dip-far <0..1>  How far above the floor it must be beyond them (default 0.5)\n"
        "  --dip-peak <x>    Only runs that peak this many sigma or more may (default 6)\n"
        "  --soft-min <0..1>  Share of a run at the trigger that is repaired (default 0.5)\n"
        "  --soft-span <x>    Sigma above the trigger where repair is full (default 3.5, 0 off)\n"
        "  --body <0|1>       The crackle-body stage (default 1)\n"
        "  --body-floor <0..1>  Deepest cut of a bin, as a gain (default 0.25)\n"
        "  --body-peak <x>    Sigma a run must peak at to count towards its key (default 6)\n"
        "  --body-rate0 <x>   Strong runs per second where the key starts to open (default 20)\n"
        "  --body-rate1 <x>   ... and where it is fully open (default 60)\n"
        "  --body-release <s> Seconds the key stays open after it last was (default 2)\n"
        "  --fine <0|1>       The fine stage, above fine-lo, under the body key (default 1)\n"
        "  --fine-lo <Hz>     Where the fine stage starts (default 4500)\n"
        "  --fine-hi <Hz>     ... and where it stops (default 20000)\n"
        "  --fine-alpha <x>   Clip a bin to this many times its background (default 2)\n"
        "  --fine-q <0..1>    Quantile the background is taken at (default 0.2)\n"
        "  --fine-floor <0..1>  Deepest cut of a bin, as a gain (default 0.126)\n"
        "  --fine-mid <Hz>    Below this the cut backs off where the band repeats with\n"
        "                     a voice's period (default 8000; fine-lo turns it off)\n"
        "  --fine-p0 <x>      Periodicity where it starts to back off (default 0.1)\n"
        "  --fine-p1 <x>      ... and where it has backed off entirely (default 0.3)\n"
        "  --micro <0|1>      The fine stage's clip again on its output, at a quarter\n"
        "                     of its frame, under its key and guard (default 1)\n"
        "  --micro-ms <x>     Its background, ms either side (default 6)\n"
        "  --micro-alpha <x>  Clip a bin to this many times its background (default 2)\n"
        "  --micro-q <0..1>   Quantile the background is taken at (default 0.2)\n"
        "  --mid <0|1>        The fine stage's clip from mid-lo up to fine-lo, in\n"
        "                     frames holding a strong run, under its key (default 1)\n"
        "  --mid-lo <Hz>      Where it starts (default 2000)\n"
        "  --mid-peak <x>     Sigma a repaired run must peak at to open a frame (default 4)\n"
        "  --mid-p0 <x>       Periodicity of its band where it starts to back off (default 0.3)\n"
        "  --mid-p1 <x>       ... and where it has backed off entirely (default 0.5)\n"
        "  --mid-yield <0|1>  In the body band, yield to the body stage by its key (default 1)\n"
        "  --flank <0|1>      Repair the flanks of runs again, above flank-hz, on\n"
        "                     band-limited transfers under the body key (default 1)\n"
        "  --flank-pad-ms <x> How far each run is widened either side (default 0.18)\n"
        "  --flank-hz <Hz>    Above this the widened repair is kept (default 3000)\n"
        "  --flank-edge0 <Hz> -40 dB edge at or below which the pad is full (default 13000)\n"
        "  --flank-edge1 <Hz> ... and at or above which there is none (default 15000)\n"
        "  --flank-shut <0..1> Its strength where the key is shut, band-limited only (default 0)\n"
        "  --flank-wide <0..1> Share of the pad a full-band transfer gets under the key (default 1)\n"
        "  --flank-shut-gain <0..1> ... where it is shut, only runs repaired at this gain or more (default 0)\n"
        "  --flank-hold <s>   Seconds it fades out over once the key shuts (default 12)\n"
        "  --carpet <0|1>     The carpet key: opens the fine stage and the flank repair\n"
        "                     on a steady carpet of crackle, where the body key is shut (default 1)\n"
        "  --carpet-peak <x>  Runs counted peak at this many sigma or more (default 4)\n"
        "  --carpet-win <s>   Seconds of per-second rates it looks back over, 1..64 (default 20)\n"
        "  --carpet-q <0..1>  Quantile of those rates that is the key's value (default 0.2)\n"
        "  --carpet-rate0 <x> Runs per second (per 44.1k samples) where it starts to open (default 110)\n"
        "  --carpet-rate1 <x> ... and where it is fully open (default 150)\n"
        "  --carpet-warm <x>  Thresholds stand this much higher while the window fills (default 0.5)\n"
        "  --carpet-min <s>   Seconds before it may open (default 3)\n"
        "  --carpet-first <s> Before that, from this many seconds on, the rate so far\n"
        "                     against the warmest thresholds (default 0.25, 0 off)\n"
        "  --carpet-fano <0|1> The Poisson key: the carpet key also opens where the\n"
        "                     runs come at random, as a surface's crackle does (default 1)\n"
        "  --fano-rate0 <x>   Its runs per second where it starts to open (default 80)\n"
        "  --fano-rate1 <x>   ... and where it is fully open (default 110)\n"
        "  --fano0 <x>        Fano factor of the runs where it starts to open (default 1.4)\n"
        "  --fano1 <x>        ... and where it is fully open (default 1.2)\n"
        "  --fano-first <s>   Before carpet-min, from this many seconds on, by the runs\n"
        "                     so far (default 0.5)\n"
        "  --denoise <0|1>    A spectral denoiser on the output, under the fine stage's\n"
        "                     key; leaves an unnatural high-frequency noise (default 0)\n"
        "  --denoise-lo <Hz>  Where it starts, ramped in over the half octave below (default 1000)\n"
        "  --denoise-beta <x> Cut against this many times the noise floor (default 1.2)\n"
        "  --denoise-min <0..1> Deepest cut of a bin, as a gain (default 0.32)\n"
        "  --denoise-q <0..1> Quantile of a bin's power that is its floor (default 0.1)\n"
        "  --denoise-win <s>  Seconds of history the floor is taken over (default 6)\n"
        "  --denoise-release <s> Time constant the cut comes back with (default 0.06)\n");
}

bool parseFloat(const char * s, float & out) {
    char * end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    out = (float)v;
    return true;
}

} // anonymous namespace

int main(int argc, char ** argv) {
    if (argc < 3) { usage(); return 2; }

    const std::string inPath = argv[1];
    const std::string outPath = argv[2];
    declick::Params params = declick::Params::defaults();
    bool delta = false, quiet = false;
    float alpha = -1.0f, wmax = -1.0f;   // negative: leave the calibrated value
    float rerepair = -1.0f;
    float dip = -1.0f;
    float dipFar = -1.0f;
    float dipPeak = -1.0f;
    float softMin = -1.0f;
    float softSpan = -1.0f;
    float body = -1.0f, bodyFloor = -1.0f, bodyPeak = -1.0f;
    float bodyRate0 = -1.0f, bodyRate1 = -1.0f, bodyRelease = -1.0f;
    float fine = -1.0f, fineLo = -1.0f, fineHi = -1.0f, fineAlpha = -1.0f, fineQ = -1.0f;
    float fineFloor = -1.0f, fineMid = -1.0f, fineP0 = -1.0f, fineP1 = -1.0f;
    float micro = -1.0f, microMs = -1.0f, microAlpha = -1.0f, microQ = -1.0f;
    float mid = -1.0f, midLo = -1.0f, midPeak = -1.0f, midP0 = -1.0f, midP1 = -1.0f, midYield = -1.0f;
    float carpetFirst = -1.0f;
    float carpetFano = -1.0f, fanoRate0 = -1.0f, fanoRate1 = -1.0f, fano0 = -1.0f, fano1 = -1.0f, fanoFirst = -1.0f;
    float denoise = -1.0f, denoiseLo = -1.0f, denoiseBeta = -1.0f, denoiseMin = -1.0f;
    float denoiseQ = -1.0f, denoiseWin = -1.0f, denoiseRelease = -1.0f;
    float flank = -1.0f, flankPadMs = -1.0f, flankHz = -1.0f, flankEdge0 = -1.0f, flankEdge1 = -1.0f;
    float flankShut = -1.0f, flankWide = -1.0f, flankShutGain = -1.0f, flankHold = -1.0f;
    float carpet = -1.0f, carpetPeak = -1.0f, carpetWin = -1.0f, carpetQ = -1.0f;
    float carpetRate0 = -1.0f, carpetRate1 = -1.0f, carpetWarm = -1.0f, carpetMin = -1.0f;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--delta") { delta = true; continue; }
        if (a == "--quiet") { quiet = true; continue; }
        if (a == "--no-avx2" || a == "--no-neon") { declick::allowWideKernels(false); continue; }
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); return 2; }
        float v = 0.0f;
        if (!parseFloat(argv[i + 1], v)) {
            fprintf(stderr, "not a number: %s\n", argv[i + 1]);
            return 2;
        }
        ++i;
        if      (a == "--alpha") alpha = v;
        else if (a == "--wmax") wmax = v;
        else if (a == "--rerepair") rerepair = v;
        else if (a == "--dip") dip = v;
        else if (a == "--dip-far") dipFar = v;
        else if (a == "--dip-peak") dipPeak = v;
        else if (a == "--soft-min") softMin = v;
        else if (a == "--soft-span") softSpan = v;
        else if (a == "--body") body = v;
        else if (a == "--body-floor") bodyFloor = v;
        else if (a == "--body-peak") bodyPeak = v;
        else if (a == "--body-rate0") bodyRate0 = v;
        else if (a == "--body-rate1") bodyRate1 = v;
        else if (a == "--body-release") bodyRelease = v;
        else if (a == "--fine") fine = v;
        else if (a == "--fine-lo") fineLo = v;
        else if (a == "--fine-hi") fineHi = v;
        else if (a == "--fine-alpha") fineAlpha = v;
        else if (a == "--fine-q") fineQ = v;
        else if (a == "--fine-floor") fineFloor = v;
        else if (a == "--fine-mid") fineMid = v;
        else if (a == "--fine-p0") fineP0 = v;
        else if (a == "--fine-p1") fineP1 = v;
        else if (a == "--micro") micro = v;
        else if (a == "--micro-ms") microMs = v;
        else if (a == "--micro-alpha") microAlpha = v;
        else if (a == "--micro-q") microQ = v;
        else if (a == "--mid") mid = v;
        else if (a == "--mid-lo") midLo = v;
        else if (a == "--mid-peak") midPeak = v;
        else if (a == "--mid-p0") midP0 = v;
        else if (a == "--mid-p1") midP1 = v;
        else if (a == "--mid-yield") midYield = v;
        else if (a == "--carpet-first") carpetFirst = v;
        else if (a == "--carpet-fano") carpetFano = v;
        else if (a == "--fano-rate0") fanoRate0 = v;
        else if (a == "--fano-rate1") fanoRate1 = v;
        else if (a == "--fano0") fano0 = v;
        else if (a == "--fano1") fano1 = v;
        else if (a == "--fano-first") fanoFirst = v;
        else if (a == "--denoise") denoise = v;
        else if (a == "--denoise-lo") denoiseLo = v;
        else if (a == "--denoise-beta") denoiseBeta = v;
        else if (a == "--denoise-min") denoiseMin = v;
        else if (a == "--denoise-q") denoiseQ = v;
        else if (a == "--denoise-win") denoiseWin = v;
        else if (a == "--denoise-release") denoiseRelease = v;
        else if (a == "--flank") flank = v;
        else if (a == "--flank-pad-ms") flankPadMs = v;
        else if (a == "--flank-hz") flankHz = v;
        else if (a == "--flank-edge0") flankEdge0 = v;
        else if (a == "--flank-edge1") flankEdge1 = v;
        else if (a == "--flank-shut") flankShut = v;
        else if (a == "--flank-wide") flankWide = v;
        else if (a == "--flank-shut-gain") flankShutGain = v;
        else if (a == "--flank-hold") flankHold = v;
        else if (a == "--carpet") carpet = v;
        else if (a == "--carpet-peak") carpetPeak = v;
        else if (a == "--carpet-win") carpetWin = v;
        else if (a == "--carpet-q") carpetQ = v;
        else if (a == "--carpet-rate0") carpetRate0 = v;
        else if (a == "--carpet-rate1") carpetRate1 = v;
        else if (a == "--carpet-warm") carpetWarm = v;
        else if (a == "--carpet-min") carpetMin = v;
        else if (a == "-s") params.sensitivity = v;
        else if (a == "-e") params.extent = v;
        else if (a == "-l") params.maxLengthMs = v;
        else if (a == "-p") params.passes = (int)v;
        else if (a == "-o") params.order = (int)v;
        else if (a == "-d") params.depth = v;
        else if (a == "-w") params.dryWet = v;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    params.sanitize();

    wavio::Audio audio;
    std::string err;
    if (!wavio::read(inPath, audio, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    const unsigned channels = audio.channels;
    const size_t frames = audio.frames();

    declick::Config cfg;
    // Before compute(): whether the body stage and the flank repair run sets
    // the latency.
    if (body >= 0.0f) cfg.body = (int)body;
    if (bodyFloor >= 0.0f) cfg.bodyFloor = bodyFloor;
    if (bodyPeak >= 0.0f) cfg.bodyPeak = bodyPeak;
    if (bodyRate0 >= 0.0f) cfg.bodyRate0 = bodyRate0;
    if (bodyRate1 >= 0.0f) cfg.bodyRate1 = bodyRate1;
    if (bodyRelease >= 0.0f) cfg.bodyRelease = bodyRelease;
    if (fine >= 0.0f) cfg.fine = (int)fine;
    if (fineLo >= 0.0f) cfg.fineLo = fineLo;
    if (fineHi >= 0.0f) cfg.fineHi = fineHi;
    if (fineAlpha >= 0.0f) cfg.fineAlpha = fineAlpha;
    if (fineQ >= 0.0f) cfg.fineQ = fineQ;
    if (fineFloor >= 0.0f) cfg.fineFloor = fineFloor;
    if (fineMid >= 0.0f) cfg.fineMid = fineMid;
    if (fineP0 >= 0.0f) cfg.finePeriodic0 = fineP0;
    if (fineP1 >= 0.0f) cfg.finePeriodic1 = fineP1;
    if (micro >= 0.0f) cfg.micro = (int)micro;
    if (microMs >= 0.0f) cfg.microMs = microMs;
    if (microAlpha >= 0.0f) cfg.microAlpha = microAlpha;
    if (microQ >= 0.0f) cfg.microQ = microQ;
    if (mid >= 0.0f) cfg.mid = (int)mid;
    if (midLo >= 0.0f) cfg.midLo = midLo;
    if (midPeak >= 0.0f) cfg.midPeak = midPeak;
    if (midP0 >= 0.0f) cfg.midPeriodic0 = midP0;
    if (midP1 >= 0.0f) cfg.midPeriodic1 = midP1;
    if (midYield >= 0.0f) cfg.midYield = (int)midYield;
    if (carpetFirst >= 0.0f) cfg.carpetFirst = carpetFirst;
    if (carpetFano >= 0.0f) cfg.carpetFano = (int)carpetFano;
    if (fanoRate0 >= 0.0f) cfg.fanoRate0 = fanoRate0;
    if (fanoRate1 >= 0.0f) cfg.fanoRate1 = fanoRate1;
    if (fano0 >= 0.0f) cfg.fano0 = fano0;
    if (fano1 >= 0.0f) cfg.fano1 = fano1;
    if (fanoFirst >= 0.0f) cfg.fanoFirst = fanoFirst;
    if (denoise >= 0.0f) cfg.denoise = (int)denoise;
    if (denoiseLo >= 0.0f) cfg.denoiseLo = denoiseLo;
    if (denoiseBeta >= 0.0f) cfg.denoiseBeta = denoiseBeta;
    if (denoiseMin >= 0.0f) cfg.denoiseMin = denoiseMin;
    if (denoiseQ >= 0.0f) cfg.denoiseQ = denoiseQ;
    if (denoiseWin >= 0.0f) cfg.denoiseWin = denoiseWin;
    if (denoiseRelease >= 0.0f) cfg.denoiseRelease = denoiseRelease;
    if (flank >= 0.0f) cfg.flank = (int)flank;
    if (flankPadMs >= 0.0f) cfg.flankPadMs = flankPadMs;
    if (flankHz >= 0.0f) cfg.flankHz = flankHz;
    if (flankEdge0 >= 0.0f) cfg.flankEdge0 = flankEdge0;
    if (flankEdge1 >= 0.0f) cfg.flankEdge1 = flankEdge1;
    if (flankShut >= 0.0f) cfg.flankShut = flankShut;
    if (flankWide >= 0.0f) cfg.flankWide = flankWide;
    if (flankShutGain >= 0.0f) cfg.flankShutGain = flankShutGain;
    if (flankHold >= 0.0f) cfg.flankHold = flankHold;
    if (carpet >= 0.0f) cfg.carpet = (int)carpet;
    if (carpetPeak >= 0.0f) cfg.carpetPeak = carpetPeak;
    if (carpetWin >= 0.0f) cfg.carpetWin = (int)carpetWin;
    if (carpetQ >= 0.0f) cfg.carpetQ = carpetQ;
    if (carpetRate0 >= 0.0f) cfg.carpetRate0 = carpetRate0;
    if (carpetRate1 >= 0.0f) cfg.carpetRate1 = carpetRate1;
    if (carpetWarm >= 0.0f) cfg.carpetWarm = carpetWarm;
    if (carpetMin >= 0.0f) cfg.carpetMin = (int)carpetMin;
    cfg.compute(params, (double)audio.sampleRate);
    if (alpha >= 0.0f) cfg.wienerAlpha = alpha;
    if (wmax >= 0.0f) cfg.wienerMax = wmax;
    if (rerepair >= 0.0f) cfg.rerepair = (int)rerepair;
    if (dip >= 0.0f) cfg.dip = (int)dip;
    if (dipFar >= 0.0f) cfg.dipFar = dipFar;
    if (dipPeak >= 0.0f) cfg.dipPeak = dipPeak;
    if (softMin >= 0.0f) cfg.softMin = softMin;
    if (softSpan >= 0.0f) cfg.softSpan = softSpan;

    std::vector<std::unique_ptr<declick::Channel> > chans;
    for (unsigned c = 0; c < channels; ++c) {
        std::unique_ptr<declick::Channel> ch(new declick::Channel());
        ch->configure(cfg);
        chans.push_back(std::move(ch));
    }

    wavio::Audio out;
    out.channels = channels;
    out.sampleRate = audio.sampleRate;
    out.samples.assign(audio.samples.size(), 0.0);

    // Feed in modest blocks so the streaming path gets a realistic workout.
    const size_t CHUNK = 4096;
    size_t written = 0;
    std::vector<double> scratch(CHUNK * channels);
    for (size_t pos = 0; pos < frames; pos += CHUNK) {
        const size_t n = (frames - pos < CHUNK) ? (frames - pos) : CHUNK;
        for (unsigned c = 0; c < channels; ++c) {
            chans[c]->push(&audio.samples[pos * channels + c], n, channels);
        }
        size_t avail = chans[0]->available();
        while (avail > 0 && written < frames) {
            size_t k = (avail < CHUNK) ? avail : CHUNK;
            if (k > frames - written) k = frames - written;
            for (unsigned c = 0; c < channels; ++c) {
                chans[c]->pull(&scratch[c], k, channels);
            }
            for (size_t i = 0; i < k * channels && written * channels + i < out.samples.size(); ++i) {
                out.samples[written * channels + i] = scratch[i];
            }
            written += k;
            avail = chans[0]->available();
        }
    }
    // Flush the tail.
    for (unsigned c = 0; c < channels; ++c) chans[c]->drain();
    size_t avail = chans[0]->available();
    while (avail > 0 && written < frames) {
        size_t k = (avail < CHUNK) ? avail : CHUNK;
        if (k > frames - written) k = frames - written;
        for (unsigned c = 0; c < channels; ++c) chans[c]->pull(&scratch[c], k, channels);
        for (size_t i = 0; i < k * channels && written * channels + i < out.samples.size(); ++i) {
            out.samples[written * channels + i] = scratch[i];
        }
        written += k;
        avail = chans[0]->available();
    }

    uint64_t repaired = 0, seen = 0, clamped = 0, reclamped = 0, flSpans = 0, flSkipped = 0;
    uint64_t carpetOpen = 0, midCuts = 0, denoiseCuts = 0;
    for (unsigned c = 0; c < channels; ++c) {
        repaired += chans[c]->repairedSamples();
        seen += chans[c]->seenSamples();
        clamped += chans[c]->clampedSamples();
        reclamped += chans[c]->reclampedSamples();
        flSpans += chans[c]->flankSpans();
        flSkipped += chans[c]->flankSkipped();
        carpetOpen += chans[c]->carpetOpenBlocks();
        midCuts += chans[c]->midFrames();
        denoiseCuts += chans[c]->denoiseFrames();
    }

    if (delta) {
        for (size_t i = 0; i < out.samples.size(); ++i) {
            out.samples[i] = audio.samples[i] - out.samples[i];
        }
    }

    if (!wavio::writeFloat32(outPath, out, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    if (!quiet) {
        printf("%s -> %s\n", inPath.c_str(), outPath.c_str());
        printf("  %u Hz, %u ch, %zu frames (%zu written)\n",
               audio.sampleRate, channels, frames, written);
        printf("  sensitivity %.3f (threshold %.2f sigma), extent %.3f (floor %.2f), "
               "max %.1f ms, %d pass%s, order %d\n",
               params.sensitivity, cfg.thresholdHi, params.extent, cfg.thresholdLo,
               params.maxLengthMs, cfg.passes, cfg.passes == 1 ? "" : "es", cfg.order);
        printf("  depth %.3f, alpha %.3f, wmax %.3f\n",
               params.depth, cfg.wienerAlpha, cfg.wienerMax);
        printf("  kernels %s\n", declick::kernelName());
        printf("  latency %d samples (%.1f ms)\n", cfg.latency,
               1000.0 * cfg.latency / (double)audio.sampleRate);
        // Clamped: estimates a repair had to pull back into the local range,
        // which means its solve went wrong. See Channel::clampedSamples().
        printf("  repaired %llu of %llu samples (%.2f%%), clamped %llu first, %llu again\n",
               (unsigned long long)repaired, (unsigned long long)seen,
               seen ? 100.0 * (double)repaired / (double)seen : 0.0,
               (unsigned long long)clamped, (unsigned long long)reclamped);
        // The flank repair: the transfer's -40 dB edge as it stood at the
        // end, the pad that gave, and the spans repaired again.
        if (cfg.flankStage > 0) {
            printf("  flank: edge %.0f Hz, pad %d of %d, %llu spans, %llu passed over\n",
                   chans[0]->bandEdge(), chans[0]->flankPadNow(), cfg.flankPad,
                   (unsigned long long)flSpans, (unsigned long long)flSkipped);
        }
        // The carpet key: the share of the stream it was open over, which is
        // where it opened the fine stage and the flank repair.
        if (cfg.carpet) {
            printf("  carpet: key open over %.1f%% of the stream\n",
                   seen ? 100.0 * (double)(carpetOpen * declick::kBlock) / (double)seen : 0.0);
        }
        // The mid stage: fine-stage frames it cut something from.
        if (cfg.mid && cfg.fine) {
            printf("  mid: %llu frames cut\n", (unsigned long long)midCuts);
        }
        // The denoise stage: frames it cut something from.
        if (cfg.denoise) {
            printf("  denoise: %llu frames cut\n", (unsigned long long)denoiseCuts);
        }
    }
    return 0;
}
