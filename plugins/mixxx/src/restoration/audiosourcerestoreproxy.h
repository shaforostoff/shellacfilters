/* ========================================
 *  Mixxx restoration - the AudioSource that does it
 *
 *  Where the restoration is inserted, and the reason the whole port is shaped
 *  the way it is. Everything Mixxx-flavoured lives in this pair of files;
 *  restorationpipeline.h below it knows nothing about Mixxx, Qt or decoding, and
 *  is tested without any of them.
 *
 *  WHY HERE AND NOT IN AN EFFECT. A Mixxx effect is handed pInput and pOutput
 *  and a GroupFeatureState carrying beat length, beat fraction and gain. It is
 *  not told the track, the play position, or anything else that would let it
 *  look at audio it has not been given - which is the same position a VST, an
 *  Audio Unit and an LV2 plug-in are in. Declick under that constraint has to
 *  delay the signal by Config::latency to get the lookahead its repair needs,
 *  and 20 ms on a deck is 20 ms that deck is late against everything it is being
 *  mixed with.
 *
 *  mixxx::AudioSource is random access over the whole decoded track. Asked for
 *  [a, b), this reads [a, b + latency) from the decoder underneath and hands
 *  back audio aligned with what was asked for: the lookahead becomes an extra
 *  read instead of a delay. declick_mixxx_verify checks that claim the only way
 *  it can be checked - the served audio is bit-identical to the core run
 *  straight through the whole track, at every block size, in mono and stereo.
 *
 *  The same access is what lets Dehum scout the opening of the record faster
 *  than it plays, which takes hum acquisition from tens of seconds to about one.
 *
 *  WHERE IT IS WRAPPED, and why that is playback only. Three places in Mixxx
 *  open an AudioSource: CachingReaderWorker for playback, AnalyzerThread for
 *  waveforms and beats, and chromaprinter for fingerprinting. Only the first is
 *  wrapped, so the waveform still shows the record as it is and an analysis
 *  result never depends on what the restoration settings happened to be when it
 *  ran. See scripts/integrate.sh for the one line that does it.
 *
 *  THREADING. Every call here is on CachingReaderWorker's thread. That is not
 *  the audio callback, so a restart or a slice of scouting delays a chunk being
 *  read ahead rather than a deadline - but it is the thread a deck is waiting
 *  on, which is why the scout is budgeted rather than run flat out.
 * ======================================== */

#pragma once

#include "restoration/restorationpipeline.h"
#include "restoration/restorationsettings.h"
#include "sources/audiosourceproxy.h"

namespace mixxx {

class AudioSourceRestoreProxy : public AudioSourceProxy,
                                private restoration::Source {
  public:
    //! Wrap `pAudioSource` if there is anything to do, and hand it back
    //! untouched if there is not.
    //!
    //! Returning the inner source unwrapped is the point rather than an
    //! optimisation: with both filters off - which is every user who has not
    //! switched this on - the decode path is exactly what it was before this
    //! patch, with no extra virtual call and nothing allocated.
    static AudioSourcePointer wrap(
            AudioSourcePointer pAudioSource,
            const restoration::Settings& settings);

    explicit AudioSourceRestoreProxy(AudioSourcePointer pAudioSource);
    ~AudioSourceRestoreProxy() override = default;

    //! False if the cores could not be sized - a sample rate or channel count
    //! this cannot serve, or memory it could not get. The caller then uses the
    //! unwrapped source, so a track still plays.
    bool configure(const restoration::Settings& settings);

  protected:
    ReadableSampleFrames readSampleFramesClamped(
            const WritableSampleFrames& sampleFrames) override;

  private:
    //! restoration::Source - how the pipeline and the scout reach the decoder.
    //! Private because it is an implementation detail of this proxy: everything
    //! outside sees an AudioSource.
    int64_t read(int64_t startFrame, int64_t frames, float* out) override;

    restoration::Pipeline m_pipeline;
};

} // namespace mixxx
