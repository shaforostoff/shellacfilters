#include "restoration/audiosourcerestoreproxy.h"

#include "util/logger.h"
#include "util/sample.h"

namespace {
const mixxx::Logger kLogger("AudioSourceRestoreProxy");
} // namespace

namespace mixxx {

// static
AudioSourcePointer AudioSourceRestoreProxy::wrap(
        AudioSourcePointer pAudioSource,
        const restoration::Settings& settings) {
    if (!pAudioSource || !settings.anyEnabled()) {
        return pAudioSource;
    }
    auto pProxy = std::make_shared<AudioSourceRestoreProxy>(pAudioSource);
    if (!pProxy->configure(settings)) {
        kLogger.warning()
                << "Could not size the restoration pipeline; playing the track "
                   "unrestored";
        return pAudioSource;
    }
    return pProxy;
}

AudioSourceRestoreProxy::AudioSourceRestoreProxy(AudioSourcePointer pAudioSource)
        : AudioSourceProxy(std::move(pAudioSource)) {
}

bool AudioSourceRestoreProxy::configure(const restoration::Settings& settings) {
    // Sized here, at track load, on the reader's own thread: one declick and one
    // dehum Channel per audio channel plus their staging buffers. Nothing after
    // this allocates, because every buffer either core holds follows from the
    // sample rate alone.
    return m_pipeline.configure(settings,
            getSignalInfo().getSampleRate(),
            getSignalInfo().getChannelCount(),
            frameIndexRange().length());
}

ReadableSampleFrames AudioSourceRestoreProxy::readSampleFramesClamped(
        const WritableSampleFrames& sampleFrames) {
    const auto range = sampleFrames.frameIndexRange();
    if (range.empty()) {
        return ReadableSampleFrames(range);
    }

    CSAMPLE* pOutput = sampleFrames.writableData();
    VERIFY_OR_DEBUG_ASSERT(pOutput) {
        return ReadableSampleFrames(IndexRange());
    }

    // The pipeline reads the track through read() below, which is this same
    // proxy's own private Source - so a request for [a, b) can pull [a, b +
    // latency) out of the decoder without the caller knowing or waiting.
    const SINT produced = static_cast<SINT>(
            m_pipeline.serve(*this, range.start(), range.length(), pOutput));

    const auto producedRange = IndexRange::forward(range.start(), produced);
    return ReadableSampleFrames(producedRange,
            SampleBuffer::ReadableSlice(pOutput,
                    getSignalInfo().frames2samples(produced)));
}

int64_t AudioSourceRestoreProxy::read(int64_t startFrame, int64_t frames, float* out) {
    const auto wanted = intersect(
            IndexRange::forward(static_cast<SINT>(startFrame), static_cast<SINT>(frames)),
            frameIndexRange());
    if (wanted.empty()) {
        return 0;
    }

    const auto readable = readSampleFramesClampedOn(*m_pAudioSource,
            WritableSampleFrames(wanted,
                    SampleBuffer::WritableSlice(out,
                            getSignalInfo().frames2samples(wanted.length()))));

    const SINT got = readable.frameIndexRange().length();
    if (got <= 0) {
        return 0;
    }
    // "The returned buffer is just a view/slice of the provided writable buffer
    // if the result is not empty", says IAudioSourceReader - so normally this is
    // already `out` and the copy does not happen. It is checked rather than
    // assumed because a source that returns its own buffer would otherwise leave
    // the pipeline processing whatever was in `out` last time.
    if (readable.readableData() != out) {
        SampleUtil::copy(out,
                readable.readableData(),
                getSignalInfo().frames2samples(got));
    }
    return got;
}

} // namespace mixxx
