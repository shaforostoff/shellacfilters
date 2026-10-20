/* ========================================
 *  vdjplugin - the shape both wrappers drive
 *
 *  VirtualDJ has two audio plug-in interfaces and this project uses both, so
 *  each DSP core is wrapped once and driven twice. What the two wrappers agree
 *  on is this file: an engine is a stereo push/pull pipeline in double, with a
 *  known lookahead, and it knows how to put its own sliders on the host.
 *
 *  Doubles rather than the floats VirtualDJ hands over, and rather than the
 *  shorts the song buffer holds, because both cores compute in double and every
 *  other port of them - foo_dsp_*, the VST2s, the Audio Unit - converts at the
 *  edge for the same reason: it is the only way the verification harnesses in
 *  ../foobar2000_dsp/tests can compare one port against another to the bit.
 *
 *  Push/pull rather than in-place even though Dehum has no latency and does
 *  work in place, because Declick holds Config::latency samples and the
 *  wrappers should not have to know which core they are carrying. Dehum pays
 *  one memcpy through Fifo below for that, which is nothing next to an FFT.
 * ======================================== */

#ifndef VDJ_ENGINE_H
#define VDJ_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include <cmath>
#include <vector>

namespace vdj {

//! VirtualDJ is stereo everywhere: OnProcessSamples and OnGetSongBuffer both
//! document their buffers as running to [2*nb], and there is no channel count
//! to ask about.
enum { kChannels = 2 };

//! Frames per pass of the inner loop, in both wrappers. It is here rather than
//! in either of them because an engine has to size its own scratch from it: a
//! zero-latency core reaches for Fifo below, and a Fifo that cannot hold a
//! whole slice would drop audio.
enum { kMaxSliceFrames = 4096 };

/*  The engine concept, for reference - it is a template parameter rather than a
 *  base class, because every call below is on the audio thread and none of them
 *  wants a vtable:
 *
 *      static const char * pluginName();      //!< "Declick"
 *      static const char * pluginVersion();   //!< "1.1.0", this plug-in's own
 *                                             //!< and the same as its AU and VST
 *      static const char * pluginDescription();
 *
 *      //! OnLoad. Register the sliders against the host's DeclareParameter*.
 *      void declareParameters(IVdjPlugin8 & host);
 *
 *      //! OnGetParameterString. False for an id it does not own.
 *      bool parameterString(int id, char * out, int size) const;
 *
 *      //! Called at the top of every processing call, because a VirtualDJ
 *      //! plug-in is told about neither the sample rate changing nor a slider
 *      //! moving: it finds out by looking. Returns true if the pipeline had to
 *      //! be rebuilt, i.e. the audio is discontinuous from here.
 *      bool update(double sampleRate);
 *
 *      //! Sample rate changed, or first use. The only call allowed to
 *      //! allocate, and false means it could not.
 *      bool setRate(double sampleRate);
 *
 *      void   reset();          //!< a new record: forget everything
 *      //! A seek: keep what is still true of the record, drop what is only
 *      //! true of the audio that was in flight. The stream starts again at
 *      //! song frame `from`.
 *      void   discontinuity(int64_t from);
 *      int    lookahead() const;              //!< frames held before output
 *
 *      //! Frames a restart should run before the audio that was asked for, so
 *      //! that whatever the core measures from its own history is settled.
 *      //! Only the engine knows this; a figure invented by the caller was half
 *      //! of a real bug - see kRestartCooldownSec in vdj_buffer_dsp.h.
 *      int    warmupFrames() const;
 *      void   push(const double * interleaved, size_t frames);
 *      size_t available() const;
 *      void   pull(double * interleaved, size_t frames);
 *
 *      //! Buffer wrapper only. A song frame the next slice of input may not
 *      //! run past, or -1 for none; and after every slice, once its output
 *      //! has been taken, where the stream stands: input fed up to `in`,
 *      //! output taken up to `out`. Declick's warm start uses the pair to
 *      //! swap a caught-up channel pair in at an exact frame
 *      //! (declick_vdj_warm.h); Dehum has nothing to swap.
 *      int64_t holdAt() const;
 *      void    reached(int64_t in, int64_t out);
 *
 *      //! Buffer wrapper only, once per served buffer, with the number of
 *      //! frames just served so the engine can budget against playback. This
 *      //! is where random access to the song gets used for something other
 *      //! than lookahead - Dehum reads the opening of the record here to find
 *      //! the hum early. Empty for an engine with nothing to learn.
 *      void scout(SongSource & src, size_t servedFrames);
 */

//! Random access to the decoded song, which is what a buffer plug-in has and a
//! live one does not. One virtual call per slice of a few thousand frames, so
//! the vtable costs nothing measurable, and it is what lets the test harness
//! drive the same pipeline from a file without VirtualDJ.
//!
//! `out` receives a pointer VirtualDJ owns: interleaved stereo shorts, `nb`
//! frames, read only. Writing through it would edit the deck's own decoded
//! audio, which is why every wrapper here returns a buffer of its own instead.
//!
//! False means the request could not be served - past the end of the song, or
//! not decoded yet - and the caller substitutes silence.
class SongSource {
public:
    virtual ~SongSource() {}
    virtual bool read(int pos, int nb, const short ** out) = 0;
};

// ---------------------------------------------------------------------------
// Sample conversion at the edges.

//! VirtualDJ's song buffer is 16 bit - GetSongBuffer hands back short* - so the
//! buffer wrappers convert both ways, and the way back is where the interesting
//! decision is. See toShorts().
inline void fromShorts(const short * in, double * out, size_t frames) {
    const size_t n = frames * kChannels;
    for (size_t i = 0; i < n; ++i) out[i] = (double)in[i] * (1.0 / 32768.0);
}

//! Round to nearest, no dither, and that is deliberate.
//!
//! Dither is the right answer when a wider signal is being narrowed. This is
//! not that: the input was already 16 bit, so a sample Declick did not repair
//! divides by 32768 and multiplies back exactly, and rounding returns the
//! original short. Dithering instead would put half an LSB of noise on all of
//! the audio to smooth the quantisation of the fraction of a percent of it that
//! actually changed. Bit-exact pass-through of everything the detector left
//! alone is worth more than that, and it also makes "did this plug-in touch
//! anything" a question a diff can answer.
//!
//! Dehum, which does modify every sample, therefore rounds without dither too.
//! The error that adds sits at the level the material was already quantised to,
//! against a notch that is removing something 40 dB up.
inline void toShorts(const double * in, short * out, size_t frames) {
    const size_t n = frames * kChannels;
    for (size_t i = 0; i < n; ++i) {
        double v = in[i] * 32768.0;
        // NaN cannot reach here - both cores silence non-finite input - but a
        // repair can overshoot, and wrapping a clipped sample to the far rail
        // is a click of its own.
        v = (v > 32767.0) ? 32767.0 : ((v < -32768.0) ? -32768.0 : v);
        out[i] = (short)((v < 0.0) ? (v - 0.5) : (v + 0.5));
    }
}

// ---------------------------------------------------------------------------

//! Fixed-capacity interleaved-stereo ring, so a zero-latency core can present
//! the same push/pull face as one with lookahead.
//!
//! Fixed capacity on purpose: reserve() is the only thing here that touches the
//! heap, so once the wrapper has called it the audio thread cannot allocate.
//! Overrunning it drops the excess rather than growing, and the wrappers never
//! do because they pull after every push.
class Fifo {
public:
    void reserve(size_t frames) {
        m_buf.assign((frames + 1) * kChannels, 0.0);
        m_head = m_count = 0;
    }
    void clear() { m_head = m_count = 0; }

