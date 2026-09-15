/* ========================================
 *  vdjplugin - IVdjPluginBufferDsp8 wrapper
 *
 *  The readahead path, and the answer to "does VirtualDJ let a plug-in read the
 *  file ahead of the play head the way foobar2000 does".
 *
 *  It does, and more directly. foobar2000's DSP is handed chunks in order and a
 *  component that wants to be ahead has to arrange to be fed early; VirtualDJ
 *  turns it inside out. A buffer plug-in is *asked* for the song:
 *
 *      short * OnGetSongBuffer(int pos, int nb);
 *
 *  and it may call GetSongBuffer(pos, nb, &buf) for any position it likes to
 *  get the decoded audio. So the lookahead Declick needs is not a delay to be
 *  declared and compensated - it is two reads instead of one. Asked for
 *  [pos, pos+nb) we read [pos, pos+nb+latency) and hand back audio that is
 *  aligned with what was asked for, no delay at all. That is exactly the recipe
 *  in the READ AHEAD note on declick::Channel::prime(), which the foobar2000
 *  component cannot use and this one can.
 *
 *  Four things follow from the interface that are worth stating before the code.
 *
 *  1. It is 16 bit. GetSongBuffer hands back short*, so VirtualDJ's decoded
 *     song cache is 16 bit PCM and so is what we give back. That is the format
 *     78 rpm and vinyl transfers arrive in anyway, and see toShorts() in
 *     vdj_engine.h for why the way back is rounded rather than dithered.
 *
 *  2. The buffer we return must be ours. The pointer GetSongBuffer gives out
 *     is into the deck's own decoded audio - the same samples the waveform, the
 *     beatgrid and every other deck read - so processing in place there would
 *     corrupt the song rather than the playback. Everything below writes into
 *     m_serve.
 *
 *  3. `pos` is not monotonic. It follows the deck, so scratching, seeking, cue
 *     juggling and loops all arrive as jumps, forwards and backwards, at audio
 *     rate. Both cores are sequential - Declick's window and Dehum's detector
 *     are both history - so a jump cannot simply be processed where it lands.
 *     Hence the cache: recently produced output is kept, position-addressed, so
 *     scratching about inside it is free and, more to the point, *consistent* -
 *     a sample heard twice sounds the same both times. Only a jump out of the
 *     cache restarts the pipeline.
 *
 *  4. It is in the song's own timebase. IVdjPluginBufferDsp8::SampleRate is
 *     documented as "samplerate of the song", and pos counts song samples, so
 *     the pitch fader and the timestretcher are downstream of here. A record
 *     played at +8% is declicked at its own rate, on its own samples, which is
 *     the only place the maths is calibrated for.
 * ======================================== */

#ifndef VDJ_BUFFER_DSP_H
#define VDJ_BUFFER_DSP_H

#include "vdjDsp8.h"

#include "vdj_engine.h"
#include "vdj_trace.h"

#include <stdint.h>

#include <algorithm>
#include <new>
#include <string.h>
#include <vector>

