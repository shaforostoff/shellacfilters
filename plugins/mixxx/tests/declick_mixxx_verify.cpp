/* ========================================
 *  declick_mixxx_verify - the readahead claim
 *
 *  The reason this port exists rather than a Mixxx effect or an LV2 plug-in is
 *  one sentence: because mixxx::AudioSource can be read anywhere, Declick's
 *  lookahead costs an extra read instead of 20 ms of delay. Everything here
 *  exists to make that sentence checkable.
 *
 *  The central test is testNoDelay: the audio the pipeline returns for a frame
 *  range must be bit-identical to the core run straight through the whole track,
 *  with output frame i being input frame i. If the pipeline had a delay, or lost
 *  a block at a chunk boundary, or double-counted its warm-up, that comparison
 *  fails - there is nowhere for an error of alignment to hide in it.
 *
 *  Nothing here re-tests the DSP. declick_verify in ../../foobar2000_dsp/tests
 *  does that against an independent reference, and this binary compiles the
 *  identical core.
 * ======================================== */

#include "restore_test_support.h"

#include <stdio.h>

using namespace restoretest;

int restoretest::g_failures = 0;

namespace {

const double kRate = 44100.0;
const int kChannels = 2;
//! Twelve seconds: long enough for several of Mixxx's 8192-frame chunks, the
//! noise estimate to settle, and a few dozen clicks.
const int64_t kFrames = (int64_t)(12.0 * kRate);

restoration::Settings declickOnly() {
    restoration::Settings s;
    s.declickEnabled = true;
    s.dehumEnabled = false;
    s.declick = declick::Params::defaults();
    return s;
}

//! Mixxx reads the track in CachingReaderChunk::kFrames chunks.
const int64_t kMixxxChunk = 8192;

// ---------------------------------------------------------------------------

//! THE claim: no delay, and the same samples the core produces run straight
//! through.
void testNoDelay(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    check(pipe.configure(declickOnly(), kRate, kChannels, src.frames()),
            "pipeline configures");

    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);
    const std::vector<float> want =
            declickReference(track, kChannels, kRate, declick::Params::defaults());

    const bool same = sameBits(got, want);
    check(same, "served audio is the core run straight through, to the bit");
    if (!same) {
        const int64_t at = firstDifference(got, want);
        printf("        first difference at sample %lld (frame %lld of %lld)\n",
                (long long)at,
                (long long)(at / kChannels),
                (long long)src.frames());
        printf("        rms difference %.3e\n", rmsDifference(got, want));
    }

    // And the delay really is zero, not merely consistent: the pipeline says so
    // and the audio agrees with the un-delayed reference above.
    check(pipe.latency() > 0, "the core does hold a lookahead worth paying for");
    printf("        core latency %d frames (%.1f ms), paid in reads\n",
            pipe.latency(),
            1000.0 * pipe.latency() / kRate);
}

//! Mixxx's chunk size is 8192 today. The pipeline must not care.
void testBlockSizeInvariance(const std::vector<float>& track) {
    const int64_t blocks[] = {kMixxxChunk, 4096, 1000, 512, 64};
    std::vector<float> first;

    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); ++b) {
        MemorySource src(track, kChannels);
        restoration::Pipeline pipe;
        pipe.configure(declickOnly(), kRate, kChannels, src.frames());
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

//! Off is off: not "almost the same", the decoder's own samples.
void testDisabledIsPassThrough(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    restoration::Settings off;  // both filters default to disabled
    check(pipe.configure(off, kRate, kChannels, src.frames()),
            "pipeline configures with everything off");

    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);
    check(sameBits(got, track), "disabled is bit-exact pass-through");
    check(src.framesRead() == src.frames(),
            "disabled reads the track exactly once");
}

//! What the readahead actually costs, which is the trade this port is making.
//! Sequential reading should cost one restart and about `latency` frames more
//! than the track is long.
void testSequentialReadCost(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(declickOnly(), kRate, kChannels, src.frames());
    serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);

    check(pipe.restarts() == 1, "sequential reading restarts the pipeline once");

    const int64_t extra = src.framesRead() - src.frames();
    printf("        read %lld frames for a %lld frame track (%+lld)\n",
            (long long)src.framesRead(),
            (long long)src.frames(),
            (long long)extra);
    // Everything past the end of the track is silence the pipeline makes itself,
    // so the decoder is never asked for more than the track holds; the overhead
    // is that it is asked for the last slice twice at most.
    check(extra <= (int64_t)restoration::kSliceFrames,
            "readahead costs no more than a slice of extra decoding");
    check(extra >= 0, "the whole track is read");
}

