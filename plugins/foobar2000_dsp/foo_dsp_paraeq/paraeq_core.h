/* ========================================
 *  foo_dsp_paraeq - portable DSP core
 *
 *  A console channel equaliser: high-pass, low shelf, two peaking bands, high
 *  shelf, output trim. Fixed layout, twelve controls, zero latency, no heap.
 *
 *  Why a fixed layout rather than a bank of identical bands. The bank is the
 *  more general instrument and the worse tool for this job. Restoring disc
 *  transfers is repetitive work: the same four moves serve most of a box of
 *  records, and what an operator wants is to set frequency and Q once, before
 *  the session, and then reach for four gain knobs that always mean the same
 *  four things. On 1926-1949 shellac those four are
 *
 *      low shelf     60-125 Hz    weight the transfer lost at the bottom
 *      low-mid bell  around 1 kHz the boxy room the horn or the hall adds
 *      high-mid bell 4-6 kHz      the brilliance sitting under the surface noise
 *      high shelf    around 8 kHz the surface noise itself
 *
 *  plus a high-pass for rumble and turntable roar under all of it. Every range
 *  below is chosen to put its target near the middle of the control's travel,
 *  which is what makes the knob usable rather than merely capable.
 *
 *  Nothing here is novel as filter design - the stages are the biquads from
 *  Robert Bristow-Johnson's Audio EQ Cookbook, whose formulas are public
 *  domain. Two decisions are worth the reading.
 *
 *  1. Every control reaches the audio as a move of the same five numbers per
 *     stage. Gain, frequency and Q obviously do; so do the two shelf/bell
 *     switches and the high-pass slope, because a stage that is off is a unity
 *     biquad rather than a stage that is skipped. One glide mechanism therefore
 *     covers every control, including the discrete ones, and the count of
 *     biquads actually run never changes.
 *
 *     That glide is on the coefficients, not on the controls, and it runs per
 *     sample. Per sub-block was the first attempt and it is audible: a jump in
 *     b0 puts a step of b0*x straight into the output, so 32-sample blocks
 *     leave a staircase about 35 dB below the signal on a fast move. Stepping
 *     every sample is five multiply-adds per stage, which is affordable
 *     precisely because a settled equaliser skips the whole thing - and one is
 *     settled for all but a fraction of a second after a knob stops moving.
 *
 *     It is also safe for a reason rather than by measurement: a biquad is
 *     stable exactly when
 *     (a1, a2) lies inside |a2| < 1, |a1| < 1 + a2, which is a triangle and so
 *     convex. A straight line between two stable settings cannot leave a convex
 *     region, so no intermediate coefficient set can ring or blow up, whatever
 *     the two endpoints are - shelf to bell, off to 24 dB/oct, 16 Hz to 16 kHz.
 *     Interpolating the controls instead would need a design per sample and
 *     would still leave the discrete switches to special-case.
 *
 *  2. Bypass glides the whole equaliser to flat. It does not blend a dry path
 *     in, which is what declick and dehum do and what would be wrong here: the
 *     wet mix is meaningful on those because the difference signal is the
 *     repair, whereas summing a dry path across an EQ combs. Retargeting every
 *     stage to unity and the trim to 0 dB reaches the same place, silently, and
 *     the signal is a real equaliser curve at every instant of the transition.
 *
 *  The cascade runs high-pass, low shelf, low-mid, high-mid, high shelf, trim.
 *  The result does not depend on that order - this is an LTI cascade - so the
 *  order only decides what the intermediate signal looks like; the high-pass
 *  goes first so a large low boost is not applied to rumble about to be
 *  removed.
 *
 *  Latency is zero and there is nothing to acquire, so unlike dehum there is no
 *  detector to warm up and unlike declick no read-ahead: what goes in comes
 *  out, same count, processed in place. heapBytes() is 0. Every buffer is a
 *  fixed member, nothing is sized by the parameters or by the sample rate, so
 *  retune() always succeeds and no control move can allocate.
 *
 *  Denormals are the caller's job, as in the sibling cores: hold a
 *  scoped_flush_denormals across the processing loop. The biquad states are
 *  recursions towards zero, so a long fade-out will reach denormal range and
 *  pay for it on hardware that traps.
 *
 *  No foobar2000, VST or Win32 dependency.
 * ======================================== */

