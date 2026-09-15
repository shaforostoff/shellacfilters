/* ========================================
 *  Mixxx restoration - where the audio comes from
 *
 *  The one thing this port needs that a plug-in format cannot give it: random
 *  access to the whole decoded track. Both users of it - the readahead in
 *  restorationpipeline.h and the scout in dehumscout.h - want to read somewhere
 *  other than where playback is, which is the entire reason the restoration sits
 *  on mixxx::AudioSource rather than in an effect slot.
 *
 *  It is an interface rather than the AudioSource itself so that the test
 *  harness can drive the pipeline from a generated signal, with no Mixxx, no
 *  decoder and no file. audiosourcerestoreproxy.cpp has the one implementation
 *  that reads a real track.
 * ======================================== */

#pragma once

#include <stdint.h>

namespace restoration {

class Source {
  public:
    virtual ~Source() = default;

    //! Read [startFrame, startFrame + frames) into `out`, interleaved, the
    //! track's own channel count.
    //!
    //! Returns the frames actually written, which is fewer than asked for only
    //! at the end of the track; 0 is not an error. The caller substitutes
    //! silence for the rest, which is also how Declick's tail gets flushed.
    virtual int64_t read(int64_t startFrame, int64_t frames, float* out) = 0;
};

} // namespace restoration
