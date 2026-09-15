/* ========================================
 *  dehum_mixxx_verify - the scout claim, and the zero-latency one
 *
 *  Dehum needs no lookahead: its detector reads the signal but does not sit in
 *  the path, so what goes in comes out aligned. What it wants from being able to
 *  read the whole track is time. Unaided, the detector needs about 9 s to
 *  confirm a line it can see outright and up to 43 s for one only the coherence
 *  route reaches, and all of it is record playing with the hum still in it.
 *
 *  The scout reads the opening faster than the deck plays it and hands what it
 *  finds to Channel::adopt(). testScoutCancelsTheLineSooner is the check that
 *  this is worth anything: the same fixture, with and without the scout, judged
 *  over the second between the scout publishing and the unaided detector
 *  catching up. Picking that window is most of the work - see the note there.
 *
 *  Nothing here re-tests the DSP. dehum_verify in ../../foobar2000_dsp/tests does
 *  that against an independent reference, and this binary compiles the identical
 *  core.
 * ======================================== */

#include "restore_test_support.h"

#include <stdio.h>

using namespace restoretest;

int restoretest::g_failures = 0;

namespace {

const double kRate = 44100.0;
const int kChannels = 2;
const double kHumHz = 50.0;
//! Thirty seconds. The scout wants ten before it will publish anything
//! (kScoutMinSeconds) and reads at kScoutSpeedup times the rate the caller is
//! served, so it has finished well inside this.
const int64_t kFrames = (int64_t)(30.0 * kRate);
const int64_t kMixxxChunk = 8192;

//! Dehum with the line pinned, which turns the search - and therefore the scout
//! - off. That is what makes a bit-exact comparison against the plain core
//! possible: with the scout running the pipeline adopts lines partway through and
//! the reference, which has no scout, never does.
restoration::Settings dehumPinned() {
    restoration::Settings s;
    s.dehumEnabled = true;
    s.dehum = dehum::Params::defaults();
    s.dehum.frequency = (float)kHumHz;
    return s;
}

//! Dehum searching for itself, which is how it is actually used.
restoration::Settings dehumAuto() {
    restoration::Settings s;
    s.dehumEnabled = true;
    s.dehum = dehum::Params::defaults();
    return s;
}

// ---------------------------------------------------------------------------

//! Zero latency, and the same samples the core produces run straight through.
void testZeroLatencyAlignment(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    check(pipe.configure(dehumPinned(), kRate, kChannels, src.frames()),
            "pipeline configures");
    check(pipe.latency() == 0, "dehum alone adds no latency");

    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);
    const std::vector<float> want =
            dehumReference(track, kChannels, kRate, dehumPinned().dehum);

    const bool same = sameBits(got, want);
    check(same, "served audio is the core run straight through, to the bit");
    if (!same) {
        const int64_t at = firstDifference(got, want);
        printf("        first difference at sample %lld, rms %.3e\n",
                (long long)at, rmsDifference(got, want));
    }
    check(src.framesRead() == src.frames(),
            "with no lookahead to pay for, the track is read exactly once");
}

void testBlockSizeInvariance(const std::vector<float>& track) {
    const int64_t blocks[] = {kMixxxChunk, 4096, 1000, 512, 64};
    std::vector<float> first;

    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); ++b) {
        MemorySource src(track, kChannels);
        restoration::Pipeline pipe;
        pipe.configure(dehumPinned(), kRate, kChannels, src.frames());
        const std::vector<float> got =
                serveAll(pipe, src, src.frames(), kChannels, blocks[b]);
        if (b == 0) {
            first = got;
            continue;
        }
        char what[96];
        snprintf(what, sizeof what,
                "block size %lld gives the same audio as %lld",
                (long long)blocks[b], (long long)kMixxxChunk);
        check(sameBits(got, first), what);
    }
}

