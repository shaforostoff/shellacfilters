/* ========================================
 *  Mixxx restoration - test support
 *
 *  Enough of Mixxx to drive the pipeline without it. The cores are already
 *  pinned by declick_verify, dehum_verify and friends in
 *  ../../foobar2000_dsp/tests, and nothing here re-tests the maths. What is new
 *  in this port, and therefore what is checked, is restorationpipeline.h: a
 *  sequential pair of cores driven by a caller that asks for the track by frame
 *  range and jumps about.
 *
 *  Its central claim is the one the whole port exists for - that the audio it
 *  returns for [a, b) is what the core produces running straight through the
 *  track, aligned with what was asked for and with NO DELAY - and that is what
 *  referenceRun below is for: written directly against the core, so it shares no
 *  indexing arithmetic with the thing it is checking.
 * ======================================== */

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "restoration/restorationpipeline.h"

namespace restoretest {

// ---------------------------------------------------------------------------
// Reporting

extern int g_failures;

inline void check(bool ok, const char* what) {
    if (ok) {
        printf("  ok    %s\n", what);
        return;
    }
    printf("  FAIL  %s\n", what);
    ++g_failures;
}

inline void checkf(bool ok, const char* fmt, double a, double b) {
    if (ok) {
        return;
    }
    printf("  FAIL  ");
    printf(fmt, a, b);
    printf("\n");
    ++g_failures;
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        printf("%s: all checks passed\n", name);
        return 0;
    }
    printf("%s: %d check(s) FAILED\n", name, g_failures);
    return 1;
}

// ---------------------------------------------------------------------------
// Signals
//
// Deterministic, because a test that fails once in twenty runs is a test nobody
// trusts. A fixed-seed LCG rather than rand(), whose sequence is not the same
// across platforms.

class Rng {
  public:
    explicit Rng(uint32_t seed = 12345u)
            : m_s(seed) {
    }
    double uniform() {
        m_s = m_s * 1664525u + 1013904223u;
        return (double)(m_s >> 8) / (double)(1u << 24) * 2.0 - 1.0;
    }

  private:
    uint32_t m_s;
};

//! Music with clicks on it, interleaved, `channels` wide.
inline std::vector<float> makeClicky(int64_t frames,
        int channels,
        double rate,
        uint32_t seed = 12345u) {
    std::vector<float> x((size_t)(frames * channels), 0.0f);
    Rng rng(seed);
    for (int64_t i = 0; i < frames; ++i) {
        const double t = (double)i / rate;
        const double v = 0.34 * sin(2.0 * M_PI * 220.0 * t) +
                0.18 * sin(2.0 * M_PI * 587.33 * t) + 0.05 * rng.uniform();
        for (int c = 0; c < channels; ++c) {
            x[(size_t)(i * channels + c)] =
                    (float)(v * (c == 0 ? 1.0 : 0.9) + 0.03 * rng.uniform());
        }
    }
    // Clicks every 0.21 s, alternating sign.
    const int64_t step = (int64_t)(0.21 * rate);
    int sign = 1;
    for (int64_t at = step; at + 16 < frames; at += step, sign = -sign) {
        for (int k = 0; k < 9; ++k) {
            const float a = (float)(sign * 0.7 * exp(-0.35 * k));
            for (int c = 0; c < channels; ++c) {
                x[(size_t)((at + k) * channels + c)] += a;
            }
        }
    }
    return x;
}

//! A tonal bed with no clicks and no hum: something for a line to hide in.
inline std::vector<float> makeMusic(int64_t frames,
        int channels,
        double rate,
        uint32_t seed = 5150u) {
    std::vector<float> x((size_t)(frames * channels), 0.0f);
    Rng rng(seed);
    for (int64_t i = 0; i < frames; ++i) {
        const double t = (double)i / rate;
        const double v = 0.30 * sin(2.0 * M_PI * 196.0 * t) +
                0.15 * sin(2.0 * M_PI * 493.88 * t) +
                0.08 * sin(2.0 * M_PI * 987.77 * t) + 0.06 * rng.uniform();
        for (int c = 0; c < channels; ++c) {
            x[(size_t)(i * channels + c)] =
                    (float)(v * (c == 0 ? 1.0 : 0.92) + 0.03 * rng.uniform());
        }
    }
    return x;
}

