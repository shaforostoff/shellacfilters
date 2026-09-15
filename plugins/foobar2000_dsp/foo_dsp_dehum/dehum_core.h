/* ========================================
 *  foo_dsp_dehum - portable DSP core
 *
 *  Removes continuous narrowband tones - mains hum and its harmonics, and the
 *  off-frequency drones that turn up on speed-corrected disc transfers - from
 *  mono or stereo material, without being told which frequency to look at.
 *
 *  Two parts.
 *
 *  1. A detector, off the signal path. Every hop it takes the magnitude spectrum
 *     of a long sliding window and pushes it into a per-bin history; the median
 *     of that history is the part of the spectrum that is always there, and a
 *     peak standing proud of a local baseline in that median is a candidate
 *     line. Candidates must recur before they are acted on.
 *
 *     The window is long - 1.5 s at 44.1 kHz - and that is the whole trick. A
 *     coherent line's peak grows with the window length while noise and music
 *     grow with its square root, so prominence separates a hum from a musical
 *     peak 3 dB better per doubling. Measured on the reference transfers, the
 *     41.3 Hz line gains 24.7 -> 26.8 -> 28.2 dB across 0.37/0.74/1.49 s
 *     windows while the loudest music peak stays at 19-22 dB and does not grow.
 *     The same length gives the 0.67 Hz bins the notch needs to be placed on.
 *
 *  2. A canceller per line. Heterodyne the signal so the line sits at DC,
 *     lowpass to recover its complex amplitude, rotate back and subtract:
 *
 *         z = x * exp(-i*theta)
 *         w += lam * (z - w)                 lam = 2*pi*halfWidth/rate
 *         y = x - 2*Re{w * exp(i*theta)}
 *
 *     theta is a deterministic phase ramp, so nothing adapts on the signal:
 *     this is a linear time-invariant notch whose 3 dB half width is the lowpass
 *     cutoff, and it cannot ring or go unstable. Away from the line the response
 *     is |d|/sqrt(d^2 + halfWidth^2), which is 0.15 dB five half widths out.
 *
 *     Its depth is not infinite. Heterodyning also puts an image of the line at
 *     -2*f0, and what the one-pole lets through of that lands back on f0 when it
 *     is rotated up again, so the floor is halfWidth/(2*f0): 40 dB for a 1 Hz
 *     notch at 50 Hz, and deeper the lower the bandwidth or the higher the line.
 *     That is far below the residue any real detector leaves, so a second pole
 *     to square the term would buy nothing worth the state.
 *
 *     The frequency is then refined from the rotation of w. A residual error d
 *     makes w rotate at d Hz, so reading its phase advance over a quarter second
 *     measures d directly. This matters more than anything else here: the notch
 *     is narrow, so being 0.5 Hz off leaves the tone only 14 dB down, and
 *     tracking takes the same case to 92 dB down.
 *
 *  Latency is zero. The detector only reads the signal, so the canceller runs on
 *  the live sample and this core processes in place - no push/pull FIFO, unlike
 *  declick.
 *
 *  Acquisition, though, is not free, and how long it takes is a property of the
 *  material rather than of how the caller feeds it. The line is confirmed at a
 *  fixed point in the stream, so pushing longer blocks does not bring it forward -
 *  measured against a synthetic 40 dB line it lands at 2.97 s whether the caller
 *  hands over 4 s or 20 s. What moves it is prominence and therefore which route
 *  finds it. On 78 rpm tango transfers at 44.1 kHz, with the defaults:
 *
 *    - a line the prominence route can see - 19.6 dB above its local baseline -
 *      is confirmed about 9 s in, near the floor the analysis window filling and
 *      the evidence counter climbing set between them.
 *
 *    - a line sitting down in the rumble at 7.8 dB never clears the 16 dB
 *      threshold at all, so it can only arrive by the coherence route, and that
 *      ratio accumulates over kCohWindowSec. Confirmed about 43 s in, with a
 *      second line on the same transfer arriving at 67 s.
 *
 *  Both are a long time to be playing a record with the hum still in it. A host
 *  that has the whole file - and a file player does - can spend a scratch Channel
 *  on the opening of it off-thread and hand the result to adopt(), which is what
 *  that method is for. Reading the better part of a minute is what it takes to
 *  cover the coherence case.
 *
 *  No foobar2000, VST or Win32 dependency.
 * ======================================== */

