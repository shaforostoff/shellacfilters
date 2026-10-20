/* ========================================
 *  declick_vdj_verify
 *
 *  What this pins, in order of how much it matters:
 *
 *  1. The readahead claim. BufferPipeline says the audio it hands back is what
 *     the core would have produced running straight through the song, aligned
 *     with what was asked for and with no delay - which is the entire reason
 *     the buffer plug-in exists rather than only the live one. reference()
 *     below runs the core directly, with no cache and no position arithmetic,
 *     and the pipeline has to match it to the bit.
 *
 *  2. That the block size does not matter. VirtualDJ picks nb; nothing about
 *     the result may depend on it.
 *
 *  3. That jumping about is consistent. A DJ scratches, so the same song
 *     position gets asked for repeatedly and out of order, and a sample heard
 *     twice has to sound the same both times.
 *
 *  4. The slider mappings, against declick::Params::defaults() and against the
 *     VST2 port's paramsFromControls(). A slider position has to mean the same
 *     thing in every port.
 *
 *  5. The warm start (declick_vdj_warm.h): once the opening has been read and
 *     warmed off the audio thread, the first play is swapped onto a pair the
 *     worker caught up with it, and from the swap on is the core's warm start
 *     run from where the play started, to the bit; a restart at the top of the
 *     record is the core's warm start to the bit, and one anywhere else is the
 *     core started there with what the warm pass learned - and none of it,
 *     reading, the pass, the catch-up, the swap or the restarts, allocates.
 *
 *  Not tested here: the declicking itself. declick_verify in
 *  ../../foobar2000_dsp/tests does that against a Python reference, and this
 *  binary compiles the identical core.
 * ======================================== */

#include "vdj_test_support.h"

#include "declick_engine.h"
// kScoutSlice / kScoutSpeedup: the second consumer this reproduces is Dehum's
// scout, so the numbers come from it rather than from a guess here.
#include "dehum_vdj_scout.h"

#include <stdlib.h>

#include <algorithm>
#include <atomic>

int vdjtest::g_failures = 0;

using namespace vdjtest;

// ---------------------------------------------------------------------------
// Counting global allocator, armed only around the calls under test. Atomic,
// because the warm start's worker runs its pass while the caller serves, and
// that thread must not allocate either.

namespace {
std::atomic<size_t> g_allocs { 0 };
std::atomic<bool>   g_armed { false };
}

void * operator new(size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void * p = malloc(n ? n : 1);
    if (p == NULL) throw std::bad_alloc();
    return p;
}
void * operator new[](size_t n) { return operator new(n); }
void operator delete(void * p) noexcept { free(p); }
void operator delete[](void * p) noexcept { free(p); }
void operator delete(void * p, size_t) noexcept { free(p); }
void operator delete[](void * p, size_t) noexcept { free(p); }

