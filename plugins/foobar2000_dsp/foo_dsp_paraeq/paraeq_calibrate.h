/* ========================================
 *  foo_dsp_paraeq - room calibration for the Reverb cut band
 *
 *  What it is for. A room that rings at one frequency makes every record sound
 *  as if it had more reverb than it has, and the Reverb cut band is the tool
 *  for taking that frequency down. Which frequency it is depends on the room,
 *  so it is measured there: music plays through the equaliser, a microphone
 *  - a USB one, or a phone streaming through a virtual input device - is
 *  carried slowly through the middle of the room for a minute or two, and the
 *  two are compared.
 *
 *  What is compared. The equaliser's own output, not the file, so the room is
 *  measured as it is being fed: a setting already on the Reverb cut band does
 *  not hide the bump it is cutting, and applying the suggestion twice gives the
 *  same suggestion. foobar2000 runs its DSP chain up to ten seconds ahead of
 *  what is heard, so the output is kept for kHistorySec and each stretch of
 *  microphone signal is looked for in all of it rather than at a guessed
 *  delay. That search runs per block, which also absorbs what a phone over
 *  Wi-Fi does to timing: drift, jitter and the odd resync each cost a block at
 *  most.
 *
 *  What is measured. The power response, microphone over output, per
 *  frequency, summed over every block that was found in the output. Power and
 *  not a complex transfer function, because the microphone is moving: the
 *  phase at one spot means nothing at the next, while the energy average over
 *  the walk is what a listener anywhere in the room hears. It includes the
 *  reverberant tail, which is the point - a mode that rings long collects
 *  more energy than its neighbours and shows up as a bump.
 *
 *  What is suggested. A phone microphone has no calibration, and loudspeakers
 *  are not flat, so the absolute curve is not trusted - only its shape. The
 *  response is smoothed to a sixth of an octave and bumps are found above a
 *  broad trend (Hann, two octaves either side, bumps weighted down). Each
 *  bump is then fitted, two octaves either side, with a bell plus a quadratic
 *  in log frequency for the loudspeaker and microphone; the bell's centre,
 *  Q and gain are the suggestion, since a bell is what will cancel it. Only
 *  the Reverb cut band's range, 200 Hz to 3 kHz, is ever suggested.
 *
 *  When the measurement cannot tell which bump is the main one - a runner-up
 *  nearly as tall, or the two halves of the walk pointing at different
 *  frequencies - it says so and offers up to three, to be chosen by ear.
 *
 *  The block that cannot be matched is dropped: talking, a door, a pause in
 *  playback, a burst of network trouble. Nothing is assumed about the
 *  microphone's sample rate, its channel count or its clock.
 *
 *  No foobar2000 or Win32 dependency, so the tests can drive it with
 *  synthetic rooms.
 * ======================================== */

#ifndef PARAEQ_CALIBRATE_H
#define PARAEQ_CALIBRATE_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <vector>

namespace paraeq_calib {

//! Both streams are brought to this rate before anything else. The Reverb cut
//! band stops at 3 kHz and the resampler passes 0.45 of the rate, 3.6 kHz, so
//! nothing the suggestion can use is lost, and a 20-second search costs a
//! 2^18-point transform rather than a 2^21-point one.
const double kRate = 8000.0;

//! How much of the equaliser's output is kept to search. foobar2000's output
//! buffer can be ten seconds long; on top of that come a block of microphone
//! signal, the device's latency and a phone's network delay. Twenty seconds
//! covers all of them with room to spare, at 640 KB. A host that knows its
//! buffer can ask for more, up to kMaxHistorySec, which is what the search
//! transform holds with a block beside it.
const double kHistorySec = 20.0;
const double kMaxHistorySec = 30.0;

//! History for a host whose output buffer is `bufferSec` long: the buffer
//! plus eight seconds for the block, the device and a phone's network, never
//! less than kHistorySec nor more than kMaxHistorySec.
inline double historyFor(double bufferSec)
{
    const double h = bufferSec + 8.0;
    return h < kHistorySec ? kHistorySec : (h > kMaxHistorySec ? kMaxHistorySec : h);
}

enum {
    //! Microphone samples per match, 2.05 s. Long enough that music finds
    //! itself unambiguously in twenty seconds of output, short enough that a
    //! walking listener is in one place for it and that clock drift inside it
    //! is a fraction of a sample.
    kBlock = 16384,

    //! Spectral frame and hop, 256 ms and 128 ms. 3.9 Hz bins, so a sixth of
    //! an octave at 200 Hz still spans six of them.
    kFrame = 2048,
    kHop   = 1024,
    kBins  = kFrame / 2 + 1,

    //! log2 of the search transform: history plus one block, rounded up.
    kCorrOrder = 18,