namespace vdj {

// ---------------------------------------------------------------------------

//! How far forward the pipeline will be run to catch up with a jump rather than
//! restarting at it.
const double kMaxGapSec = 0.25;

//! A restart runs the engine over Engine::warmupFrames() before the audio that
//! was asked for, so the core is settled by the time anyone hears it. That
//! figure comes from the engine rather than from here because only the engine
//! knows it - Declick's is its robust noise estimate window, Dehum's is nothing
//! at all - and a number invented at this level was the first half of a real
//! bug. See the note on kRestartCooldownSec.
//!
//! kRestartCooldownSec is the second half. A warm-up is worth paying once when
//! the stream genuinely moves; it is not worth paying when something is
//! restarting the pipeline over and over, because then it is the whole cost and
//! none of the benefit. So a restart that follows another within this much
//! *produced* audio gets no warm-up at all.
//!
//! What made both of these necessary: `pos` does not come from one consumer.
//! Playback asks for its block at the play head, and something else - VirtualDJ
//! scanning the song, or another buffer plug-in upstream reading ahead of it,
//! including Dehum's own scout in dehum_vdj_scout.h - asks for a quite
//! different part of it in between. Every one of those alien reads is a cache
//! miss, so it restarts the pipeline; the playback read that follows it is then
//! a miss too, and restarts it again. Two restarts per audio buffer.
//!
//! With a warm-up of 0.25 s invented here, that was 22000 frames of Declick per
//! 512 frames of audio - 43 times the necessary work, which stutters. With the
//! engine's own figure (1323 frames at 44.1 kHz) and this cooldown it is about
//! 1400, and the alien reads cost roughly what the audio they actually read is
//! worth. Measured against a VirtualDJ 2025 log: Dehum Buffer's scout reading
//! through Declick Buffer, 4096 frames at a time from the start of the record
//! while the deck played from the middle of it.
const double kRestartCooldownSec = 1.0;

//! How much finished output is kept. Scratching inside this is free; leaving it
//! costs a restart. 4 s is about two beats either side of the play head at any
//! tempo a DJ is likely to be scratching at, and 4 s of 16 bit stereo is 700 kB.
const double kCacheSec = 4.0;

//! Input read, and output produced, per pass of the inner loop. Sized so that
//! it comfortably clears a device block without being large enough for the
//! scratch buffers to fall out of cache.
//!
//! kMinPushFrames is the floor on one slice, which is what guarantees the loop
//! in serve() makes progress even when the arithmetic says no more input is
//! owed. It is a block of Declick's analysis hop and change; anything at least
//! that big brings the next emission out.
enum { kSliceFrames = kMaxSliceFrames, kMinPushFrames = 512 };

// ---------------------------------------------------------------------------

//! Finished output, addressed by song position. A plain ring: capacity is fixed
//! at configure() time and never allocates again, and the oldest frames are
//! dropped as new ones arrive.
class OutputCache {
public:
    void reserve(size_t frames) {
        m_ring.assign(frames * kChannels, 0);
        m_cap = frames;
        m_start = m_end = 0;
    }

    void restart(int64_t pos) { m_start = m_end = pos; }

    int64_t start() const { return m_start; }
    int64_t end() const { return m_end; }
    size_t  capacity() const { return m_cap; }
    bool    holds(int64_t pos, int64_t frames) const {
        return pos >= m_start && pos + frames <= m_end;
    }

    void append(const short * src, size_t frames) {
        if (m_cap == 0) return;
        // A slice longer than the whole cache can only be the tail of it; skip
        // the part that would be evicted before it was ever readable.
        if (frames > m_cap) {
            src += (frames - m_cap) * kChannels;
            m_end += (int64_t)(frames - m_cap);
            frames = m_cap;
        }
        size_t off = (size_t)(m_end % (int64_t)m_cap);
        size_t first = std::min(frames, m_cap - off);
        memcpy(&m_ring[off * kChannels], src, first * kChannels * sizeof(short));
        if (first < frames) {
            memcpy(&m_ring[0], src + first * kChannels,
                   (frames - first) * kChannels * sizeof(short));
        }
        m_end += (int64_t)frames;
        if (m_end - m_start > (int64_t)m_cap) m_start = m_end - (int64_t)m_cap;
    }

    void read(int64_t pos, size_t frames, short * dst) const {
        if (m_cap == 0) return;
        size_t off = (size_t)(pos % (int64_t)m_cap);
        size_t first = std::min(frames, m_cap - off);
        memcpy(dst, &m_ring[off * kChannels], first * kChannels * sizeof(short));
        if (first < frames) {
            memcpy(dst + first * kChannels, &m_ring[0],
                   (frames - first) * kChannels * sizeof(short));
        }
    }