void testDisabledIsPassThrough(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    restoration::Settings off;
    pipe.configure(off, kRate, kChannels, src.frames());
    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);
    check(sameBits(got, track), "disabled is bit-exact pass-through");
}

//! Wiring: the settings really do reach the core and the notch really does cut.
void testPinnedFrequencyIsCancelled(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(dehumPinned(), kRate, kChannels, src.frames());
    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);

    // Judged over the back half, by when a pinned notch has long converged.
    const int64_t from = src.frames() / 2;
    const double before =
            toneLevel(track, kChannels, 0, from, src.frames(), kRate, kHumHz);
    const double after =
            toneLevel(got, kChannels, 0, from, src.frames(), kRate, kHumHz);
    const double dB = 20.0 * log10((after > 1e-12 ? after : 1e-12) / before);
    printf("        pinned notch: %.1f dB at %.0f Hz\n", dB, kHumHz);
    check(dB < -20.0, "a pinned line is cancelled by more than 20 dB");
}

//! THE scout claim. Same fixture, same parameters, searching rather than
//! pinned: the pipeline reads the opening of the track early and adopts what it
//! finds, so the line is gone while the unaided detector is still convincing
//! itself.
//!
//! The fixture and the window are both chosen to make that a real comparison,
//! and the window is the fiddly half. The scout publishes once it has read
//! kScoutMinSeconds and has a line - here the track runs out at 11 s, just past
//! it, so the reads stop there and publication lands at 11/kScoutSpeedup = 1.4 s
//! of serving. The unaided detector gets to the same place a second or so later.
//! So the gap is real but narrow, and a window that starts too late measures two
//! runs that have both converged - which is exactly what an earlier version of
//! this test did, and it happily reported no difference.
//!
//! On a synthetic tone the unaided detector always catches up soon after. The
//! gap the scout exists for is the one on real transfers, where a line sitting
//! in the rumble is only reachable by the coherence route and takes 43 s - which
//! no synthetic fixture reproduces honestly, so it is not attempted here.
void testScoutCancelsTheLineSooner() {
    const double hz = 41.3;
    const int64_t frames = (int64_t)(11.0 * kRate);
    std::vector<float> track = makeMusic(frames, kChannels, kRate);
    addTone(track, kChannels, kRate, hz, 0.1);

    const int64_t from = (int64_t)(1.6 * kRate);
    const int64_t to = (int64_t)(2.6 * kRate);

    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(dehumAuto(), kRate, kChannels, src.frames());
    const std::vector<float> scouted =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);

    // The same core, same parameters, no scout: reference() drives
    // dehum::Channel itself and never calls adopt(), so it is the detector
    // working unaided - what a filter that could only see the audio as it played
    // would manage.
    const std::vector<float> unaided =
            dehumReference(track, kChannels, kRate, dehumAuto().dehum);

    const double raw = toneLevel(track, kChannels, 0, from, to, kRate, hz);
    const double withScout =
            toneLevel(scouted, kChannels, 0, from, to, kRate, hz);
    const double without =
            toneLevel(unaided, kChannels, 0, from, to, kRate, hz);

    const double dBScout = 20.0 * log10((withScout > 1e-12 ? withScout : 1e-12) / raw);
    const double dBPlain = 20.0 * log10((without > 1e-12 ? without : 1e-12) / raw);
    printf("        %.1f Hz over 1.6-2.6 s: scouted %.1f dB, unaided %.1f dB\n",
            hz, dBScout, dBPlain);

    check(!pipe.scoutRunning(), "the scout finishes inside the track");
    checkf(dBScout < -6.0,
            "scouted removal only reached %.1f dB (unaided %.1f)", dBScout, dBPlain);
    checkf(dBScout < dBPlain - 6.0,
            "scouted %.1f dB is no better than unaided %.1f dB - is adopt() wired up?",
            dBScout, dBPlain);
}

