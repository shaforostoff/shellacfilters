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

#include <chrono>
#include <memory>
#include <string>
#include <utility>
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
        "  --wmax <x>   Ceiling on the subtracted fraction\n"
        "  --rerepair <0..2>  Repair runs again once the model is refitted (default 1)\n"
        "  --dip <n>    Samples below the floor a run may step over (default 1)\n"
        "  --dip-far <0..1>  How far above the floor it must be beyond them (default 0.5)\n"
        "  --dip-peak <x>    Only runs that peak this many sigma or more may (default 6)\n"
        "  --soft-min <0..1>  Share of a run at the trigger that is repaired (default 0.5)\n"
        "  --soft-span <x>    Sigma above the trigger where repair is full (default 3.5, 0 off)\n"
        "  --shape <0|1>      The shape fit: clicks fitted as three fixed shapes and\n"
        "                     subtracted before the first pass (default 0: research)\n"
        "  --shape-chi <x>    Its matched filter's threshold, sigma as for two shapes (default 3)\n"
        "  --shape-gate <x>   2-8 kHz power over 1 ms against its median over +-15 ms (default 3.5)\n"
        "  --shape-ms <ms>    Half length of the shapes (default 0.75)\n"
        "  --shape-fitq <x>   Share of the residual the shapes must explain (default 0.3)\n"
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
        "  --flank-wide <0..1> Share of the pad a full-band transfer gets under the key (default 1)\n"
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
        "  --crackle-floor <0|1>\n"
        "                     Lowers the floor dense crackle forms, a spectral denoiser\n"
        "                     on the output under the fine stage's key (default 1)\n"
        "  --crackle-floor-lo <Hz>\n"
        "                     Where it starts, ramped in over the half octave below (default 1000)\n"
        "  --crackle-floor-hi <Hz>\n"
        "                     Where it stops, faded out over the half octave below; the top\n"
        "                     is left as it was. 0: to Nyquist (default 4500)\n"
        "  --crackle-floor-beta <x>\n"
        "                     Cut against this many times the noise floor (default 1.2)\n"
        "  --crackle-floor-min <0..1>\n"
        "                     Deepest cut of a bin, as a gain (default 0.32)\n"
        "  --crackle-floor-q <0..1>\n"
        "                     Quantile of a bin's power that is its floor (default 0.1)\n"
        "  --crackle-floor-win <s>\n"
        "                     Seconds of history the floor is taken over (default 6)\n"
        "  --crackle-floor-release <s>\n"
        "                     Time constant the cut comes back with (default 0.06)\n"
        "  --warm-start <s>   Before the stream, run the core over its first s seconds\n"
        "                     with the output held back, so the start is processed with\n"
        "                     the keys and crackle floor those seconds give; -1 for the\n"
        "                     core's own length (Config::warmSeconds). 0: off (default)\n");
}

bool parseFloat(const char * s, float & out) {
    char * end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    out = (float)v;
    return true;
}

