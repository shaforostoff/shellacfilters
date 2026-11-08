/* ========================================
 *  dehum_vdj_verify
 *
 *  Same shape as declick_vdj_verify, plus the one thing Dehum has that Declick
 *  does not: a scout. See dehum_vdj_scout.h - it is the reason the buffer
 *  plug-in is worth having for this core, which has no latency and therefore
 *  nothing to gain from readahead as such.
 *
 *  What is pinned here:
 *
 *  1. That BufferPipeline hands back exactly what the core produces running
 *     straight through the song, at any block size. Dehum has no lookahead, so
 *     this is a weaker claim than Declick's - but the cache and the position
 *     arithmetic are the same code, and this is the cheaper place to catch a
 *     mistake in them.
 *
 *  2. That the scout finds the line, at the frequency it is actually at, and
 *     that it spends the budget it says it spends.
 *
 *  3. The slider mappings, against dehum::Params::defaults() and the VST2 port.
 *
 *  Not tested here: the detector and the notch. dehum_verify in
 *  ../../foobar2000_dsp/tests does that, and this binary compiles the identical
 *  core.
 * ======================================== */

#include "vdj_test_support.h"

#include "dehum_engine.h"

#include <algorithm>

int vdjtest::g_failures = 0;

using namespace vdjtest;