#ifndef PARAEQ_CORE_H
#define PARAEQ_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace paraeq {

enum {
    //! Biquads per channel, in cascade order: two for the high-pass, then the
    //! low shelf, the two peaking bands and the high shelf. The high-pass is
    //! two stages because 24 dB/oct needs two; at 12 dB/oct and at off the
    //! spare one is unity. The count never varies - see the header note.
    kStages = 6,

    kStageHighPass1 = 0,
    kStageHighPass2 = 1,
    kStageLowShelf  = 2,
    kStageLowMid    = 3,
    kStageHighMid   = 4,
    kStageHighShelf = 5,

    //! Samples between tests for whether the glide has arrived. The glide
    //! itself steps every sample; only the test is periodic, because it costs
    //! more than the step it is checking on and its sole purpose is to get
    //! process() back on its fast path.
    kSettleCheck = 32
};

//! Time constant of a control move, in seconds. 20 ms puts a move 95% of the
//! way there in 60 ms, which is short enough that a knob feels connected to
//! what is coming out and long enough that no single step is audible.
const double kGlideSec = 0.02;

//! Control ranges. A host builds its knobs from these, so every port agrees on
//! what a stored setting means. Each is bounded by what the band is for rather
//! than by what a biquad can do: the high-pass stops at 350 Hz because past
//! that it is removing a cello rather than a rumble, and the low-mid stops at
//! 3 kHz because above that it is the high-mid's job.
const float kHpFreqMin  =   16.0f, kHpFreqMax  =   350.0f;
const float kLfFreqMin  =   30.0f, kLfFreqMax  =   450.0f;
const float kLmfFreqMin =  200.0f, kLmfFreqMax =  3000.0f;
const float kHmfFreqMin =  600.0f, kHmfFreqMax =  8000.0f;
const float kHfFreqMin  = 1500.0f, kHfFreqMax  = 16000.0f;

//! Q for the two peaking bands. The top end is well past a console's, and it is
//! there because disc transfers carry resonances a console never had to remove -
//! a horn honk or a turntable ring is a single narrow feature, and taking it out
//! with a Q of 3 costs the music either side of it.
const float kQMin = 0.5f, kQMax = 8.0f;

//! Boost and cut available to each band, and to the output trim, in dB either
//! way. Wider than a console strip, again because a transfer can need it: 20 dB
//! of 8 kHz cut is a plausible setting on a worn shellac and an implausible one
//! on a microphone.
const float kGainMaxDb   = 20.0f;
const float kOutputMaxDb = 20.0f;

//! Shelf and bell corners are held below this fraction of the sample rate. The
//! bilinear transform warps a response increasingly badly as its corner
//! approaches Nyquist, and a shelf designed at or above it is not a shelf.
const double kMaxFreqFraction = 0.45;

//! Q of a single second-order section in a Butterworth high-pass, by order.
//! Order 2 is one section at 1/sqrt(2); order 4 is two sections at these,
//! which is what makes the pair maximally flat rather than peaky.
const double kButterworthQ2    = 0.70710678118654752;
const double kButterworthQ4Lo  = 0.54119610014619698;
const double kButterworthQ4Hi  = 1.30656296487637652;

//! Q used for the shelves. S = 1 in the cookbook's shelf slope parameter, the
//! steepest a shelf goes without overshooting into a peak at the corner.
const double kShelfQ = 0.70710678118654752;

//! What a stage is. kUnity is not a filter type in the cookbook - it is how an
//! off stage is expressed, so that switching one on or off is a coefficient
//! move like any other.
enum Kind {
    kUnity = 0,
    kHighPass,
    kLowShelf,
    kHighShelf,
    kPeaking
};

//! One second-order section, a0 normalised to 1. Default-constructs to unity.
struct Biquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

//! User-facing controls. Frequencies in Hz, gains in dB.
struct Params {
    float hpFrequency;   //!< corner of the high-pass
    int   hpSlope;       //!< 0 = off, 1 = 12 dB/oct, 2 = 24 dB/oct

    float lfGain;        //!< the bass knob
    float lfFrequency;
    bool  lfBell;        //!< peaking rather than shelving

