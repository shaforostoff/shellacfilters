/* ========================================
 *  WarmHold - the foobar2000 wrapper's warm start
 *
 *  Channel::beginWarm() wants the stream's coming seconds before the stream
 *  itself. foobar2000 gives a DSP no way to read ahead, but it keeps decoding
 *  while the DSP returns nothing - which is what the core's latency already
 *  relies on. So after a track change or a seek this holds the incoming
 *  chunks instead of pushing them; once Config::warmSeconds() are in, or the
 *  track ends first, it runs the warm pass over them, a thread per channel,
 *  and then hands them to the core as the stream, a little at a time: twice
 *  what comes in, per chunk, until nothing is held. The stall before the
 *  track's first sample is then the warm pass alone, and the extra delay
 *  drains away over the next warmSeconds() of playback.
 *
 *  Free of the foobar2000 SDK so that declick_rt_verify drives the very code
 *  the component runs.
 * ======================================== */

#ifndef DECLICK_WARM_HOLD_H
#define DECLICK_WARM_HOLD_H

#include "declick_core.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace declick {

template<typename Sample>
class WarmHold {
public:
    WarmHold() {}
    ~WarmHold() { stopWorkers(); }
    WarmHold(const WarmHold &) = delete;
    void operator=(const WarmHold &) = delete;

    //! A new format: `channels` Channels configured with `cfg` at `rate`. The
    //! only call that allocates - the ring, warmSeconds() of every channel,
    //! and up to `threads` - 1 worker threads, which wait from here to the
    //! next configure() for a warm pass to share. Fewer threads than asked
    //! for if the system will not start them; one does it all on the
    //! caller's thread. The Channels stay the caller's, and must outlive this
    //! or the next configure(). Passes straight through until arm(), and
    //! for good with `warm` false: then it takes no ring and no threads.
    //!
    //! Throws std::bad_alloc. If channels() is `channels` after it did, only
    //! the ring could not be had: there is no warm start, and arm() leaves it
    //! passing straight through. If it is 0, there is nothing to feed.
    void configure(Channel * const * chans, unsigned channels, const Config & cfg,
                   double rate, unsigned threads, bool warm = true) {
        clear();
        m_chan.assign(chans, chans + channels);
        m_ch = channels;
        // As declick_cli --warm-start -1 counts it, so that the two agree.
        m_warm = (size_t)(cfg.warmSeconds() * rate);
        if (m_warm == 0) m_warm = 1;
        m_slice = (size_t)cfg.maxBlock;
        if (!warm) return;
        m_ring.assign(m_warm * channels, Sample());
        if (threads > channels) threads = channels;
        m_workers.reserve(threads > 1 ? threads - 1 : 0);
        for (unsigned w = 1; w < threads; ++w) {
            try {
                m_workers.push_back(std::thread(&WarmHold::work, this, w));
            } catch (const std::system_error &) {
                break;
            }
        }
        m_threads = 1 + (unsigned)m_workers.size();
    }

    //! Back to nothing: no Channels, no ring, no workers. For a caller whose
    //! Channels go away with no new ones to configure() with.
    void clear() {
        stopWorkers();
        m_chan.clear();
        m_ch = 0;
        std::vector<Sample>().swap(m_ring);
        m_warm = 0;
        m_head = m_count = 0;
        m_state = Passing;
    }

    //! Hold what comes from the next feed() on. Whatever was held goes: a
    //! seek's, or a track's the caller has already settle()d.
    void arm() {
        m_head = m_count = 0;
        m_state = m_ring.empty() ? Passing : Holding;
    }

    //! Pass straight through. Whatever was held goes.
    void disarm() {
        m_head = m_count = 0;
        m_state = Passing;
    }

    bool holding() const { return m_state == Holding; }

    //! The Channels it feeds.
    unsigned channels() const { return m_ch; }

    //! Frames taken in and not yet pushed: what the hold adds to the latency.
    size_t pending() const { return m_count; }

    //! Frames the warm pass takes, Config::warmSeconds() at the rate.
    size_t warmFrames() const { return m_warm; }

    //! Threads that share a warm pass, the caller's included.
    unsigned threads() const { return m_threads; }

    //! How many warm passes have run; the last one's frames, and how long it
    //! took - the stall.
    uint64_t warmPasses() const { return m_passes; }
    size_t lastWarmFrames() const { return m_lastFrames; }
    double lastWarmMs() const { return m_lastMs; }

    //! One chunk in, interleaved. Pushes into the Channels whatever the hold
    //! lets go, at most Config::maxBlock frames at a time, and after each
    //! push calls emit(frames) - which must pull everything ready, so that no
    //! push overruns the core's output ring.
    template<class Emit>
    void feed(const Sample * in, size_t frames, Emit && emit) {
        if (m_state == Passing) { push(in, frames, emit); return; }
        if (m_state == Holding) {
            // The ring starts at 0 while holding: the warm pass reads it
            // straight through.
            const size_t k = std::min(frames, m_warm - m_count);
            std::copy(in, in + k * m_ch, m_ring.begin() + (ptrdiff_t)(m_count * m_ch));
            m_count += k;
            if (m_count < m_warm) return;
            warm(m_count);
            m_state = Replaying;
            replay(in + k * m_ch, frames - k, 2 * frames, emit);
            return;
        }
        replay(in, frames, 2 * frames, emit);
    }

