/* ========================================
 *  foo_dsp_declick - foobar2000 DSP wrapper
 * ======================================== */

#include "stdafx.h"

#include "declick_core.h"
#include "declick_preset.h"
#include "resource.h"
#include "warm_hold.h"

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using declick::Channel;
using declick::Config;
using declick::Params;
using declick::scoped_flush_denormals;   // FTZ; lives in the core so that every
                                         // port of it rounds the same way

namespace {

// ---------------------------------------------------------------------------
// Advanced Preferences > Playback > Declick. Machine settings rather than
// preset ones: whether the warm start's stall at a track start is worth it
// depends on the CPU, not on the record. Read when the DSP is created, so a
// change takes effect the next time playback starts.

// {A1184A46-0832-4206-80F5-235F0F58D6E8}
const GUID guid_adv_branch =
    { 0xa1184a46, 0x0832, 0x4206, { 0x80, 0xf5, 0x23, 0x5f, 0x0f, 0x58, 0xd6, 0xe8 } };
// {B2FBBE48-B460-4CCC-AA8A-E31CF3DAB734}
const GUID guid_adv_warm =
    { 0xb2fbbe48, 0xb460, 0x4ccc, { 0xaa, 0x8a, 0xe3, 0x1c, 0xf3, 0xda, 0xb7, 0x34 } };
// {866972D9-81FC-47D5-BA06-550D23ACA75C}
const GUID guid_adv_warm_log =
    { 0x866972d9, 0x81fc, 0x47d5, { 0xba, 0x06, 0x55, 0x0d, 0x23, 0xac, 0xa7, 0x5c } };

advconfig_branch_factory g_adv_branch("Declick", guid_adv_branch,
                                      advconfig_branch::guid_branch_playback, 0);
advconfig_checkbox_factory g_adv_warm(
    "Warm start: learn the first seconds of a track or seek before playing them",
    guid_adv_warm, guid_adv_branch, 0, true);
advconfig_checkbox_factory g_adv_warm_log(
    "Log each warm start's stall to the console",
    guid_adv_warm_log, guid_adv_branch, 1, false);

// ---------------------------------------------------------------------------

class dsp_declick : public dsp_impl_base_t<dsp_v3> {
public:
    dsp_declick(const dsp_preset & in) {
        m_pending = declick_preset::parse(in);
        m_active = m_pending;
        m_cfg.compute(m_active, 44100.0);
        m_warmOn = g_adv_warm.get();
        m_warmLog = g_adv_warm_log.get();
    }

    static GUID g_get_guid() { return declick_preset::guid(); }

    static void g_get_name(pfc::string_base & out) { out = "Declick (AR interpolation)"; }

    static bool g_get_default_preset(dsp_preset & out) {
        declick_preset::make(Params::defaults(), out);
        return true;
    }

#ifdef _WIN32
    static bool g_have_config_popup() { return true; }

    static void g_show_config_popup(const dsp_preset & data, HWND parent,
                                    dsp_preset_edit_callback & callback) {
        declick_config_popup(data, parent, callback);
    }
#else
    static bool g_have_config_popup() { return true; }

    //! foobar2000 for Mac asks for a view controller to embed in a window of
    //! its own rather than running a modal dialog; `parent` is not needed.
    static service_ptr g_show_config_popup(fb2k::hwnd_t, dsp_preset_edit_callback_v2::ptr callback) {
        return declick_config_view(callback);
    }
#endif

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

        // Straight into the pipeline, or held for the warm start; see
        // WarmHold. Either way our own chunks carry the audio and the
        // original is dropped.
        {
            scoped_flush_denormals ftz;
            const uint64_t passes = m_hold.warmPasses();
            m_hold.feed(data, frames, Emitter(this));
            if (m_hold.warmPasses() != passes) logWarm();
        }
        return false;
    }

    void on_endofplayback(abort_callback &) override {
        finishTrack();
        m_track.release();
    }

    //! Never called: need_track_change_mark() is false and the boundary is
    //! noticed from on_chunk() instead. See updateTrack() for why.
    void on_endoftrack(abort_callback &) override {}