//! The calibration flags, each one Config field: set before compute(), which
//! derives the rest from them (whether the body stage and the flank repair
//! run sets the latency), or after it, where compute() would overwrite them.
//! A negative value leaves the calibrated one.
struct ConfigFlag {
    const char * name;
    double declick::Config::* d;     //!< one of these two
    int    declick::Config::* i;
    bool   late;                     //!< after compute()
};
#define DBL(n, f)      { n, &declick::Config::f, nullptr, false }
#define INT(n, f)      { n, nullptr, &declick::Config::f, false }
#define DBL_LATE(n, f) { n, &declick::Config::f, nullptr, true }
#define INT_LATE(n, f) { n, nullptr, &declick::Config::f, true }
const ConfigFlag kConfigFlags[] = {
    INT("--body",                  body),
    DBL("--body-floor",            bodyFloor),
    DBL("--body-peak",             bodyPeak),
    DBL("--body-rate0",            bodyRate0),
    DBL("--body-rate1",            bodyRate1),
    DBL("--body-release",          bodyRelease),
    INT("--fine",                  fine),
    DBL("--fine-lo",               fineLo),
    DBL("--fine-hi",               fineHi),
    DBL("--fine-alpha",            fineAlpha),
    DBL("--fine-q",                fineQ),
    DBL("--fine-floor",            fineFloor),
    DBL("--fine-mid",              fineMid),
    DBL("--fine-p0",               finePeriodic0),
    DBL("--fine-p1",               finePeriodic1),
    INT("--micro",                 micro),
    DBL("--micro-ms",              microMs),
    DBL("--micro-alpha",           microAlpha),
    DBL("--micro-q",               microQ),
    INT("--shape",                 shape),
    DBL("--shape-chi",             shapeChi),
    DBL("--shape-gate",            shapeGate),
    DBL("--shape-ms",              shapeMs),
    DBL("--shape-fitq",            shapeFitq),
    INT("--mid",                   mid),
    DBL("--mid-lo",                midLo),
    DBL("--mid-peak",              midPeak),
    DBL("--mid-p0",                midPeriodic0),
    DBL("--mid-p1",                midPeriodic1),
    INT("--mid-yield",             midYield),
    DBL("--carpet-first",          carpetFirst),
    INT("--carpet-fano",           carpetFano),
    DBL("--fano-rate0",            fanoRate0),
    DBL("--fano-rate1",            fanoRate1),
    DBL("--fano0",                 fano0),
    DBL("--fano1",                 fano1),
    DBL("--fano-first",            fanoFirst),
    INT("--crackle-floor",         crackleFloor),
    DBL("--crackle-floor-lo",      crackleFloorLo),
    DBL("--crackle-floor-beta",    crackleFloorBeta),
    DBL("--crackle-floor-min",     crackleFloorMin),
    DBL("--crackle-floor-q",       crackleFloorQ),
    DBL("--crackle-floor-win",     crackleFloorWin),
    DBL("--crackle-floor-release", crackleFloorRelease),
    DBL("--crackle-floor-hi",      crackleFloorHi),
    INT("--flank",                 flank),
    DBL("--flank-pad-ms",          flankPadMs),
    DBL("--flank-hz",              flankHz),
    DBL("--flank-edge0",           flankEdge0),
    DBL("--flank-edge1",           flankEdge1),
    DBL("--flank-wide",            flankWide),
    DBL("--flank-hold",            flankHold),
    INT("--carpet",                carpet),
    DBL("--carpet-peak",           carpetPeak),
    INT("--carpet-win",            carpetWin),
    DBL("--carpet-q",              carpetQ),
    DBL("--carpet-rate0",          carpetRate0),
    DBL("--carpet-rate1",          carpetRate1),
    DBL("--carpet-warm",           carpetWarm),
    INT("--carpet-min",            carpetMin),
    DBL_LATE("--wmax",             wienerMax),
    INT_LATE("--rerepair",         rerepair),
    INT_LATE("--dip",              dip),
    DBL_LATE("--dip-far",          dipFar),
    DBL_LATE("--dip-peak",         dipPeak),
    DBL_LATE("--soft-min",         softMin),
    DBL_LATE("--soft-span",        softSpan),
};
#undef DBL
#undef INT
#undef DBL_LATE
#undef INT_LATE

const ConfigFlag * findFlag(const std::string & a) {
    for (const ConfigFlag & f : kConfigFlags) if (a == f.name) return &f;
    return nullptr;
}

} // anonymous namespace

