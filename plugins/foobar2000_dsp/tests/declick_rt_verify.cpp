/* ========================================
 *  declick_rt_verify - the audio thread must not touch the heap.
 *
 *  A real-time audio callback cannot allocate: malloc may take a lock, and a
 *  lock held by a lower-priority thread is a dropout. Declick therefore sizes
 *  every buffer it will ever need in configure(), and nothing in the processing
 *  path grows any of them. This test replaces global operator new and requires
 *  the count to be zero.
 *
 *  It exists because the original code failed it twice over, in ways that were
 *  invisible until counted:
 *
 *    - the output FIFO was a vector that grew by push_back to 65536 samples and
 *      was then compacted with erase(), so about a second and a half into every
 *      stream the audio thread took a 720 kB reallocation;
 *    - the per-click interpolation scratch was assign()ed to a length that
 *      varies with the run being repaired, so it grew whenever a longer click
 *      turned up than any seen so far.
 *
 *  Both are now reserved against the envelope in Config, which is derived from
 *  the sample rate alone - so even a parameter change that resizes the pipeline
 *  reassigns each vector to the length it already has.
 *
 *  The one case that still allocates is a caller pushing more than
 *  Config::maxBlock between pulls. That is asserted too, rather than hidden: it
 *  grows the ring once and is then quiet.
 *
 *  It also drives the route the foobar2000 wrapper takes through a parameter
 *  change, because for a long time that was where the allocation actually was:
 *  dsp_declick built fresh Channels on every control move - 38 allocations and
 *  4.3 MB, stereo at 44.1 kHz, inside on_chunk() - and every case here drove a
 *  Channel directly, so none of them saw it. Channel::Channel() sized a whole
 *  44.1 kHz envelope of its own on top, for the caller's configure() to replace.
 * ======================================== */

#include "declick_core.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <vector>

using namespace declick;

// ---------------------------------------------------------------------------
// Counting global allocator. Only armed inside the regions under test, because
// the harness itself - and printf - are entitled to allocate.

namespace {

size_t g_count = 0, g_bytes = 0, g_biggest = 0;
bool   g_armed = false;

int g_failures = 0;

} // anonymous namespace

void * operator new(size_t n) {
    if (g_armed) {
        ++g_count;
        g_bytes += n;
        if (n > g_biggest) g_biggest = n;
    }
    void * p = malloc(n ? n : 1);
    if (p == NULL) throw std::bad_alloc();
    return p;
}
void * operator new[](size_t n) { return operator new(n); }
void operator delete(void * p) throw() { free(p); }
void operator delete[](void * p) throw() { free(p); }
void operator delete(void * p, size_t) throw() { free(p); }
void operator delete[](void * p, size_t) throw() { free(p); }