#ifndef DEHUM_CORE_H
#define DEHUM_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace dehum {

enum {
    //! Slots for candidate fundamentals. Most are never cancelled: the coherence
    //! detector needs somewhere to park a nominee while it works out whether it
    //! is a tone, so the pool is larger than the number of lines one would
    //! expect to remove.
    kMaxLines     = 8,
    kMaxHarmonics = 8,    //!< multiples cancelled per fundamental
    kNominees     = 8,    //!< spectral peaks handed to probes each hop

    //! Analysis window, as a power of two picked so the bin spacing lands near
    //! kBinTargetHz whatever the sample rate. A fixed size would give 0.67 Hz
    //! bins at 44.1 kHz and 2.9 Hz at 192 kHz, and 2.9 Hz bins cannot place a
    //! 50 Hz line well enough for a 1 Hz notch to catch it.
    kMinFftOrder  = 11,
    kMaxFftOrder  = 17,

    kHistory      = 24,   //!< detector frames behind the median

    kSearchFloor  = 16,   //!< Hz, bottom of the automatic search range
    kSearchCeil   = 500,  //!< Hz, highest the top of the range may be set to

    //! Halfband stages ahead of the analysis, at most. Six covers 192 kHz.
    kDecimMaxStages = 8
};

//! Decimation ahead of the detector.
//!
//! The search range stops at kSearchCeil, so the detector is reading the bottom
//! 500 Hz of a signal that may run to 96 kHz - a full-rate transform spends
//! every one of its 32768 bins at 44.1 kHz to have 724 of them looked at. A
//! cascade of halfband decimators in front of the window fixes that: the window
//! still spans the same second and a half and its bins are still kBinTargetHz
//! apart, there are simply far fewer of them, and the transform gets smaller in
//! proportion. Nothing downstream changes - binLo, binHi, baselineBins and the
//! quadratic interpolation are all in bins, and a bin is still sampleRate /
//! fftSize Hz wide.
//!
//! The cascade runs on the detector's copy of the signal only. The audio path
//! never sees it, so its group delay - about 3 ms, against a 1.5 s window -
//! costs nothing but a correspondingly stale nomination.

//! Passband every stage has to keep flat: kSearchCeil with margin, so a line
//! sitting on the ceiling is not out on the filter's shoulder.
const double kDecimPassHz = 560.0;

//! The cascade stops here rather than going further. Nyquist is then at least
//! 1350 Hz against a 560 Hz passband, and that 2.4:1 margin is what keeps the
//! halfband transitions wide and so the filters short - the last stage is the
//! expensive one and it is the one the margin buys down.
const double kDecimMinRate = 2700.0;

//! Stopband of every stage. What survives it is an alias, and an alias is
//! exactly the failure that matters here: it is narrow, so it looks like a line.
//! At 120 dB a full scale partial folds down to -120 dBFS, some 25 dB under the
//! per-bin noise floor of a 78 rpm transfer, which cannot raise a peak the
//! detector would believe.
const double kDecimStopDb = 120.0;

//! Bin spacing the analysis window aims for, in Hz. 0.7 gives a 1.5 s window at
//! 44.1 kHz; see the note on window length above for why it is not shorter.
const double kBinTargetHz = 0.7;

//! Half width of the local baseline the detector measures prominence against.
const double kBaselineHz = 20.0;

//! The coherence detector: a second way in, for lines the first one cannot see.
//!
//! Prominence fails when a line stands on a broad pedestal, because the baseline
//! rides up with it. On one reference transfer the hum sits *at* the level of
//! the turntable rumble around it and its duty cycle above threshold is 0.0% at
//! every baseline geometry tried, guard-banded ones included. A magnitude
//! spectrum cannot separate a coherent tone at level X from noise at level X;
//! only phase can.
//!
//! So each candidate also gets two heterodyne integrators at the same frequency
//! and very different bandwidths. A continuous tone drives both to the same
//! complex amplitude; noise and separate note attacks arrive with independent
//! phases, so the narrow one averages them away. |w_narrow|/|w_wide| is then a
//! tonality measure that does not care how loud the surroundings are.
//!
//! kCohNarrowHz is 0.15 rather than the notch's own bandwidth because the
//! integration has to be longer than a musical note: at 1 Hz the time constant
//! is 0.16 s, shorter than a note, and a recurring note scored 0.92 - the same
//! as a real hum. At 0.15 Hz the time constant is 1.1 s and it scores 0.41.
//!
//! This needs the frequency to about 0.15 Hz, far finer than the spectrum can
//! nominate - which is why it only became possible once the frequency tracker
//! existed. The nominee is parked on a probe, the tracker locks it, and the
//! coherence is read at the locked frequency.
const double kCohNarrowHz = 0.15;
const double kCohWideHz   = 3.0;