//! A seek. The pipeline has to restart, and the restart has to be deterministic:
//! the same jump twice must give the same audio, or a deck that loops a bar
//! would sound different every time round.
void testRestartDeterminism(const std::vector<float>& track) {
    const int64_t at = 5 * kMixxxChunk;
    const int64_t n = kMixxxChunk;

    std::vector<float> a((size_t)(n * kChannels), 0.0f);
    std::vector<float> b((size_t)(n * kChannels), 0.0f);

    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(declickOnly(), kRate, kChannels, src.frames());

    // Read somewhere else first, so each of these is a jump rather than a
    // continuation.
    std::vector<float> scratch((size_t)(n * kChannels), 0.0f);
    pipe.serve(src, 0, n, scratch.data());
    pipe.serve(src, at, n, a.data());
    pipe.serve(src, 0, n, scratch.data());
    pipe.serve(src, at, n, b.data());

    check(sameBits(a, b), "the same jump twice gives the same audio");
}

//! The warm-up. A chunk reached by jumping cannot be bit-identical to the same
//! chunk reached by playing into it - the core genuinely has less history - but
//! it must be close, and it must be a repair rather than the raw track. Both
//! halves are checked, because passing the second one alone would also describe
//! a pipeline that did nothing at all.
void testWarmupMakesAJumpUsable(const std::vector<float>& track) {
    const int64_t at = 5 * kMixxxChunk;
    const int64_t n = kMixxxChunk;
    const size_t from = (size_t)(at * kChannels);
    const size_t to = from + (size_t)(n * kChannels);

    MemorySource seqSrc(track, kChannels);
    restoration::Pipeline seq;
    seq.configure(declickOnly(), kRate, kChannels, seqSrc.frames());
    const std::vector<float> sequential =
            serveAll(seq, seqSrc, seqSrc.frames(), kChannels, kMixxxChunk);

    MemorySource jumpSrc(track, kChannels);
    restoration::Pipeline jump;
    jump.configure(declickOnly(), kRate, kChannels, jumpSrc.frames());
    std::vector<float> jumped((size_t)(n * kChannels), 0.0f);
    jump.serve(jumpSrc, at, n, jumped.data());

    // How far the jumped chunk is from the played-into one, against how far the
    // undeclicked track is from it. The second is the repair itself, so the
    // ratio says what fraction of the repair the warm-up recovers.
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < (size_t)(n * kChannels); ++i) {
        const double dj = (double)jumped[i] - (double)sequential[from + i];
        const double dr = (double)track[from + i] - (double)sequential[from + i];
        num += dj * dj;
        den += dr * dr;
    }
    const double ratio = (den > 0.0) ? sqrt(num / den) : 0.0;
    printf("        jumped chunk differs from played-into by %.1f%% of the repair\n",
            100.0 * ratio);
    check(den > 0.0, "the reference run repaired something in this chunk");
    check(ratio < 0.25,
            "a warmed-up restart recovers most of the repair it jumped into");
    (void)to;
}

//! The end of the track. Declick holds `latency` frames, so without the drain in
//! feedDeclick() the last 20 ms of every record would be missing.
void testTailIsServed(const std::vector<float>& track) {
    MemorySource src(track, kChannels);
    restoration::Pipeline pipe;
    pipe.configure(declickOnly(), kRate, kChannels, src.frames());

    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), kChannels, kMixxxChunk);

    // The last latency frames exist and are not silence.
    double energy = 0.0;
    const int64_t tail = pipe.latency();
    for (int64_t i = src.frames() - tail; i < src.frames(); ++i) {
        for (int c = 0; c < kChannels; ++c) {
            const double v = (double)got[(size_t)(i * kChannels + c)];
            energy += v * v;
        }
    }
    check(energy > 0.0, "the last `latency` frames of the track are served");
}

//! A mono transfer, which is what most shellac is. The cores are per channel and
//! the count comes from the decoder, so one is a case the pipeline has to handle
//! rather than a case it is lucky to survive.
void testMonoTrack() {
    const int channels = 1;
    const std::vector<float> mono = makeClicky(kFrames, channels, kRate);
    MemorySource src(mono, channels);
    restoration::Pipeline pipe;
    check(pipe.configure(declickOnly(), kRate, channels, src.frames()),
            "pipeline configures for a mono track");

    const std::vector<float> got =
            serveAll(pipe, src, src.frames(), channels, kMixxxChunk);
    const std::vector<float> want =
            declickReference(mono, channels, kRate, declick::Params::defaults());
    check(sameBits(got, want), "mono is the core run straight through, to the bit");
}

} // namespace

int main() {
    printf("declick_mixxx_verify\n");
    const std::vector<float> track = makeClicky(kFrames, kChannels, kRate);

    testNoDelay(track);
    testBlockSizeInvariance(track);
    testDisabledIsPassThrough(track);
    testSequentialReadCost(track);
    testRestartDeterminism(track);
    testWarmupMakesAJumpUsable(track);
    testTailIsServed(track);
    testMonoTrack();

    return finish("declick_mixxx_verify");
}
