/* ========================================
 *  Mixxx restoration - the sequential pipeline, with readahead
 *
 *  This is the part of the port that is not a copy of anything: the cores are
 *  sequential and Mixxx asks for the track by frame range, so something has to
 *  turn one into the other. It is the same problem the VirtualDJ buffer plug-ins
 *  solve in ../../../vdjplugin/common/vdj_buffer_dsp.h, and the same answer,
 *  simplified by two things Mixxx gives that VirtualDJ does not.
 *
 *  WHY THIS LAYER EXISTS AT ALL. Declick repairs a damaged sample from the audio
 *  on *both* sides of it, so the pipeline holds Config::latency samples - 880 at
 *  44.1 kHz with the defaults. A filter that is handed n samples and must return
 *  n can only do that by delaying the output, which on a deck means the deck is
 *  20 ms late against everything it is being mixed with. That is what a VST, an
 *  Audio Unit, an LV2 plug-in and a Mixxx builtin *effect* all have to accept:
 *  none of them is given the track, only the next buffer of it.
 *
 *  mixxx::AudioSource is given the track. It is random access over the whole
 *  decoded file, so the lookahead stops being a delay and becomes an extra read:
 *  asked for [a, b), this reads [a, b + latency) from the decoder and returns
 *  audio aligned with what was asked for. No delay at all. That is the whole
 *  argument for putting the restoration here rather than in an effect slot, and
 *  it is the same argument the foobar2000 components make - a player that can
 *  scan the file does not have to pay a declicker's latency.
 *
 *  WHAT MIXXX MAKES EASIER than VirtualDJ:
 *
 *    - There is already a cache above this one. CachingReader keeps decoded
 *      chunks and serves scratching, looping and small seeks out of them without
 *      coming back here, so this layer does not need the ring buffer addressed
 *      by song position that the VirtualDJ port needs. It needs to be correct
 *      when the requested range jumps, and fast when it does not.
 *
 *    - None of this is on the audio thread. readSampleFramesClamped() is called
 *      from CachingReaderWorker's own thread, so a restart costing a few
 *      milliseconds delays a chunk that was being read ahead, not a callback
 *      that was about to miss its deadline.
 *
 *  Requests still arrive out of order - a seek asks for a chunk somewhere else -
 *  and the cores are sequential, so a jump means starting them again. A restart
 *  rewinds warmupFrames() before the audio that was asked for and runs the cores
 *  over that first, so Declick's noise estimate is settled by the time anything
 *  it produced is heard. Sequential reading, which is what playback does, costs
 *  exactly one extra latency-sized read at the start of the track and nothing
 *  afterwards.
 *
 *  ORDER: Declick, then Dehum. Clicks are broadband impulses, and they land in
 *  every hop of Dehum's analysis window, lifting the noise floor it measures a
 *  line's prominence against - so declicking first makes the hum easier to find.
 *  The reverse is not true to any degree that matters: hum is a low tone, and a
 *  tone is exactly what Declick's AR model predicts well, so it barely shows up
 *  in the residual the detector thresholds.
 * ======================================== */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <new>
#include <vector>

#include "restoration/declick_core.h"
#include "restoration/dehum_core.h"
#include "restoration/dehumscout.h"
#include "restoration/restorationsource.h"
#include "restoration/restorationsettings.h"

namespace restoration {

//! Frames per pass of the inner loop. Independent of the chunk size Mixxx asks
//! in - CachingReaderChunk::kFrames is 8192 - so that the staging buffers are
//! sized by this file and not by a constant in the engine that could change.
enum { kSliceFrames = 4096 };

//! More than stereo only happens with stems. Nothing here assumes 2: the cores
//! are per-channel and the count comes from the decoder, which for a mono
//! shellac transfer may well hand over one.
enum { kMaxChannels = 8 };

class Pipeline {
  public:
    //! Allocates - one declick::Channel and one dehum::Channel per audio
    //! channel, plus the staging buffers. Called when a track is loaded, on
    //! CachingReaderWorker's thread, which is the same thread that will read.
    //!
    //! `length` is the track in frames, and it is not decoration: past the end
    //! the pipeline feeds its own zeros rather than asking the decoder for audio
    //! that is not there, which is how Declick's last `latency` frames get
    //! flushed out of it.
    bool configure(const Settings& settings,
            double sampleRate,
            int channels,
            int64_t length) {
        if (channels < 1 || channels > kMaxChannels) {
            return false;
        }
        if (!(sampleRate >= 1000.0)) {
            sampleRate = 44100.0;
        }

        m_settings = settings;
        m_settings.sanitize();
        m_rate = sampleRate;
        m_channels = channels;
        m_length = (length > 0) ? length : 0;
        m_configured = false;

        if (!m_settings.anyEnabled()) {
            // Nothing to do, and nothing allocated: serve() hands reads straight
            // through. Worth keeping free, because this is what every user who
            // has not switched the feature on is running.
            m_configured = true;
            return true;
        }

        try {
            if (m_settings.declickEnabled) {
                m_declickCfg.compute(m_settings.declick, sampleRate);
                m_declick.resize((size_t)channels);
                for (int c = 0; c < channels; ++c) {
                    m_declick[(size_t)c].configure(m_declickCfg);
                }
            } else {
                m_declick.clear();
            }

            if (m_settings.dehumEnabled) {
                m_dehumCfg.compute(m_settings.dehum, sampleRate);
                m_dehum.resize((size_t)channels);
                for (int c = 0; c < channels; ++c) {
                    m_dehum[(size_t)c].configure(m_dehumCfg);
                }
                m_scout.begin(m_settings.dehum, sampleRate);
            } else {
                m_dehum.clear();
            }

            const size_t staging = (size_t)kSliceFrames * (size_t)channels;
            m_read.assign(staging, 0.0f);
            m_work.assign(staging, 0.0f);
        } catch (const std::bad_alloc&) {
            m_declick.clear();
            m_dehum.clear();
            return false;
        }

        m_configured = true;
        newTrack();
        return true;
    }