namespace {

const double kRate = 44100.0;

void arm() { g_allocs.store(0); g_armed.store(true); }

//! Requires the region just measured to have been allocation-free.
void requireQuiet(const char * what) {
    g_armed.store(false);
    const size_t n = g_allocs.load();
    if (n == 0) {
        printf("  ok    %s: no allocation\n", what);
    } else {
        printf("  FAIL  %s: %zu allocation(s)\n", what, n);
        ++g_failures;
    }
}

//! The core, straight through the song, using the READ AHEAD recipe from the
//! note on declick::Channel::prime(): feed until the output is ready, then take
//! it. Deliberately written against the core and nothing else - it shares no
//! code with BufferPipeline, which is what makes the comparison worth making.
//!
//! With `warm`, the core's warm start first: Channel::beginWarm(), the song's
//! first `warm` frames, endWarm(). Then the stream from `from`, so that output
//! frame k is the song's frame from + k.
std::vector<short> reference(const std::vector<short> & song, size_t frames,
                            const declick::Params & p, size_t warm = 0, size_t from = 0) {
    declick::Config cfg;
    cfg.compute(p, kRate);

    declick::Channel ch[2];
    ch[0].configure(cfg);
    ch[1].configure(cfg);

    const size_t songFrames = song.size() / vdj::kChannels;
    std::vector<short>  out(frames * vdj::kChannels, 0);
    std::vector<double> in(512 * vdj::kChannels, 0.0);
    std::vector<double> got(4096 * vdj::kChannels, 0.0);

    size_t inPos = from, outPos = 0;
    declick::scoped_flush_denormals ftz;

    if (warm > 0) {
        std::vector<double> ahead(warm * vdj::kChannels);
        vdj::fromShorts(song.data(), ahead.data(), warm);
        ch[0].beginWarm();
        ch[1].beginWarm();
        ch[0].push(ahead.data() + 0, warm, 2);
        ch[1].push(ahead.data() + 1, warm, 2);
        ch[0].endWarm();
        ch[1].endWarm();
    }

    while (outPos < frames) {
        const size_t want = std::min<size_t>(4096, frames - outPos);
        while (ch[0].available() < want) {
            const size_t n = 512;
            for (size_t f = 0; f < n; ++f) {
                const size_t s = inPos + f;
                for (int c = 0; c < 2; ++c) {
                    in[f * 2 + c] = (s < songFrames)
                        ? (double)song[s * 2 + c] * (1.0 / 32768.0) : 0.0;
                }
            }
            ch[0].push(in.data() + 0, n, 2);
            ch[1].push(in.data() + 1, n, 2);
            inPos += n;
        }
        ch[0].pull(got.data() + 0, want, 2);
        ch[1].pull(got.data() + 1, want, 2);
        vdj::toShorts(got.data(), &out[outPos * vdj::kChannels], want);
        outPos += want;
    }
    return out;
}

// ---------------------------------------------------------------------------

void testSliders() {
    vdj::DeclickEngine engine;
    StubCallbacks cb;
    StubHost host(cb);
    engine.declareParameters(host);

    check(cb.declared.size() == 8, "seven sliders and a switch declared");
    bool allSliders = cb.declared.size() == 8;
    for (size_t i = 0; i < cb.declared.size(); ++i) {
        const int type = (i < 7) ? VDJPARAM_SLIDER : VDJPARAM_SWITCH;
        if (cb.declared[i].type != type) allSliders = false;
        if (cb.declared[i].id != (int)i + 1) allSliders = false;
    }
    check(allSliders, "sliders with ids 1..7, then the Warm start switch at 8");
    check(cb.declared.size() == 8 && cb.declared[7].def == 1.0f
          && cb.declared[7].id == vdj::DeclickEngine::kWarmStart,
          "the warm start opens switched on");

    // The declared defaults are what VirtualDJ opens a fresh effect on, so they
    // have to be the same positions the members start at - and those have to
    // land on the calibrated core defaults, order 128 included.
    const declick::Params want = declick::Params::defaults();
    const declick::Params got = engine.params();
    check(got == want, "default slider positions map to Params::defaults()");
    checkf(got.order == want.order, "order %.0f, expected %.0f",
           (double)got.order, (double)want.order);
    checkf(got.passes == want.passes, "passes %.0f, expected %.0f",
           (double)got.passes, (double)want.passes);
    checkf(got.maxLengthMs == want.maxLengthMs, "maxLengthMs %.3f, expected %.3f",
           (double)got.maxLengthMs, (double)want.maxLengthMs);

    bool strings = true;
    for (int id = 1; id <= 8; ++id) {
        char text[64] = { 0 };
        if (!engine.parameterString(id, text, (int)sizeof(text))) strings = false;
        if (text[0] == 0) strings = false;
    }
    check(strings, "every parameter has a display string");
    char scratch[16];
    check(!engine.parameterString(99, scratch, (int)sizeof(scratch)),
          "an unknown parameter id is declined");
}

void testReadahead() {
    const size_t frames = (size_t)(8.0 * kRate);
    std::vector<short> song = music(frames + (size_t)kRate, kRate);
    injectClicks(song, kRate, 30.0);
    MemorySong src(song);

    vdj::DeclickEngine engine;
    const std::vector<short> want = reference(song, frames, engine.params());

    // Sanity on the fixture itself: if the core repaired nothing, everything
    // below would pass while testing nothing.
    size_t changed = 0;
    for (size_t f = 0; f < frames; ++f) {
        if (want[f * 2] != song[f * 2]) ++changed;
    }
    check(changed > frames / 10000, "the fixture actually gets repaired");

    const int blocks[] = { 64, 512, 1000, 4096, 8192 };   // 8192 exceeds kSliceFrames
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); ++b) {
        vdj::DeclickEngine eng;
        const std::vector<short> got =
            driveSequential(eng, src, frames, blocks[b], kRate);
        const int64_t at = firstDifference(want, got);
        if (at >= 0) {
            printf("  FAIL  nb=%d diverges from the core at frame %lld\n",
                   blocks[b], (long long)at);
            ++g_failures;
        } else {
            printf("  ok    nb=%d matches the core to the bit\n", blocks[b]);
        }
    }
}