    size_t heapBytes() const { return m_ring.capacity() * sizeof(short); }

private:
    std::vector<short> m_ring;
    size_t  m_cap = 0;
    int64_t m_start = 0, m_end = 0;
};

// ---------------------------------------------------------------------------

//! Runs an engine over a song that is being read out of order, and keeps the
//! result addressable by position. All of the awkwardness in this file is here;
//! the plug-in class below is thin.
template<class Engine>
class BufferPipeline {
public:
    //! Allocates. Not for the audio thread - but it is called from it anyway
    //! the first time a rate is seen, because a VirtualDJ plug-in is never told
    //! the rate in advance. Everything after that is allocation-free: the cores
    //! size their own buffers from the sample rate alone (see the buffer
    //! envelope note in either Config), so a slider move rebuilds the pipeline
    //! without touching the heap.
    bool configure(Engine & engine, double rate) {
        if (!(rate >= 1000.0)) rate = 44100.0;
        try {
            engine.reset();
            m_maxGap   = (int)(kMaxGapSec * rate);
            m_cooldown = (int64_t)(kRestartCooldownSec * rate);
            m_cache.reserve((size_t)(kCacheSec * rate) + kSliceFrames);
            m_in.assign((size_t)kSliceFrames * kChannels, 0.0);
            m_out.assign((size_t)kSliceFrames * kChannels, 0.0);
            m_stage.assign((size_t)kSliceFrames * kChannels, 0);
            m_serve.assign((size_t)kSliceFrames * kChannels, 0);
        } catch (const std::bad_alloc &) {
            return false;
        }
        m_rate = rate;
        m_primed = false;
        m_produced = 0;
        // Backdated by a whole cooldown so the *first* restart counts as
        // settled and gets its warm-up. It is the one that most needs it -
        // nothing has run at all - and leaving this at zero made a fresh
        // pipeline behave differently from a running one at the same position,
        // which declick_vdj_verify's reproducibility check caught.
        m_lastRestart = -m_cooldown;
        m_restarts = 0;
        return true;
    }

    double rate() const { return m_rate; }
    bool ready() const { return m_rate > 0.0; }

    //! A new record on the deck. Everything learned from the last one goes -
    //! see Engine::reset() against Engine::discontinuity().
    void newTrack(Engine & engine) {
        engine.reset();
        m_primed = false;
    }

    //! The engine was rebuilt under us - a structural parameter moved - so the
    //! next request restarts and warms up rather than splicing the new settings
    //! onto the end of what the old ones produced. What the engine had learned
    //! is its own business: update() has already kept or dropped it.
    void invalidate() { m_primed = false; }

    //! The whole of OnGetSongBuffer. Returns `nb` frames of processed audio at
    //! `pos`, in a buffer this object owns, valid until the next call.
    const short * serve(Engine & engine, SongSource & src, int pos, int nb) {
        if (nb <= 0) return m_serve.data();
        if ((size_t)nb * kChannels > m_serve.size()) {
            // Only reachable if VirtualDJ asks for more in one go than
            // kSliceFrames, which nothing observed does. Grow once and keep it:
            // a request cannot be served out of a buffer too small for it, and
            // it cannot be served out of a cache that would evict the front of
            // it before the back had been produced either.
            try {
                m_serve.assign((size_t)nb * kChannels, 0);
                const size_t need = (size_t)nb
                                  + (size_t)engine.warmupFrames() + kSliceFrames;
                if (m_cache.capacity() < need) m_cache.reserve(need);
            } catch (const std::bad_alloc &) {
                return NULL;
            }
            m_primed = false;   // reserve() empties the ring
        }

        const int64_t want = (int64_t)pos;
        const int64_t wantEnd = want + nb;

        if (!m_primed || want < m_cache.start() || want > m_cache.end() + m_maxGap) {
            restart(engine, want);
        }

        // Everything from the cache's end up to what was asked for has to go
        // through the engine in order, whether or not anybody will hear it.
        //
        // Bounded rather than "until it is done" on purpose: this runs on the
        // audio thread, and a core that had somehow stopped producing would
        // otherwise spin it forever. The bound is what the arithmetic says the
        // loop needs plus slack, so reaching it means something is wrong and a
        // gap in the audio is the right way to find out.
        int guard = (int)((wantEnd - m_cache.end() + engine.lookahead())
                          / (int64_t)kSliceFrames) + 4;
        while (m_cache.end() < wantEnd && guard-- > 0) {
            advance(engine, src, wantEnd);
        }

        if (!m_cache.holds(want, nb)) {
            // Could not be produced - out of memory, or a request so far past
            // the end of the song that even the silence substitution ran out.
            // Silence beats stale audio from somewhere else in the record.
            memset(m_serve.data(), 0, (size_t)nb * kChannels * sizeof(short));
            return m_serve.data();
        }

        m_cache.read(want, (size_t)nb, m_serve.data());

        // Off the critical path: give the engine its look at the song. Dehum
        // uses this to read the opening of the record and find the hum before
        // the detector could have; Declick's is empty.
        engine.scout(src, (size_t)nb);

        return m_serve.data();
    }

    //! Frames the engine has been made to produce, and how many times the stream
    //! has been moved under it. Diagnostics, and what the interleaved-consumer
    //! test measures: produced() against the frames actually served is the ratio
    //! a second consumer inflates, and it was 43 when this was a bug.
    int64_t produced() const { return m_produced; }
    uint64_t restarts() const { return m_restarts; }

