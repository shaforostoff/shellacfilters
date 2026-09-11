/* ========================================
 *  foo_dsp_paraeq - foobar2000 DSP wrapper
 *
 *  The shortest of the three wrappers, because the core asks the least of one:
 *  zero latency, no read-ahead, no detector and nothing sized by the
 *  parameters, so what goes in comes out and on_chunk edits the caller's chunk
 *  in place and keeps it.
 *
 *  There is also no per-track handling, which the other two need and this does
 *  not. Their state is a claim about the record - where the hum sits, what a
 *  click looks like here - and carrying it into the next one would be wrong.
 *  An equaliser's state is a claim about the user, and a gapless join is
 *  continuous audio, so resetting anything at a track boundary would put a step
 *  in the middle of a continuous signal rather than take one out.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_core.h"
#include "paraeq_preset.h"

#include <memory>
#include <mutex>
#include <vector>

using paraeq::Channel;
using paraeq::Config;
using paraeq::Params;
using paraeq::scoped_flush_denormals;   // FTZ; lives in the core so that every
                                        // port of it rounds the same way

namespace {

class dsp_paraeq : public dsp_impl_base_t<dsp_v3> {
public:
    dsp_paraeq(const dsp_preset & in) {
        m_pending = paraeq_preset::parse(in);
        m_active  = m_pending;
        m_cfg.compute(m_active, 44100.0);
    }

    static GUID g_get_guid() { return paraeq_preset::guid(); }

    static void g_get_name(pfc::string_base & out) { out = "Parametric EQ"; }

    static bool g_get_default_preset(dsp_preset & out) {
        paraeq_preset::make(Params::defaults(), out);
        return true;
    }

    static bool g_have_config_popup() { return true; }

    static void g_show_config_popup(const dsp_preset & data, HWND parent,
                                    dsp_preset_edit_callback & callback) {
        paraeq_config_popup(data, parent, callback);
    }

    // -- dsp_v3 --------------------------------------------------------------

    //! Live settings change. Saying yes here is what makes dragging a curve
    //! usable: the alternative is the host destroying this object and building
    //! another for every new value, which would zero the filter state and put a
    //! transient in the output on each one, and would skip the glide entirely
    //! since a fresh Channel starts at its target rather than gliding to it.
    bool apply_preset(const dsp_preset & preset) override {
        if (preset.get_owner() != g_get_guid()) return false;
        const Params p = paraeq_preset::parse(preset);
        std::lock_guard<std::mutex> lock(m_lock);
        m_pending = p;
        return true;
    }

    // -- dsp_impl_base -------------------------------------------------------

    bool on_chunk(audio_chunk * chunk, abort_callback &) override {
        const unsigned channels = chunk->get_channels();
        const unsigned rate     = chunk->get_sample_rate();
        const t_size   frames   = chunk->get_sample_count();
        if (channels == 0 || rate == 0 || frames == 0) return true;

        Params params;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            params = m_pending;
        }

        if (channels != m_channels || rate != m_rate) {
            if (!rebuild(channels, rate, params)) return true;
        } else if (params != m_active) {
            // Nothing in this core is sized by the parameters, so every control
            // move retunes in place: no reallocation, no reset, and the curve
            // glides to the new one over 20 ms rather than switching to it.
            Config cfg;
            cfg.compute(params, (double)rate);
            for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->retune(cfg);
            m_cfg    = cfg;
            m_active = params;
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
    }

    void on_endoftrack(abort_callback &) override {}

    //! A seek. The filter state describes samples that are no longer adjacent
    //! to what comes next, so it goes; the coefficients and where a glide had
    //! got to are the user's settings and stay.
    void flush() override {
        for (size_t c = 0; c < m_chan.size(); ++c) m_chan[c]->flush();
    }

    double get_latency() override { return 0.0; }

    bool need_track_change_mark() override { return false; }

private:
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
        m_cfg      = cfg;
        m_channels = channels;
        m_rate     = rate;
        m_active   = params;
        return true;
    }

    std::mutex m_lock;
    Params m_pending;   //!< written by apply_preset, read by on_chunk
    Params m_active;    //!< playback thread only
    Config m_cfg;

    std::vector<std::unique_ptr<Channel> > m_chan;
    unsigned m_channels = 0;
    unsigned m_rate = 0;
};

static dsp_factory_t<dsp_paraeq> g_dsp_paraeq_factory;

} // anonymous namespace
