/* ========================================
 *  dehum_cli - run the dehum core over a WAV file offline.
 *
 *    dehum_cli <in.wav> <out.wav> [options]
 *
 *  Development utility: used to calibrate the detector against reference
 *  transfers and to batch-process material for listening.
 * ======================================== */

#include "../foo_dsp_dehum/dehum_core.h"
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
        "dehum_cli - narrowband hum removal by detection and coherent subtraction\n"
        "\n"
        "  dehum_cli <in.wav> <out.wav> [options]\n"
        "\n"
        "  -s <0..1>    Sensitivity (default 0.5 -> 24 dB prominence)\n"
        "  -b <Hz>      Notch half width (default 1.0)\n"
        "  -t <Hz>      Top of the automatic search range (default 150)\n"
        "  -n <1..8>    Harmonics of each line to cancel (default 1)\n"
        "  -f <Hz>      Pin the fundamental here; 0 = detect (default 0)\n"
        "  -r <Hz>      Rumble high-pass corner; 0 = off (default 40)\n"
        "  -w <0..1>    Dry/wet (default 1)\n"
        "  --delta      Write what was removed instead of the cleaned audio.\n"
        "               Meaningful for the notch, whose delta really is confined\n"
        "               to the line. NOT meaningful for -r: what a minimum-phase\n"
        "               high-pass removes is 1-H, which decays at 6 dB/octave\n"
        "               whatever its order, so its delta carries the whole\n"
        "               spectrum as phase shift while the output's magnitude\n"
        "               above the corner is untouched. See the README.\n"
        "  --quiet\n"
        "  --no-avx     Use the base probe kernel even where AVX is available.\n"
        "               Same output either way; for timing.\n"
        "\n"
        "  Calibration only, not exposed in the component:\n"
        "  --notrack        Freeze the frequency tracker\n"
        "  --trackgain <x>  Fraction of the measured frequency error applied\n"
        "  --trackclamp <Hz> Furthest a line may be pulled from where it was found\n");
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
    dehum::Params params = dehum::Params::defaults();
    bool delta = false, quiet = false;
    float trackGain = -1.0f, trackClamp = -1.0f;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--delta") { delta = true; continue; }
        if (a == "--quiet") { quiet = true; continue; }
        if (a == "--notrack") { trackGain = 0.0f; continue; }
        if (a == "--no-avx") { dehum::allowWideKernels(false); continue; }
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); return 2; }
        float v = 0.0f;
        if (!parseFloat(argv[i + 1], v)) {
            fprintf(stderr, "not a number: %s\n", argv[i + 1]);
            return 2;
        }
        ++i;
        if      (a == "--trackgain") trackGain = v;
        else if (a == "--trackclamp") trackClamp = v;
        else if (a == "-s") params.sensitivity = v;
        else if (a == "-b") params.bandwidth = v;
        else if (a == "-t") params.searchTo = v;
        else if (a == "-n") params.harmonics = (int)v;
        else if (a == "-f") params.frequency = v;
        else if (a == "-r") params.rumbleHz = v;
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

    dehum::Config cfg;
    cfg.compute(params, (double)audio.sampleRate);
    if (trackGain >= 0.0f) cfg.trackGain = trackGain;
    if (trackClamp >= 0.0f) cfg.trackClampHz = trackClamp;

    std::vector<std::unique_ptr<dehum::Channel> > chans;
    for (unsigned c = 0; c < channels; ++c) {
        std::unique_ptr<dehum::Channel> ch(new dehum::Channel());
        ch->configure(cfg);
        chans.push_back(std::move(ch));
    }

    wavio::Audio out;
    out.channels = channels;
    out.sampleRate = audio.sampleRate;
    out.samples = audio.samples;      // processed in place, zero latency

    // Modest blocks, so the streaming path gets a realistic workout.
    const size_t CHUNK = 4096;
    {
        dehum::scoped_flush_denormals ftz;
        for (size_t pos = 0; pos < frames; pos += CHUNK) {
            const size_t n = (frames - pos < CHUNK) ? (frames - pos) : CHUNK;
            for (unsigned c = 0; c < channels; ++c) {
                chans[c]->process(&out.samples[pos * channels + c], n, channels);
            }
        }
    }

    if (!quiet) {
        printf("%s -> %s\n", inPath.c_str(), outPath.c_str());
        printf("  %u Hz, %u ch, %zu frames\n", audio.sampleRate, channels, frames);
        printf("  window %d (%.2f s, %.3f Hz bins), hop %d, threshold %.1f dB\n",
               cfg.fftSize, cfg.fftSize / (double)audio.sampleRate,
               audio.sampleRate / (double)cfg.fftSize, cfg.hop, cfg.promDb);
        printf("  search %.0f-%d Hz, bandwidth %.2f Hz, %d harmonic%s",
               (double)dehum::kSearchFloor,
               (int)(cfg.binHi * audio.sampleRate / cfg.fftSize),
               cfg.halfWidth, cfg.harmonics, cfg.harmonics == 1 ? "" : "s");
        if (cfg.manualFreq > 0.0) printf(", pinned at %.2f Hz", cfg.manualFreq);
        if (cfg.rumbleHz > 0.0) printf(", rumble high-pass %.0f Hz", cfg.rumbleHz);
        printf("\n  dry/wet %.3f, %zu kB per channel, %s probe kernel\n",
               cfg.wet, chans.empty() ? 0 : chans[0]->heapBytes() / 1024,
               dehum::kernelName());

        for (unsigned c = 0; c < channels; ++c) {
            dehum::LineReport rep[dehum::kMaxLines];
            int n = 0;
            chans[c]->report(rep, (int)dehum::kMaxLines, &n);
            printf("  channel %u: %d line%s (%u confirmed, %u dropped out)\n",
                   c, n, n == 1 ? "" : "s",
                   chans[c]->confirmations(), chans[c]->dropouts());
            for (int i = 0; i < n; ++i) {
                printf("    %8.3f Hz (detected %8.3f, prominence %5.1f dB, "
                       "coherence %.3f, via %s, amplitude %.1f dBFS, "
                       "%d harmonic%s)\n",
                       rep[i].frequency, rep[i].detected, rep[i].prominence,
                       rep[i].coherence,
                       rep[i].viaCoherence ? "coherence" : "prominence",
                       20.0 * log10(rep[i].amplitude + 1e-30),
                       rep[i].harmonics, rep[i].harmonics == 1 ? "" : "s");
            }
        }

        // What was taken out, overall and relative to the programme.
        double sx = 0.0, sd = 0.0;
        for (size_t i = 0; i < out.samples.size(); ++i) {
            const double d = audio.samples[i] - out.samples[i];
            sx += audio.samples[i] * audio.samples[i];
            sd += d * d;
        }
        const size_t nAll = out.samples.size() ? out.samples.size() : 1;
        const double rx = sqrt(sx / (double)nAll), rd = sqrt(sd / (double)nAll);
        printf("  input %.1f dBFS rms, removed %.1f dBFS rms (%.1f dB below input)\n",
               20.0 * log10(rx + 1e-30), 20.0 * log10(rd + 1e-30),
               20.0 * log10((rd + 1e-30) / (rx + 1e-30)));
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
    return 0;
}