    float lmfGain;       //!< the knob that takes the room out
    float lmfFrequency;
    float lmfQ;

    float hmfGain;       //!< the brilliance knob
    float hmfFrequency;
    float hmfQ;

    float hfGain;        //!< the knob that takes the hiss down
    float hfFrequency;
    bool  hfBell;

    float outputGain;    //!< makeup for whatever the bands did
    bool  bypass;        //!< glide the whole equaliser to flat

    //! Flat, with every band parked on the target its knob is for. These are
    //! starting positions, not a curve: an equaliser that did something on
    //! being added to a chain would be a surprise, and which way each band
    //! should go is a property of the record, not of the era.
    static Params defaults() {
        Params p;
        // Off, at the bottom of its range. Rumble is real on disc transfers but
        // how much of it there is depends on the deck the transfer was made on,
        // so this is the one band with an explicit off position rather than a
        // gain that happens to be zero.
        p.hpFrequency  =   16.0f;
        p.hpSlope      =      0;
        p.lfGain       =    0.0f;
        // 100 Hz. Centre of the 60-125 Hz the era's transfers want weight back
        // in, and low enough that a shelf there leaves the cello alone.
        p.lfFrequency  =  100.0f;
        p.lfBell       =  false;
        p.lmfGain      =    0.0f;
        // 1 kHz, where the boxiness of a horn or a hall sits.
        p.lmfFrequency = 1000.0f;
        p.lmfQ         =    1.0f;
        p.hmfGain      =    0.0f;
        // 5 kHz. Middle of the 4-6 kHz brilliance band, and far enough below
        // the hiss shelf that the two knobs do not fight.
        p.hmfFrequency = 5000.0f;
        p.hmfQ         =    1.0f;
        p.hfGain       =    0.0f;
        // 8 kHz. Above what most shellacs carry as programme and squarely on
        // what they carry as noise.
        p.hfFrequency  = 8000.0f;
        p.hfBell       =  false;
        p.outputGain   =    0.0f;
        p.bypass       =  false;
        return p;
    }

    //! Brings every control into range. A value that is merely outside its
    //! range is clamped to the near end of it; one that is not a plausible
    //! control value at all - a NaN, an infinity, anything past 1e30 - is
    //! replaced by its default, because clamping a NaN leaves a NaN and NaN
    //! coefficients stay in a biquad's state for the rest of the stream.
    void sanitize();

    bool operator==(const Params & o) const;
    bool operator!=(const Params & o) const { return !(*this == o); }
};

//! Everything derived from (params, sample rate): the target coefficients and
//! the rate at which the active ones move towards them.
struct Config {
    double sampleRate = 44100.0;
    Biquad stage[kStages];      //!< targets, in cascade order
    double outputGain = 1.0;    //!< linear, not dB
    double glide      = 0.0;    //!< fraction of the remaining distance covered
                                //!< per sample

    void compute(const Params & p, double sampleRate);

    //! Always true. Nothing here is sized by the parameters or by the sample
    //! rate, so every change - a sample rate change included - can be retuned
    //! into a running Channel. Kept for symmetry with the sibling cores, whose
    //! callers branch on it.
    bool structurallyEquals(const Config &) const { return true; }
};

//! Flush-to-zero for the duration of a scope. The biquad states are recursions
//! towards zero, so denormals are reachable on a fade-out and slow. FTZ changes
//! results in the last bits, so it is part of the numerical contract rather
//! than an optimisation, and every wrapper holds one across its processing
//! loop.
//!
//! MXCSR.FTZ on x86, FPCR.FZ on AArch64.
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

//! One independent channel of processing.
class Channel {
public:
    Channel();

    //! Installs cfg and snaps the active coefficients to it, so the next sample
    //! out is already equalised the new way. For a stream that has not started;
    //! use retune() on one that has.
    void configure(const Config & cfg);

    //! Filter state to zero and coefficients snapped to the target.
    void reset();

    //! A discontinuity in the input - a seek. The filter state describes
    //! samples that are no longer adjacent to what comes next, so it goes; the
    //! coefficients and where the glide had got to are the user's settings and
    //! stay.
    void flush();

