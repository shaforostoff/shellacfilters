/* ========================================
 *  Declick - VirtualDJ warm start
 *
 *  Declick's carpet key and its crackle floor start with no history, and in
 *  the first seconds of a record that shows: the key opens on a loud opening
 *  and the stage cuts it, or stays shut over a crackly one. The warm start
 *  (declick::Channel::beginWarm()) runs the record's first Config::
 *  warmSeconds() through the core before the stream, so the first seconds are
 *  judged with what those seconds tell it - and a buffer plug-in can read
 *  them, because GetSongBuffer hands over any part of the decoded song.
 *
 *  What it cannot do is wait for the pass. foobar2000 holds the first seconds
 *  and stalls for it (foo_dsp_declick's warm_hold.h), behind an output buffer.
 *  Here OnGetSongBuffer is the audio callback, for every deck at once, and the
 *  pass is 0.5-1.4 s of work on the 2014 MacBook Air: a stall would take the
 *  deck the audience is hearing down with it. So, like Dehum's scout
 *  (../vdj_dehum/dehum_vdj_scout.h):
 *
 *    - The opening is copied on the caller's thread, kWarmSpeedup times the
 *      frames just served per call, into a buffer sized at configure(). The
 *      copy is because nothing in the SDK says GetSongBuffer may be called
 *      from any other thread; the budget is because it is not a file read but
 *      whatever is upstream in the chain - see kRestartCooldownSec in
 *      ../common/vdj_buffer_dsp.h.
 *
 *    - A worker thread runs the warm pass over the copy, on a channel pair of
 *      its own, at below-normal priority.
 *
 *    - Every restart of the pipeline after that, in that record - a cue, a
 *      seek, a loop, a scratch out of the cache - starts with what the pass
 *      learned (Channel::adoptWarm()). At the top of the record that is the
 *      warm start to the bit; anywhere else, the restart starts with the
 *      opening's key and floor ceiling instead of an empty history, which is
 *      a fair prior for a record whose surface does not change.
 *
 *  The first play after a record is loaded is cold, as before: there is no
 *  "track loaded" event for a buffer plug-in, so the opening is not known
 *  until the deck first asks for audio, and the pass is not done until a
 *  second or two after that. A DJ who cues the record before playing it out
 *  - which is a restart - gets the warm start where the audience hears it.
 *
 *  Nothing here allocates after configure(), on either thread.
 * ======================================== */

#ifndef VDJ_DECLICK_WARM_H
#define VDJ_DECLICK_WARM_H

#include "declick_core.h"
#include "vdj_engine.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace vdj {

//! Reading the opening, as a multiple of playback: Dehum's scout's figure.
//! Seven seconds are copied inside the first 0.9 s of play. Through nothing
//! it is a memcpy; through an upstream buffer plug-in it is that plug-in's
//! work at a second position, which is what keeps it from being more.
const int kWarmSpeedup = 8;

//! Frames per read, and the ceiling on one call's worth of reading however
//! much was served - so a host that asks for a very long buffer gets a slower
//! read rather than a stall. The worker pushes kWarmPassSlice at a time and
//! looks for a cancel between them, which bounds how long configure() waits
//! for a pass to let go of the buffers: under 10 ms on the Air.
enum { kWarmReadSlice = 4096, kWarmMaxPerCall = 16384, kWarmPassSlice = 2048 };

class DeclickWarm {
public:
    DeclickWarm() {}
    ~DeclickWarm() { stopWorker(); }
    DeclickWarm(const DeclickWarm &) = delete;
    void operator=(const DeclickWarm &) = delete;

    //! Allocates: the warm pass's two channels, the copy of the opening, and
    //! the first time, the worker. Only where the engine allocates anyway - a
    //! new sample rate, or a slider that resizes the pipeline - since what the
    //! pass learns depends on the config and has to be learned again. Waits
    //! for a pass in progress to let go of the buffers first. False if it
    //! could not; the plug-in then restarts cold, as it did before.
    bool configure(const declick::Config & cfg, double rate) {
        settle();
        m_usable = false;
        m_state.store(kIdle, std::memory_order_relaxed);
        try {
            m_ch[0].configure(cfg);
            m_ch[1].configure(cfg);
            m_want = (size_t)(cfg.warmSeconds() * rate);
            m_ahead.assign(m_want * kChannels, 0);
            m_scratch.assign((size_t)kWarmPassSlice * kChannels, 0.0);
        } catch (const std::bad_alloc &) {
            return false;
        }
        if (m_want == 0 || !startWorker()) return false;
        m_usable = true;
        newTrack();
        return true;
    }

