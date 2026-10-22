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
 *      ../common/vdj_buffer_dsp.h. It goes on kWarmCopyBeyondSec past the
 *      warm length, for the catch-up below.
 *
 *    - A worker thread runs the warm pass over the copy, on a channel pair of
 *      its own, at below-normal priority, and keeps what the pass learned in
 *      a declick::WarmState - a few kB, where a third pair would be 6 MB.
 *
 *    - Every restart of the pipeline after that, in that record - a cue, a
 *      seek, a loop, a scratch out of the cache - starts with it
 *      (Channel::adoptWarm()). At the top of the record that is the warm
 *      start to the bit; anywhere else, the restart starts with the opening's
 *      key and floor ceiling instead of an empty history, which is a fair
 *      prior for a record whose surface does not change.
 *
 *    - The play that was already going when the pass finished - the first
 *      play of the record, there being no "track loaded" event for a buffer
 *      plug-in - started cold. The worker catches its own pair up with it:
 *      from where that play's run started, over the copy, to a frame X
 *      kWarmParkSec ahead of the live input, and parks there with every
 *      output taken. BufferPipeline::advance() stops a slice exactly at X
 *      (holdAt()), and once the live pair has taken [from, X) as well and
 *      its output is in the cache, the two pairs change places (reached()).
 *      Both have consumed the same audio from the same frame, so nothing is
 *      restarted and nothing spliced: from the swap on, the stream is the
 *      core's warm start run from `from`, to the bit. A restart first, or a
 *      play head that outruns the copy, and there is no swap.
 *
 *  Nothing here allocates after configure(), on either thread.
 * ======================================== */

#ifndef VDJ_DECLICK_WARM_H
#define VDJ_DECLICK_WARM_H

#include "declick_core.h"
#include "vdj_engine.h"

#include <stdint.h>
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

//! The copy goes on this far past the warm length, while the pass runs: a
//! first play that starts a few seconds in can still be caught up with. On
//! the Air the worker catches a play from the top about 2.5 s in, so this
//! covers first plays starting up to about 6.5 s in; 350 kB at 44.1 kHz.
const double kWarmCopyBeyondSec = 2.0;

//! How far ahead of the live input the worker parks: how long the audio
//! thread has to see it before passing X. A tenth of a second is eight 512
//! frame calls; missed, the worker goes on and parks again further on.
const double kWarmParkSec = 0.1;

//! Frames per read, and the ceiling on one call's worth of reading however
//! much was served - so a host that asks for a very long buffer gets a slower
//! read rather than a stall. The worker pushes kWarmPassSlice at a time and
//! looks for a cancel between them, which bounds how long configure() waits
//! for it to let go of the buffers: under 10 ms on the Air.
enum { kWarmReadSlice = 4096, kWarmMaxPerCall = 16384, kWarmPassSlice = 2048 };

class DeclickWarm {
public:
    //! `pair`: two channels the engine owns, which this works on. They change
    //! places with the engine's live pair at a swap; see reached().
    explicit DeclickWarm(declick::Channel * pair) : m_ch(pair) {}
    ~DeclickWarm() { stopWorker(); }
    DeclickWarm(const DeclickWarm &) = delete;
    void operator=(const DeclickWarm &) = delete;

