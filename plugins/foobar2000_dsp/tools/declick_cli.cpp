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
        "  -s <0..1>    Sensitivity (default 0.8, higher catches more)\n"
        "  -e <0..1>    Extent, how far a detection spreads (default 0.5)\n"
        "  -l <ms>      Longest single repair, ms (default 4)\n"
        "  -p <1..3>    Passes (default 2)\n"
        "  -o <8..256>  AR model order (default 128)\n"
        "  -d <0..1>    Repair depth: 0 = 45%% subtraction, 1 = full replacement (default)\n"
        "  -w <0..1>    Dry/wet (default 1)\n"
        "  --delta      Write what was removed instead of the repaired audio\n"
        "  --quiet\n"
        "\n"
        "  Calibration only, not exposed in the component:\n"
        "  --alpha <x>  Over-subtraction on the estimate variance\n"
        "  --wmax <x>   Ceiling on the subtracted fraction\n"
        "  --rerepair <0..2>  Repair runs again once the model is refitted (default 1)\n");
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

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--delta") { delta = true; continue; }
        if (a == "--quiet") { quiet = true; continue; }
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
    cfg.compute(params, (double)audio.sampleRate);
    if (alpha >= 0.0f) cfg.wienerAlpha = alpha;
    if (wmax >= 0.0f) cfg.wienerMax = wmax;
    if (rerepair >= 0.0f) cfg.rerepair = (int)rerepair;

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

    uint64_t repaired = 0, seen = 0;
    for (unsigned c = 0; c < channels; ++c) {
        repaired += chans[c]->repairedSamples();
        seen += chans[c]->seenSamples();
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
        printf("  latency %d samples (%.1f ms)\n", cfg.latency,
               1000.0 * cfg.latency / (double)audio.sampleRate);
        printf("  repaired %llu of %llu samples (%.2f%%)\n",
               (unsigned long long)repaired, (unsigned long long)seen,
               seen ? 100.0 * (double)repaired / (double)seen : 0.0);
    }
    return 0;
}