namespace {

const double kRate = 44100.0;

//! The core, straight through, in place - which is all this core needs, having
//! no latency. Written against dehum::Channel and nothing else.
std::vector<short> reference(const std::vector<short> & song, size_t frames,
                            const dehum::Params & p) {
    dehum::Config cfg;
    cfg.compute(p, kRate);

    dehum::Channel ch[2];
    ch[0].configure(cfg);
    ch[1].configure(cfg);

    const size_t songFrames = song.size() / vdj::kChannels;
    std::vector<short>  out(frames * vdj::kChannels, 0);
    std::vector<double> work(512 * vdj::kChannels, 0.0);

    dehum::scoped_flush_denormals ftz;
    for (size_t pos = 0; pos < frames; ) {
        const size_t n = std::min<size_t>(512, frames - pos);
        for (size_t f = 0; f < n; ++f) {
            const size_t s = pos + f;
            for (int c = 0; c < 2; ++c) {
                work[f * 2 + c] = (s < songFrames)
                    ? (double)song[s * 2 + c] * (1.0 / 32768.0) : 0.0;
            }
        }
        ch[0].process(work.data() + 0, n, 2);
        ch[1].process(work.data() + 1, n, 2);
        vdj::toShorts(work.data(), &out[pos * vdj::kChannels], n);
        pos += n;
    }
    return out;
}

//! Level at one frequency over a window, by Goertzel, in dB relative to full
//! scale. How the harness sees whether a line was actually removed.
double toneDb(const std::vector<short> & pcm, double hz, size_t from, size_t frames) {
    const size_t total = pcm.size() / vdj::kChannels;
    if (from + frames > total) return -999.0;
    const double w = 2.0 * 3.14159265358979 * hz / kRate;
    const double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t f = 0; f < frames; ++f) {
        const double x = (double)pcm[(from + f) * 2] * (1.0 / 32768.0);
        const double s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    const double amp = 2.0 * sqrt(power > 0.0 ? power : 0.0) / (double)frames;
    return 20.0 * log10(amp > 1e-12 ? amp : 1e-12);
}

// ---------------------------------------------------------------------------

void testSliders() {
    vdj::DehumEngine engine;
    StubCallbacks cb;
    StubHost host(cb);
    engine.declareParameters(host);

    check(cb.declared.size() == 6, "six sliders declared");
    bool allSliders = true;
    for (size_t i = 0; i < cb.declared.size(); ++i) {
        if (cb.declared[i].type != VDJPARAM_SLIDER) allSliders = false;
        if (cb.declared[i].id != (int)i + 1) allSliders = false;
    }
    check(allSliders, "all six are sliders with ids 1..6; Dry/Wet's 7 is retired");

    const dehum::Params want = dehum::Params::defaults();
    const dehum::Params got = engine.params();
    check(got == want, "default slider positions map to Params::defaults()");
    check(got.dryWet == 1.0f, "always fully wet");
    // The two that are not the round number they look like, called out because
    // they are the ones a careless edit silently moves.
    checkf(got.bandwidth == want.bandwidth, "bandwidth %.4f Hz, expected %.4f",
           (double)got.bandwidth, (double)want.bandwidth);
    checkf(got.rumbleHz == want.rumbleHz, "rumbleHz %.4f Hz, expected %.4f",
           (double)got.rumbleHz, (double)want.rumbleHz);
    checkf(got.searchTo == want.searchTo, "searchTo %.4f Hz, expected %.4f",
           (double)got.searchTo, (double)want.searchTo);

    bool strings = true;
    for (int id = 1; id <= 6; ++id) {
        char text[64] = { 0 };
        if (!engine.parameterString(id, text, (int)sizeof(text))) strings = false;
        if (text[0] == 0) strings = false;
    }
    check(strings, "every slider has a display string");

    // Frequency and Rumble read as words at the bottom of their travel, not as
    // 0 Hz, because that is what the off position means.
    char text[64] = { 0 };
    engine.parameterString(vdj::DehumEngine::kFrequency, text, (int)sizeof(text));
    check(strcmp(text, "auto") == 0, "Frequency reads 'auto' at the bottom");
}

void testPipelineMatchesCore() {
    const size_t frames = (size_t)(8.0 * kRate);
    std::vector<short> song = music(frames + (size_t)kRate, kRate);
    addTone(song, kRate, 50.0, 0.05);
    MemorySong src(song);

    vdj::DehumEngine engine;
    const std::vector<short> want = reference(song, frames, engine.params());

    const int blocks[] = { 64, 512, 1000, 4096, 8192 };   // 8192 exceeds kSliceFrames
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); ++b) {
        vdj::DehumEngine eng;
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

void testJumps() {
    const size_t frames = (size_t)(12.0 * kRate);
    std::vector<short> song = music(frames, kRate, 31337);
    addTone(song, kRate, 50.0, 0.05);
    MemorySong src(song);

    vdj::DehumEngine engine;
    engine.setRate(kRate);
    vdj::BufferPipeline<vdj::DehumEngine> pipe;
    check(pipe.configure(engine, kRate), "pipeline configures");

    const int nb = 512;
    std::vector<short> first(nb * vdj::kChannels), again(nb * vdj::kChannels);

    for (int i = 0; i < 400; ++i) pipe.serve(engine, src, i * nb, nb);

    const int inside = 300 * nb;
    const short * p = pipe.serve(engine, src, inside, nb);
    memcpy(first.data(), p, first.size() * sizeof(short));
    pipe.serve(engine, src, inside + 4 * nb, nb);
    p = pipe.serve(engine, src, inside, nb);
    memcpy(again.data(), p, again.size() * sizeof(short));
    check(firstDifference(first, again) < 0,
          "a position inside the cache reads the same twice");

    const int elsewhere = (int)(10.0 * kRate);
    p = pipe.serve(engine, src, elsewhere, nb);
    check(!allZero(p, nb), "a restart returns audio, not silence");
}

//! The scout, driven directly. What it is for is finding the line before the
//! live detector could have, so what is checked is that it finds it at all, at
//! the right frequency, and inside the budget it advertises.
void testScout() {
    const size_t frames = (size_t)(20.0 * kRate);
    std::vector<short> song = music(frames, kRate, 2024);
    // 20 dB of hum at 41.3 Hz - a real frequency off one of the reference
    // transfers, and not a multiple of anything in music().
    addTone(song, kRate, 41.3, 0.1);
    MemorySong src(song);

    vdj::DehumScout scout;
    vdj::DehumEngine engine;                 // only to borrow its parameters
    check(scout.begin(engine.params(), kRate), "the scout configures");
    scout.newTrack(0.0f);
    check(scout.running(), "the scout starts on a track with automatic frequency");

    // The song is shorter than kScoutSeconds, so the reads run out and the
    // scout finishes with what it has - which is more than kScoutMinSeconds and
    // therefore worth reporting.
    const size_t served = 512;
    int calls = 0;
    while (scout.running() && calls < 100000) {
        scout.feed(src, served);
        ++calls;
    }
    check(!scout.running(), "the scout finishes");

    // Two things are pinned here.
    //
    // The early stop: the scout is allowed to give up as soon as it has a
    // confirmed line and has read kScoutMinSeconds, and on this fixture - a
    // prominent tone - it should. Reading the other fifty seconds would cost
    // whatever is upstream in the chain for nothing, which is half of the
    // stutter this and kRestartCooldownSec were written for.
    const double read = (double)scout.framesRead() / kRate;
    printf("        read %.1f s of the record before stopping\n", read);
    checkf(read >= vdj::kScoutMinSeconds - 0.2 && read < vdj::kScoutSeconds,
           "scout read %.1f s; it should stop early, after at least %.1f",
           read, vdj::kScoutMinSeconds);

    // And the budget: kScoutSpeedup times what was served, per call, so
    // scouting runs at a fixed multiple of playback whatever the block size.
    const int expected = (int)(read * kRate / (double)(served * vdj::kScoutSpeedup));
    checkf(calls >= expected && calls <= expected + 32,
           "scouted in %.0f calls, expected about %.0f", (double)calls, (double)expected);

    dehum::LineReport lines[dehum::kMaxLines];
    const int count = scout.take(lines, (int)dehum::kMaxLines);
    check(count > 0, "the scout reports at least one line");

    bool found = false;
    for (int i = 0; i < count; ++i) {
        printf("        line %d: %.3f Hz (%s)\n", i, lines[i].frequency,
               lines[i].viaCoherence ? "coherence" : "prominence");
        if (fabs(lines[i].frequency - 41.3) < 1.0) found = true;
    }
    check(found, "the line the scout found is the one that is there");

    check(scout.take(lines, (int)dehum::kMaxLines) == 0,
          "the result is handed over exactly once");
}

//! End to end: the buffer engine adopts what its scout found, so the hum is
//! gone from audio the live detector would still have been thinking about.
void testScoutedRemoval() {
    // Eleven seconds, which is just past kScoutMinSeconds: the reads run out
    // there, so the scout publishes after 11/kScoutSpeedup = 1.4 s of playing.
    // The measurement window sits after that and before the 3 s or so the
    // unaided detector needs on a line this prominent, which is what makes the
    // comparison below mean anything.
    const size_t frames = (size_t)(11.0 * kRate);
    const size_t from = (size_t)(1.6 * kRate);
    const size_t span = (size_t)(1.0 * kRate);
    std::vector<short> song = music(frames, kRate, 5150);
    addTone(song, kRate, 41.3, 0.1);
    MemorySong src(song);

    const double before = toneDb(song, 41.3, from, span);

    vdj::DehumEngine scouted;
    const std::vector<short> withScout = driveSequential(scouted, src, frames, 512, kRate);
    const double after = toneDb(withScout, 41.3, from, span);

    // The same audio through the bare core - reference() drives dehum::Channel
    // itself and never calls adopt(), so it is the detector working unaided.
    // That is the comparison that says the scout's wiring does something: at
    // 1.6-2.6 s the scout has published and the unaided detector has not
    // finished convincing itself, so the notch is already deep in one and still
    // coming up in the other. Measured at 23 dB apart, and the margin below is
    // well inside that.
    //
    // On a synthetic tone this prominent the unaided detector gets there soon
    // after, and by 3 s the two are identical. The gap the scout exists for is
    // the one on real transfers, where a line sitting in the rumble is only
    // reachable by the coherence route and takes 43 s - which no synthetic
    // fixture reproduces honestly, so it is not attempted here.
    const std::vector<short> without = reference(song, frames, scouted.params());
    const double plain = toneDb(without, 41.3, from, span);

    printf("        41.3 Hz over 1.6-2.6 s: %.1f dB in, %.1f dB scouted, %.1f dB unaided\n",
           before, after, plain);
    checkf(after < before - 12.0,
           "scouted removal only reached %.1f dB from %.1f dB", after, before);
    checkf(after < plain - 10.0,
           "scouted %.1f dB is no better than unaided %.1f dB - is adopt() wired up?",
           after, plain);
}

} // anonymous namespace

int main() {
    printf("dehum_vdj_verify\n");
    testSliders();
    testPipelineMatchesCore();
    testJumps();
    testScout();
    testScoutedRemoval();
    return finish("dehum_vdj_verify");
}