    //! A new record: what was learned from the last one goes, and this one's
    //! opening is read from the top. If a pass is running it is told to stop,
    //! and the reading starts once it has.
    void newTrack() {
        if (!m_usable) return;
        if (m_state.load(std::memory_order_acquire) == kWarming) {
            m_cancel.store(true, std::memory_order_relaxed);
            m_pending = true;
            return;
        }
        begin();
    }

    //! The "Warm start" switch, read every call. Off, restarts are cold and
    //! nothing is read; turned on again, this record's opening is read anew.
    void setEnabled(bool on) {
        if (on == m_enabled) return;
        m_enabled = on;
        if (on) newTrack();
    }
    bool enabled() const { return m_enabled; }

    //! Once per served buffer: a little more of the opening, and once it is
    //! all here, or the record has ended, the pass is handed to the worker.
    void feed(SongSource & src, size_t served) {
        if (!m_usable || !m_enabled) return;
        if (m_pending) {
            if (m_state.load(std::memory_order_acquire) == kWarming) return;
            begin();
        }
        if (m_state.load(std::memory_order_relaxed) != kReading) return;

        size_t budget = std::min(served * (size_t)kWarmSpeedup, (size_t)kWarmMaxPerCall);
        while (budget > 0 && m_pos < m_want) {
            const size_t n = std::min(std::min(budget, (size_t)kWarmReadSlice), m_want - m_pos);
            const size_t got = read(src, m_pos, n);
            if (got == 0) {
                // The end of a short record, or a part of it VirtualDJ has
                // not decoded yet. Wait a few calls before deciding which,
                // then warm on what there is: a record shorter than the warm
                // length is warmed over all of it, as foobar2000 does.
                if (++m_failures >= kMaxFailures) handOff();
                return;
            }
            m_failures = 0;
            m_pos += got;
            budget -= got;
            if (got < n) return;
        }
        if (m_pos >= m_want) handOff();
    }

    //! A restart of the pipeline: both channels start afresh, with what the
    //! warm pass learned if it has finished for this record. False if they
    //! start cold. Checked once for the pair, so that a pass finishing between
    //! the two cannot leave one side warm and the other cold.
    bool start(declick::Channel * ch) const {
        const bool warm = ready()
            && ch[0].adoptWarm(m_ch[0]) && ch[1].adoptWarm(m_ch[1]);
        if (!warm) {
            ch[0].reset();
            ch[1].reset();
        }
        return warm;
    }

    //! The pass has finished for this record, and restarts start warm.
    bool ready() const {
        return m_usable && m_enabled && !m_pending
            && m_state.load(std::memory_order_acquire) == kReady;
    }

    //! Diagnostics, and what the tests read.
    bool    reading() const { return m_state.load(std::memory_order_relaxed) == kReading; }
    size_t  framesRead() const { return m_pos; }
    size_t  warmFrames() const { return m_want; }
    uint64_t passes() const { return m_passes.load(std::memory_order_acquire); }
    double  lastPassMs() const { return (double)m_lastUs.load(std::memory_order_acquire) / 1000.0; }

    //! Not for the audio thread: blocks until no pass is running, or for at
    //! most `ms`. For the tests and benchmarks, which need to know a restart
    //! will find the pass finished rather than hope so.
    bool waitForPass(int ms) {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_done.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            return m_state.load(std::memory_order_acquire) != kWarming;
        });
    }