namespace {

void arm() { g_count = g_bytes = g_biggest = 0; g_armed = true; }

//! Requires the region just measured to have been allocation-free.
void requireQuiet(const char * what) {
    g_armed = false;
    const bool ok = (g_count == 0);
    printf("  %-50s %-4s %zu allocs, %.1f kB\n", what, ok ? "ok" : "FAIL",
           g_count, (double)g_bytes / 1024.0);
    if (!ok) ++g_failures;
}

//! Requires it to have allocated - for the documented overflow path, so that
//! the boundary is recorded rather than assumed away.
void requireGrowth(const char * what) {
    g_armed = false;
    const bool ok = (g_count > 0);
    printf("  %-50s %-4s %zu allocs, %.1f kB\n", what, ok ? "ok" : "FAIL",
           g_count, (double)g_bytes / 1024.0);
    if (!ok) ++g_failures;
}

//! Requires the region to have stayed under `limit` bytes - for a place where
//! an object genuinely has to be created, but must bring no buffers with it.
void requireUnder(const char * what, size_t limit) {
    g_armed = false;
    const bool ok = (g_bytes < limit);
    printf("  %-50s %-4s %zu allocs, %.1f kB\n", what, ok ? "ok" : "FAIL",
           g_count, (double)g_bytes / 1024.0);
    if (!ok) ++g_failures;
}

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Clicks every 700 samples, so interpolate() runs constantly and its scratch
//! is asked for every run length the detector can produce.
std::vector<double> signal(int n, double rate) {
    std::vector<double> v((size_t)n, 0.0);
    Rng rng(22222u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / rate;
        v[(size_t)i] = 0.30 * sin(6.283185307179586 * 220.0 * t)
                     + 0.15 * sin(6.283185307179586 * 661.0 * t)
                     + rng.centred() * 2.0e-3;
        if (i % 700 == 0) {
            // Vary the burst length, so the longest run the scratch has to hold
            // turns up late rather than in the first block.
            const int len = 1 + (i / 700) % 12;
            for (int k = 0; k < len && i + k < n; ++k) {
                v[(size_t)(i + k)] += (k & 1) ? -0.6 : 0.85;
            }
        }
    }
    return v;
}

void perSample(Channel & ch, const std::vector<double> & sig, int from, int to) {
    for (int i = from; i < to; ++i) {
        double x = sig[(size_t)i];
        ch.push(&x, 1, 1);
        ch.pull(&x, 1, 1);
    }
}

//! Feeds the channels the way dsp_declick::on_chunk() does: push a chunk, take
//! whatever is ready. No prime() - the wrapper reads ahead instead.
void pushChunks(std::vector<std::unique_ptr<Channel> > & chan,
                const std::vector<double> & sig, int upTo,
                std::vector<double> & sink) {
    for (int b = 0; b + 4096 < (int)sig.size() && b < upTo; b += 4096) {
        for (size_t c = 0; c < chan.size(); ++c) {
            chan[c]->push(&sig[(size_t)b], 4096, 1);
            const size_t got = chan[c]->available();
            chan[c]->pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
        }
    }
}

//! What dsp_declick::applyParams() does when the controls move: reuse the
//! channels already in hand - retune() where the pipeline keeps its shape,
//! configure() on those same channels where it does not.
//!
//! Mirrored by hand, because this test does not link the foobar2000 SDK. Keep
//! the two in step: the whole point is that neither branch touches the heap.
void applyParams(std::vector<std::unique_ptr<Channel> > & chan,
                 const Config & cfg, std::vector<double> & sink) {
    bool retuned = !chan.empty();
    for (size_t c = 0; c < chan.size(); ++c) {
        if (!chan[c]->retune(cfg)) { retuned = false; break; }
    }
    if (retuned) return;
    for (size_t c = 0; c < chan.size(); ++c) {
        chan[c]->drain();                    // the wrapper's drainAndEmit()
        const size_t got = chan[c]->available();
        chan[c]->pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
        chan[c]->configure(cfg);
    }
}

} // anonymous namespace

