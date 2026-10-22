/* ========================================
 *  Declick - the VirtualDJ engine
 *
 *  Sliders in, declick::Channel out. Everything VirtualDJ-shaped is here and
 *  nothing algorithmic is: the maths lives in declick_core.{h,cpp}, which is a
 *  verbatim copy of the file foo_dsp_declick builds - see the note at the top
 *  of that header, and ../foobar2000_dsp/scripts/sync_cores.ps1 for what keeps
 *  the copies from drifting.
 *
 *  The slider mappings are the ones the VST2 port uses, constant for constant.
 *  That is not tidiness: a slider position has to mean the same thing in every
 *  port or the measurements in the foobar2000 README stop describing any of
 *  them, and dehum_vst_verify / declick_vst_verify exist to pin exactly that.
 * ======================================== */

#ifndef VDJ_DECLICK_ENGINE_H
#define VDJ_DECLICK_ENGINE_H

#include "vdjPlugin8.h"

#include "declick_core.h"
#include "declick_vdj_warm.h"
#include "vdj_engine.h"

#include <stdio.h>

#include <new>

namespace vdj {

class DeclickEngine {
public:
    // -- identity ------------------------------------------------------------

    static const char * pluginName() { return "Declick"; }
    static const char * pluginVersion() { return "1.1.1"; }
    static const char * pluginDescription() {
        return "Autoregressive detect-and-interpolate declicker for shellac and "
               "vinyl transfers. Reads the song ahead of the play head, so the "
               "repair gets the lookahead it needs and the deck stays in time - "
               "no delay at all. Learns the record's surface from its first "
               "seconds in the background, switches the first play over to it "
               "a second or two in, and starts every cue, seek and loop with "
               "it. Works on a loaded song, not on a live input.";
    }

    // -- parameters ----------------------------------------------------------

    enum {
        kSensitivity = 1, kExtent, kMaxLength, kDepth, kPasses, kOrder, kDryWet,
        kWarmStart
    };

    void declareParameters(IVdjPlugin8 & host) {
        host.DeclareParameterSlider(&m_a, kSensitivity, "Sensitivity", "Sensitv", kDefA);
        host.DeclareParameterSlider(&m_b, kExtent,      "Extent",     "Extent",  kDefB);
        host.DeclareParameterSlider(&m_c, kMaxLength,   "Max repair", "MaxLen",  kDefC);
        host.DeclareParameterSlider(&m_d, kDepth,       "Depth",      "Depth",   kDefD);
        host.DeclareParameterSlider(&m_e, kPasses,      "Passes",     "Passes",  kDefE);
        host.DeclareParameterSlider(&m_f, kOrder,       "Model order", "Order",  kDefF);
        host.DeclareParameterSlider(&m_g, kDryWet,      "Dry/Wet",    "Dry/Wet", kDefG);
        // Last, so the seven sliders keep the ids their saved positions are
        // filed under. See declick_vdj_warm.h.
        host.DeclareParameterSwitch(&m_warmOn, kWarmStart, "Warm start", "Warm", true);
    }

    bool parameterString(int id, char * out, int size) const {
        const declick::Params p = params();
        switch (id) {
            case kSensitivity: snprintf(out, size, "%.0f %%", pinParameter(m_a) * 100.0); return true;
            case kExtent:      snprintf(out, size, "%.0f %%", pinParameter(m_b) * 100.0); return true;
            case kMaxLength:   snprintf(out, size, "%.1f ms", (double)p.maxLengthMs);     return true;
            case kDepth:       snprintf(out, size, "%.0f %%", pinParameter(m_d) * 100.0); return true;
            case kPasses:      snprintf(out, size, "%d", p.passes);                       return true;
            case kOrder:       snprintf(out, size, "%d", p.order);                        return true;
            case kDryWet:      snprintf(out, size, "%.0f %%", pinParameter(m_g) * 100.0); return true;
            case kWarmStart:   snprintf(out, size, "%s", m_warmOn ? "on" : "off");        return true;
            default: return false;
        }
    }

    //! Sliders to core units: the core's own mapping, as the VST2 and AU
    //! ports'.
    declick::Params params() const {
        const float s[7] = { m_a, m_b, m_c, m_d, m_e, m_f, m_g };
        return declick::Params::fromSliders(s);
    }

    // -- pipeline ------------------------------------------------------------

    //! Allocates. The only call here that can, and only the first time at a
    //! given sample rate: see the buffer envelope note in declick::Config.
    bool setRate(double rate) {
        try {
            rebuild(rate, params());
        } catch (const std::bad_alloc &) {
            m_configured = false;
            return false;
        }
        return true;
    }

    //! True if the pipeline was rebuilt, i.e. the audio is discontinuous here.
    bool update(double rate) {
        m_warm.setEnabled(m_warmOn != 0);
        const declick::Params p = params();
        if (m_configured && rate == m_rate && p == m_active) return false;

        declick::Config next;
        next.compute(p, rate);

        // Sensitivity, Extent, Depth, Passes and Dry/Wet need the same buffers,
        // so they go in live and the audio does not break. Max repair and Model
        // order resize the pipeline and cannot.
        if (m_configured && rate == m_rate && next.structurallyEquals(m_cfg)
            && m_chan[0].retune(next) && m_chan[1].retune(next)) {
            m_cfg = next;
            m_active = p;
            return false;
        }

        try {
            rebuild(rate, p);
        } catch (const std::bad_alloc &) {
            m_configured = false;
        }
        return true;
    }