    //! A new track: everything either core learned belongs to the last one.
    void newTrack() {
        for (auto& ch : m_declick) {
            ch.reset();
        }
        for (auto& ch : m_dehum) {
            ch.reset();
        }
        m_scout.newTrack(m_settings.dehum.frequency);
        m_outPos = -1;  // nothing is positioned: the next serve() restarts
        m_inPos = 0;
    }

    //! Frames the output is delayed by, which is what this layer exists to be
    //! zero: the readahead in produce() pays it in reads instead. Reported for
    //! the harness to check rather than for anyone to compensate.
    int latency() const {
        return m_settings.declickEnabled ? m_declickCfg.latency : 0;
    }

    //! Serve [startFrame, startFrame + frames) into `out`, interleaved.
    //! Returns the frames produced, which is short only at the end of the track.
    int64_t serve(Source& src, int64_t startFrame, int64_t frames, float* out) {
        if (frames <= 0) {
            return 0;
        }
        if (!m_configured || !m_settings.anyEnabled()) {
            return src.read(startFrame, frames, out);
        }

        if (startFrame != m_outPos) {
            restart(src, startFrame);
        }

        const int64_t produced = produce(src, frames, out);
        m_outPos += produced;

        // The scout is budgeted against what was just served, so it runs at a
        // fixed multiple of reading speed however big the chunks are. See
        // dehumscout.h.
        if (m_settings.dehumEnabled) {
            dehum::LineReport lines[dehum::kMaxLines];
            const int found = m_scout.take(lines, (int)dehum::kMaxLines);
            if (found > 0) {
                for (auto& ch : m_dehum) {
                    ch.adopt(lines, found);
                }
            }
            m_scout.feed(src, m_channels, (size_t)produced);
        }

        return produced;
    }

    // -- diagnostics, for the harness ----------------------------------------

    int64_t restarts() const {
        return m_restarts;
    }
    int64_t sourceFramesRead() const {
        return m_sourceFrames;
    }
    bool scoutRunning() const {
        return m_scout.running();
    }
    const Settings& settings() const {
        return m_settings;
    }

  private:
    //! Start the cores again, `warmup` frames before the audio that was asked
    //! for, and throw that warm-up away.
    //!
    //! Declick is reset outright: its window is the previous audio and its noise
    //! estimate was measured from it, so audio from somewhere else in the track
    //! is worth nothing to it. Dehum is only flushed - the analysis window is
    //! stale but the hum on the far side of a seek is the same hum, and
    //! re-acquiring it every time the DJ moves the play position would be worse
    //! than doing nothing.
    void restart(Source& src, int64_t at) {
        ++m_restarts;
        for (auto& ch : m_declick) {
            ch.reset();
        }
        for (auto& ch : m_dehum) {
            ch.flush();
        }

        const int64_t warm = warmupFrames();
        int64_t from = at - warm;
        if (from < 0) {
            from = 0;
        }

        m_inPos = from;
        m_outPos = from;
        if (at > from) {
            produce(src, at - from, nullptr);
        }
        m_outPos = at;
    }

    //! What a restart has to run before the audio anyone hears.
    //!
    //! Declick's figure is not a guess: every threshold in its detector is
    //! relative to a robust noise scale measured over madWindow samples - 30 ms,
    //! 1323 at 44.1 kHz - and cold, that scale starts at 1e-6 and the first block
    //! is judged against nothing. The model itself needs no warming, because
    //! fitModel() refits from the window every block.
    //!
    //! Dehum's is zero, and for a different reason from having no latency: what a
    //! seek invalidates there is the 1.5 s analysis window, which is far too much
    //! to run through for every seek and pointless besides - the lines survive a
    //! flush by design, so the notch keeps cancelling what it was already
    //! cancelling while the detector refills the window in its own time.
    int64_t warmupFrames() const {
        return m_settings.declickEnabled ? (int64_t)m_declickCfg.madWindow : 0;
    }

