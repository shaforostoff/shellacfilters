/* ========================================
 *  foo_dsp_declick - foobar2000 DSP wrapper
 * ======================================== */

#include "stdafx.h"

#include "declick_core.h"
#include "declick_preset.h"
#include "resource.h"

#include <memory>
#include <mutex>
#include <vector>

using declick::Channel;
using declick::Config;
using declick::Params;
using declick::scoped_flush_denormals;   // FTZ; lives in the core so that every
                                         // port of it rounds the same way

namespace {

// ---------------------------------------------------------------------------

class dsp_declick : public dsp_impl_base_t<dsp_v3> {
public:
    dsp_declick(const dsp_preset & in) {
        m_pending = declick_preset::parse(in);
        m_active = m_pending;
        m_cfg.compute(m_active, 44100.0);
    }

    static GUID g_get_guid() { return declick_preset::guid(); }

    static void g_get_name(pfc::string_base & out) { out = "Declick (AR interpolation)"; }

    static bool g_get_default_preset(dsp_preset & out) {
        declick_preset::make(Params::defaults(), out);
        return true;
    }

    static bool g_have_config_popup() { return true; }

    static void g_show_config_popup(const dsp_preset & data, HWND parent,
                                    dsp_preset_edit_callback & callback) {
        declick_config_popup(data, parent, callback);
    }

    // -- dsp_v3 --------------------------------------------------------------

    bool apply_preset(const dsp_preset & preset) override {
        if (preset.get_owner() != g_get_guid()) return false;
        const Params p = declick_preset::parse(preset);
        std::lock_guard<std::mutex> lock(m_lock);
        m_pending = p;
        return true;
    }

    // -- dsp_impl_base -------------------------------------------------------

    bool on_chunk(audio_chunk * chunk, abort_callback &) override {
        const unsigned channels = chunk->get_channels();
        const unsigned rate = chunk->get_sample_rate();
        const t_size frames = chunk->get_sample_count();
        if (channels == 0 || rate == 0 || frames == 0) return true;

        // Before anything else, so that no part of the previous record reaches
        // this one: neither the audio still sitting in the window nor what the
        // model measured from it.
        updateTrack();

        Params params;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            params = m_pending;
        }

        const unsigned config = chunk->get_channel_config();
        if (channels != m_channels || rate != m_rate) {
            // Only the format builds a new pipeline. Everything else moves the
            // one already running - see applyParams().
            if (!rebuild(channels, rate, config, params)) return true;
        } else {
            // Nothing here is sized by the channel layout, so a change of it
            // only has to reach the chunks emit() labels.
            m_config = config;
            if (params != m_active) applyParams(params, rate);
        }

        audio_sample * const data = chunk->get_data();
        if (data == NULL) return true;

        {
            scoped_flush_denormals ftz;
            for (unsigned c = 0; c < channels; ++c) {
                m_chan[c]->push(data + c, frames, channels);
            }
        }
        m_owed += frames;

        return emit();
    }

    void on_endofplayback(abort_callback &) override {
        finishTrack();
        m_track.release();
    }

    //! Never called: need_track_change_mark() is false and the boundary is
    //! noticed from on_chunk() instead. See updateTrack() for why.
    void on_endoftrack(abort_callback &) override {}

    //! A seek. Everything held is audio the listener is not going to hear, so it
    //! goes, and with it the noise floor measured on the far side.
    void flush() override {
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->reset();
        m_owed = 0;
    }

    double get_latency() override {
        if (m_rate == 0) return 0.0;
        return (double)m_cfg.latency / (double)m_rate;
    }

    bool need_track_change_mark() override { return false; }

private:
    //! Notices the track changing under us, which is the only place the per-track
    //! handling starts.
    //!
    //! A DSP can ask to be told about the boundary directly, through
    //! need_track_change_mark() and on_endoftrack(), but that force-flushes every
    //! DSP placed ahead of this one - the SDK calls it out as a way to break
    //! gapless playback - and get_cur_file() answers the same question for
    //! nothing. The dehummer reads the boundary the same way, for the same
    //! reason.
    void updateTrack() {
        metadb_handle_ptr track;
        get_cur_file(track);
        if (track.get_ptr() == m_track.get_ptr()) return;
        finishTrack();
        m_track = track;
    }

    //! Runs the tail of a track out and puts the pipeline back to its opening
    //! state. Both halves matter.
    //!
    //! The drain is what keeps the last `latency` samples of a track from being
    //! emitted into the beginning of the next one - the pipeline is read ahead
    //! of what is being heard, so at the moment the last chunk of a track
    //! arrives those samples are still inside it.
    //!
    //! The reset is what keeps each record to itself. The window still holds the
    //! previous track's audio, and `m_scale` and the MAD ring hold the noise
    //! floor measured from it; a transfer with a quieter surface would then be
    //! judged against the louder one for the first second or so of the model
    //! re-converging, and every detection threshold in the core is relative to
    //! that scale.
    //!
    //! A no-op before the first chunk of the first track, when there is neither
    //! a pipeline nor anything owed.
    void finishTrack() {
        if (m_chan.empty()) return;
        drainAndEmit();
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->reset();
    }