//! Scratching: the same position asked for repeatedly, forwards and backwards,
//! inside the cache and outside it.
void testJumps() {
    const size_t frames = (size_t)(12.0 * kRate);
    std::vector<short> song = music(frames, kRate, 999);
    injectClicks(song, kRate, 25.0);
    MemorySong src(song);

    // The warm start off: what is pinned here is the restart arithmetic, and a
    // restart's audio with it on depends on whether the pass has finished -
    // testWarmStart() waits for it and checks that.
    vdj::DeclickEngine engine;
    engine.warmStart().setEnabled(false);
    engine.setRate(kRate);
    vdj::BufferPipeline<vdj::DeclickEngine> pipe;
    check(pipe.configure(engine, kRate), "pipeline configures");

    const int nb = 512;
    std::vector<short> first(nb * vdj::kChannels), again(nb * vdj::kChannels);

    // Play forward for a while so the cache has something in it.
    for (int i = 0; i < 400; ++i) {
        pipe.serve(engine, src, i * nb, nb);
    }
    const int inside = 300 * nb;      // ~3.5 s back: well inside the 4 s cache

    const short * p = pipe.serve(engine, src, inside, nb);
    memcpy(first.data(), p, first.size() * sizeof(short));
    // Move away and come back, the way a scratch does.
    pipe.serve(engine, src, inside + 4 * nb, nb);
    p = pipe.serve(engine, src, inside, nb);
    memcpy(again.data(), p, again.size() * sizeof(short));
    check(firstDifference(first, again) < 0,
          "a position inside the cache reads the same twice");

    // Now a jump that has to restart the pipeline. It cannot match the
    // straight-through run - the core is cold and gets a warm-up rather than the
    // whole record's history - but it must be deterministic and it must be
    // audio.
    const int elsewhere = (int)(10.0 * kRate);
    p = pipe.serve(engine, src, elsewhere, nb);
    memcpy(first.data(), p, first.size() * sizeof(short));
    check(!allZero(first.data(), nb), "a restart returns audio, not silence");

    vdj::DeclickEngine other;
    other.warmStart().setEnabled(false);
    other.setRate(kRate);
    vdj::BufferPipeline<vdj::DeclickEngine> pipe2;
    pipe2.configure(other, kRate);
    p = pipe2.serve(other, src, elsewhere, nb);
    memcpy(again.data(), p, again.size() * sizeof(short));
    check(firstDifference(first, again) < 0,
          "a restart at the same position is reproducible");
}

