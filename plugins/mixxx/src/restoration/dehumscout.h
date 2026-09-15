/* ========================================
 *  Dehum - Mixxx track scout
 *
 *  Dehum's detector will not commit to a line until it has several seconds of
 *  steady evidence, and on the transfers this exists for that is not a few
 *  seconds but the better part of a minute: see the acquisition note at the top
 *  of dehum_core.h. A line the prominence route can see arrives about 9 s in,
 *  one that only the coherence route can reach at 43 s. All of it is time the
 *  record plays with the hum still in it.
 *
 *  A filter that sits on mixxx::AudioSource does not have to spend it, because
 *  the whole decoded track is one read away. So a second, throwaway
 *  dehum::Channel is run over the *opening* of the record while the deck plays,
 *  faster than the deck plays it, and what it finds goes to Channel::adopt() -
 *  which starts those lines confirmed and leaves the live detector running, so
 *  it still tracks them, still drops them if the evidence is not really there,
 *  and can still find others.
 *
 *  Deliberately per track: each record carries its own hum, so the scout is
 *  rewound for every one and publishes exactly once.
 *
 *  This is the VirtualDJ scout with two changes, both from where it now runs.
 *  Reads come back as float rather than 16-bit, because that is what a Mixxx
 *  AudioSource decodes to; and the budget is scaled against frames *read* rather
 *  than frames played, because CachingReaderWorker reads ahead in bursts. The
 *  effect of the second is that a track which has just been loaded - when the
 *  reader is filling its cache as fast as it can - gets scouted almost at once,
 *  which is exactly when it is worth the most.
 *
 *  None of this is on the audio thread. It is on CachingReaderWorker's, which is
 *  also why it is still budgeted rather than run flat out: that thread has a
 *  deck to keep fed, and a scout that decodes 60 s of audio in one call would
 *  stall the chunk somebody is waiting for.
 * ======================================== */

#pragma once

#include <stdint.h>

#include <algorithm>
#include <new>
#include <vector>

#include "restoration/dehum_core.h"
#include "restoration/restorationsource.h"

namespace restoration {

//! Seconds of the opening to read. Sixty, the same figure and for the same
//! reason as the foobar2000 scout: the coherence route accumulates its ratio
//! over dehum::kCohWindowSec, so the lines that most need scouting are exactly
//! the ones that need most of this window.
const double kScoutSeconds = 60.0;

//! How much less than that is still worth acting on - and also how soon the
//! scout is allowed to stop early, once it has actually confirmed a line. Below
//! this the prominence route has barely had time to confirm anything and the
//! coherence route none at all, so there would be nothing to hand over.
const double kScoutMinSeconds = 10.0;

//! Scouting speed as a multiple of reading.
const int kScoutSpeedup = 8;

//! Frames per read, and the ceiling on one call's worth of work regardless of
//! how much audio was served - so a caller that asks for a very long range gets
//! a slower scout rather than a stall.
//!
//! The ceiling is kScoutSpeedup times CachingReaderChunk::kFrames on purpose: at
//! the chunk size Mixxx actually reads in, the budget is what kScoutSpeedup says
//! it is rather than quietly half of it. A caller asking for more than one chunk
//! at a time gets a scout that runs slower than 8x, which is the right way round
//! - it is the long call that risks stalling the deck.
enum { kScoutSlice = 4096, kScoutMaxPerCall = 8192 * kScoutSpeedup };

class DehumScout {
  public:
    //! Allocates: one dehum::Channel plus a slice. Called at track load, like
    //! everything else here that touches the heap.
    bool begin(const dehum::Params& params, double rate) {
        if (!(rate >= 1000.0)) {
            rate = 44100.0;
        }
        dehum::Params p = params;
        // Belt and braces. A pinned frequency turns the search off, so there
        // would be nothing to find, and adopt() refuses lines while one is set
        // anyway - newTrack() below declines to run in that case.
        p.frequency = 0.0f;
        p.sanitize();

        dehum::Config cfg;
        cfg.compute(p, rate);
        try {
            m_ch.configure(cfg);
            m_slice.assign((size_t)kScoutSlice, 0.0);
            m_raw.assign((size_t)kScoutSlice * 2, 0.0f);
        } catch (const std::bad_alloc&) {
            m_usable = false;
            return false;
        }
        m_rate = rate;
        m_usable = true;
        m_running = false;
        m_ready = false;
        return true;
    }