    //! Points per octave of the response grid.
    kGridPerOctave = 48,

    //! Up to this many bumps are reported, largest first.
    kMaxPeaks = 3
};

const double kGridLoHz = 100.0;     //!< response grid, bottom
const double kGridHiHz = 3400.0;    //!< and top, below the resampler's edge
const double kSearchLoHz = 200.0;   //!< where a suggestion may land: the
const double kSearchHiHz = 3000.0;  //!< Reverb cut band's frequency range

//! Matched audio before a suggestion is offered. Ten blocks is about twenty
//! seconds, which is a few positions in the room.
const int kMinBlocks = 10;

//! A block is taken as found when the search peak stands this far above the
//! search's own RMS. Unrelated audio peaks at about 5 over 150 000 lags, but
//! an input that has gone quiet - a muted device, a stalled phone stream -
//! has been seen to reach 12 on whitened noise; real matches through a USB
//! loop and through a simulated room score 30 to 130.
const double kMinConfidence = 15.0;

//! A phone in a hall with several loudspeakers hears each block blurred:
//! arrivals tens of milliseconds apart, its own processing, clipping. A
//! recording of a milonga, against the very file the DJ played, peaked at the
//! right delay in most blocks, but at 7 to 11, never 15. So weaker peaks
//! vote: kMinVotes of them at or above kVoteConfidence, within kTrackSec of
//! each other, fix the delay, and then a block whose peak within kTrackSec
//! of it reaches kTrackConfidence is taken. Chance peaks land anywhere in
//! many seconds of lags, so they do not agree; and over a window of a
//! thousand lags, chance seldom reaches 5.
const double kVoteConfidence  = 7.0;
const double kTrackConfidence = 6.0;
const double kTrackSec        = 0.06;   //!< walking about, and clocks drifting
const int    kMinVotes        = 3;
const int    kMaxVotes        = 32;     //!< the latest; a seek or a new buffer moves the delay

//! A microphone block quieter than this is not searched at all: it is a
//! stalled or muted input, not a room.
const double kMinMicDb = -80.0;

//! A bump smaller than this is not offered as a cut. A cabled loop, which is
//! flat, shows bumps up to about 2 dB from the music alone; a room resonance
//! worth taking out stands well above that.
const double kMinSuggestDb = 3.0;

//! Unsure, and offering several frequencies, when the runner-up bump is at
//! least this fraction of the tallest one's height...
const double kAmbiguousRatio = 0.75;

//! ...or when either half of the walk, measured on its own, puts its tallest
//! bump further than this from the whole walk's, in octaves.
const double kAgreeOctaves = 1.0 / 6.0;

//! Streaming windowed-sinc resampler from any rate to kRate. Not polyphase:
//! the kernel is evaluated from a table at each output point, which costs a
//! little more and handles any ratio, 44.1 to 8 or a phone's 47 998.6 to 8.
class Resampler {
public:
    void   reset(double inRate);
    double inRate() const { return m_in; }

    //! Appends what `in` completes to `out`. Output lags input by the kernel's
    //! half-width, a few milliseconds, the same for both streams.
    void process(const float * in, size_t n, std::vector<float> & out);

private:
    double m_in   = 0.0;
    double m_step = 1.0;    //!< input samples per output sample
    double m_scale = 1.0;   //!< kernel units per input sample, 2 * cutoff
    double m_half = 0.0;    //!< kernel half-width in input samples
    double m_pos  = 0.0;    //!< next output, in m_buf's coordinates
    std::vector<float> m_buf;
    std::vector<float> m_table;
};

//! Radix-2 complex FFT, unscaled in both directions.
class Fft {
public:
    void   init(unsigned order);
    size_t size() const { return m_cos.size() * 2; }
    void   run(double * re, double * im, bool inverse) const;

private:
    std::vector<double>   m_cos, m_sin;
    std::vector<uint32_t> m_rev;
};

//! One bump in the response, as Reverb cut settings.
struct Peak {
    double frequency = 0.0;    //!< Hz
    double q         = 1.0;    //!< as the band's Q control means it
    double gainDb    = 0.0;    //!< the suggested setting, negative
    double heightDb  = 0.0;    //!< how far the bump stands above the trend
};

struct Result {
    int    blocks      = 0;      //!< microphone blocks analysed
    int    matched     = 0;      //!< of which found in the output
    double lagSec      = -1.0;   //!< last match: output written to heard
    double confidence  = 0.0;    //!< last block's search peak over RMS
    double lockedLagSec = -1.0;  //!< the delay the blocks agree on, -1 before they do
    int    agreeing    = 0;      //!< recent blocks that agree on it
    double micDb       = -200.0; //!< last block, RMS dBFS
    double outDb       = -200.0;
    bool   micClipped  = false;  //!< last block reached full scale