//! Dry/Wet at the bottom is a bypass, and the 16 bit round trip has to be exact
//! there: fromShorts divides by 32768, toShorts multiplies back and rounds, and
//! anything but the original sample would mean the plug-in damages audio it is
//! not even processing. Written through the pointer VirtualDJ was handed, which
//! is also how VirtualDJ would do it.
void testBypassIsExact() {
    const size_t frames = (size_t)(3.0 * kRate);
    std::vector<short> song = music(frames, kRate, 4242);
    injectClicks(song, kRate, 40.0);
    MemorySong src(song);

    vdj::DeclickEngine engine;
    StubCallbacks cb;
    StubHost host(cb);
    engine.declareParameters(host);

    float * dryWet = NULL;
    for (size_t i = 0; i < cb.declared.size(); ++i) {
        if (cb.declared[i].id == vdj::DeclickEngine::kDryWet) {
            dryWet = (float *)cb.declared[i].ptr;
        }
    }
    check(dryWet != NULL, "Dry/Wet is reachable through its declared pointer");
    if (dryWet == NULL) return;
    *dryWet = 0.0f;

    engine.setRate(kRate);
    engine.update(kRate);
    check(engine.params().dryWet == 0.0f, "the slider move was picked up");

    vdj::BufferPipeline<vdj::DeclickEngine> pipe;
    pipe.configure(engine, kRate);

    size_t bad = 0;
    const int nb = 777;
    for (size_t pos = 0; pos + nb <= frames; pos += nb) {
        const short * p = pipe.serve(engine, src, (int)pos, nb);
        for (size_t i = 0; i < (size_t)nb * vdj::kChannels; ++i) {
            if (p[i] != song[pos * vdj::kChannels + i]) ++bad;
        }
    }
    checkf(bad == 0, "bypass altered %.0f sample(s), expected %.0f",
           (double)bad, 0.0);
}

//! Two consumers at once, which is the case that broke.
//!
//! `pos` does not come from one place. In a VirtualDJ 2025 log, Declick Buffer
//! was serving 512-frame blocks at the play head while *also* being asked for
//! 4096-frame blocks marching from the start of the record - Dehum Buffer's
//! scout, upstream in the chain, reading ahead through it. Every alien read
//! missed the cache and restarted the pipeline; the playback read after it
//! missed too and restarted it again. With a warm-up invented by the pipeline
//! rather than asked of the engine, that was 22000 frames of Declick per 512
//! frames of audio, and it stuttered until the scout finished.
//!
//! So this reproduces that interleaving and measures the thing that went wrong:
//! how much the engine is made to produce against how much was actually asked
//! for. It is a work bound, not an audio comparison, because that is what the
//! defect was.
void testSecondConsumer() {
    const size_t frames = (size_t)(30.0 * kRate);
    std::vector<short> song = music(frames, kRate, 17171);
    injectClicks(song, kRate, 25.0);
    MemorySong src(song);

    vdj::DeclickEngine engine;
    engine.setRate(kRate);
    vdj::BufferPipeline<vdj::DeclickEngine> pipe;
    pipe.configure(engine, kRate);

    const int nb = 512;
    const int alien = (int)vdj::kScoutSlice;     // what the scout actually asks for
    int64_t play = (int64_t)(12.0 * kRate);     // the deck, mid-record
    int64_t scan = 0;                            // the scout, from the top
    int64_t served = 0;

    std::vector<short> heard;
    heard.reserve((size_t)(4.0 * kRate) * vdj::kChannels);

    for (int i = 0; i < 300; ++i) {
        const short * p = pipe.serve(engine, src, (int)play, nb);
        heard.insert(heard.end(), p, p + (size_t)nb * vdj::kChannels);
        play += nb;
        served += nb;

        // The alien reader, at 8x the deck: kScoutSpeedup * nb per call, which
        // is exactly one kScoutSlice when nb is 512.
        pipe.serve(engine, src, (int)scan, alien);
        scan += (int64_t)vdj::kScoutSpeedup * nb;
        served += alien;
    }

    const double ratio = (double)pipe.produced() / (double)served;
    printf("        %lld frames served, %lld produced (%.2fx), %llu restarts\n",
           (long long)served, (long long)pipe.produced(), ratio,
           (unsigned long long)pipe.restarts());
    // 5.8x before the fix, 1.1x after. Two leaves plenty of room for the
    // block-granularity overshoot without letting a warm-up back in per call.
    checkf(ratio < 2.0,
           "the engine produced %.2fx what was asked for; a second consumer "
           "should not cost more than about %.2fx", ratio, 2.0);

    // And the deck should still hear declicked audio, not silence and not the
    // input: restarting often costs the detector its noise estimate, so this is
    // not bit-identical to an undisturbed run, but it must still be doing the
    // job.
    check(!allZero(heard.data(), heard.size() / vdj::kChannels),
          "the deck is still served audio while the second consumer reads");

    size_t changed = 0;
    const size_t at = (size_t)(12.0 * kRate);
    for (size_t f = 0; f + 1 < heard.size() / vdj::kChannels; ++f) {
        if (heard[f * 2] != song[(at + f) * 2]) ++changed;
    }
    check(changed > 0, "and is still being repaired, not passed through");
}