    //! Allocates: the warm pair, the saved state, the copy of the opening,
    //! and the first time, the worker. Only where the engine allocates anyway
    //! - a new sample rate, or a slider that resizes the pipeline - since what
    //! the pass learns depends on the config and has to be learned again.
    //! Waits for the worker to let go of the buffers first. False if it could
    //! not; the plug-in then restarts cold, as it did before.
    bool configure(const declick::Config & cfg, double rate) {
        settle();
        m_usable = false;
        m_state.store(kIdle, std::memory_order_relaxed);
        m_warm.store(false, std::memory_order_relaxed);
        try {
            for (int c = 0; c < kChannels; ++c) {
                m_ch[c].configure(cfg);
                // Sizes the saved state here, where allocating is allowed,
                // so the worker's save does not. What it saves - a reset
                // channel - is not used: m_warm says so.
                m_ch[c].saveWarm(m_saved[c]);
                m_saved[c].clear();
            }
            m_want = (size_t)(cfg.warmSeconds() * rate);
            m_cap = m_want + (size_t)(kWarmCopyBeyondSec * rate);
            m_margin = (int64_t)(kWarmParkSec * rate);
            m_ahead.assign(m_cap * kChannels, 0);
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
    //! opening is read from the top. If the worker is busy it is told to
    //! stop, and the reading starts once it has.
    void newTrack() {
        if (!m_usable) return;
        if (busy(m_state.load(std::memory_order_acquire))) {
            m_cancel.store(true, std::memory_order_relaxed);
            m_pending = true;
            unpark();
            return;
        }
        begin();
    }

    //! The "Warm start" switch, read every call. Off, restarts are cold,
    //! nothing is read and the worker stops; turned on again, this record's
    //! opening is read anew.
    void setEnabled(bool on) {
        if (on == m_enabled) return;
        m_enabled = on;
        if (on) {
            newTrack();
        } else if (busy(m_state.load(std::memory_order_acquire))) {
            m_cancel.store(true, std::memory_order_relaxed);
            unpark();
        }
    }
    bool enabled() const { return m_enabled; }

    //! Once per served buffer: a little more of the opening. Once the warm
    //! length is in, or the record has ended, the pass is handed to the
    //! worker; the copy then goes on to m_cap for the catch-up, unless that
    //! is already over.
    void feed(SongSource & src, size_t served) {
        if (!m_usable || !m_enabled) return;
        if (m_pending) {
            if (busy(m_state.load(std::memory_order_acquire))) return;
            begin();
        }
        int st = m_state.load(std::memory_order_acquire);
        if (st == kIdle || m_ended || m_pos >= m_cap) return;
        if (st == kReady && m_pos >= m_want) return;

        size_t budget = std::min(served * (size_t)kWarmSpeedup, (size_t)kWarmMaxPerCall);
        while (budget > 0 && m_pos < m_cap) {
            const size_t end = (st == kReading) ? m_want : m_cap;
            const size_t n = std::min(std::min(budget, (size_t)kWarmReadSlice), end - m_pos);
            const size_t got = read(src, m_pos, n);
            if (got == 0) {
                // The end of a short record, or a part of it VirtualDJ has
                // not decoded yet. Wait a few calls before deciding which,
                // then warm on what there is: a record shorter than the warm
                // length is warmed over all of it, as foobar2000 does.
                if (++m_failures >= kMaxFailures) {
                    m_ended = true;
                    if (st == kReading) handOff();
                }
                return;
            }
            m_failures = 0;
            m_pos += got;
            budget -= got;
            m_copied.store(m_pos, std::memory_order_release);
            if (st == kReading && m_pos >= m_want) {
                handOff();
                st = kWarming;
            }
            if (got < n) return;
        }
    }

    //! A restart of the pipeline at song frame `from`: both channels start
    //! afresh, with what the warm pass learned if it has finished for this
    //! record. False if they start cold - and then the worker, once its pass
    //! is done, catches up with this run. Checked once for the pair, so that
    //! a pass finishing between the two cannot leave one side warm and the
    //! other cold.
    bool start(declick::Channel * ch, int64_t from) {
        const bool warm = ready()
            && ch[0].adoptWarm(m_saved[0]) && ch[1].adoptWarm(m_saved[1]);
        if (!warm) {
            ch[0].reset();
            ch[1].reset();
        }
        if (from < 0) from = 0;
        if (from > 0x7fffffff) from = 0x7fffffff;
        ++m_epoch;
        m_liveIn.store(from, std::memory_order_relaxed);
        m_run.store(((uint64_t)m_epoch << 32) | (warm ? 0 : kCold) | (uint64_t)from,
                    std::memory_order_release);
        // A parked worker was waiting on the run that just ended.
        unpark();
        return warm;
    }

    //! The frame the live input must stop at for one slice, or -1: where the
    //! worker has parked the caught-up pair, if it is this run's.
    int64_t holdAt() const {
        if (m_state.load(std::memory_order_acquire) != kParked) return -1;
        if (m_parkEpoch.load(std::memory_order_relaxed) != m_epoch) return -1;
        return m_swapAt.load(std::memory_order_relaxed);
    }

    //! After every slice of the live run: input fed up to `in`, output taken
    //! up to `out`. At the parked frame, the caught-up pair goes live and the
    //! live one becomes the worker's: true, and `live` points at the new pair.
    //! Past it, the worker is told to go on.
    bool reached(int64_t in, int64_t out, declick::Channel *& live) {
        m_liveIn.store(in, std::memory_order_relaxed);
        if (m_state.load(std::memory_order_acquire) != kParked) return false;
        if (m_parkEpoch.load(std::memory_order_relaxed) != m_epoch) {
            unpark();
            return false;
        }
        const int64_t x = m_swapAt.load(std::memory_order_relaxed);
        if (in == x && m_enabled && !m_pending) {
            // Only this thread takes the worker out of kParked, so it is
            // still there, holding nothing, with every output taken.
            std::swap(live, m_ch);
            m_swapOut = out;
            m_swapIn = in;
            m_swaps.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> g(m_mutex);
                m_state.store(kReady, std::memory_order_release);
            }
            m_wake.notify_one();
            return true;
        }
        if (in > x) unpark();
        return false;
    }

    //! The pass has finished for this record, and restarts start warm.
    bool ready() const {
        return m_usable && m_enabled && !m_pending
            && m_warm.load(std::memory_order_acquire);
    }

    //! Diagnostics, and what the tests read.
    bool    reading() const { return m_state.load(std::memory_order_relaxed) == kReading; }
    bool    parked() const { return m_state.load(std::memory_order_acquire) == kParked; }
    size_t  framesRead() const { return m_pos; }
    size_t  warmFrames() const { return m_want; }
    size_t  copyFrames() const { return m_cap; }
    uint64_t passes() const { return m_passes.load(std::memory_order_acquire); }
    double  lastPassMs() const { return (double)m_lastUs.load(std::memory_order_acquire) / 1000.0; }
    //! Swaps made, and for the last one the song frames where the live input
    //! and output stood: from `swapOut()` on, the stream is the warm one.
    uint64_t swaps() const { return m_swaps.load(std::memory_order_relaxed); }
    int64_t swapIn() const { return m_swapIn; }
    int64_t swapOut() const { return m_swapOut; }
    //! The last catch-up, as of its last park: worker time spent pushing,
    //! and the frames pushed.
    double  lastChaseMs() const { return (double)m_chaseUs.load(std::memory_order_relaxed) / 1000.0; }
    int64_t lastChaseFrames() const { return m_chaseFrames.load(std::memory_order_relaxed); }
    //! Bytes held besides the warm pair's channels, which are the engine's;
    //! and of them, what keeps the pass for the restarts.
    size_t  heapBytes() const {
        return m_ahead.capacity() * sizeof(short) + m_scratch.capacity() * sizeof(double)
             + m_saved[0].heapBytes() + m_saved[1].heapBytes();
    }
    size_t  savedBytes() const {
        return sizeof(m_saved) + m_saved[0].heapBytes() + m_saved[1].heapBytes();
    }

    //! Not for the audio thread: blocks until the worker is idle, or for at
    //! most `ms`. For the tests and benchmarks, which need to know a restart
    //! will find the pass finished rather than hope so. A parked worker is
    //! not idle: it waits for the audio thread.
    bool waitForPass(int ms) {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_done.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            return !busy(m_state.load(std::memory_order_acquire));
        });
    }

    //! As waitForPass(), but also returns once the worker has parked.
    bool waitForPark(int ms) {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_done.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            const int st = m_state.load(std::memory_order_acquire);
            return st == kParked || !busy(st);
        });
    }