private:
    //! kReading: the caller's thread is copying the opening. kWarming: the
    //! worker owns the copy and the channels. kReady: the channels hold the
    //! pass, read only. kIdle: nothing for this record.
    enum { kIdle, kReading, kWarming, kReady };
    enum { kMaxFailures = 16 };

    void begin() {
        m_pending = false;
        m_cancel.store(false, std::memory_order_relaxed);
        m_pos = 0;
        m_failures = 0;
        m_state.store(m_enabled ? kReading : kIdle, std::memory_order_relaxed);
    }

    //! Up to `n` frames from `pos` into the copy: fewer where the song stops
    //! reading, found by halving as BufferPipeline::advance() does at the end
    //! of a song.
    size_t read(SongSource & src, size_t pos, size_t n) {
        const short * raw = NULL;
        if (!src.read((int)pos, (int)n, &raw) || raw == NULL) {
            size_t ok = 0, bad = n;
            while (bad - ok > 1) {
                const size_t mid = ok + (bad - ok) / 2;
                const short * part = NULL;
                if (src.read((int)pos, (int)mid, &part) && part != NULL) ok = mid;
                else bad = mid;
            }
            raw = NULL;
            if (ok == 0 || !src.read((int)pos, (int)ok, &raw) || raw == NULL) return 0;
            n = ok;
        }
        memcpy(&m_ahead[pos * kChannels], raw, n * kChannels * sizeof(short));
        return n;
    }

    void handOff() {
        if (m_pos == 0) {
            m_state.store(kIdle, std::memory_order_relaxed);
            return;
        }
        m_frames = m_pos;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_state.store(kWarming, std::memory_order_release);
        }
        m_wake.notify_one();
    }

    //! Waits for a pass in progress to stop: one slice at most.
    void settle() {
        if (m_state.load(std::memory_order_acquire) != kWarming) return;
        m_cancel.store(true, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lk(m_mutex);
        m_done.wait(lk, [this] {
            return m_state.load(std::memory_order_acquire) != kWarming;
        });
    }

    bool startWorker() {
        if (m_thread.joinable()) return true;
        try {
            m_quit = false;
            m_thread = std::thread(&DeclickWarm::run, this);
        } catch (const std::system_error &) {
            return false;
        } catch (const std::bad_alloc &) {
            return false;
        }
        return true;
    }

    void stopWorker() {
        if (!m_thread.joinable()) return;
        m_cancel.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    void run() {
        // Below the UI and the decoder: nothing waits on this but the next
        // restart, and on a two-core machine the audio thread needs the other.
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        // Per thread, and part of the core's numerical contract.
        declick::scoped_flush_denormals ftz;
        std::unique_lock<std::mutex> lk(m_mutex);
        for (;;) {
            m_wake.wait(lk, [this] {
                return m_quit || m_state.load(std::memory_order_acquire) == kWarming;
            });
            if (m_quit) return;
            lk.unlock();
            const bool done = pass();
            lk.lock();
            m_state.store(done ? kReady : kIdle, std::memory_order_release);
            m_done.notify_all();
        }
    }

    //! The warm pass, both channels a slice at a time. The same samples the
    //! pipeline pushes - fromShorts() of the song - so that a restart at the
    //! top of the record is the warm start exactly.
    bool pass() {
        typedef std::chrono::steady_clock clock;
        const clock::time_point t0 = clock::now();
        const size_t n = m_frames;
        m_ch[0].beginWarm();
        m_ch[1].beginWarm();
        for (size_t b = 0; b < n; b += kWarmPassSlice) {
            if (m_cancel.load(std::memory_order_relaxed)) return false;
            const size_t k = std::min((size_t)kWarmPassSlice, n - b);
            fromShorts(&m_ahead[b * kChannels], m_scratch.data(), k);
            m_ch[0].push(m_scratch.data() + 0, k, kChannels);
            m_ch[1].push(m_scratch.data() + 1, k, kChannels);
        }
        m_ch[0].endWarm();
        m_ch[1].endWarm();
        m_lastUs.store((int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - t0).count(), std::memory_order_release);
        m_passes.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    declick::Channel    m_ch[kChannels];
    std::vector<short>  m_ahead;     //!< the opening, interleaved, as the song holds it
    std::vector<double> m_scratch;   //!< one pass slice, as the core takes it

    // The caller's thread's.
    size_t m_want = 0;               //!< Config::warmSeconds() in frames
    size_t m_pos = 0;                //!< frames of the opening copied
    int    m_failures = 0;
    bool   m_usable = false;
    bool   m_enabled = true;
    bool   m_pending = false;        //!< a new record, waiting for a pass to stop

    // Shared. m_frames is written before the hand-off and read after it.
    size_t m_frames = 0;
    std::atomic<int>  m_state { kIdle };
    std::atomic<bool> m_cancel { false };
    std::atomic<uint64_t> m_passes { 0 };
    std::atomic<int64_t>  m_lastUs { 0 };

    std::thread m_thread;
    std::mutex  m_mutex;
    std::condition_variable m_wake, m_done;
    bool m_quit = false;             //!< under m_mutex
};

} // namespace vdj

#endif // VDJ_DECLICK_WARM_H