    //! Runs the pipeline out and hands what comes back to the caller. Goes
    //! ahead of anything that resets or replaces it, so audio still in flight
    //! leaves rather than being dropped.
    void drainAndEmit() {
        if (m_chan.empty()) return;
        {
            scoped_flush_denormals ftz;
            for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->drain();
        }
        emit();
        m_owed = 0;
    }

    //! How much of what the pipeline has finished is real audio. In the steady
    //! state that is all of it and this is just available().
    //!
    //! It stops being all of it during a drain: those `latency` zeros are fed in
    //! to push the last real samples out, and the model answers with `latency`
    //! samples of which only the first m_owed belong to the track - the rest is
    //! the zeros coming back round. Emitting them would put up to a block of
    //! silence into the middle of a gapless playlist, which is the one thing
    //! draining at a track boundary must not do, so they are left in the ring
    //! for reset() to drop.
    size_t ready() const {
        const size_t avail = m_chan[0]->available();
        return ((uint64_t)avail > m_owed) ? (size_t)m_owed : avail;
    }

    //! Moves whatever the pipeline has finished into the chunk list. Returns
    //! false if the caller's chunk should be dropped (nothing ready yet).
    bool emit() {
        if (m_chan.empty()) return true;
        size_t avail = ready();
        if (avail == 0) return false;      // still filling; drop the input chunk

        const unsigned channels = m_channels;
        while (avail > 0) {
            const size_t n = (avail > 16384) ? 16384 : avail;
            audio_chunk * out = insert_chunk(n * channels);
            if (out == NULL) break;
            out->set_sample_rate(m_rate);
            out->set_channels(channels, m_config);
            out->set_data_size(n * channels);
            out->set_sample_count(n);
            audio_sample * p = out->get_data();
            for (unsigned c = 0; c < channels; ++c) {
                m_chan[c]->pull(p + c, n, channels);
            }
            m_owed -= n;
            avail = ready();
        }
        return false;    // our own chunks carry the audio; drop the original
    }

    //! A parameter move, through the channels already in hand.
    //!
    //! Sensitivity, Extent, Repair depth, Passes and Dry/Wet are read per
    //! block, so retune() swaps them in under the stream with no gap at all.
    //! Model order and Max repair reshape the pipeline and need configure(),
    //! which resets it - but configure() reassigns every vector to the length
    //! the envelope in Config already gave it, so neither route touches the
    //! heap. That is what the envelope is for.
    //!
    //! What must not happen is what this used to do: build fresh Channels and
    //! configure those. Measured stereo at 44.1 kHz that was 38 allocations and
    //! 4.3 MB, one block of it 1.8 MB, inside on_chunk() - the audio thread.
    //! declick_rt_verify covers both routes.
    void applyParams(const Params & params, unsigned rate) {
        Config cfg;
        cfg.compute(params, (double)rate);

        bool retuned = !m_chan.empty();
        for (size_t c = 0; c < m_chan.size(); ++c) {
            if (!m_chan[c]->retune(cfg)) { retuned = false; break; }
        }
        if (!retuned) {
            // configure() below resets, so empty the pipeline first.
            drainAndEmit();
            for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->configure(cfg);
        }

        m_cfg    = cfg;
        m_active = params;
    }

    //! A new format: channel count or sample rate. The only route that is
    //! allowed to allocate, because it is the only one where the buffers
    //! genuinely change size.
    bool rebuild(unsigned channels, unsigned rate, unsigned config,
                 const Params & params) {
        // Push out anything still held before the buffers are rebuilt.
        drainAndEmit();

        Config cfg;
        cfg.compute(params, (double)rate);

        std::vector<std::unique_ptr<Channel> > chans;
        try {
            for (unsigned c = 0; c < channels; ++c) {
                std::unique_ptr<Channel> ch(new Channel());
                ch->configure(cfg);
                chans.push_back(std::move(ch));
            }
        } catch (const std::bad_alloc &) {
            m_chan.clear();
            m_channels = 0; m_rate = 0;
            return false;
        }

        m_chan.swap(chans);
        m_cfg = cfg;
        m_channels = channels;
        m_rate = rate;
        m_config = config;
        m_active = params;
        return true;
    }

    std::mutex m_lock;
    Params m_pending;
    Params m_active;
    Config m_cfg;

    std::vector<std::unique_ptr<Channel> > m_chan;
    unsigned m_channels = 0;
    unsigned m_rate = 0;
    unsigned m_config = 0;

    //! The track whose audio the pipeline is holding, and how much of that audio
    //! has gone in but not yet come out. See updateTrack() and ready().
    metadb_handle_ptr m_track;
    uint64_t m_owed = 0;
};

static dsp_factory_t<dsp_declick> g_dsp_declick_factory;

} // anonymous namespace