private:
    //! kReading: the caller's thread is copying the opening. kWarming: the
    //! worker owns the warm pair and runs the pass. kChasing: it is catching
    //! the pair up with the live run. kParked: the pair has taken the live
    //! run's audio up to m_swapAt and all its output, and the worker waits -
    //! only the caller's thread moves it on from here, to kReady by swapping
    //! or to kChasing to have it look again. kReady: the worker is idle.
    //! kIdle: nothing for this record. m_warm says separately whether
    //! m_saved holds this record's pass.
    enum { kIdle, kReading, kWarming, kChasing, kParked, kReady };
    enum { kMaxFailures = 16 };
    static constexpr uint64_t kCold = (uint64_t)1 << 31;   //!< in m_run

    static bool busy(int st) { return st == kWarming || st == kChasing || st == kParked; }

    void begin() {
        m_pending = false;
        m_cancel.store(false, std::memory_order_relaxed);
        m_warm.store(false, std::memory_order_relaxed);
        m_pos = 0;
        m_copied.store(0, std::memory_order_relaxed);
        m_failures = 0;
        m_ended = false;
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
        m_frames = std::min(m_pos, m_want);
        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_state.store(kWarming, std::memory_order_release);
        }
        m_wake.notify_one();
    }

    //! The caller's thread: a parked worker is to look again - at a cancel,
    //! a new run, or a live input that went past it.
    void unpark() {
        if (m_state.load(std::memory_order_acquire) != kParked) return;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_state.store(kChasing, std::memory_order_release);
        }
        m_wake.notify_one();
    }

    //! Waits for the worker to stop: one slice at most.
    void settle() {
        if (!busy(m_state.load(std::memory_order_acquire))) return;
        m_cancel.store(true, std::memory_order_relaxed);
        unpark();
        std::unique_lock<std::mutex> lk(m_mutex);
        m_done.wait(lk, [this] {
            // A worker that was still chasing when unpark() looked parks
            // after it, without reading m_cancel: send it on to read it.
            const int st = m_state.load(std::memory_order_acquire);
            if (st == kParked) {
                m_state.store(kChasing, std::memory_order_release);
                m_wake.notify_one();
            }
            return !busy(st);
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
            job();
            lk.lock();
            m_done.notify_all();
        }
    }

    //! The pass, then the catch-up. Leaves the state at kReady or kIdle, or
    //! with the caller's thread having set kReady at a swap.
    void job() {
        declick::Channel * const ch = m_ch;
        if (!pass(ch)) {
            m_state.store(kIdle, std::memory_order_release);
            return;
        }
        // Sized at configure(), so neither allocates.
        ch[0].saveWarm(m_saved[0]);
        ch[1].saveWarm(m_saved[1]);
        m_warm.store(true, std::memory_order_release);
        chase(ch);
    }

    //! The warm pass, both channels a slice at a time. The same samples the
    //! pipeline pushes - fromShorts() of the song - so that a restart at the
    //! top of the record is the warm start exactly.
    bool pass(declick::Channel * ch) {
        typedef std::chrono::steady_clock clock;
        const clock::time_point t0 = clock::now();
        const size_t n = m_frames;
        ch[0].beginWarm();
        ch[1].beginWarm();
        for (size_t b = 0; b < n; b += kWarmPassSlice) {
            if (m_cancel.load(std::memory_order_relaxed)) return false;
            const size_t k = std::min((size_t)kWarmPassSlice, n - b);
            fromShorts(&m_ahead[b * kChannels], m_scratch.data(), k);
            ch[0].push(m_scratch.data() + 0, k, kChannels);
            ch[1].push(m_scratch.data() + 1, k, kChannels);
        }
        ch[0].endWarm();
        ch[1].endWarm();
        m_lastUs.store((int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - t0).count(), std::memory_order_release);
        m_passes.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    //! Catches the warm pair up with the live run, if that run started cold,
    //! and parks it ahead of the live input. Follows the latest run: one that
    //! started warm needs nothing, and ends it.
    void chase(declick::Channel * ch) {
        typedef std::chrono::steady_clock clock;
        clock::time_point t0 = clock::now();
        int64_t us = 0, frames = 0;
        bool following = false, fresh = true;
        uint32_t epoch = 0;
        int64_t p = 0;                  // the next frame the pair takes
        for (;;) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                m_state.store(kIdle, std::memory_order_release);
                return;
            }
            const uint64_t run = m_run.load(std::memory_order_acquire);
            if (!following || (uint32_t)(run >> 32) != epoch) {
                following = true;
                epoch = (uint32_t)(run >> 32);
                if ((run & kCold) == 0) {
                    m_state.store(kReady, std::memory_order_release);
                    return;
                }
                // Back to where the pass ended, if it has taken a run since.
                if (!fresh) {
                    ch[0].adoptWarm(m_saved[0]);
                    ch[1].adoptWarm(m_saved[1]);
                }
                fresh = false;
                p = (int64_t)(run & (kCold - 1));
                m_state.store(kChasing, std::memory_order_release);
            }
            if (p >= m_liveIn.load(std::memory_order_relaxed) + m_margin) {
                us += (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    clock::now() - t0).count();
                m_chaseUs.store(us, std::memory_order_relaxed);
                m_chaseFrames.store(frames, std::memory_order_relaxed);
                if (!park(p, epoch)) return;
                t0 = clock::now();
                continue;
            }
            const int64_t limit = (int64_t)m_copied.load(std::memory_order_acquire);
            if (p >= limit) {
                // The live input is further on than the copy reaches.
                m_state.store(kReady, std::memory_order_release);
                return;
            }
            const size_t k = (size_t)std::min<int64_t>(kWarmPassSlice, limit - p);
            fromShorts(&m_ahead[(size_t)p * kChannels], m_scratch.data(), k);
            ch[0].push(m_scratch.data() + 0, k, kChannels);
            ch[1].push(m_scratch.data() + 1, k, kChannels);
            p += (int64_t)k;
            frames += (int64_t)k;
            // Taken and dropped: the live pair has made this output already.
            for (;;) {
                size_t got = ch[0].available();
                if (got == 0) break;
                if (got > (size_t)kWarmPassSlice) got = (size_t)kWarmPassSlice;
                ch[0].pull(m_scratch.data() + 0, got, kChannels);
                ch[1].pull(m_scratch.data() + 1, got, kChannels);
            }
        }
    }

    //! Waits at `x` for the caller's thread. False when there is nothing more
    //! to do: the pair was swapped in - and is not this thread's any more -
    //! or the worker is quitting. True to look again.
    bool park(int64_t x, uint32_t epoch) {
        m_swapAt.store(x, std::memory_order_relaxed);
        m_parkEpoch.store(epoch, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lk(m_mutex);
        m_state.store(kParked, std::memory_order_release);
        m_done.notify_all();
        m_wake.wait(lk, [this] {
            return m_quit || m_state.load(std::memory_order_acquire) != kParked;
        });
        if (m_quit) return false;
        return m_state.load(std::memory_order_acquire) != kReady;
    }

    declick::Channel *  m_ch;        //!< the warm pair: the worker's while busy
    declick::WarmState  m_saved[kChannels];
    std::vector<short>  m_ahead;     //!< the opening, interleaved, as the song holds it
    std::vector<double> m_scratch;   //!< one slice, as the core takes it

    // The caller's thread's.
    size_t m_want = 0;               //!< Config::warmSeconds() in frames
    size_t m_cap = 0;                //!< ... and kWarmCopyBeyondSec more: the copy's size
    int64_t m_margin = 0;            //!< kWarmParkSec in frames
    size_t m_pos = 0;                //!< frames of the opening copied
    int    m_failures = 0;
    bool   m_ended = false;          //!< the record stopped reading before m_cap
    bool   m_usable = false;
    bool   m_enabled = true;
    bool   m_pending = false;        //!< a new record, waiting for the worker to stop
    uint32_t m_epoch = 0;            //!< restarts, counted; the live run's in m_run
    int64_t m_swapIn = -1, m_swapOut = -1;

    // Shared. m_frames is written before the hand-off and read after it;
    // m_swapAt and m_parkEpoch before kParked, read after it.
    size_t m_frames = 0;
    std::atomic<int>  m_state { kIdle };
    std::atomic<bool> m_warm { false };   //!< m_saved holds this record's pass
    std::atomic<bool> m_cancel { false };
    std::atomic<size_t> m_copied { 0 };   //!< m_pos, for the worker
    //! The live run: its epoch << 32, kCold if it started cold, the song frame
    //! it started from.
    std::atomic<uint64_t> m_run { 0 };
    std::atomic<int64_t>  m_liveIn { 0 }; //!< the live run's input, as of its last slice
    std::atomic<int64_t>  m_swapAt { -1 };
    std::atomic<uint32_t> m_parkEpoch { 0 };
    std::atomic<uint64_t> m_passes { 0 };
    std::atomic<uint64_t> m_swaps { 0 };
    std::atomic<int64_t>  m_lastUs { 0 };
    std::atomic<int64_t>  m_chaseUs { 0 }, m_chaseFrames { 0 };

    std::thread m_thread;
    std::mutex  m_mutex;
    std::condition_variable m_wake, m_done;
    bool m_quit = false;             //!< under m_mutex
};

} // namespace vdj

#endif // VDJ_DECLICK_WARM_H