    size_t heapBytes() const {
        return m_cache.heapBytes()
             + (m_in.capacity() + m_out.capacity()) * sizeof(double)
             + (m_stage.capacity() + m_serve.capacity()) * sizeof(short);
    }

private:
    //! Start the pipeline again, `m_warmup` frames before the audio that was
    //! asked for so that the cores are settled by the time it matters. The
    //! warm-up output is not thrown away - it goes into the cache like anything
    //! else, which is what makes scratching back over a seek point free.
    void restart(Engine & engine, int64_t pos) {
        // No warm-up if the last restart was recent: see kRestartCooldownSec.
        // m_produced counts output, not calls, so this measures how much audio
        // the pipeline actually got to run before being moved again.
        const bool settled = (m_produced - m_lastRestart) >= m_cooldown;
        const int64_t warm = settled ? (int64_t)engine.warmupFrames() : 0;

        int64_t from = pos - warm;
        if (from < 0) from = 0;
        engine.discontinuity();
        m_inPos = from;
        m_cache.restart(from);
        m_primed = true;
        m_lastRestart = m_produced;
        ++m_restarts;
    }

    //! One slice: read input, push it, take whatever came out, cache it.
    void advance(Engine & engine, SongSource & src, int64_t wantEnd) {
        // How much input is still owed for the output that is still owed. The
        // engine holds lookahead() frames, so the input runs that far ahead of
        // the output - this is the whole of the readahead, and there is no
        // arithmetic about latency anywhere else.
        //
        // It is an upper bound rather than an exact figure: Declick emits in
        // blocks of 512, so the input it actually needs to be ahead by lands
        // somewhere below Config::latency. Over-reading is free - the surplus
        // output goes in the cache and is that much less to do next time -
        // whereas under-reading would stall the loop above, so the floor below
        // guarantees the slice makes progress either way.
        const int64_t needIn = wantEnd + engine.lookahead() - m_inPos;
        size_t frames = (size_t)std::min<int64_t>(
            std::max<int64_t>(needIn, (int64_t)kMinPushFrames), (int64_t)kSliceFrames);

        const short * raw = NULL;
        if (src.read((int)m_inPos, (int)frames, &raw) && raw != NULL) {
            fromShorts(raw, m_in.data(), frames);
        } else {
            // Past the end of the song, or not decoded yet. Zeros are what
            // Channel::drain() feeds for the same purpose: they push the tail
            // of the real audio out of the pipeline instead of stranding it.
            std::fill(m_in.begin(), m_in.begin() + frames * kChannels, 0.0);
        }

        engine.push(m_in.data(), frames);
        m_inPos += (int64_t)frames;

        // Nothing to take on the first slices after a restart, while the engine
        // is still filling its lookahead. m_inPos is monotonic, so the caller's
        // loop gets there.
        for (;;) {
            size_t k = engine.available();
            if (k == 0) break;
            if (k > (size_t)kSliceFrames) k = (size_t)kSliceFrames;
            engine.pull(m_out.data(), k);
            toShorts(m_out.data(), m_stage.data(), k);
            m_cache.append(m_stage.data(), k);
            m_produced += (int64_t)k;
        }
    }

    OutputCache m_cache;
    std::vector<double> m_in, m_out;
    std::vector<short>  m_stage;   //!< engine output, quantised, before caching
    std::vector<short>  m_serve;   //!< what OnGetSongBuffer hands back

    double  m_rate = 0.0;
    int     m_maxGap = 0;
    int64_t m_cooldown = 0;
    int64_t m_inPos = 0;           //!< next song position to be fed in
    bool    m_primed = false;

