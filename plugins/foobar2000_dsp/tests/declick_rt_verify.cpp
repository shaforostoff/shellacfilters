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
#include "warm_hold.h"

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
//! is asked for every run length the detector can produce; none from sample
//! `quietFrom` to `quietTo`, if given.
std::vector<double> signal(int n, double rate, int quietFrom = 0, int quietTo = 0) {
    std::vector<double> v((size_t)n, 0.0);
    Rng rng(22222u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / rate;
        v[(size_t)i] = 0.30 * sin(6.283185307179586 * 220.0 * t)
                     + 0.15 * sin(6.283185307179586 * 661.0 * t)
                     + rng.centred() * 2.0e-3;
        if (i % 700 == 0 && !(i >= quietFrom && i < quietTo)) {
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

//! The signal through a windowed-sinc low-pass at `hz`: a transfer band-limited
//! like the .m4a ones, which is what the flank repair needs before it acts.
std::vector<double> lowpassed(const std::vector<double> & x, double rate, double hz) {
    const int H = 50;
    const double fc = hz / rate;
    std::vector<double> h((size_t)(2 * H + 1));
    double sum = 0.0;
    for (int j = -H; j <= H; ++j) {
        const double s = (j == 0) ? 2.0 * fc : sin(6.283185307179586 * fc * j) / (3.141592653589793 * j);
        const double w = 0.5 + 0.5 * cos(3.141592653589793 * j / (H + 1));
        h[(size_t)(j + H)] = s * w;
        sum += s * w;
    }
    std::vector<double> y(x.size(), 0.0);
    for (size_t i = 0; i < x.size(); ++i) {
        double v = 0.0;
        for (int j = -H; j <= H; ++j) {
            const long k = (long)i - j;
            if (k >= 0 && k < (long)x.size()) v += h[(size_t)(j + H)] * x[(size_t)k];
        }
        y[i] = v / sum;
    }
    return y;
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

//! The foobar2000 wrapper's warm start, driven as dsp_declick drives it:
//! interleaved chunks into a WarmHold over its Channels, and after every push
//! it makes, everything ready pulled - into `out`, one vector per channel,
//! reserved up front so that collecting it allocates nothing.
struct HoldRig {
    std::vector<std::unique_ptr<Channel> > chan;
    std::vector<Channel *> ptrs;
    declick::WarmHold<float> hold;
    std::vector<std::vector<double> > out;
    std::vector<double> scratch;
    bool overflow = false;

    HoldRig(const Config & cfg, unsigned channels, double rate, unsigned threads,
            size_t expect) {
        for (unsigned c = 0; c < channels; ++c) {
            chan.push_back(std::unique_ptr<Channel>(new Channel()));
            chan.back()->configure(cfg);
            ptrs.push_back(chan.back().get());
        }
        hold.configure(&ptrs[0], channels, cfg, rate, threads);
        out.resize(channels);
        for (unsigned c = 0; c < channels; ++c) out[c].reserve(expect);
        scratch.resize((size_t)cfg.maxBlock * 4);
    }

    //! The wrapper's Emitter.
    void operator()(size_t) {
        const size_t got = chan[0]->available();
        for (size_t c = 0; c < chan.size(); ++c) {
            std::vector<double> & o = out[c];
            const size_t at = o.size();
            if (at + got > o.capacity() || chan[c]->available() != got) {
                overflow = true;
                chan[c]->pull(&scratch[0], got < scratch.size() ? got : scratch.size(), 1);
                continue;
            }
            o.resize(at + got);     // within capacity: no allocation
            chan[c]->pull(&o[at], got, 1);
        }
    }

    //! `frames` of `x` (interleaved), in chunks of `chunk`, from frame `from`.
    void feed(const std::vector<float> & x, size_t from, size_t frames, size_t chunk) {
        const size_t ch = chan.size();
        for (size_t b = from; b < from + frames; b += chunk) {
            const size_t n = (from + frames - b < chunk) ? (from + frames - b) : chunk;
            hold.feed(&x[b * ch], n, *this);
        }
    }

    //! The wrapper's finishTrack(): settle the hold, then drain.
    void finish() {
        scoped_flush_denormals ftz;
        hold.settle(*this);
        for (size_t c = 0; c < chan.size(); ++c) chan[c]->drain();
        (*this)(0);
    }
};

//! Two channels that differ, interleaved, as float: what foobar2000 hands over.
std::vector<float> stereo(const std::vector<double> & l, const std::vector<double> & r) {
    std::vector<float> v(l.size() * 2);
    for (size_t i = 0; i < l.size(); ++i) {
        v[2 * i] = (float)l[i];
        v[2 * i + 1] = (float)r[i];
    }
    return v;
}

//! Channel `c` of `x` from `from`, `frames` of it, through `ch` in chunks of
//! 4096 with everything pulled after each, then drained.
std::vector<double> streamOut(Channel & ch, const std::vector<float> & x,
                              unsigned c, size_t from, size_t frames) {
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) mono[i] = x[2 * (from + i) + c];
    scoped_flush_denormals ftz;
    std::vector<double> out;
    std::vector<double> buf((size_t)ch.config().maxBlock * 2);
    for (size_t b = 0; b < frames; b += 4096) {
        const size_t n = (frames - b < 4096) ? (frames - b) : 4096;
        ch.push(&mono[b], n, 1);
        const size_t got = ch.available();
        ch.pull(&buf[0], got, 1);
        out.insert(out.end(), buf.begin(), buf.begin() + (ptrdiff_t)got);
    }
    ch.drain();
    const size_t got = ch.available();
    std::vector<double> tail(got);
    if (got > 0) ch.pull(&tail[0], got, 1);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

//! One channel's warm pass over `warm` frames of channel `c` from `warmAt`,
//! output held back, nothing pushed after it.
void warmOn(Channel & ch, const std::vector<float> & x, unsigned c, size_t warmAt, size_t warm) {
    std::vector<float> mono(warm);
    for (size_t i = 0; i < warm; ++i) mono[i] = x[2 * (warmAt + i) + c];
    scoped_flush_denormals ftz;
    ch.beginWarm();
    if (warm > 0) ch.push(&mono[0], warm, 1);
    ch.endWarm();
}

//! The warm start as the core documents it, one channel straight through: the
//! `warm` frames from `warmAt` with the output held back, then `frames` of the
//! stream from `from`, then drained. What the wrappers must reproduce to the
//! bit: foobar2000's with warmAt == from, VirtualDJ's restarts with warmAt 0.
std::vector<double> warmReferenceAt(const Config & cfg, const std::vector<float> & x,
                                    unsigned c, size_t warmAt, size_t from,
                                    size_t warm, size_t frames) {
    Channel ch;
    ch.configure(cfg);
    if (warm > 0) warmOn(ch, x, c, warmAt, warm);
    return streamOut(ch, x, c, from, frames);
}

std::vector<double> warmReference(const Config & cfg, const std::vector<float> & x,
                                  unsigned c, size_t from, size_t warm, size_t frames) {
    return warmReferenceAt(cfg, x, c, from, from, warm, frames);
}

//! Requires `got` to be `want` sample for sample, and as long.
void requireSame(const char * what, const std::vector<double> & got,
                 const std::vector<double> & want) {
    size_t diff = 0, first = 0;
    const bool lenOk = got.size() == want.size();
    for (size_t i = 0; lenOk && i < got.size(); ++i) {
        if (got[i] != want[i]) { if (diff++ == 0) first = i; }
    }
    const bool ok = lenOk && diff == 0;
    if (!lenOk) {
        printf("  %-50s %-4s %zu samples, wanted %zu\n", what, "FAIL", got.size(), want.size());
    } else if (ok) {
        printf("  %-50s %-4s %zu samples, bit-identical\n", what, "ok", got.size());
    } else {
        printf("  %-50s %-4s %zu samples differ, the first at %zu\n", what, "FAIL", diff, first);
    }
    if (!ok) ++g_failures;
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

    // The flank repair only runs on a band-limited transfer under the key, and
    // the signal above is neither band-limited nor quiet enough above 13 kHz:
    // its pad stays 0. Low-passed at 9 kHz, the edge falls under flankEdge0
    // after the estimate's first 3 s, and the clicks hold the key open.
    {
        const std::vector<double> lp = lowpassed(sig, rate, 9000.0);
        ch.configure(cfg);
        ch.prime();
        arm();
        perSample(ch, lp, 0, 6 * R);
        requireQuiet("band-limited at 9 kHz: the flank repair");
        const bool ran = ch.flankSpans() > 0;
        printf("  %-50s %-4s %llu spans, edge %.0f Hz, pad %d\n", "... and it ran",
               ran ? "ok" : "FAIL", (unsigned long long)ch.flankSpans(),
               ch.bandEdge(), ch.flankPadNow());
        if (!ran) ++g_failures;
    }

    // The micro stage runs on the fine stage's output under the same key, which
    // the clicks above hold open; it must cut frames, and allocate nothing.
    {
        ch.configure(cfg);
        ch.prime();
        arm();
        perSample(ch, sig, 0, 6 * R);
        requireQuiet("the micro stage, under the body key");
        const bool ran = ch.microFrames() > 0;
        printf("  %-50s %-4s %llu frames cut\n", "... and it ran",
               ran ? "ok" : "FAIL", (unsigned long long)ch.microFrames());
        if (!ran) ++g_failures;
    }

    // The mid stage cuts in the fine stage's frames, under its key, where a
    // frame holds a run repaired at midPeak sigma or more - the clicks above
    // are; it must cut frames, and allocate nothing. Those clicks come every
    // 700 samples exactly, as regular as a voice's pulses, and its guard
    // backs off on them as it would on a voice; so here the guard is held off.
    {
        Config cm = cfg;
        cm.midPeriodic0 = 2.0;
        cm.midPeriodic1 = 3.0;
        ch.configure(cm);
        ch.prime();
        arm();
        perSample(ch, sig, 0, 6 * R);
        requireQuiet("the mid stage, under the body key");
        const bool ran = ch.midFrames() > 0;
        printf("  %-50s %-4s %llu frames cut\n", "... and it ran",
               ran ? "ok" : "FAIL", (unsigned long long)ch.midFrames());
        if (!ran) ++g_failures;
    }

    // The carpet key opens the fine stage and the flank repair where the body
    // key is shut. Here the body key is held shut and the carpet key's ramp
    // brought down to the clicks above, about 63 a second, so that it has to
    // open on its own - from carpetFirst on, by the rate so far.
    {
        Config cc = cfg;
        cc.bodyRate0 = 1e9;
        cc.bodyRate1 = 2e9;
        cc.carpetRate0 = 20.0;
        cc.carpetRate1 = 40.0;
        const std::vector<double> lp = lowpassed(sig, rate, 9000.0);
        ch.configure(cc);
        ch.prime();
        arm();
        perSample(ch, lp, 0, 6 * R);
        requireQuiet("the carpet key, with the body key held shut");
        const bool ran = ch.carpetOpenBlocks() > 0 && ch.flankSpans() > 0;
        printf("  %-50s %-4s open %llu blocks, key %.2f, %llu flank spans\n", "... and it opened",
               ran ? "ok" : "FAIL", (unsigned long long)ch.carpetOpenBlocks(), ch.carpetKey(),
               (unsigned long long)ch.flankSpans());
        if (!ran) ++g_failures;
        ch.configure(cfg);
    }

    // The crackle floor stage, on by default (so in every case above), runs on
    // the output under the key the fine stage follows, which the clicks above
    // hold open. It holds its buffers only while it is on, so turning it on or
    // off is a configure() - outside the counted region, like the rest of this
    // test's setup - and it must cut frames and allocate nothing.
    // Then with its top shelf, which follows the floor further up and keeps a
    // ring of its ratios.
    for (int shelf = 0; shelf <= 1; ++shelf) {
        Config cn;
        cn.crackleFloor = 1;
        cn.crackleFloorShelf = shelf;
        cn.compute(Params::defaults(), rate);
        ch.configure(cn);
        ch.prime();
        arm();
        perSample(ch, sig, 0, 6 * R);
        requireQuiet(shelf ? "the crackle floor stage with its shelf" : "the crackle floor stage, under the body key");
        const bool ran = ch.crackleFloorFrames() > 0;
        printf("  %-50s %-4s %llu frames cut\n", "... and it ran",
               ran ? "ok" : "FAIL", (unsigned long long)ch.crackleFloorFrames());
        if (!ran) ++g_failures;
        ch.configure(cfg);
    }

    // Where the key shuts after the stream's first two seconds the stage's
    // floor history goes on, a value every few hops standing for four, and
    // after it opens again the floor is kept under the one it shut with: the
    // window lets several values go at once, and a second floor is kept.
    // Clicks for 4 s, none for 6 s, clicks again: the key must shut and open
    // again, the stage cut after it, and none of it allocate.
    {
        const std::vector<double> gated = signal(R * 15, rate, 4 * R, 10 * R);
        Config cn;
        cn.crackleFloor = 1;
        cn.compute(Params::defaults(), rate);
        ch.configure(cn);
        ch.prime();
        arm();
        // Shut is a tenth of a second the stage cuts nothing in.
        bool shut = false, reopened = false;
        uint64_t cutsShut = 0, cuts = 0;
        for (int s0 = 0; s0 < 15 * R; s0 += R / 10) {
            perSample(ch, gated, s0, s0 + R / 10);
            const uint64_t now = ch.crackleFloorFrames();
            if (!shut && s0 >= 4 * R && s0 < 10 * R && now == cuts) {
                shut = true;
                cutsShut = now;
            }
            if (shut && s0 >= 10 * R && now > cuts) reopened = true;
            cuts = now;
        }
        requireQuiet("the crackle floor stage, key shut and open again");
        const bool ran = shut && reopened && ch.crackleFloorFrames() > cutsShut;
        printf("  %-50s %-4s shut %s, open again %s, %llu frames cut after\n", "... and it ran",
               ran ? "ok" : "FAIL", shut ? "yes" : "no", reopened ? "yes" : "no",
               (unsigned long long)(ch.crackleFloorFrames() - cutsShut));
        if (!ran) ++g_failures;
        ch.configure(cfg);
    }

    // The warm start (Channel::beginWarm()): a host that reads ahead runs the
    // coming Config::warmSeconds() through with the output held back, then the
    // stream from its start, the way the wrapper feeds it. Nothing may come out
    // of the warm pass, and neither it nor the stream after it may allocate:
    // with the crackle floor stage off, where the pass stops at the repair, and
    // on, where it runs on to the floor's history and the stage then cuts
    // under the warm pass's floor.
    for (int floorOn = 0; floorOn <= 1; ++floorOn) {
        const std::vector<double> s8 = signal(R * 8, rate);
        Config cw;
        cw.crackleFloor = floorOn;
        cw.compute(Params::defaults(), rate);
        ch.configure(cw);
        const int ahead = (int)(cw.warmSeconds() * rate);
        arm();
        ch.beginWarm();
        size_t held = 0;
        for (int b = 0; b + 4096 <= ahead; b += 4096) {
            ch.push(&s8[(size_t)b], 4096, 1);
            held += ch.available();
        }
        ch.endWarm();
        held += ch.available();
        for (int b = 0; b + 4096 <= 6 * R; b += 4096) {
            ch.push(&s8[(size_t)b], 4096, 1);
            const size_t got = ch.available();
            ch.pull(&sink[0], got < sink.size() ? got : sink.size(), 1);
        }
        requireQuiet(floorOn ? "warm start, the crackle floor stage on" : "warm start, the crackle floor stage off");
        const bool ran = held == 0 && ch.seenSamples() < (uint64_t)(6 * R)
                      && (!floorOn || ch.crackleFloorFrames() > 0);
        printf("  %-50s %-4s %zu samples out of the warm pass, %llu frames cut\n", "... and it held back",
               ran ? "ok" : "FAIL", held, (unsigned long long)ch.crackleFloorFrames());
        if (!ran) ++g_failures;
        ch.configure(cfg);
    }

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
        // pipeline it is about to be told the real shape of. An object is ~5 kB:
        // the carpet key's two rings of kCarpetMax seconds and every stage's
        // vector handles; a pipeline would be hundreds.
        requireUnder("constructing two Channels brings no buffers", 12u * 1024u);

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

    // --- the wrapper's warm start ------------------------------------------
    //
    // dsp_declick holds a track's first Config::warmSeconds() in a WarmHold,
    // runs the warm pass over them on a thread per channel, then hands them to
    // the core as the stream, twice what comes in per chunk. All of that is
    // inside on_chunk(), on the audio thread: the ring and the threads come
    // from configure(), at a format change, and nothing after may allocate.
    // And what comes out must be the warm start's to the bit - one channel
    // warmed, then run straight through - however the chunks fall, and on
    // whichever thread a channel warmed.
    {
        printf("\n");
        const size_t T = (size_t)R * 16;
        const std::vector<float> x = stereo(signal((int)T, rate), signal((int)T, rate, 2 * R, 5 * R));
        const size_t W = (size_t)(cfg.warmSeconds() * rate);
        const size_t expect = T + (size_t)cfg.latency + (size_t)R;
        std::vector<double> ref[2];
        for (unsigned c = 0; c < 2; ++c) ref[c] = warmReference(cfg, x, c, 0, W, T);

        // The chunk lengths foobar2000's decoders hand over - FLAC's and MP3's
        // - and one past three times maxBlock, which the hold has to cut up.
        const size_t chunks[3] = { 4096, 1152, (size_t)cfg.maxBlock * 3 + 5 };
        const unsigned threads[3] = { 2, 1, 2 };
        for (int k = 0; k < 3; ++k) {
            HoldRig rig(cfg, 2, rate, threads[k], expect);
            rig.hold.arm();
            arm();
            {
                scoped_flush_denormals ftz;
                rig.feed(x, 0, T, chunks[k]);
            }
            rig.finish();
            char what[80];
            snprintf(what, sizeof what, "warm hold: chunks of %zu, %u thread(s)", chunks[k], rig.hold.threads());
            requireQuiet(what);
            const bool ran = rig.hold.warmPasses() == 1 && rig.hold.lastWarmFrames() == W
                          && rig.hold.pending() == 0 && !rig.overflow
                          && rig.hold.threads() == threads[k];
            printf("  %-50s %-4s %llu warm pass of %zu frames, %.0f ms\n", "... and it warmed once",
                   ran ? "ok" : "FAIL", (unsigned long long)rig.hold.warmPasses(),
                   rig.hold.lastWarmFrames(), rig.hold.lastWarmMs());
            if (!ran) ++g_failures;
            requireSame("... left channel against the core's warm start", rig.out[0], ref[0]);
            requireSame("... right channel", rig.out[1], ref[1]);
        }

        // A track shorter than the warm length ends while it is still held:
        // the wrapper's finishTrack() warms on what there is and pushes it all.
        {
            const size_t S = (size_t)R * 3;
            HoldRig rig(cfg, 2, rate, 2, expect);
            rig.hold.arm();
            arm();
            {
                scoped_flush_denormals ftz;
                rig.feed(x, 0, S, 4096);
            }
            const bool held = rig.hold.holding() && rig.hold.pending() == S && rig.out[0].empty();
            rig.finish();
            requireQuiet("warm hold: a 3 s track, settled while held");
            printf("  %-50s %-4s %zu frames held, nothing out until the end\n", "... and it held",
                   held ? "ok" : "FAIL", S);
            if (!held) ++g_failures;
            requireSame("... left channel against the core's warm start",
                        rig.out[0], warmReference(cfg, x, 0, 0, S, S));
            requireSame("... right channel", rig.out[1], warmReference(cfg, x, 1, 0, S, S));
        }

        // A seek while the warm start is still handing back: the wrapper's
        // flush() resets the channels and holds again from the new place.
        {
            const size_t at = (size_t)R * 4, S = (size_t)R * 11;
            HoldRig rig(cfg, 2, rate, 2, expect);
            rig.hold.arm();
            arm();
            {
                scoped_flush_denormals ftz;
                rig.feed(x, 0, (size_t)R * 9, 4096);
            }
            const bool replaying = !rig.hold.holding() && rig.hold.pending() > 0;
            for (size_t c = 0; c < 2; ++c) { rig.chan[c]->reset(); rig.out[c].clear(); }
            rig.hold.arm();
            {
                scoped_flush_denormals ftz;
                rig.feed(x, at, S, 4096);
            }
            rig.finish();
            requireQuiet("warm hold: a seek while handing back");
            printf("  %-50s %-4s %llu warm passes\n", "... and it warmed again",
                   (replaying && rig.hold.warmPasses() == 2) ? "ok" : "FAIL",
                   (unsigned long long)rig.hold.warmPasses());
            if (!(replaying && rig.hold.warmPasses() == 2)) ++g_failures;
            requireSame("... left channel against the core's warm start",
                        rig.out[0], warmReference(cfg, x, 0, at, W, S));
            requireSame("... right channel", rig.out[1], warmReference(cfg, x, 1, at, W, S));
        }

        // Off: the hold passes straight through, and the output is the cold
        // stream's - what the component did before the warm start.
        {
            HoldRig rig(cfg, 2, rate, 2, expect);
            rig.hold.disarm();
            arm();
            {
                scoped_flush_denormals ftz;
                rig.feed(x, 0, T, 4096);
            }
            rig.finish();
            requireQuiet("warm hold off: straight through");
            requireSame("... left channel against the cold stream",
                        rig.out[0], warmReference(cfg, x, 0, 0, 0, T));
            requireSame("... right channel", rig.out[1], warmReference(cfg, x, 1, 0, 0, T));
        }

        // --- the warm start on another channel: Channel::adoptWarm() -------
        //
        // VirtualDJ's wrapper warms a channel of its own on a worker thread
        // and starts the stream on its live one at every restart, at the top
        // of the record or anywhere in it. The live channel has been playing
        // something else entirely; once it has adopted, the stream must come
        // out as the warmed channel's own would, to the bit, and the adoption
        // must not allocate.
        printf("\n");
        Channel warmed[2];
        for (unsigned c = 0; c < 2; ++c) {
            warmed[c].configure(cfg);
            warmOn(warmed[c], x, c, 0, W);
        }
        const size_t at = (size_t)R * 9, S = (size_t)R * 6;
        for (int where = 0; where < 2; ++where) {
            const size_t from = where ? at : 0;
            for (unsigned c = 0; c < 2; ++c) {
                Channel live;
                live.configure(cfg);
                streamOut(live, x, 1 - c, (size_t)R * 2, (size_t)R * 3);   // something else
                arm();
                const bool took = live.adoptWarm(warmed[c]);
                requireQuiet(where ? "adoptWarm(), then the stream from 9 s"
                                   : "adoptWarm(), then the stream from the top");
                if (!took) { printf("  FAIL  adoptWarm() refused a finished warm pass\n"); ++g_failures; }
                requireSame(c ? "... right channel" : "... left channel against the core's warm start",
                            streamOut(live, x, c, from, S), warmReferenceAt(cfg, x, c, 0, from, W, S));
            }
        }
        // The cold restart it replaces, so that the comparison above is one a
        // warm start can fail: at 9 s the cold stream is not the warm one.
        {
            Channel cold;
            cold.configure(cfg);
            const std::vector<double> a = streamOut(cold, x, 0, at, S);
            const std::vector<double> b = warmReferenceAt(cfg, x, 0, 0, at, W, S);
            size_t diff = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff += (a[i] != b[i]);
            printf("  %-50s %-4s %zu samples differ from a cold start\n", "... and it is a warm start",
                   diff > 0 ? "ok" : "FAIL", diff);
            if (diff == 0) ++g_failures;
        }
        // What it refuses, leaving the channel reset: a channel that has taken
        // audio since its warm pass, one in the middle of it, and one of
        // another shape. Each must then stream as a cold start would.
        {
            Channel used, mid, other, live;
            used.configure(cfg);
            warmOn(used, x, 0, 0, W);
            streamOut(used, x, 0, 0, 4096);
            mid.configure(cfg);
            mid.beginWarm();
            other.configure(c64);
            warmOn(other, x, 0, 0, W);
            live.configure(cfg);
            const Channel * bad[3] = { &used, &mid, &other };
            const char * what[3] = { "adoptWarm() refuses a channel that has streamed since",
                                     "... one still in its warm pass",
                                     "... one of another shape" };
            const std::vector<double> cold = warmReferenceAt(cfg, x, 0, 0, 0, 0, S);
            for (int k = 0; k < 3; ++k) {
                streamOut(live, x, 1, (size_t)R * 2, (size_t)R);
                const bool took = live.adoptWarm(*bad[k]);
                const std::vector<double> got = streamOut(live, x, 0, 0, S);
                const bool ok = !took && got == cold;
                printf("  %-50s %-4s %s\n", what[k], ok ? "ok" : "FAIL",
                       took ? "took it" : (got == cold ? "and starts cold" : "but does not start cold"));
                if (!ok) ++g_failures;
            }
        }
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