int main(int argc, char ** argv) {
    if (argc < 3) { usage(); return 2; }

    const std::string inPath = argv[1];
    const std::string outPath = argv[2];
    declick::Params params = declick::Params::defaults();
    bool delta = false, quiet = false;
    std::vector<std::pair<const ConfigFlag *, float> > flags;   // in the order given
    float warmStart = 0.0f;
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
        if (const ConfigFlag * f = findFlag(a)) flags.push_back(std::make_pair(f, v));
        else if (a == "--warm-start") warmStart = v;
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
    const auto apply = [&](bool late) {
        for (const auto & s : flags) {
            if (s.first->late != late || s.second < 0.0f) continue;
            if (s.first->d) cfg.*(s.first->d) = s.second;
            else cfg.*(s.first->i) = (int)s.second;
        }
    };
    apply(false);
    cfg.compute(params, (double)audio.sampleRate);
    apply(true);

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

    // As every host does around the core: denormals cost x86 dearly.
    const declick::scoped_flush_denormals ftz;

    // Feed in modest blocks so the streaming path gets a realistic workout.
    const size_t CHUNK = 4096;
    size_t written = 0;
    std::vector<double> scratch(CHUNK * channels);

    // The warm start: the file's first seconds through the core with the
    // output held back, then the file from its start. See Channel::beginWarm().
    size_t warmFrames = 0;
    double warmMs = 0.0;
    if (warmStart != 0.0f) {
        const double ws = (warmStart < 0.0f) ? cfg.warmSeconds() : (double)warmStart;
        warmFrames = (size_t)(ws * (double)audio.sampleRate);
        if (warmFrames > frames) warmFrames = frames;
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned c = 0; c < channels; ++c) chans[c]->beginWarm();
        for (size_t pos = 0; pos < warmFrames; pos += CHUNK) {
            const size_t n = (warmFrames - pos < CHUNK) ? (warmFrames - pos) : CHUNK;
            for (unsigned c = 0; c < channels; ++c)
                chans[c]->push(&audio.samples[pos * channels + c], n, channels);
        }
        for (unsigned c = 0; c < channels; ++c) chans[c]->endWarm();
        warmMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
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
    uint64_t carpetOpen = 0, midCuts = 0, crackleFloorCuts = 0, shapeFits = 0;
    for (unsigned c = 0; c < channels; ++c) {
        repaired += chans[c]->repairedSamples();
        seen += chans[c]->seenSamples();
        clamped += chans[c]->clampedSamples();
        reclamped += chans[c]->reclampedSamples();
        flSpans += chans[c]->flankSpans();
        flSkipped += chans[c]->flankSkipped();
        carpetOpen += chans[c]->carpetOpenBlocks();
        midCuts += chans[c]->midFrames();
        crackleFloorCuts += chans[c]->crackleFloorFrames();
        shapeFits += chans[c]->shapeFits();
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
        printf("  depth %.3f, wmax %.3f\n", params.depth, cfg.wienerMax);
        printf("  kernels %s\n", declick::kernelName());
        printf("  latency %d samples (%.1f ms)\n", cfg.latency,
               1000.0 * cfg.latency / (double)audio.sampleRate);
        if (warmFrames > 0) {
            printf("  warm start: %.2f s of audio ahead, %.1f ms\n",
                   (double)warmFrames / (double)audio.sampleRate, warmMs);
        }
        // Clamped: estimates a repair had to pull back into the local range,
        // which means its solve went wrong. See Channel::clampedSamples().
        printf("  repaired %llu of %llu samples (%.2f%%), clamped %llu first, %llu again\n",
               (unsigned long long)repaired, (unsigned long long)seen,
               seen ? 100.0 * (double)repaired / (double)seen : 0.0,
               (unsigned long long)clamped, (unsigned long long)reclamped);
        // The shape fit: the clicks it fitted and took, per second per channel.
        if (cfg.shape) {
            printf("  shape fit: %llu clicks (%.1f/s)\n", (unsigned long long)shapeFits,
                   frames ? (double)shapeFits * (double)audio.sampleRate / (double)frames / (double)channels : 0.0);
        }
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
        // The crackle floor stage: frames it cut something from.
        if (cfg.crackleFloor) {
            printf("  crackle floor: %llu frames cut\n", (unsigned long long)crackleFloorCuts);
        }
    }
    return 0;
}