    //! Produce `count` frames, reading as much of the track as that needs.
    //! `out` may be null, which is how the warm-up is thrown away.
    int64_t produce(Source& src, int64_t count, float* out) {
        int64_t done = 0;

        while (done < count) {
            int64_t k = 0;

            if (m_settings.declickEnabled) {
                // Top up until the pipeline can give something back. This is the
                // readahead: m_inPos runs `latency` frames ahead of m_outPos,
                // which is exactly the delay not being paid.
                int guard = kFeedGuard;
                while (available() == 0 && guard-- > 0) {
                    if (!feedDeclick(src)) {
                        break;
                    }
                }
                k = (int64_t)available();
                if (k <= 0) {
                    break;  // past the end of the track, tail already flushed
                }
                if (k > count - done) {
                    k = count - done;
                }
                if (k > (int64_t)kSliceFrames) {
                    k = (int64_t)kSliceFrames;
                }
                for (int c = 0; c < m_channels; ++c) {
                    m_declick[(size_t)c].pull(
                            m_work.data() + c, (size_t)k, (size_t)m_channels);
                }
            } else {
                k = count - done;
                if (k > (int64_t)kSliceFrames) {
                    k = (int64_t)kSliceFrames;
                }
                k = readSource(src, m_inPos, k, m_work.data());
                if (k <= 0) {
                    break;
                }
                m_inPos += k;
            }

            if (m_settings.dehumEnabled) {
                dehum::scoped_flush_denormals ftz;
                for (int c = 0; c < m_channels; ++c) {
                    m_dehum[(size_t)c].process(
                            m_work.data() + c, (size_t)k, (size_t)m_channels);
                }
            }

            if (out) {
                const size_t samples = (size_t)k * (size_t)m_channels;
                const size_t at = (size_t)done * (size_t)m_channels;
                for (size_t i = 0; i < samples; ++i) {
                    out[at + i] = m_work[i];
                }
            }
            done += k;
        }

        return done;
    }

    size_t available() const {
        return m_declick.empty() ? 0 : m_declick[0].available();
    }

    //! One slice of track into Declick. False when there is nothing left to feed
    //! it, which is how produce() stops rather than spinning at the end.
    bool feedDeclick(Source& src) {
        // Zeros past the end of the track, so the last `latency` frames come out
        // of the pipeline instead of staying in it. Bounded, or a track whose
        // length we were told wrongly would feed zeros for ever.
        const int64_t limit = m_length + (int64_t)m_declickCfg.latency +
                (int64_t)kSliceFrames;
        if (m_inPos >= limit) {
            return false;
        }

        const int64_t want = (int64_t)kSliceFrames;
        const int64_t got = readSource(src, m_inPos, want, m_read.data());
        if (got <= 0) {
            return false;
        }

        {
            declick::scoped_flush_denormals ftz;
            for (int c = 0; c < m_channels; ++c) {
                m_declick[(size_t)c].push(
                        m_read.data() + c, (size_t)got, (size_t)m_channels);
            }
        }
        m_inPos += got;
        return true;
    }

    //! Read from the track, substituting silence past its end. Everything the
    //! decoder actually produced is counted, which is what the harness checks the
    //! readahead claim against.
    int64_t readSource(Source& src, int64_t at, int64_t frames, float* out) {
        if (frames <= 0) {
            return 0;
        }

        int64_t got = 0;
        if (at < m_length) {
            int64_t want = frames;
            if (at + want > m_length) {
                want = m_length - at;
            }
            got = src.read(at, want, out);
            if (got < 0) {
                got = 0;
            }
            m_sourceFrames += got;
        }

        if (got < frames) {
            // Past the end, or a decoder that came up short. Either way the rest
            // of this slice is silence - and with Declick on, that silence is the
            // drain that pushes its tail out.
            if (at >= m_length || got > 0 || at + frames > m_length) {
                const size_t from = (size_t)got * (size_t)m_channels;
                const size_t to = (size_t)frames * (size_t)m_channels;
                for (size_t i = from; i < to; ++i) {
                    out[i] = 0.0f;
                }
                return frames;
            }
            return got;
        }
        return frames;
    }

    //! How many times feedDeclick() may come back empty before produce() gives
    //! up. Only reachable at the very end of a track, where one more read cannot
    //! help; it exists so that a decoder returning nothing can never spin.
    enum { kFeedGuard = 64 };

    Settings m_settings;
    double m_rate = 44100.0;
    int m_channels = 2;
    int64_t m_length = 0;
    bool m_configured = false;

    std::vector<declick::Channel> m_declick;
    declick::Config m_declickCfg;
    std::vector<dehum::Channel> m_dehum;
    dehum::Config m_dehumCfg;
    DehumScout m_scout;

    std::vector<float> m_read;  //!< one slice of track, as decoded
    std::vector<float> m_work;  //!< one slice on its way out

    int64_t m_inPos = 0;   //!< next frame to read from the track
    int64_t m_outPos = -1; //!< next frame the pipeline is positioned to produce

    int64_t m_restarts = 0;
    int64_t m_sourceFrames = 0;
};

} // namespace restoration