    //! Point the glide at a new target, keeping filter state and keeping the
    //! active coefficients where they are. Never fails - see
    //! Config::structurallyEquals - and never allocates, so a render thread may
    //! call it on every block.
    bool retune(const Config & cfg);

    //! In place, interleaved by `stride`. Zero latency: what goes in comes out,
    //! same count, same alignment.
    template<typename Sample>
    void process(Sample * io, size_t frames, size_t stride);

    const Config & config() const { return m_cfg; }

    //! True once the active coefficients have reached the target, i.e. nothing
    //! is gliding. process() takes a cheaper path in that state, which is the
    //! usual one - an equaliser spends almost all its time not being adjusted.
    //! An exponential approach never truly arrives, so this goes true once the
    //! rest of the move is inaudible; expect a few hundred milliseconds rather
    //! than the kGlideSec the ear hears.
    bool settled() const { return m_settled; }

    //! The coefficients actually in use, which lag the target during a glide.
    const Biquad & activeStage(int index) const { return m_active[index]; }

    size_t heapBytes() const { return 0; }

private:
    void   snap();
    void   glideStep();
    void   checkSettled();
    double runStage(int index, double x);

    template<typename Sample>
    void   runBlock(Sample * io, size_t frames, size_t stride, bool gliding);

    Config m_cfg;
    Biquad m_active[kStages];
    double m_z[kStages][2];      //!< transposed direct form II state
    double m_gain    = 1.0;      //!< active output gain, linear
    bool   m_settled = true;
};

//! One cookbook section. `q` is ignored by kUnity, `gainDb` by everything but
//! the shelves and the bell. Exposed so a host can draw a single band and so
//! the tests can pin the design against a reference.
Biquad design(Kind kind, double frequencyHz, double q, double gainDb, double sampleRate);

//! Magnitude of one section at a frequency, in dB.
double magnitudeDb(const Biquad & b, double frequencyHz, double sampleRate);

//! Magnitude of the whole cascade, including the output trim, in dB. This is
//! the curve a host should draw: it reads the targets, so it shows where the
//! controls are rather than where a glide has got to.
double magnitudeDb(const Config & cfg, double frequencyHz);

// ---------------------------------------------------------------------------
// Drawing the response
// ---------------------------------------------------------------------------

//! magnitudeDb(Config, f) is the honest way to ask what the curve does at one
//! frequency and the wrong way to ask it several hundred times, which is what
//! drawing costs: four trig calls per stage is twenty-four per point, and a
//! logarithm per stage on top of them.
//!
//! Both are per-point constants in disguise. cos(w) and cos(2w) depend on the
//! frequency and the sample rate and on nothing a knob can move, so they hold
//! still across a whole drag; and the six stage magnitudes are multiplied, so
//! their six logarithms are one logarithm of the product.
//!
//! So a caller builds a table once - on a resize, not on a mouse move - and
//! every redraw after that is arithmetic. Measured over 600 points, x64
//! release: 110 us the direct way, 20 us this way, and 8 us to build the table
//! on the resize that needs it. Neither figure is alarming on a fast machine;
//! the point is that a redraw is a fifth of the work on a slow one, and that it
//! stays a fifth when a second curve is overlaid for the band being dragged.
//!
//! The storage is the caller's. That is what keeps heapBytes() at zero: a table
//! is as wide as somebody's window, which is not a thing an audio core should
//! know about.
enum { kCurveTrigStride = 4 };   //!< doubles of table per point

//! Fills `trig` with kCurveTrigStride doubles for each of `count` frequencies.
//! Rebuild it when the frequencies or the sample rate change; a control move
//! changes neither.
void curveTrig(const double * frequencyHz, size_t count, double sampleRate,
               double * trig);

//! The whole cascade, output trim included, in dB at the points `trig` was
//! built for. Reads the targets rather than the gliding coefficients, so it
//! shows where the controls are and not where the glide has got to. `outDb`
//! receives `count` values.
void curveDb(const Config & cfg, const double * trig, size_t count, float * outDb);

//! One section over the same table - for drawing the band under the pointer
//! against the curve it contributes to.
void curveDb(const Biquad & b, const double * trig, size_t count, float * outDb);

} // namespace paraeq

#endif // PARAEQ_CORE_H