    //! A new record: the warm start reads its opening afresh.
    void reset() {
        m_warm.newTrack();
        if (!m_configured) return;
        m_chan[0].reset();
        m_chan[1].reset();
    }

    //! A seek, a cue, a loop, a scratch out of the cache. The audio path has
    //! nothing worth keeping across it - the window is the previous audio and
    //! the noise estimate was measured from it - but what the warm pass learned
    //! from this record's opening is still true of the record, so the restart
    //! starts with it once the pass has finished: see declick_vdj_warm.h.
    void discontinuity(int64_t from) {
        if (!m_configured) return;
        m_warm.start(m_chan, from);
    }

    //! The warm start's catch-up: a cold run is swapped for a warm pair that
    //! has taken the same audio, at the frame holdAt() names.
    int64_t holdAt() const { return m_configured ? m_warm.holdAt() : -1; }
    void reached(int64_t in, int64_t out) {
        if (!m_configured) return;
        if (m_warm.reached(in, out, m_chan)) {
            // The pair was configured with the config of the last rebuild;
            // a slider moved since went to the other one live.
            m_chan[0].retune(m_cfg);
            m_chan[1].retune(m_cfg);
        }
    }

    int lookahead() const { return m_configured ? m_cfg.latency : 0; }

    //! What a restart has to run before the audio anyone hears, and the figure
    //! is not a guess: every threshold in the detector is relative to the robust
    //! noise scale, and that is measured over madWindow samples - 30 ms, 1323 at
    //! 44.1 kHz. Cold, the scale starts at 1e-6 and the first block or so is
    //! judged against nothing.
    //!
    //! The model itself needs no warming: fitModel() refits from the window
    //! every block, so it is as good as the window is full, and the window is
    //! full as soon as the pipeline has produced anything at all.
    int warmupFrames() const { return m_configured ? m_cfg.madWindow : 0; }

    void push(const double * in, size_t frames) {
        if (!m_configured) return;
        declick::scoped_flush_denormals ftz;
        m_chan[0].push(in + 0, frames, kChannels);
        m_chan[1].push(in + 1, frames, kChannels);
    }

    size_t available() const { return m_configured ? m_chan[0].available() : 0; }

    void pull(double * out, size_t frames) {
        if (!m_configured) return;
        declick::scoped_flush_denormals ftz;
        m_chan[0].pull(out + 0, frames, kChannels);
        m_chan[1].pull(out + 1, frames, kChannels);
    }

    //! The warm start's reading of the record's opening, a little per served
    //! buffer. The noise floor converges in 30 ms and needs no scouting; the
    //! carpet key and the crackle floor take seconds, and do.
    void scout(SongSource & src, size_t served) { m_warm.feed(src, served); }

    //! The warm start's state, for the tests and the benchmark.
    DeclickWarm & warmStart() { return m_warm; }

private:
    void rebuild(double rate, const declick::Params & p) {
        declick::Config next;
        next.compute(p, rate);
        m_chan[0].configure(next);
        m_chan[1].configure(next);
        m_cfg = next;
        m_active = p;
        m_rate = rate;
        m_configured = true;
        // What the pass learned depends on the config. Failing to get a warm
        // start is not failing to get a declicker: restarts go cold.
        m_warm.configure(next, rate);
    }

    // Slider positions for the calibrated defaults, the same ones the VST2 port
    // opens on. Named because two of them are not the round number they look
    // like: MaxLen is linear in milliseconds and Order in eights.
    static constexpr float kDefA = 0.9f;   // Sensitivity -> trigger at 2.85 sigma
    static constexpr float kDefB = 0.5f;   // Extent
    static constexpr float kDefC = 0.2f;   // Max repair -> 4.0 ms
    static constexpr float kDefD = 1.0f;   // Depth -> outright replacement
    static constexpr float kDefE = 0.5f;   // Passes -> 2
    static constexpr float kDefF = 0.4677f; // Model order -> 128
    static constexpr float kDefG = 1.0f;   // Dry/Wet

    // VirtualDJ writes straight through the pointers DeclareParameterSlider was
    // given, from whichever thread moved the control, so these are read once at
    // the top of each processing call and not again.
    float m_a = kDefA, m_b = kDefB, m_c = kDefC, m_d = kDefD;
    float m_e = kDefE, m_f = kDefF, m_g = kDefG;
    int   m_warmOn = 1;

    //! Two pairs: the live one, which m_chan points at, and the warm start's.
    //! They change places when the warm start swaps a caught-up pair in.
    declick::Channel m_pair[2][kChannels];
    declick::Channel * m_chan = m_pair[0];
    DeclickWarm      m_warm { m_pair[1] };
    declick::Config  m_cfg;
    declick::Params  m_active = declick::Params::defaults();
    double m_rate = 0.0;
    bool   m_configured = false;
};

} // namespace vdj

#endif // VDJ_DECLICK_ENGINE_H