//! Coherence detection is confined below this, and that is not a tuning choice.
//! Measured on the references: below 80 Hz both hum transfers score 0.48 and
//! 0.57 while neither hum-free control has a single surviving coherent probe.
//! Above it the order reverses - a sustained bass note at 123.5 Hz scores 0.62,
//! beating both real hums - because a held musical note *is* a coherent tone and
//! no statistic computed from the signal can say otherwise. Prominence still
//! searches the full range; only this second route is capped.
const double kCohCeilingHz = 80.0;

//! What a probe must reach to count as a tone, and how much it may then fall
//! back without being given up on - the same hysteresis the prominence route
//! needs, and for the same reason. Without it the reference transfer confirmed
//! three lines and dropped all three again.
//!
//! Measured with the probe pinned across the low band, which understates a
//! tracked probe but is directly comparable between files: the hum-free controls
//! stay between 0.07 and 0.17 everywhere from 30 to 65 Hz, while the two hum
//! transfers reach 0.28 and 0.40. Free-running probes on the hum transfers clear
//! 0.42; neither control ever does.
const double kCohThreshold    = 0.42;
const double kCohRetainMargin = 0.12;

//! How long the ratio is accumulated over, and how long a probe is ignored for.
//!
//! This has to be long, and that was not obvious. Accumulated over a fifth of a
//! second the ratio reads 0.9 or better for everything on real transfers,
//! including hum-free ones - because turntable rumble is a slowly wandering
//! narrowband process that is perfectly tone-like when you only look at it for
//! 0.2 s. It stops looking like a tone over tens of seconds, and a hum does not.
//! So the sums decay with this time constant rather than being reset per hop.
//!
//! The synthetic extremes are unaffected either way: a pure tone reads 0.999 and
//! white noise 0.186 against an analytic sqrt(narrow/wide) = 0.224.
const double kCohWindowSec = 20.0;

//! Hops of sustained coherence before a probe becomes a line. As demanding as
//! the prominence route: at 12 the coherence of marginal lines fluctuates across
//! the threshold and they confirm and drop repeatedly, taking a share of the
//! music with them each time they are engaged.
const int kCohScoreActivate = 24;

//! Evidence counter, and the part that decides what counts as hum.
//!
//! It is a duty cycle test rather than a level test, because on real material
//! the levels overlap. On the reference transfers the 41.3 Hz line's prominence
//! has a median of 19 dB and a 5th percentile of 14 dB, while the loudest
//! momentary peak in the hum-free controls reaches 22 dB - so no threshold both
//! catches the line continuously and never fires on a control. What separates
//! them is that the line clears 16 dB on 87% of hops and the best a control
//! manages is 10%.
//!
//! Each hop a candidate is seen it gains 1 plus a bonus for how far past the
//! threshold it is, and each hop it is not it loses kScoreFall. The bonus is
//! what makes this fast enough to be usable: the hum sits 3 dB past the
//! threshold on average and the control peaks that reach it barely clear it, so
//! grading the evidence roughly halves the time to engage while widening the
//! gap rather than narrowing it.
//!
//! The asymmetry matters too: at 1 up and 1 down a 50% duty cycle is a random
//! walk that reaches any threshold eventually, while at 1 up and 2 down anything
//! below a 2/3 duty cycle drifts to zero and stays there.
const double kScoreBonusPerDb = 0.25;  //!< extra credit per dB past the threshold
const double kScoreBonusMax   = 3.0;
const double kScoreFall       = 2.0;   //!< per hop missed, before a line engages
const double kScoreActivate   = 24.0;
const double kScoreCap        = 48.0;

//! Once a line is engaged it is held on much weaker evidence than it took to
//! establish: sightings count from kRetainMarginDb below the threshold, and a
//! miss costs kScoreFallActive instead of kScoreFall, so a total absence takes
//! about 36 s to give up rather than 2 s.
//!
//! Without this the line was acquired and dropped ten times over one side, and
//! removal of the hum was intermittent - 16 dB below the programme rather than
//! on top of it. Hum does not come and go; a gap in the evidence means the music
//! got loud, not that the hum stopped.
const double kRetainMarginDb   = 6.0;
const double kScoreFallActive  = 0.25;