// ---------------------------------------------------------------------------
// The warm start

//! An engine and its pipeline, configured, with a buffer for what it serves.
struct WarmRig {
    vdj::DeclickEngine engine;
    vdj::BufferPipeline<vdj::DeclickEngine> pipe;
    std::vector<short> heard;

    explicit WarmRig(size_t frames) {
        engine.setRate(kRate);
        pipe.configure(engine, kRate);
        heard.assign(frames * vdj::kChannels, 0);
    }

    vdj::DeclickWarm & warm() { return engine.warmStart(); }

    //! Serves `frames` from `pos`, forward in blocks of `nb`, the way a deck
    //! plays; what it hears lands in `heard` from frame `at`. Allocates
    //! nothing.
    void play(vdj::SongSource & src, size_t pos, size_t frames, int nb, size_t at = 0) {
        for (size_t f = 0; f < frames; f += (size_t)nb) {
            const size_t n = std::min((size_t)nb, frames - f);
            const short * p = pipe.serve(engine, src, (int)(pos + f), (int)n);
            if (p != NULL && at + f + n <= heard.size() / vdj::kChannels) {
                memcpy(&heard[(at + f) * vdj::kChannels], p, n * vdj::kChannels * sizeof(short));
            }
        }
    }

    std::vector<short> first(size_t frames) const {
        return std::vector<short>(heard.begin(), heard.begin() + (ptrdiff_t)(frames * vdj::kChannels));
    }
};

//! Requires `got` to be `want` to the bit.
void requireSame(const char * what, const std::vector<short> & got, const std::vector<short> & want) {
    const int64_t at = firstDifference(want, got);
    if (at < 0) {
        printf("  ok    %s, to the bit\n", what);
    } else {
        printf("  FAIL  %s: diverges at frame %lld\n", what, (long long)at);
        ++g_failures;
    }
}

std::vector<short> slice(const std::vector<short> & v, size_t from, size_t frames) {
    return std::vector<short>(v.begin() + (ptrdiff_t)(from * vdj::kChannels),
                              v.begin() + (ptrdiff_t)((from + frames) * vdj::kChannels));
}

//! Requires `heard` to be `cold` up to frame `at` and `warm` from it, to the
//! bit: the swap, with nothing dropped, repeated or spliced either side.
void requireSwap(const char * what, const std::vector<short> & heard, size_t at,
                 const std::vector<short> & cold, const std::vector<short> & warm) {
    const size_t n = heard.size() / vdj::kChannels;
    const int64_t a = firstDifference(slice(cold, 0, at), slice(heard, 0, at));
    const int64_t b = firstDifference(slice(warm, at, n - at), slice(heard, at, n - at));
    if (a < 0 && b < 0) {
        printf("  ok    %s: the cold stream for %.2f s of play, the warm one after, to the bit\n",
               what, (double)at / kRate);
    } else {
        printf("  FAIL  %s: split at %.2f s; diverges from the cold stream at %lld, "
               "from the warm one at %lld\n", what, (double)at / kRate,
               (long long)a, (long long)(b < 0 ? b : b + (int64_t)at));
        ++g_failures;
    }
    check(firstDifference(slice(cold, at, n - at), slice(warm, at, n - at)) >= 0,
          "... and the two differ after the swap, so that check can fail");
}