    //! The response, once there is enough of it. responseDb is microphone
    //! over output; trendDb is its broad trend. Both are shifted so that
    //! the trend averages 0 dB over the search range, since only the shape
    //! means anything.
    std::vector<double> frequency;
    std::vector<double> responseDb;
    std::vector<double> trendDb;

    //! Every bump found, tallest first.
    int  peaks = 0;
    Peak peak[kMaxPeaks];

    //! What is offered as the Reverb cut. One option when the measurement is
    //! sure of the main bump; when it is not, up to three, the tallest first,
    //! to be tried by ear. All between kSearchLoHz and kSearchHiHz.
    bool sure = true;
    int  options = 0;
    Peak option[kMaxPeaks];

    bool ready() const
    {
        return matched >= kMinBlocks && options > 0 && option[0].heightDb >= kMinSuggestDb;
    }
};

class Analyzer {
public:
    //! `historySec`: how much output to keep, see historyFor().
    explicit Analyzer(double historySec = kHistorySec);

    //! Drops the measurement and keeps the output history, so a new one can
    //! start straight away.
    void resetMeasurement();

    //! The equaliser's output, mono. A rate change restarts the resampler
    //! and nothing else: the history is at kRate whatever came in.
    void feedOutput(const float * mono, size_t n, double rate);

    //! The microphone, mono. Returns true when at least one block completed.
    bool feedMic(const float * mono, size_t n, double rate);

    const Result & result() const { return m_result; }

    //! Seconds of output held, up to the history asked for.
    double historySec() const;

    //! Restart the history too, e.g. after a long gap in the output.
    void resetAll();

    //! The shortest delay a match may have, output written to heard. A host
    //! whose output buffer stays full - foobar2000 in steady playback - never
    //! plays its DSP's output sooner than the buffer length, so a shorter
    //! match is a false one. Set it a little under the buffer: just after a
    //! seek the buffer is still refilling and the true delay is shorter, and
    //! those blocks are better lost than matched wrongly.
    void setMinLag(double sec) { m_minLagSec = sec > 0.0 ? sec : 0.0; }

private:
    void analyseBlock();
    void updateResponse();

    Resampler m_outRs, m_micRs;
    std::vector<float> m_scratch;

    double m_minLagSec = 0.0;
    std::vector<float> m_hist;   //!< ring, the history at kRate
    size_t   m_histPos   = 0;    //!< next write
    uint64_t m_histTotal = 0;

    std::vector<float> m_mic;    //!< the block being collected
    float m_micPeak = 0.0f;
    std::vector<long> m_votes;   //!< delays, in samples, of blocks that voted

    Fft m_corrFft, m_frameFft;
    std::vector<double> m_re, m_im;
    std::vector<double> m_window;

    std::vector<double> m_sxx, m_syy;   //!< kBins, summed over matched blocks

    //! Each matched block's two normalised spectra, kBins each, in order, so
    //! the two halves of the walk can be measured apart. 16 KB a block.
    std::vector<float> m_blockSpectra;

    Result m_result;
};

// ---------------------------------------------------------------------------
// The tap: how the playback thread hands its output to a calibration
// ---------------------------------------------------------------------------

//! Process-wide, because the DSP instance and the window that measures are
//! made by different parts of foobar2000 and do not know about each other.
//! Closed, it costs the playback thread one relaxed atomic load per chunk.
namespace tap {

extern std::atomic<int> g_open;

//! Reference counted: the window opens it and closes it.
void open();
void close();

inline bool active() { return g_open.load(std::memory_order_relaxed) > 0; }

struct Segment {
    unsigned rate = 0;
    std::vector<float> samples;
};

void pushMono(const float * mono, size_t n, unsigned rate);

//! Moves everything pushed since the last drain into `out`, oldest first.
void drain(std::vector<Segment> & out);

//! Interleaved, any sample type, any channel count: averaged to mono.
template<typename Sample>
void push(const Sample * interleaved, size_t frames, unsigned channels, unsigned rate)
{
    if (channels == 0 || rate == 0) return;
    float buf[1024];
    const float scale = 1.0f / (float)channels;
    while (frames > 0) {
        const size_t n = frames < 1024 ? frames : 1024;
        for (size_t i = 0; i < n; ++i) {
            float s = 0.0f;
            for (unsigned c = 0; c < channels; ++c) s += (float)interleaved[i * channels + c];
            buf[i] = s * scale;
        }
        pushMono(buf, n, rate);
        interleaved += n * channels;
        frames -= n;
    }
}

} // namespace tap

} // namespace paraeq_calib

#endif // PARAEQ_CALIBRATE_H