//! User-facing controls.
struct Params {
    float sensitivity;  //!< 0 = only blatant lines, 1 = anything that stands out
    float bandwidth;    //!< notch half width, Hz
    float searchTo;     //!< top of the automatic search range, Hz
    int   harmonics;    //!< 1..8 multiples of each line to cancel
    float frequency;    //!< 0 = detect automatically, otherwise pin here (Hz)
    float rumbleHz;     //!< 0 = off, otherwise high-pass corner (Hz)
    float dryWet;       //!< 0 = bypass, 1 = full removal

    static Params defaults() {
        Params p;
        // Maps to a 16 dB prominence threshold. On the reference transfers the
        // real line clears that on 87% of hops while the best any hum-free
        // control manages is 10%, which the evidence counter turns into a
        // saturated 48 against 16 - so the gap the threshold has to sit in is
        // one of duty cycle, not of level. Raising this past about 0.7 starts
        // letting control material reach the activation score.
        p.sensitivity = 0.5f;
        // 1 Hz. The reference line is stable to +-0.02 Hz once tracked, so a
        // narrower notch would serve it, but 1 Hz absorbs the detector's own
        // error before the tracker converges and still costs nothing musically:
        // a 2 Hz hole at 41 Hz is Q = 20, and a partial 5 Hz away loses 0.15 dB.
        p.bandwidth   = 1.0f;
        // Hum lives low. Searching further up finds sustained musical notes
        // instead - during calibration a bandoneon E4 at 329 Hz was detected as
        // hum in both transfers of the same piece and duly cancelled. 100 Hz
        // keeps the search under the register where tango basses and bandoneon
        // fundamentals sit. The cost is missing a line above it, which is what
        // Frequency is there for when one turns up.
        p.searchTo    = 100.0f;
        // 1. Harmonics are a mains-hum idea and cost music when they are not
        //    there: a notch removes the coherent part at its frequency whether
        //    or not that part is hum, and multiples of a low fundamental land
        //    squarely in the musical register. Measured on the rumbly reference
        //    with 4 harmonics of two detected lines, six notches fell between
        //    80 and 200 Hz and took **84% of everything removed** with them,
        //    about a tenth of the energy in that band - against 0.8 dB gained at
        //    the line itself. Neither reference hum has harmonics worth having.
        //    Raise this for a genuine mains buzz, where they do exist.
        p.harmonics   = 1;
        p.frequency   = 0.0f;
        // 67 Hz. Broadband low-frequency rumble is a different defect from hum -
        // see the README - but it shares the band, it is what dominates one of
        // the two reference transfers, and leaving this off meant the component
        // did nothing at all to that file. 40 Hz was the cautious end of what
        // the measurements support: about 4 dB out of the 32-45 Hz band and
        // nothing above 90 Hz touched, which is safe on material that does have
        // real bass but leaves rumble audible on the transfers that have it.
        // 67 Hz goes after it properly. It takes the bottom octave of a double
        // bass with it, so wind it back towards 40 where the low end is worth
        // keeping.
        p.rumbleHz    = 67.0f;
        p.dryWet      = 1.0f;
        return p;
    }
    void sanitize();
    bool operator==(const Params & o) const;
    bool operator!=(const Params & o) const { return !(*this == o); }
};

//! Everything derived from (params, sample rate).
struct Config {
    double sampleRate  = 44100.0;
    int    fftOrder    = 16;
    //! The window in *full rate* samples. The transform actually run is winSize
    //! long at sampleRate / decim, which spans the same seconds - so this stays
    //! the number that sets the bin width, sampleRate / fftSize, and every bin
    //! index in this struct is still measured against it.
    int    fftSize     = 65536;
    int    decim       = 16;     //!< halfband stages collapse to this factor
    int    decimStages = 4;      //!< log2(decim)
    int    winSize     = 4096;   //!< fftSize / decim, the transform length
    int    hop         = 8192;   //!< full rate samples between detector runs
    int    binLo       = 24;     //!< first bin of the search range
    int    binHi       = 223;    //!< last bin of the search range
    int    baselineBins = 30;    //!< kBaselineHz either side, in bins