    //! A new track. `pinned` is the live Params::frequency: with a frequency
    //! pinned by hand there is nothing to search for, so the scout stays idle and
    //! costs nothing.
    void newTrack(float pinned) {
        m_running = false;
        m_ready = false;
        m_count = 0;
        m_pos = 0;
        m_failures = 0;
        if (!m_usable || pinned > 0.0f) {
            return;
        }
        m_ch.reset();
        m_wanted = (int64_t)(kScoutSeconds * m_rate);
        m_least = (int64_t)(kScoutMinSeconds * m_rate);
        m_running = true;
    }

    //! One call's worth of scouting. `served` is how many frames the caller just
    //! produced, which is what the budget is scaled from.
    void feed(Source& src, int channels, size_t served) {
        if (!m_running) {
            return;
        }

        int64_t budget = (int64_t)served * kScoutSpeedup;
        if (budget > (int64_t)kScoutMaxPerCall) {
            budget = (int64_t)kScoutMaxPerCall;
        }

        dehum::scoped_flush_denormals ftz;

        while (budget > 0 && m_pos < m_wanted) {
            const int64_t room = std::min<int64_t>(budget, (int64_t)kScoutSlice);
            const int64_t n = std::min<int64_t>(room, m_wanted - m_pos);
            if (n <= 0) {
                break;
            }

            if ((int64_t)m_raw.size() < n * channels) {
                // Only reachable if the track has more channels than the two this
                // was sized for - stems. Growing once at track load is allowed;
                // growing per call would not be.
                m_raw.assign((size_t)(n * channels), 0.0f);
            }

            const int64_t got = src.read(m_pos, n, m_raw.data());
            if (got <= 0) {
                // The end of a short record. Stop rather than hammering the read
                // every call for the rest of the track.
                if (++m_failures >= kMaxFailures) {
                    finish();
                }
                return;
            }
            m_failures = 0;

            // Hum is common mode, so one summed channel finds it for a fraction
            // of the work of running them all - the same reason the foobar2000
            // scout downmixes. The live channels still cancel per side.
            const double scale = 1.0 / (double)channels;
            for (int64_t f = 0; f < got; ++f) {
                double sum = 0.0;
                for (int c = 0; c < channels; ++c) {
                    sum += (double)m_raw[(size_t)(f * channels + c)];
                }
                m_slice[(size_t)f] = sum * scale;
            }
            m_ch.process(m_slice.data(), (size_t)got, 1);

            m_pos += got;
            budget -= got;

            // Stop as soon as there is something to hand over and enough of the
            // record has been read to trust it. The 60 s window exists for the
            // coherence route, which needs most of it; a line the prominence
            // route can see turns up in the first ten and there is nothing to be
            // gained by reading the other fifty - which matters here for more
            // than time, because every one of these reads is a decode competing
            // with the one the deck is waiting for.
            if (m_pos >= m_least && m_ch.lineCount() > 0) {
                finish();
                return;
            }
        }

        if (m_pos >= m_wanted) {
            finish();
        }
    }

    //! Hands the result over once. Zero every call until the scout finishes and
    //! zero again afterwards, so the caller can poll it per read without having
    //! to remember that it already did - re-adopting every call would keep
    //! resetting the score of a line the live detector had decided to let go of.
    int take(dehum::LineReport* out, int max) {
        if (!m_ready) {
            return 0;
        }
        m_ready = false;
        int count = m_count;
        if (count > max) {
            count = max;
        }
        for (int i = 0; i < count; ++i) {
            out[i] = m_lines[i];
        }
        return count;
    }

    bool running() const {
        return m_running;
    }
    int64_t framesRead() const {
        return m_pos;
    }

  private:
    friend class Pipeline;

    enum { kMaxFailures = 8 };

    void finish() {
        m_running = false;
        m_count = 0;
        if (m_pos >= m_least) {
            m_ch.report(m_lines, (int)dehum::kMaxLines, &m_count);
        }
        m_ready = (m_count > 0);
    }

    dehum::Channel m_ch;
    std::vector<double> m_slice;
    std::vector<float> m_raw;

    dehum::LineReport m_lines[dehum::kMaxLines];
    int m_count = 0;
    double m_rate = 0.0;
    int64_t m_pos = 0;
    int64_t m_wanted = 0;
    int64_t m_least = 0;
    int m_failures = 0;
    bool m_usable = false;
    bool m_running = false;
    bool m_ready = false;
};

} // namespace restoration