    //! A seek. Everything held is audio the listener is not going to hear, so it
    //! goes, and with it the noise floor measured on the far side. What comes
    //! next is warmed like a track's start.
    void flush() override {
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->reset();
        m_owed = 0;
        startHold();
    }

    //! The pipeline's, and while the warm start holds or hands back the
    //! track's first seconds, those too.
    double get_latency() override {
        if (m_rate == 0) return 0.0;
        return (double)((uint64_t)m_cfg.latency + m_hold.pending()) / (double)m_rate;
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
        startHold();
    }

    //! The warm start begins at every point the pipeline starts afresh: a
    //! track's start, a seek, a new format. Not at a Model order or Max
    //! repair move, which resets it too: that is someone at the controls,
    //! mid-song, and a stall there would be a dropout for nothing.
    void startHold() {
        if (m_warmOn) m_hold.arm(); else m_hold.disarm();
    }

    //! What WarmHold calls after each push into the pipeline: count it as
    //! owed, and emit whatever is ready, so that no push between two pulls
    //! is longer than the core's output ring is sized for.
    struct Emitter {
        dsp_declick * self;
        explicit Emitter(dsp_declick * s) : self(s) {}
        void operator()(size_t frames) {
            self->m_owed += frames;
            self->emit();
        }
    };

    //! Pushes whatever the warm start is still holding. Ahead of anything that
    //! drains the pipeline, so the held audio is not left behind.
    void settleHold() {
        if (m_chan.empty()) return;
        scoped_flush_denormals ftz;
        const uint64_t passes = m_hold.warmPasses();
        m_hold.settle(Emitter(this));
        if (m_hold.warmPasses() != passes) logWarm();
    }

    //! Off by default: printing allocates, and this is the audio thread.
    void logWarm() {
        if (!m_warmLog) return;
        FB2K_console_formatter() << "Declick: warm start, "
            << pfc::format_float((double)m_hold.lastWarmFrames() / (double)m_rate, 0, 2)
            << " s x " << m_channels << " ch on " << m_hold.threads() << " thread(s), "
            << pfc::format_float(m_hold.lastWarmMs(), 0, 0) << " ms";
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
    //!
    //! A track that ends while the warm start still holds its opening - one
    //! shorter than warmSeconds(), or a seek into its last seconds - is warmed
    //! on what there is and pushed whole, here.
    void finishTrack() {
        if (m_chan.empty()) return;
        settleHold();
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
    //! genuinely change size - the warm start's hold among them, and its
    //! threads.
    bool rebuild(unsigned channels, unsigned rate, unsigned config,
                 const Params & params) {
        // Push out anything still held before the buffers are rebuilt.
        settleHold();
        drainAndEmit();

        Config cfg;
        cfg.compute(params, (double)rate);

        std::vector<std::unique_ptr<Channel> > chans;
        std::vector<Channel *> ptrs;
        try {
            for (unsigned c = 0; c < channels; ++c) {
                std::unique_ptr<Channel> ch(new Channel());
                ch->configure(cfg);
                ptrs.push_back(ch.get());
                chans.push_back(std::move(ch));
            }
        } catch (const std::bad_alloc &) {
            m_hold.clear();
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

        // A thread per channel for the warm pass, as far as the cores go: on a
        // two-core machine, stereo warms in half the time.
        unsigned threads = std::thread::hardware_concurrency();
        if (threads < 1) threads = 1;
        try {
            m_hold.configure(&ptrs[0], channels, cfg, (double)rate, threads);
        } catch (const std::bad_alloc &) {
            // Without its ring there is no warm start in this format, and the
            // hold passes straight through; without the channels, nothing.
            if (m_hold.channels() != channels) {
                m_hold.clear();
                m_chan.clear();
                m_channels = 0; m_rate = 0;
                return false;
            }
        }
        startHold();
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

    //! The warm start, over m_chan; after it, so that its threads are gone
    //! before the channels are. Off: it passes straight through.
    declick::WarmHold<audio_sample> m_hold;
    bool m_warmOn = true;
    bool m_warmLog = false;
};

static dsp_factory_t<dsp_declick> g_dsp_declick_factory;

} // anonymous namespace