    double promDb      = 24.0;   //!< prominence a candidate needs, dB
    double halfWidth   = 1.0;    //!< notch 3 dB half width, Hz
    double lamNotch    = 0.0;    //!< one-pole coefficient, 2*pi*halfWidth/rate

    //! Coherence route. cohBinHi is where nomination for it stops - see
    //! kCohCeilingHz for why that ceiling is not negotiable.
    double lamCohNarrow = 0.0;
    double lamCohWide   = 0.0;
    int    cohBinHi     = 0;
    double cohThreshold = kCohThreshold;
    int    cohSettle    = 0;     //!< samples before a probe's ratio is believed
    double cohSmooth    = 0.0;   //!< per-hop smoothing of the ratio
    int    harmonics   = 4;
    double manualFreq  = 0.0;    //!< 0 = automatic
    double rumbleHz    = 0.0;    //!< 0 = off
    double wet         = 1.0;

    //! Score a candidate must reach before it is cancelled - see kScoreActivate.
    //! On the reference transfer the score climbs about 1.3 per hop, so the line
    //! engages some 19 hops after the window fills: about 5 s at 44.1 kHz. That
    //! is the price of a duty cycle test, and it is why Frequency exists for
    //! anyone who already knows what they are removing.
    double scoreActivate = kScoreActivate;

    //! Samples the frequency tracker integrates before reading the rotation of
    //! the notch weight. A quarter second turns a 0.1 Hz error into 0.157 rad,
    //! which is measurable; per block it would be 0.004 rad, which is noise -
    //! and an early prototype that did it per block random-walked 2 Hz off the
    //! line and left the hum untouched.
    int    trackSamples = 11025;
    double trackGain    = 0.7;   //!< fraction of the measured error applied
    double trackClampHz = 2.0;   //!< furthest a line may be pulled from where
                                 //!< the detector put it

    //! The tracker reads the rotation of the notch weight, so it is only
    //! meaningful while that weight is actually holding the line. Where the
    //! signal drops away - a run-out groove, a gap between movements - the
    //! weight collapses to noise and the reading becomes a random walk that
    //! wanders off at up to trackGain per interval. So the weight is compared
    //! against its own recent peak and the tracker freezes below this fraction
    //! of it, leaving the notch where it was.
    double trackFloor   = 0.3;
    double trackPeakDecay = 0.975;  //!< per tracker interval, about 10 s

    //! Buffer envelope. Every allocation Channel makes is sized from these, and
    //! they follow from the sample rate alone - never from the parameters. That
    //! is what lets any parameter change be handled by retune(), which does not
    //! touch the heap, so a slider move on the audio thread never allocates.
    //! Only a sample rate change reallocates.
    int    bufFftSize  = 65536;
    int    bufBins     = 720;   //!< bins spanned with searchTo at kSearchCeil
    int    bufHistory  = (int)kHistory;

    void compute(const Params & p, double sampleRate);

    //! True if `o` needs exactly the buffers this config already has. Only the
    //! sample rate sizes anything, so every parameter move can be retuned live.
    bool structurallyEquals(const Config & o) const {
        return fftSize == o.fftSize && winSize == o.winSize
            && bufBins == o.bufBins && bufHistory == o.bufHistory;
    }
};

//! Flush-to-zero for the duration of a scope. The notch integrator runs a
//! recursion towards zero, so denormals are reachable and slow. FTZ changes
//! results in the last bits, so it is part of the numerical contract rather than
//! an optimisation, and every wrapper holds one across its processing loop.
//!
//! MXCSR.FTZ on x86, FPCR.FZ on AArch64. Holding only the x86 half - which is
//! all this did until the ARM builds appeared - leaves the two ports disagreeing.
class scoped_flush_denormals {
public:
    scoped_flush_denormals(const scoped_flush_denormals &) = delete;
    void operator=(const scoped_flush_denormals &) = delete;
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__) || \
    defined(_M_ARM64) || defined(__aarch64__)
    scoped_flush_denormals();
    ~scoped_flush_denormals();
private:
    unsigned m_saved;   //!< MXCSR, or the defined low half of FPCR
#else
    scoped_flush_denormals() {}
#endif
};