    //! The track ends, or the pipeline is about to be rebuilt: warm on what is
    //! held if the warm pass has not run yet, and push all of it.
    template<class Emit>
    void settle(Emit && emit) {
        if (m_state == Holding && m_count > 0) warm(m_count);
        pushHeld(m_count, emit);
        m_head = 0;
        m_state = Passing;
    }

private:
    enum State { Passing, Holding, Replaying };

    template<class Emit>
    void push(const Sample * in, size_t frames, Emit & emit) {
        while (frames > 0) {
            const size_t k = std::min(frames, m_slice);
            for (unsigned c = 0; c < m_ch; ++c) m_chan[c]->push(in + c, k, m_ch);
            emit(k);
            in += k * m_ch;
            frames -= k;
        }
    }

    //! The oldest `frames` held, into the Channels.
    template<class Emit>
    void pushHeld(size_t frames, Emit & emit) {
        while (frames > 0) {
            const size_t k = std::min(frames, m_warm - m_head);
            push(&m_ring[m_head * m_ch], k, emit);
            m_head += k;
            if (m_head == m_warm) m_head = 0;
            m_count -= k;
            frames -= k;
        }
    }

    //! Up to `budget` held frames out, then `rest` in behind what is still
    //! held - or straight through, once nothing is. The budget is twice the
    //! chunk `rest` came in, and `rest` is at most that chunk, so whatever is
    //! still held after it has left room for `rest`.
    template<class Emit>
    void replay(const Sample * rest, size_t frames, size_t budget, Emit & emit) {
        pushHeld(std::min(m_count, budget), emit);
        if (m_count == 0) {
            m_head = 0;
            m_state = Passing;
            push(rest, frames, emit);
            return;
        }
        size_t tail = m_head + m_count;
        if (tail >= m_warm) tail -= m_warm;
        const size_t first = std::min(frames, m_warm - tail);
        std::copy(rest, rest + first * m_ch, m_ring.begin() + (ptrdiff_t)(tail * m_ch));
        std::copy(rest + first * m_ch, rest + frames * m_ch, m_ring.begin());
        m_count += frames;
    }

    //! The warm pass over the first `frames` held, every Channel, shared out
    //! over the threads. The caller's thread holds its own FTZ; see work().
    void warm(size_t frames) {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        m_job = frames;
        if (m_threads > 1) {
            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_busy = m_threads - 1;
                ++m_gen;
            }
            m_go.notify_all();
        }
        share(0);
        if (m_threads > 1) {
            std::unique_lock<std::mutex> lock(m_mx);
            m_done.wait(lock, [this] { return m_busy == 0; });
        }
        ++m_passes;
        m_lastFrames = frames;
        m_lastMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    //! Thread w's Channels: every m_threads'th from w.
    void share(unsigned w) {
        for (unsigned c = w; c < m_ch; c += m_threads) {
            Channel & ch = *m_chan[c];
            ch.beginWarm();
            ch.push(&m_ring[c], m_job, m_ch);
            ch.endWarm();
        }
    }

    //! A worker. FTZ is per thread, and part of the core's numerical contract
    //! (see scoped_flush_denormals): without it here a channel warmed on this
    //! thread would not match one warmed on the caller's.
    void work(unsigned w) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mx);
                m_go.wait(lock, [&] { return m_quit || m_gen != seen; });
                if (m_quit) return;
                seen = m_gen;
            }
            {
                scoped_flush_denormals ftz;
                share(w);
            }
            std::lock_guard<std::mutex> lock(m_mx);
            if (--m_busy == 0) m_done.notify_one();
        }
    }

    void stopWorkers() {
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_quit = true;
        }
        m_go.notify_all();
        for (size_t i = 0; i < m_workers.size(); ++i) m_workers[i].join();
        m_workers.clear();
        m_quit = false;
        m_gen = 0;
        m_threads = 1;
    }

    std::vector<Channel *> m_chan;
    unsigned m_ch = 0;
    size_t   m_warm = 0;      //!< ring length, frames: the warm pass's
    size_t   m_slice = 16384; //!< Config::maxBlock
    std::vector<Sample> m_ring;
    size_t   m_head = 0, m_count = 0;
    State    m_state = Passing;

    std::vector<std::thread> m_workers;
    unsigned m_threads = 1;
    std::mutex m_mx;
    std::condition_variable m_go, m_done;
    uint64_t m_gen = 0;
    unsigned m_busy = 0;
    bool     m_quit = false;
    size_t   m_job = 0;

    uint64_t m_passes = 0;
    size_t   m_lastFrames = 0;
    double   m_lastMs = 0.0;
};

} // namespace declick

#endif // DECLICK_WARM_HOLD_H