//! Add a steady line, the way a mains hum or a drone sits under a transfer.
inline void addTone(std::vector<float>& x,
        int channels,
        double rate,
        double hz,
        double amplitude) {
    const int64_t frames = (int64_t)x.size() / channels;
    for (int64_t i = 0; i < frames; ++i) {
        const double v = amplitude * sin(2.0 * M_PI * hz * (double)i / rate);
        for (int c = 0; c < channels; ++c) {
            x[(size_t)(i * channels + c)] += (float)v;
        }
    }
}

//! A steady line plus noise: what Dehum is for.
inline std::vector<float> makeHum(int64_t frames,
        int channels,
        double rate,
        double hz,
        double amplitude = 0.25,
        uint32_t seed = 999u) {
    std::vector<float> x((size_t)(frames * channels), 0.0f);
    Rng rng(seed);
    for (int64_t i = 0; i < frames; ++i) {
        const double t = (double)i / rate;
        const double hum = amplitude * sin(2.0 * M_PI * hz * t);
        for (int c = 0; c < channels; ++c) {
            x[(size_t)(i * channels + c)] = (float)(hum + 0.05 * rng.uniform());
        }
    }
    return x;
}

//! Level at one frequency, by direct correlation on one channel. Enough to say
//! whether a notch did anything; not a spectrum analyser and not trying to be.
inline double toneLevel(const std::vector<float>& x,
        int channels,
        int channel,
        int64_t from,
        int64_t to,
        double rate,
        double hz) {
    double re = 0.0, im = 0.0;
    const int64_t n = to - from;
    if (n <= 0) {
        return 0.0;
    }
    for (int64_t i = from; i < to; ++i) {
        const double t = 2.0 * M_PI * hz * (double)i / rate;
        const double v = (double)x[(size_t)(i * channels + channel)];
        re += v * cos(t);
        im += v * sin(t);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

// ---------------------------------------------------------------------------
// A track

//! A decoded track held in memory, counting every frame the pipeline asks for.
//!
//! The count is not decoration: the whole claim of this port is that Declick's
//! lookahead is paid in reads rather than in delay, so "how much did it read"
//! is the cost being traded for, and testSequentialReadCost pins it.
class MemorySource : public restoration::Source {
  public:
    MemorySource(const std::vector<float>& audio, int channels)
            : m_audio(audio),
              m_channels(channels),
              m_frames((int64_t)audio.size() / channels) {
    }

    int64_t read(int64_t startFrame, int64_t frames, float* out) override {
        ++m_reads;
        if (startFrame < 0 || startFrame >= m_frames || frames <= 0) {
            return 0;
        }
        int64_t got = frames;
        if (startFrame + got > m_frames) {
            got = m_frames - startFrame;
        }
        memcpy(out,
                &m_audio[(size_t)(startFrame * m_channels)],
                (size_t)(got * m_channels) * sizeof(float));
        m_framesRead += got;
        return got;
    }

    int64_t frames() const {
        return m_frames;
    }
    int64_t framesRead() const {
        return m_framesRead;
    }
    int64_t reads() const {
        return m_reads;
    }
    void resetCounters() {
        m_framesRead = 0;
        m_reads = 0;
    }

  private:
    const std::vector<float>& m_audio;
    int m_channels;
    int64_t m_frames;
    int64_t m_framesRead = 0;
    int64_t m_reads = 0;
};

// ---------------------------------------------------------------------------
// References, written straight against the cores

//! Declick over the whole track in one go, the way foo_dsp_declick would if it
//! could see the file at once: push everything, pull everything.
//!
//! No prime() and no delay compensation, because there is nothing to compensate
//! - this is the thing the pipeline claims to reproduce exactly, with output
//! frame i being input frame i.
inline std::vector<float> declickReference(const std::vector<float>& in,
        int channels,
        double rate,
        const declick::Params& params) {
    declick::Config cfg;
    cfg.compute(params, rate);

    const int64_t frames = (int64_t)in.size() / channels;
    std::vector<declick::Channel> ch((size_t)channels);
    for (int c = 0; c < channels; ++c) {
        ch[(size_t)c].configure(cfg);
    }

    // Feed the track and then enough zeros that everything comes back out. The
    // core emits whole kBlock blocks, so `latency` alone can leave the last
    // partial block inside it; one more block is what makes the pull exact.
    const int64_t pad = cfg.latency + (int64_t)declick::kBlock;
    std::vector<float> padded = in;
    padded.resize((size_t)((frames + pad) * channels), 0.0f);

    std::vector<float> out((size_t)(frames * channels), 0.0f);
    {
        declick::scoped_flush_denormals ftz;
        for (int c = 0; c < channels; ++c) {
            ch[(size_t)c].push(padded.data() + c,
                    (size_t)(frames + pad),
                    (size_t)channels);
            ch[(size_t)c].pull(out.data() + c, (size_t)frames, (size_t)channels);
        }
    }
    return out;
}

//! Dehum over the whole track in one go. Zero latency, so this is just the core
//! run in place.
inline std::vector<float> dehumReference(const std::vector<float>& in,
        int channels,
        double rate,
        const dehum::Params& params) {
    dehum::Config cfg;
    cfg.compute(params, rate);

    const int64_t frames = (int64_t)in.size() / channels;
    std::vector<dehum::Channel> ch((size_t)channels);
    for (int c = 0; c < channels; ++c) {
        ch[(size_t)c].configure(cfg);
    }

    std::vector<float> out = in;
    {
        dehum::scoped_flush_denormals ftz;
        for (int c = 0; c < channels; ++c) {
            ch[(size_t)c].process(out.data() + c, (size_t)frames, (size_t)channels);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Driving the pipeline

//! Read the whole track through the pipeline in blocks of `block`, the way
//! CachingReaderWorker reads chunks.
inline std::vector<float> serveAll(restoration::Pipeline& pipe,
        restoration::Source& src,
        int64_t frames,
        int channels,
        int64_t block) {
    std::vector<float> out((size_t)(frames * channels), 0.0f);
    int64_t at = 0;
    while (at < frames) {
        int64_t n = block;
        if (at + n > frames) {
            n = frames - at;
        }
        const int64_t got = pipe.serve(
                src, at, n, &out[(size_t)(at * channels)]);
        if (got <= 0) {
            break;
        }
        at += got;
    }
    return out;
}

//! Bit-exact float comparison. Most of what these harnesses compare is one run
//! of a deterministic pipeline against another run of the same one, and there
//! "close enough" would hide exactly the bugs being looked for.
inline bool sameBits(const std::vector<float>& a,
        const std::vector<float>& b,
        size_t from = 0,
        size_t to = (size_t)-1) {
    if (a.size() != b.size()) {
        return false;
    }
    if (to > a.size()) {
        to = a.size();
    }
    for (size_t i = from; i < to; ++i) {
        if (memcmp(&a[i], &b[i], sizeof(float)) != 0) {
            return false;
        }
    }
    return true;
}

//! Index of the first sample that differs, or -1. For reporting, so a failure
//! says where rather than only that.
inline int64_t firstDifference(const std::vector<float>& a,
        const std::vector<float>& b) {
    const size_t n = (a.size() < b.size()) ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (memcmp(&a[i], &b[i], sizeof(float)) != 0) {
            return (int64_t)i;
        }
    }
    return (a.size() == b.size()) ? -1 : (int64_t)n;
}

inline double rmsDifference(const std::vector<float>& a,
        const std::vector<float>& b,
        size_t from = 0,
        size_t to = (size_t)-1) {
    if (to > a.size()) {
        to = a.size();
    }
    if (to > b.size()) {
        to = b.size();
    }
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to; ++i) {
        const double d = (double)a[i] - (double)b[i];
        sum += d * d;
        ++n;
    }
    return n ? sqrt(sum / (double)n) : 0.0;
}

} // namespace restoretest