//! What the detector currently believes. Diagnostics, and what the CLI prints.
struct LineReport {
    double frequency  = 0.0;  //!< Hz, as tracked
    double detected   = 0.0;  //!< Hz, where the detector first put it
    double prominence = 0.0;  //!< dB above the local baseline of the median
    double amplitude  = 0.0;  //!< 2*|w|, the tone amplitude being subtracted
    double coherence  = 0.0;  //!< |w_narrow| / |w_wide|
    bool   viaCoherence = false;  //!< found by coherence rather than prominence
    int    harmonics  = 1;    //!< multiples engaged
};

//! One independent channel of processing.
class Channel {
public:
    Channel();

    void configure(const Config & cfg);

    //! Everything to zero, including what the detector has learned.
    void reset();

    //! A discontinuity in the input - a seek - without forgetting the lines. The
    //! analysis window is stale so it is dropped, but the hum on the far side of
    //! a seek is the same hum, and re-acquiring it every time the user moves the
    //! playback position would be worse than doing nothing.
    void flush();

    //! Swap in a config needing the same buffers, keeping state. Returns false
    //! if the new config is structurally different, in which case the caller has
    //! to configure() and accept the discontinuity. Since only the sample rate
    //! sizes anything, every parameter move takes this path.
    bool retune(const Config & cfg);

    //! Start from lines somebody else has already found, so a stream that begins
    //! mid-hum does not have to spend the acquisition time over again. The
    //! intended source is a scratch Channel run over the opening of the same
    //! audio - see the note on acquisition at the top of this file.
    //!
    //! Not the same as Params::frequency, which pins one line and turns the
    //! search off: syncManual() discards every other line and runDetector()
    //! returns early, so nothing else is ever found. These arrive already
    //! confirmed and the detector keeps running, so it tracks them, drops them
    //! again if the evidence is not really there, and can still find others. It
    //! also takes more than one, which matters: of the two reference transfers,
    //! one carries two lines.
    //!
    //! Only `frequency` and `viaCoherence` are read from each report; the rest is
    //! diagnostics. Frequencies outside the configured search range are ignored,
    //! as are duplicates of a line already held and anything past kMaxLines.
    //! Does nothing at all while a manual frequency is set - that is the user's
    //! choice and it outranks a guess.
    //!
    //! Allocation-free, like reset() and retune(): every buffer it touches is one
    //! the Channel already holds, so a render thread may call it.
    void adopt(const LineReport * lines, int count);

    //! In place, interleaved by `stride`. Zero latency, so there is no FIFO:
    //! what goes in comes out, same count, same alignment.
    template<typename Sample>
    void process(Sample * io, size_t frames, size_t stride);

    const Config & config() const { return m_cfg; }

    //! Diagnostics.
    int      lineCount() const;
    void     report(LineReport * out, int max, int * count) const;
    uint64_t seenSamples() const { return m_seen; }
    size_t   heapBytes() const;
    //! How many times a line has been confirmed, and how many times one has been
    //! forgotten again. A dropout count above zero on steady material means the
    //! retain threshold is too high for it.
    uint32_t confirmations() const { return m_confirmations; }
    uint32_t dropouts() const { return m_dropouts; }

private:
    //! One cancelled sinusoid: a recursive rotator for exp(i*theta), the complex
    //! amplitude estimate, and the tracker's reference.
    struct Osc {
        double freq   = 0.0;
        double cosInc = 1.0, sinInc = 0.0;
        double cosPh  = 1.0, sinPh  = 0.0;
        double wRe = 0.0, wIm = 0.0;
        double refRe = 0.0, refIm = 0.0;
        double wPeak = 0.0;      //!< decaying peak of |w|, gates the tracker
        //! Coherence probe, fundamental only. Sums of squares rather than of
        //! magnitudes so no per-sample sqrt is needed; the ratio of the roots is
        //! what gets compared, and it was calibrated in that form.
        double cnRe = 0.0, cnIm = 0.0;
        double cwRe = 0.0, cwIm = 0.0;
        double sumN = 0.0, sumW = 0.0;
        double coh  = 0.0;       //!< smoothed |w_narrow| / |w_wide|
        int    lived = 0;        //!< samples since this probe started
        int    trackAcc = 0;
        int    renorm   = 0;
        bool   live     = false;
    };

    struct Line {
        double detected = 0.0;   //!< where the detector put it
        double prom     = 0.0;
        double score    = 0.0;   //!< prominence evidence, see kScoreActivate
        double cohScore = 0.0;   //!< coherence evidence, the second route in
        bool   active   = false;
        bool   manual   = false;
        bool   viaCoh   = false; //!< which route confirmed it, for diagnostics
        Osc    osc[kMaxHarmonics];
    };