int main() {
    const double rate = 44100.0;
    const int R = (int)rate;

    Config cfg;   cfg.compute(Params::defaults(), rate);
    Config c8;    { Params p = Params::defaults(); p.order = 8;  c8.compute(p, rate); }
    Config c64;   { Params p = Params::defaults(); p.order = 64; c64.compute(p, rate); }
    Config cLong; { Params p = Params::defaults(); p.maxLengthMs = 20.0f;
                    cLong.compute(p, rate); }
    Config cTune; { Params p = Params::defaults(); p.sensitivity = 0.95f;
                    p.depth = 0.7f; p.dryWet = 0.4f; p.passes = 3;
                    cTune.compute(p, rate); }

    const std::vector<double> sig = signal(R * 6, rate);
    std::vector<double> sink((size_t)cfg.maxBlock * 4, 0.0);

    Channel ch;
    ch.configure(cfg);
    printf("declick_rt_verify: %.1f kB per channel at %.0f Hz, envelope %d/%d\n\n",
           (double)ch.heapBytes() / 1024.0, rate, cfg.bufMaxRun, cfg.bufOrder);

    // --- the processing path, in both access patterns ----------------------
    ch.prime();
    arm();
    perSample(ch, sig, 0, R / 4);
    requireQuiet("push/pull one sample at a time, first 0.25 s");

    arm();
    perSample(ch, sig, R / 4, 2 * R);
    requireQuiet("... through the point the old FIFO reallocated");

    arm();
    perSample(ch, sig, 2 * R, 6 * R);
    requireQuiet("... and the next four seconds");

    // Whole-chunk push then drain, which is how the foobar2000 wrapper works.
    ch.configure(cfg);
    ch.prime();
    arm();
    for (int b = 0; b + 4096 < (int)sig.size(); b += 4096) {
        ch.push(&sig[(size_t)b], 4096, 1);
        const size_t got = ch.available();
        ch.pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
    }
    requireQuiet("push 4096 / drain everything, whole signal");

    // A push right up to the declared maximum must still be quiet.
    ch.configure(cfg);
    ch.prime();
    arm();
    for (int b = 0; b + cfg.maxBlock < (int)sig.size(); b += cfg.maxBlock) {
        ch.push(&sig[(size_t)b], (size_t)cfg.maxBlock, 1);
        const size_t got = ch.available();
        ch.pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
    }
    requireQuiet("push exactly maxBlock / drain");

    // --- parameter changes -------------------------------------------------
    arm();
    const bool retuned = ch.retune(cTune);
    requireQuiet("retune(): Sensitivity, Depth, Passes, Dry/Wet");
    if (!retuned) {
        printf("  FAIL  retune() refused a same-envelope config\n");
        ++g_failures;
    }
    arm();
    perSample(ch, sig, 0, R);
    requireQuiet("... and a second of audio after it");

    // These resize the pipeline, so they reset it - but the buffers are sized
    // from the envelope, so there is nothing left to allocate.
    arm();
    ch.configure(c64);
    ch.prime();
    requireQuiet("configure(): Model order 32 -> 64");
    arm();
    ch.configure(c8);
    ch.prime();
    requireQuiet("configure(): Model order 64 -> 8");
    arm();
    ch.configure(cLong);
    ch.prime();
    requireQuiet("configure(): Max repair 4 -> 20 ms");
    arm();
    perSample(ch, sig, 0, R);
    requireQuiet("... and a second of audio at the longest repair");

    arm();
    ch.reset();
    ch.prime();
    requireQuiet("reset() + prime(), i.e. a transport restart");

    arm();
    ch.drain();
    requireQuiet("drain(), i.e. end of stream");

    // --- the route the component itself takes ------------------------------
    //
    // Everything above drives one Channel directly. dsp_declick does not: it
    // holds one per channel and moves the parameters through those, and that is
    // where the allocation used to be.
    {
        std::vector<std::unique_ptr<Channel> > chan;
        chan.reserve(2);
        arm();
        for (int c = 0; c < 2; ++c) {
            chan.push_back(std::unique_ptr<Channel>(new Channel()));
        }
        // Two Channel objects and nothing else. The constructor must not size a
        // pipeline it is about to be told the real shape of.
        requireUnder("constructing two Channels brings no buffers", 8u * 1024u);

        for (size_t c = 0; c < chan.size(); ++c) chan[c]->configure(cfg);
        pushChunks(chan, sig, 2 * R, sink);

        arm();
        applyParams(chan, cTune, sink);
        requireQuiet("wrapper: a control move that keeps the shape");

        arm();
        applyParams(chan, cLong, sink);
        requireQuiet("wrapper: Max repair, which reshapes the pipeline");

        arm();
        applyParams(chan, c8, sink);
        requireQuiet("wrapper: Model order, the reshaping route again");

        arm();
        pushChunks(chan, sig, R, sink);
        requireQuiet("wrapper: audio after the moves");
    }

    // --- the Wiener path, which is off by default -------------------------
    {
        Config cw = cfg;
        cw.wienerAlpha = 4.0;
        Channel w;
        w.configure(cw);
        w.prime();
        printf("\n  (Wiener weighting on: %.1f kB per channel)\n",
               (double)w.heapBytes() / 1024.0);
        arm();
        perSample(w, sig, 0, 2 * R);
        requireQuiet("push/pull with Wiener weighting engaged");
    }

    // --- the one documented exception --------------------------------------
    {
        Channel wide;
        wide.configure(cfg);
        wide.prime();
        const std::vector<double> lots((size_t)cfg.maxBlock * 2, 0.0);
        arm();
        wide.push(&lots[0], lots.size(), 1);
        requireGrowth("push 2x maxBlock in one call grows the ring, once");
        arm();
        const size_t got = wide.available();
        wide.pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
        wide.push(&lots[0], lots.size(), 1);
        requireQuiet("... and is quiet from then on");
    }

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