    size_t capacity() const { return m_buf.size() / kChannels; }
    size_t available() const { return m_count; }

    void push(const double * in, size_t frames) {
        const size_t cap = capacity();
        if (cap == 0) return;
        if (frames > cap - m_count) frames = cap - m_count;
        size_t pos = (m_head + m_count) % cap;
        for (size_t f = 0; f < frames; ++f) {
            m_buf[pos * kChannels + 0] = in[f * kChannels + 0];
            m_buf[pos * kChannels + 1] = in[f * kChannels + 1];
            if (++pos == cap) pos = 0;
        }
        m_count += frames;
    }

    void pull(double * out, size_t frames) {
        const size_t cap = capacity();
        if (cap == 0) return;
        if (frames > m_count) frames = m_count;
        for (size_t f = 0; f < frames; ++f) {
            out[f * kChannels + 0] = m_buf[m_head * kChannels + 0];
            out[f * kChannels + 1] = m_buf[m_head * kChannels + 1];
            if (++m_head == cap) m_head = 0;
        }
        m_count -= frames;
    }

    size_t heapBytes() const { return m_buf.capacity() * sizeof(double); }

private:
    std::vector<double> m_buf;
    size_t m_head = 0;
    size_t m_count = 0;
};

//! Slider mapping shared by both cores: two of Dehum's controls, and none of
//! Declick's, have an off position at the bottom rather than a range that
//! reaches zero. Copied from the VST2 wrappers so the ports agree about what
//! any given slider position means.
inline float withOffPosition(float control, float lo, float hi) {
    const float kOffZone = 0.02f;
    if (control <= kOffZone) return 0.0f;
    return lo + ((control - kOffZone) / (1.0f - kOffZone)) * (hi - lo);
}

inline float pinParameter(float v) {
    if (!(v > 0.0f)) return 0.0f;      // catches NaN too
    if (v > 1.0f) return 1.0f;
    return v;
}

} // namespace vdj

#endif // VDJ_ENGINE_H
