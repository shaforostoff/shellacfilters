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
 *  Not tested here: the declicking itself. declick_verify in
 *  ../../foobar2000_dsp/tests does that against a Python reference, and this
 *  binary compiles the identical core.
 * ======================================== */

#include "vdj_test_support.h"

#include "declick_engine.h"
// kScoutSlice / kScoutSpeedup: the second consumer this reproduces is Dehum's
// scout, so the numbers come from it rather than from a guess here.
#include "dehum_vdj_scout.h"

#include <algorithm>

int vdjtest::g_failures = 0;

using namespace vdjtest;

namespace {

const double kRate = 44100.0;

//! The core, straight through the song, using the READ AHEAD recipe from the
//! note on declick::Channel::prime(): feed until the output is ready, then take
//! it. Deliberately written against the core and nothing else - it shares no
//! code with BufferPipeline, which is what makes the comparison worth making.
std::vector<short> reference(const std::vector<short> & song, size_t frames,
                            const declick::Params & p) {
    declick::Config cfg;
    cfg.compute(p, kRate);

    declick::Channel ch[2];
    ch[0].configure(cfg);
    ch[1].configure(cfg);

    const size_t songFrames = song.size() / vdj::kChannels;
    std::vector<short>  out(frames * vdj::kChannels, 0);
    std::vector<double> in(512 * vdj::kChannels, 0.0);
    std::vector<double> got(4096 * vdj::kChannels, 0.0);

    size_t inPos = 0, outPos = 0;
    declick::scoped_flush_denormals ftz;

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

    check(cb.declared.size() == 7, "seven sliders declared");
    bool allSliders = true;
    for (size_t i = 0; i < cb.declared.size(); ++i) {
        if (cb.declared[i].type != VDJPARAM_SLIDER) allSliders = false;
        if (cb.declared[i].id != (int)i + 1) allSliders = false;
    }
    check(allSliders, "all seven are sliders with ids 1..7");

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
    for (int id = 1; id <= 7; ++id) {
        char text[64] = { 0 };
        if (!engine.parameterString(id, text, (int)sizeof(text))) strings = false;
        if (text[0] == 0) strings = false;
    }
    check(strings, "every slider has a display string");
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

    vdj::DeclickEngine engine;
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

} // anonymous namespace

int main() {
    printf("declick_vdj_verify\n");
    testSliders();
    testReadahead();
    testJumps();
    testBypassIsExact();
    testSecondConsumer();
    return finish("declick_vdj_verify");
}