    //! Diagnostics, and what the interleaved-consumer test measures. m_produced
    //! is every frame the engine has been made to produce, which against the
    //! frames actually served is the ratio the bug in kRestartCooldownSec's note
    //! sent to 43.
    int64_t m_produced = 0;
    int64_t m_lastRestart = 0;
    uint64_t m_restarts = 0;
};

// ---------------------------------------------------------------------------

//! The plug-in itself.
template<class Engine>
class BufferDsp : public IVdjPluginBufferDsp8, public SongSource {
public:
    HRESULT VDJ_API OnLoad() override {
        VDJ_TRACEF("%s: OnLoad", Engine::pluginName());
        m_engine.declareParameters(*this);
        return S_OK;
    }

    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8 * info) override {
        info->PluginName  = Engine::pluginName();
        info->Author      = "ShellacFilters (MIT)";
        info->Description = Engine::pluginDescription();
        info->Version     = "1.0.1";
        info->Flags       = 0x00;
        info->Bitmap      = NULL;
        VDJ_TRACEF("%s: OnGetPluginInfo - registered as \"%s\"",
                   Engine::pluginName(), info->PluginName);
        return S_OK;
    }

    ULONG VDJ_API Release() override { delete this; return 0; }

    HRESULT VDJ_API OnStart() override {
        // Switched on. Whatever is in the pipeline is from the last time it was
        // on, which may be a different record.
        VDJ_TRACEF("%s: OnStart, SampleRate %d", Engine::pluginName(), SampleRate);
        m_pipeline.newTrack(m_engine);
        m_track[0] = 0;
        m_traced = false;
        return S_OK;
    }

    HRESULT VDJ_API OnStop() override { return S_OK; }

    HRESULT VDJ_API OnGetParameterString(int id, char * out, int size) override {
        return m_engine.parameterString(id, out, size) ? S_OK : E_NOTIMPL;
    }

    short * VDJ_API OnGetSongBuffer(int pos, int nb) override {
        // Non-finite input is silenced inside both cores and every buffer here
        // is fixed size, so the only thing that can throw is the first
        // configure() at a new sample rate. Nothing may cross back into
        // VirtualDJ.
        try {
            if (!m_traced) {
                VDJ_TRACEF("%s: first buffer, %d frames at pos %d, %d Hz",
                           Engine::pluginName(), nb, pos, SampleRate);
                m_traced = true;
            }
            const double rate = (SampleRate > 0) ? (double)SampleRate : 44100.0;
            if (!m_pipeline.ready() || rate != m_pipeline.rate()) {
                if (!m_engine.setRate(rate)) return NULL;
                if (!m_pipeline.configure(m_engine, rate)) return NULL;
            }
            if (m_engine.update(rate)) m_pipeline.invalidate();
            checkTrack();

            const short * out = m_pipeline.serve(m_engine, *this, pos, nb);
            return const_cast<short *>(out);
        } catch (...) {
            return NULL;
        }
    }

    // -- SongSource ----------------------------------------------------------

    bool read(int pos, int nb, const short ** out) override {
        if (pos < 0 || nb <= 0) return false;
        short * buf = NULL;
        if (GetSongBuffer(pos, nb, &buf) != S_OK || buf == NULL) return false;
        *out = buf;
        return true;
    }

private:
    //! Notices a new record on the deck.
    //!
    //! There is no callback for it - OnStart and OnStop are about the effect
    //! being switched on, not about the deck being loaded - so this polls the
    //! deck's file path, which is what the SDK forum recommends for exactly
    //! this: call GetStringInfo from OnProcessSamples and compare against the
    //! last value. Once per buffer, not once per sample.
    //!
    //! It matters most to Dehum, whose detector has learned this record's hum
    //! and must not carry it into the next one, and to its scout, which is
    //! per-record by construction. If the query is unsupported the path comes
    //! back empty and this degrades to doing nothing - the pipeline still
    //! restarts on the position jump that a new track produces, so the audio is
    //! correct either way; only the learned state would linger.
    void checkTrack() {
        char path[512];
        path[0] = 0;
        if (GetStringInfo("get_filepath", path, (int)sizeof(path)) != S_OK) return;
        path[sizeof(path) - 1] = 0;
        if (path[0] == 0) return;
        if (strncmp(path, m_track, sizeof(m_track)) == 0) return;

#if defined(_MSC_VER)
        strncpy_s(m_track, path, sizeof(m_track) - 1);
#else
        strncpy(m_track, path, sizeof(m_track) - 1);
        m_track[sizeof(m_track) - 1] = 0;
#endif
        m_pipeline.newTrack(m_engine);
    }

    Engine m_engine;
    BufferPipeline<Engine> m_pipeline;
    char m_track[512] = { 0 };
    //! Only so the trace records the first buffer rather than every buffer.
    bool m_traced = false;
};

} // namespace vdj

#endif // VDJ_BUFFER_DSP_H