    void   runDetector();
    void   detectPeaks(int bins);
    void   baselineMedian(int bins);
    bool   historyMedian(int bins);
    void   updateCoherence();
    void   clearHistory();
    double scoreFor(double prominence) const;
    void   syncManual();
    void   startOsc(Osc & o, double freq);
    void   setOscFreq(Osc & o, double freq);
    //! `probe` also runs the coherence pair; only the fundamental needs it.
    double runOsc(Osc & o, double x, bool probe);
    double runLine(Line & line, double x);
    void   syncHarmonics(Line & line);
    void   designRumble();
    double runRumble(double x);
    void   realFftMagnitudes();
    void   designDecimator();
    void   clearDecimator();
    //! One input sample in; true when a decimated sample comes out.
    bool   decimate(double x, double * out);

    Config m_cfg;

    // --- detector ---
    //! The halfband cascade. Coefficients and delay lines for every stage live
    //! in one allocation each, indexed by the per-stage offsets - a stage is a
    //! span, not an object, so configure() makes two allocations rather than
    //! kDecimMaxStages of them. Only the nonzero taps are stored: half of a
    //! halfband's coefficients are exactly zero and skipping them is most of
    //! why the cascade is cheap.
    std::vector<double> m_hbCoef;  //!< nonzero taps, stage after stage
    std::vector<int>    m_hbTap;   //!< where each sits in the delay line
    std::vector<double> m_hbZ;     //!< delay lines, stage after stage
    int m_hbFirst[kDecimMaxStages + 1];  //!< first tap of stage i
    int m_hbZAt[kDecimMaxStages + 1];    //!< first delay slot of stage i
    int m_hbZMask[kDecimMaxStages];      //!< its length, a power of two, less 1
    int m_hbPos[kDecimMaxStages];        //!< write cursor
    int m_hbPhase[kDecimMaxStages];      //!< 0 emits, 1 swallows

    std::vector<double> m_win;    //!< sliding analysis window, a ring
    std::vector<double> m_taper;  //!< Blackman-Harris, precomputed - rebuilding
                                  //!< it per hop would be a cos() per sample
    int m_winPos = 0;
    int m_hopAcc = 0;
    int m_filled = 0;

    std::vector<double> m_fftRe, m_fftIm;  //!< N/2 complex working buffer
    std::vector<double> m_twRe, m_twIm;    //!< FFT twiddles
    std::vector<int>    m_rev;             //!< bit-reversal permutation
    std::vector<double> m_mag;             //!< |X| over the search range

    std::vector<double> m_hist;    //!< bufHistory x bufBins magnitudes
    //! The same magnitudes again, bufBins x bufHistory and each bin's row held
    //! sorted. It buys the median over the history for the price of a memmove
    //! a frame instead of a sort a frame - see historyMedian().
    std::vector<double> m_sorted;
    int m_histPos = 0, m_histFill = 0;
    std::vector<double> m_med;     //!< median over history, dB, per bin
    std::vector<double> m_base;    //!< local baseline of m_med, per bin
    std::vector<double> m_baseBuf; //!< the baseline window, held sorted

    Line m_line[kMaxLines];
    int  m_lines = 0;

    // --- rumble high-pass: two biquads, Butterworth order 4 ---
    double m_hpB[2][3] = { { 1, 0, 0 }, { 1, 0, 0 } };
    double m_hpA[2][2] = { { 0, 0 }, { 0, 0 } };
    double m_hpZ[2][2] = { { 0, 0 }, { 0, 0 } };
    bool   m_hpOn = false;

    uint64_t m_seen = 0;
    uint32_t m_confirmations = 0;
    uint32_t m_dropouts = 0;
};

//! Median of `n` values, reordering `buf`. Exposed so the tests can pin it.
double medianInPlace(double * buf, int n);

//! In the sorted `buf[0, n)`, drop the value sitting at `at` and admit `v`,
//! leaving it sorted. Everything between the two positions shifts by one, so
//! the step is a binary search and a memmove where re-sorting the window would
//! be quadratic. Both of the detector's medians slide this way; exposed so the
//! tests can pin it against a sort.
void sortedReplaceAt(double * buf, int n, int at, double v);

} // namespace dehum

#endif // DEHUM_CORE_H