//! A pinned frequency turns the search off, so there is nothing to scout for and
//! the scout must not spend a decode finding it out.
void testScoutIdleWhenPinned(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(dehumPinned(), kRate, kChannels, src.frames());
    std::vector<float> out((size_t)(kMixxxChunk * kChannels), 0.0f);
    pipe.serve(src, 0, kMixxxChunk, out.data());
    check(!pipe.scoutRunning(), "the scout stays idle when a frequency is pinned");
    check(src.framesRead() <= kMixxxChunk + (int64_t)restoration::kSliceFrames,
            "an idle scout reads nothing");
}

//! A seek. Unlike Declick, Dehum keeps what it learned across one: the hum on
//! the far side of a seek is the same hum, and re-acquiring it every time the DJ
//! moves the play position would be worse than doing nothing.
void testSeekKeepsTheLines(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(dehumAuto(), kRate, kChannels, src.frames());

    // Play far enough in that the line is being cancelled.
    const int64_t settle = (int64_t)(12.0 * kRate);
    serveAll(pipe, src, settle, kChannels, kMixxxChunk);

    // Now jump to the back of the track and read a stretch of it.
    const int64_t at = (int64_t)(20.0 * kRate);
    const int64_t n = (int64_t)(4.0 * kRate);
    std::vector<float> after((size_t)(n * kChannels), 0.0f);
    int64_t done = 0;
    while (done < n) {
        int64_t want = kMixxxChunk;
        if (done + want > n) {
            want = n - done;
        }
        done += pipe.serve(src,
                at + done,
                want,
                &after[(size_t)(done * kChannels)]);
    }

    const double raw = toneLevel(track, kChannels, 0, at, at + n, kRate, kHumHz);
    const double got = toneLevel(after, kChannels, 0, 0, n, kRate, kHumHz);
    const double dB = 20.0 * log10((got > 1e-12 ? got : 1e-12) / raw);
    printf("        after a seek: %.1f dB at %.0f Hz\n", dB, kHumHz);
    check(dB < -12.0, "the notch keeps cancelling across a seek");
}

//! Both filters at once. The chain is Declick then Dehum, and it has to be
//! exactly that - not almost that - or the two cores are interacting in some way
//! neither was measured under.
void testChainedWithDeclick() {
    const int64_t frames = (int64_t)(12.0 * kRate);
    std::vector<float> track = makeClicky(frames, kChannels, kRate);
    // Put a line under the clicks so both filters have work to do.
    const std::vector<float> hum =
            makeHum(frames, kChannels, kRate, kHumHz, 0.25, 4242u);
    for (size_t i = 0; i < track.size(); ++i) {
        track[i] += hum[i];
    }

    restoration::Settings both = dehumPinned();
    both.declickEnabled = true;
    both.declick = declick::Params::defaults();

    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    check(pipe.configure(both, kRate, kChannels, src.frames()),
            "pipeline configures with both filters on");
    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);

    const std::vector<float> want = dehumReference(
            declickReference(track, kChannels, kRate, both.declick),
            kChannels,
            kRate,
            both.dehum);

    const bool same = sameBits(got, want);
    check(same, "both filters together are exactly Dehum after Declick");
    if (!same) {
        printf("        first difference at sample %lld, rms %.3e\n",
                (long long)firstDifference(got, want), rmsDifference(got, want));
    }
    check(pipe.latency() > 0, "the chain reports Declick's lookahead");
}

} // namespace

int main() {
    printf("dehum_mixxx_verify\n");
    const std::vector<float> track =
            makeHum(kFrames, kChannels, kRate, kHumHz);

    testZeroLatencyAlignment(track);
    testBlockSizeInvariance(track);
    testDisabledIsPassThrough(track);
    testPinnedFrequencyIsCancelled(track);
    testScoutCancelsTheLineSooner();
    testScoutIdleWhenPinned(track);
    testSeekKeepsTheLines(track);
    testChainedWithDeclick();

    return finish("dehum_mixxx_verify");
}
