/* ========================================
 *  foo_dsp_dehum - foobar2000 DSP wrapper
 *
 *  Much shorter than the declick wrapper, because the core has no latency and
 *  works in place: there is no FIFO, no chunk re-emission and no drain. What
 *  goes in comes out, so on_chunk edits the caller's chunk and keeps it.
 *
 *  The one thing it does keep track of is which track is playing, for two
 *  reasons that pull the same way: each record carries its own hum, so nothing
 *  learned on one may be used on the next, and a file player can read the
 *  opening of the next one ahead of time rather than waiting for the detector
 *  to find the line the slow way. See dehum_scout.h.
 * ======================================== */

#include "stdafx.h"

#include "dehum_core.h"
#include "dehum_preset.h"
#include "dehum_scout.h"
#include "resource.h"

#include <memory>
#include <mutex>
#include <vector>

using dehum::Channel;
using dehum::Config;
using dehum::Params;
using dehum::scoped_flush_denormals;   // FTZ; lives in the core so that every
                                       // port of it rounds the same way

namespace {

// ---------------------------------------------------------------------------

class dsp_dehum : public dsp_impl_base_t<dsp_v3> {
public:
    dsp_dehum(const dsp_preset & in) {
        m_pending = dehum_preset::parse(in);
        m_active = m_pending;
        m_cfg.compute(m_active, 44100.0);
    }

    static GUID g_get_guid() { return dehum_preset::guid(); }

    static void g_get_name(pfc::string_base & out) { out = "Dehum (line detection)"; }

    static bool g_get_default_preset(dsp_preset & out) {
        dehum_preset::make(Params::defaults(), out);
        return true;
    }

#ifdef _WIN32
    static bool g_have_config_popup() { return true; }

    static void g_show_config_popup(const dsp_preset & data, HWND parent,
                                    dsp_preset_edit_callback & callback) {
        dehum_config_popup(data, parent, callback);
    }
#else
    // foobar2000 for Mac asks for a view controller to embed rather than
    // running a modal dialog, and there is no Cocoa one yet - so the host is
    // told there is nothing to show, and never calls the stub below. The SDK's
    // entry template wants the method to exist all the same.
    static bool g_have_config_popup() { return false; }

    static service_ptr g_show_config_popup(fb2k::hwnd_t, dsp_preset_edit_callback_v2::ptr) {
        throw pfc::exception_not_implemented();
    }
#endif

    // -- dsp_v3 --------------------------------------------------------------

    bool apply_preset(const dsp_preset & preset) override {
        if (preset.get_owner() != g_get_guid()) return false;
        const Params p = dehum_preset::parse(preset);
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

        Params params;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            params = m_pending;
        }

        const bool newTrack = updateTrack(params);

        if (channels != m_channels || rate != m_rate) {
            if (!rebuild(channels, rate, params)) return true;
        } else if (params != m_active) {
            // Only the sample rate sizes anything in this core, so every slider
            // move retunes in place: no reallocation, no reset, and no gap in
            // the audio while the user is dragging.
            Config cfg;
            cfg.compute(params, (double)rate);
            bool ok = true;
            for (size_t c = 0; c < m_chan.size(); ++c) {
                if (!m_chan[c]->retune(cfg)) { ok = false; break; }
            }
            if (!ok) {
                if (!rebuild(channels, rate, params)) return true;
            } else {
                m_cfg = cfg;
                m_active = params;
            }
        }

        // After the rebuild, because rebuild() would have thrown it away anyway,
        // and before adopting, because reset() discards adopted lines too.
        if (newTrack) {
            for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->reset();
        }

        if (m_scout) {
            dehum::LineReport lines[dehum::kMaxLines];
            const int found = m_scout->take(lines, (int)dehum::kMaxLines);
            if (found > 0) {
                for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->adopt(lines, found);
            }
        }

        audio_sample * const data = chunk->get_data();
        if (data == NULL) return true;

        {
            scoped_flush_denormals ftz;
            for (unsigned c = 0; c < channels; ++c) {
                m_chan[c]->process(data + c, frames, channels);
            }
        }
        return true;
    }

    void on_endofplayback(abort_callback &) override {
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->reset();
        m_track.release();
        m_scout.reset();
    }

    void on_endoftrack(abort_callback &) override {}

    //! A seek. The lines are kept: the hum on the far side of a seek is the same
    //! hum, and re-acquiring it every time the user moves the playback position
    //! would mean several seconds of it coming back each time.
    void flush() override {
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->flush();
    }

    double get_latency() override { return 0.0; }

    bool need_track_change_mark() override { return false; }

private:
    //! Notices the track changing under us, and is the only place either half of
    //! the per-track handling starts. Returns true on the first chunk of a new
    //! track, which is the caller's cue to forget the previous record's hum.
    //!
    //! get_cur_file() is the earliest a DSP can know what it is playing - there
    //! is no callback ahead of the audio - so the scout starts here rather than
    //! at some tidier moment.
    bool updateTrack(const Params & params) {
        metadb_handle_ptr track;
        get_cur_file(track);

        if (track.get_ptr() == m_track.get_ptr()) return false;
        m_track = track;

        // Cancels and waits for the previous scout. The wait is bounded: the
        // worker polls its abort callback per decoded chunk, and so does the I/O
        // beneath it. Doing it here rather than leaving the thread detached is
        // what keeps the component safe to unload.
        m_scout.reset();

        // A frequency the user pinned by hand is theirs, not ours to overwrite -
        // and adopt() would refuse the lines anyway.
        if (track.is_valid() && !(params.frequency > 0.0f)) {
            const playable_location & loc = track->get_location();
            m_scout = dehum_scout::Scout::start(loc.get_path(),
                                                loc.get_subsong_index(), params);
        }
        return true;
    }

    bool rebuild(unsigned channels, unsigned rate, const Params & params) {
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

    //! Playback thread only, both of them: on_chunk and on_endofplayback are the
    //! only things that touch either.
    metadb_handle_ptr m_track;
    std::shared_ptr<dehum_scout::Scout> m_scout;
};

static dsp_factory_t<dsp_dehum> g_dsp_dehum_factory;

} // anonymous namespace