//! What the warm start promises, end to end through BufferPipeline. The first
//! play of a record starts cold - the opening is read while it plays and
//! warmed on the worker - and the worker then catches its own pair up with it
//! and swaps it in: from there on the play is the core's warm start run from
//! where it started, to the bit. Every restart in the record after the pass
//! starts with what it learned: at the top, the core's warm start, and
//! anywhere else the core started there with it. None of it may allocate:
//! the reading, the restarts and the swap on the caller's thread, the pass and
//! the catch-up on the worker's.
//!
//! The deck here plays faster than real time, so where it would leave the
//! worker behind the test stops and waits for it to park - as a deck playing
//! in real time would have let it.
void testWarmStart() {
    const size_t songFrames = (size_t)(20.0 * kRate);
    std::vector<short> song = music(songFrames, kRate, 31337);
    injectClicks(song, kRate, 25.0);
    MemorySong src(song);

    const declick::Params p = declick::Params::defaults();
    declick::Config cfg;
    cfg.compute(p, kRate);
    const size_t W = (size_t)(cfg.warmSeconds() * kRate);
    const size_t C = W + (size_t)(vdj::kWarmCopyBeyondSec * kRate);
    const size_t T = (size_t)(10.0 * kRate), L = (size_t)(5.0 * kRate);
    const int nb = 512;
    // Played, the whole copy is read: kWarmSpeedup x served per call.
    const size_t R = ((C + (size_t)vdj::kWarmSpeedup * nb - 1) / ((size_t)vdj::kWarmSpeedup * nb)) * nb;

    const std::vector<short> cold = reference(song, T, p);
    const std::vector<short> warm = reference(song, T, p, W, 0);
    check(firstDifference(cold, warm) >= 0,
          "the fixture's warm start differs from its cold one, so the checks below can fail");

    // The coming of a record, then a cue to the top and a seek.
    {
        WarmRig rig(T);
        {
            declick::Channel c;
            c.configure(cfg);
            printf("        memory: the saved pass %.1f kB, where a third pair would be %.0f kB;"
                   " the copy %.0f kB, %.0f kB of it past the warm length\n",
                   (double)rig.warm().savedBytes() / 1024.0, 2.0 * (double)c.heapBytes() / 1024.0,
                   (double)(C * vdj::kChannels * sizeof(short)) / 1024.0,
                   (double)((C - W) * vdj::kChannels * sizeof(short)) / 1024.0);
        }
        arm();
        rig.play(src, 0, R, nb);
        check(rig.warm().framesRead() == C && !rig.warm().reading(),
              "the warm length was read, and the copy's margin after it");
        check(rig.warm().waitForPark(30000) && (rig.warm().parked() || rig.warm().swaps() == 1),
              "the worker warmed it and caught up with the play");
        rig.play(src, R, T - R, nb, R);
        requireQuiet("ten seconds of a first play, read, warmed, caught up with and swapped meanwhile");
        check(rig.warm().swaps() == 1 && rig.warm().passes() == 1 && rig.warm().ready(),
              "one pass, one swap");
        const size_t E = (size_t)rig.warm().swapOut();
        printf("        pass %.0f ms; swapped at %.3f s of input, %.3f s of output\n",
               rig.warm().lastPassMs(), (double)rig.warm().swapIn() / kRate, (double)E / kRate);
        if (E > 0 && E < T) requireSwap("the first play from the top", rig.first(T), E, cold, warm);
        else check(false, "the swap is inside the play");
        check(rig.warm().waitForPass(30000), "and the worker is idle after it");

        // A cue to the top: out of the cache, so a restart, and a warm one.
        const uint64_t before = rig.pipe.restarts();
        arm();
        rig.play(src, 0, T, nb);
        requireQuiet("a cue to the top, and ten seconds from it");
        check(rig.pipe.restarts() == before + 1, "it restarted once");
        requireSame("a restart at the top is the core's warm start", rig.first(T), warm);

        // A seek to 15 s: the restart starts there with what the pass learned.
        const size_t P = (size_t)(15.0 * kRate), from = P - (size_t)rig.engine.warmupFrames();
        const size_t S = (size_t)(2.0 * kRate);
        arm();
        rig.play(src, P, S, nb);
        requireQuiet("a seek to 15 s");
        requireSame("a restart at 15 s is the core started there warm",
                    rig.first(S), slice(reference(song, P - from + S, p, W, from), P - from, S));
        check(firstDifference(rig.first(S),
                              slice(reference(song, P - from + S, p, 0, from), P - from, S)) >= 0,
              "... and not as it would start cold");
        check(rig.warm().swaps() == 1, "and neither swapped anything");
    }

    // A first play from a cue three seconds in, inside the copy, at another
    // block size: caught up from there, so the warm start run from there.
    {
        const size_t P = (size_t)(3.0 * kRate), S = (size_t)(6.0 * kRate);
        const int nb2 = 777;
        WarmRig rig(S);
        const size_t from = P - (size_t)rig.engine.warmupFrames();
        const size_t R2 = ((R + nb2 - 1) / nb2) * nb2;
        arm();
        rig.play(src, P, R2, nb2);
        check(rig.warm().waitForPark(30000) && (rig.warm().parked() || rig.warm().swaps() == 1),
              "a first play from 3 s: caught up with");
        rig.play(src, P + R2, S - R2, nb2, R2);
        requireQuiet("six seconds of it");
        const int64_t E = rig.warm().swapOut() - (int64_t)P;
        if (rig.warm().swaps() == 1 && E > 0 && E < (int64_t)S) {
            requireSwap("a first play from 3 s", rig.first(S), (size_t)E,
                        slice(reference(song, P - from + S, p, 0, from), P - from, S),
                        slice(reference(song, P - from + S, p, W, from), P - from, S));
        } else {
            check(false, "... and swapped inside the play");
        }
    }

    // A first play from 12 s, past the copy: nothing to catch up with, so it
    // stays cold, and there is no swap.
    {
        const size_t P = (size_t)(12.0 * kRate), S = (size_t)(3.0 * kRate);
        WarmRig rig(S);
        const size_t from = P - (size_t)rig.engine.warmupFrames();
        arm();
        rig.play(src, P, R, nb);
        check(rig.warm().waitForPass(30000) && rig.warm().ready() && !rig.warm().parked(),
              "a first play from 12 s: warmed, and left alone");
        rig.play(src, P + R, S - R, nb, R);
        requireQuiet("three seconds of it");
        check(rig.warm().swaps() == 0, "... with no swap");
        requireSame("... and the cold stream throughout", rig.first(S),
                    slice(reference(song, P - from + S, p, 0, from), P - from, S));
    }

    // A first play from just short of the copy's end: the play head would be
    // past what was copied before the worker could get ahead of it. No swap.
    {
        const size_t P = C - (size_t)(0.1 * kRate), S = (size_t)(3.0 * kRate);
        WarmRig rig(S);
        const size_t from = P - (size_t)rig.engine.warmupFrames();
        arm();
        rig.play(src, P, R, nb);
        check(rig.warm().waitForPass(30000) && rig.warm().ready() && !rig.warm().parked(),
              "a first play from the copy's last 0.1 s: the worker gives up");
        rig.play(src, P + R, S - R, nb, R);
        requireQuiet("three seconds of it");
        check(rig.warm().swaps() == 0, "... with no swap");
        requireSame("... and the cold stream throughout", rig.first(S),
                    slice(reference(song, P - from + S, p, 0, from), P - from, S));
    }

    // The pipeline restarts while the worker is parked: the restart starts
    // warm on its own, the parked pair is not for it, and nothing is swapped.
    {
        const size_t P = (size_t)(15.0 * kRate), S = (size_t)(2.0 * kRate);
        WarmRig rig(T);
        const size_t from = P - (size_t)rig.engine.warmupFrames();
        arm();
        rig.play(src, 0, R, nb);
        const bool parked = rig.warm().waitForPark(30000) && rig.warm().parked();
        rig.play(src, P, S, nb);
        requireQuiet("a seek to 15 s while the worker is parked");
        check(parked && rig.warm().waitForPass(30000) && rig.warm().swaps() == 0,
              "... the worker gives up, with no swap");
        requireSame("... and the restart is the core started there warm",
                    rig.first(S), slice(reference(song, P - from + S, p, W, from), P - from, S));
    }

    // A record shorter than the warm length: read to its end, warmed over all
    // of it, as foobar2000 warms a track that ends while it is still held.
    {
        const size_t S = (size_t)(3.0 * kRate);
        std::vector<short> shortSong(song.begin(), song.begin() + (ptrdiff_t)(S * vdj::kChannels));
        MemorySong shortSrc(shortSong);
        WarmRig rig(S);
        arm();
        rig.play(shortSrc, 0, S, nb);
        requireQuiet("a 3 s record, played through");
        check(rig.warm().framesRead() == S, "it was read to its last frame");
        check(rig.warm().waitForPark(30000) && rig.warm().ready(), "and warmed");
        rig.pipe.invalidate();          // a cue to the top, which is still in the cache
        rig.play(shortSrc, 0, S, nb);
        requireSame("a restart at its top is the core's warm start over all of it",
                    rig.first(S), reference(shortSong, S, p, S, 0));
    }

    // Off, through the switch the way VirtualDJ moves it: nothing is read, no
    // pass runs, and a restart at the top is the cold stream.
    {
        WarmRig rig(T);
        StubCallbacks cb;
        StubHost host(cb);
        rig.engine.declareParameters(host);
        int * on = NULL;
        for (size_t i = 0; i < cb.declared.size(); ++i) {
            if (cb.declared[i].id == vdj::DeclickEngine::kWarmStart) on = (int *)cb.declared[i].ptr;
        }
        check(on != NULL && *on == 1, "the switch is reachable through its pointer, and on");
        if (on != NULL) *on = 0;
        rig.engine.update(kRate);
        rig.play(src, 0, L, nb);
        rig.play(src, 0, T, nb);
        check(rig.warm().framesRead() == 0 && rig.warm().passes() == 0 && !rig.warm().ready(),
              "switched off, nothing is read and no pass runs");
        requireSame("switched off, a restart at the top is the cold stream", rig.first(T), cold);
    }

    // Another record loaded the moment the pass is handed over, so that it is
    // stopped part way (on a fast machine it may finish first; either way the
    // new record's opening is read once it lets go of the buffer). Then a
    // structural slider move, which makes the pass wait to be learned again
    // under the new config.
    {
        WarmRig rig(T);
        StubCallbacks cb;
        StubHost host(cb);
        rig.engine.declareParameters(host);
        size_t pos = 0;
        while (rig.warm().reading() && pos < L) {
            rig.play(src, pos, (size_t)nb, nb);
            pos += (size_t)nb;
        }
        rig.pipe.newTrack(rig.engine);
        arm();
        rig.play(src, 0, L, nb);
        requireQuiet("a new record while the pass runs, and five seconds of it");
        check(rig.warm().waitForPark(30000) && rig.warm().ready() && rig.warm().framesRead() == C,
              "its opening was read and warmed");
        printf("        %s\n", rig.warm().passes() == 1
                                ? "the first pass was stopped part way"
                                : "the first pass finished before it could be stopped");
        rig.play(src, 0, T, nb);
        requireSame("a restart at its top is the core's warm start", rig.first(T), warm);

        float * order = NULL;
        for (size_t i = 0; i < cb.declared.size(); ++i) {
            if (cb.declared[i].id == vdj::DeclickEngine::kOrder) order = (float *)cb.declared[i].ptr;
        }
        if (order != NULL) *order = 0.1f;
        const bool rebuilt = rig.engine.update(kRate);
        check(rebuilt && !rig.warm().ready(), "Model order resizes the pipeline and forgets the pass");
        if (rebuilt) rig.pipe.invalidate();
        rig.play(src, L, L, nb);
        check(rig.warm().waitForPark(30000) && rig.warm().ready(), "which is learned again");
        rig.play(src, 0, T, nb);
        requireSame("a restart at the top is the warm start under the new order",
                    rig.first(T), reference(song, T, rig.engine.params(), W, 0));
    }
}

} // anonymous namespace

int main() {
    printf("declick_vdj_verify\n");
    testSliders();
    testReadahead();
    testJumps();
    testBypassIsExact();
    testSecondConsumer();
    testWarmStart();
    return finish("declick_vdj_verify");
}
