/* ========================================
 *  foo_dsp_paraeq - portable DSP core
 * ======================================== */

#include "paraeq_core.h"

#include <math.h>
#include <string.h>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#define PARAEQ_HAVE_MXCSR 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define PARAEQ_HAVE_FPCR 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace paraeq {

namespace {

const double kPi = 3.14159265358979323846;

//! How close a coefficient has to be to its target before the glide is called
//! done. An exponential approach never arrives, so something has to end it, and
//! the only thing at stake is when process() may return to its fast path.
//! Coefficients are of order 1, so 1e-6 is a residual move about 120 dB down -
//! inaudible by a wide margin, and reached about 280 ms after a control stops
//! moving at the 20 ms time constant. A tighter figure only lengthens that.
const double kSettleTolerance = 1e-6;

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//! Finite and within a sane range. NaN fails both comparisons, so it is caught
//! here too - which is the point: a NaN reaching a biquad's state would stay
//! there for the rest of the stream.
inline bool sane(double v) {
    return v > -1e30 && v < 1e30;
}

//! In range if it can be, its default if it cannot. Used by Params::sanitize,
//! where clamping a NaN would leave it a NaN.
inline float saneRange(float v, float fallback, float lo, float hi) {
    if (!sane(v)) return fallback;
    return (float)clampd(v, lo, hi);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Flush-to-zero
// ---------------------------------------------------------------------------

#if defined(PARAEQ_HAVE_MXCSR)
scoped_flush_denormals::scoped_flush_denormals() : m_saved(_mm_getcsr()) {
    // FTZ only. DAZ lives in bit 6, which the earliest SSE2 parts treat as
    // reserved, and writing it there raises a general protection fault.
    //
    // Both halves are conditional because ldmxcsr flushes the pipeline and a
    // host has usually set FTZ already, so the ordinary case is to read a bit
    // and touch nothing. Same form as the sibling cores.
    if ((m_saved & 0x8000u) == 0u) _mm_setcsr(m_saved | 0x8000u);
}
scoped_flush_denormals::~scoped_flush_denormals() {
    if ((m_saved & 0x8000u) == 0u) _mm_setcsr(m_saved);
}

#elif defined(PARAEQ_HAVE_FPCR)

namespace {

// FPCR bit 24 (FZ) is AArch64's MXCSR.FTZ. Bits 63:32 are RES0, so keeping only
// the low half in m_saved round-trips the register.
const unsigned kFpcrFlushToZero = 1u << 24;

inline unsigned readFpcr() {
#if defined(_MSC_VER)
    return (unsigned)_ReadStatusReg(ARM64_FPCR);
#else
    uint64_t value;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
    return (unsigned)value;
#endif
}

inline void writeFpcr(unsigned value) {
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_FPCR, (int64_t)value);
#else
    uint64_t wide = value;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(wide));
#endif
}

} // anonymous namespace

scoped_flush_denormals::scoped_flush_denormals() : m_saved(readFpcr()) {
    writeFpcr(m_saved | kFpcrFlushToZero);
}
scoped_flush_denormals::~scoped_flush_denormals() {
    writeFpcr(m_saved);
}
#endif

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

void Params::sanitize()
{
    Params d = defaults();

    hpFrequency  = saneRange(hpFrequency,  d.hpFrequency,  kHpFreqMin,  kHpFreqMax);
    hpSlope      = clampi(hpSlope, 0, 2);

    lfGain       = saneRange(lfGain,       d.lfGain,       -kGainMaxDb, kGainMaxDb);
    lfFrequency  = saneRange(lfFrequency,  d.lfFrequency,  kLfFreqMin,  kLfFreqMax);

    lmfGain      = saneRange(lmfGain,      d.lmfGain,      -kGainMaxDb, kGainMaxDb);
    lmfFrequency = saneRange(lmfFrequency, d.lmfFrequency, kLmfFreqMin, kLmfFreqMax);
    lmfQ         = saneRange(lmfQ,         d.lmfQ,         kQMin,       kQMax);

    hmfGain      = saneRange(hmfGain,      d.hmfGain,      -kGainMaxDb, kGainMaxDb);
    hmfFrequency = saneRange(hmfFrequency, d.hmfFrequency, kHmfFreqMin, kHmfFreqMax);
    hmfQ         = saneRange(hmfQ,         d.hmfQ,         kQMin,       kQMax);

    hfGain       = saneRange(hfGain,       d.hfGain,       -kGainMaxDb, kGainMaxDb);
    hfFrequency  = saneRange(hfFrequency,  d.hfFrequency,  kHfFreqMin,  kHfFreqMax);

    outputGain   = saneRange(outputGain,   d.outputGain,   -kOutputMaxDb, kOutputMaxDb);
}


bool Params::operator==(const Params & o) const
{
    return hpFrequency  == o.hpFrequency
        && hpSlope      == o.hpSlope
        && lfGain       == o.lfGain
        && lfFrequency  == o.lfFrequency
        && lfBell       == o.lfBell
        && lmfGain      == o.lmfGain
        && lmfFrequency == o.lmfFrequency
        && lmfQ         == o.lmfQ
        && hmfGain      == o.hmfGain
        && hmfFrequency == o.hmfFrequency
        && hmfQ         == o.hmfQ
        && hfGain       == o.hfGain
        && hfFrequency  == o.hfFrequency
        && hfBell       == o.hfBell
        && outputGain   == o.outputGain
        && bypass       == o.bypass;
}

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

Biquad design(Kind kind, double frequencyHz, double q, double gainDb, double sampleRate)
{
    Biquad unity;

    if (kind == kUnity)     return unity;
    if (!(sampleRate > 0.0) || !sane(sampleRate)) return unity;
    if (!sane(frequencyHz) || !sane(q) || !sane(gainDb)) return unity;

    // Above kMaxFreqFraction the bilinear transform has warped the response
    // past the shape being asked for, so a corner up there is held down rather
    // than designed badly.
    double f0 = clampd(frequencyHz, 1.0, sampleRate * kMaxFreqFraction);
    double w0 = 2.0 * kPi * f0 / sampleRate;
    double cw = cos(w0);
    double sw = sin(w0);

    double alpha = sw / (2.0 * clampd(q, 0.05, 100.0));

    double b0, b1, b2, a0, a1, a2;

    if (kind == kHighPass) {
        double onePlusCw = 1.0 + cw;

        b0 =  onePlusCw * 0.5;
        b1 = -onePlusCw;
        b2 =  onePlusCw * 0.5;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cw;
        a2 =  1.0 - alpha;

    } else if (kind == kPeaking) {
        // A is the square root of the linear gain: the cookbook's peaking form
        // boosts by A on the numerator and cuts by A on the denominator, so the
        // two together land on A^2.
        double A = pow(10.0, clampd(gainDb, -96.0, 96.0) / 40.0);

        b0 =  1.0 + alpha * A;
        b1 = -2.0 * cw;
        b2 =  1.0 - alpha * A;
        a0 =  1.0 + alpha / A;
        a1 = -2.0 * cw;
        a2 =  1.0 - alpha / A;

    } else {
        double A     = pow(10.0, clampd(gainDb, -96.0, 96.0) / 40.0);
        double beta  = 2.0 * sqrt(A) * alpha;
        double Aplus = A + 1.0;
        double Aless = A - 1.0;

        if (kind == kLowShelf) {
            b0 =        A * (Aplus - Aless * cw + beta);
            b1 =  2.0 * A * (Aless - Aplus * cw);
            b2 =        A * (Aplus - Aless * cw - beta);
            a0 =            (Aplus + Aless * cw + beta);
            a1 = -2.0 *     (Aless + Aplus * cw);
            a2 =            (Aplus + Aless * cw - beta);

        } else {
            b0 =        A * (Aplus + Aless * cw + beta);
            b1 = -2.0 * A * (Aless + Aplus * cw);
            b2 =        A * (Aplus + Aless * cw - beta);
            a0 =            (Aplus - Aless * cw + beta);
            a1 =  2.0 *     (Aless - Aplus * cw);
            a2 =            (Aplus - Aless * cw - beta);
        }
    }

    if (!(a0 > 1e-12) || !sane(a0)) return unity;

    Biquad out;
    out.b0 = b0 / a0;
    out.b1 = b1 / a0;
    out.b2 = b2 / a0;
    out.a1 = a1 / a0;
    out.a2 = a2 / a0;

    if (!sane(out.b0) || !sane(out.b1) || !sane(out.b2) ||
        !sane(out.a1) || !sane(out.a2))
    {
        return unity;
    }

    return out;
}


double magnitudeDb(const Biquad & b, double frequencyHz, double sampleRate)
{
    if (!(sampleRate > 0.0) || !sane(frequencyHz)) return 0.0;

    double w  = 2.0 * kPi * frequencyHz / sampleRate;
    double c1 = cos(w),       s1 = sin(w);
    double c2 = cos(2.0 * w), s2 = sin(2.0 * w);

    // H(z) at z = exp(i*w), i.e. z^-1 = exp(-i*w), hence the negated imaginary
    // parts.
    double numRe = b.b0 + b.b1 * c1 + b.b2 * c2;
    double numIm =      -(b.b1 * s1 + b.b2 * s2);
    double denRe = 1.0  + b.a1 * c1 + b.a2 * c2;
    double denIm =      -(b.a1 * s1 + b.a2 * s2);

    double num = numRe * numRe + numIm * numIm;
    double den = denRe * denRe + denIm * denIm;

    if (!(den > 1e-300)) return 0.0;

    // These are squared magnitudes, so 10*log10 rather than 20. The floor keeps
    // a perfect notch finite instead of -inf.
    return 10.0 * log10((num / den) + 1e-300);
}


double magnitudeDb(const Config & cfg, double frequencyHz)
{
    double total = 0.0;

    for (int i = 0; i < kStages; i++) {
        total += magnitudeDb(cfg.stage[i], frequencyHz, cfg.sampleRate);
    }

    if (cfg.outputGain > 0.0) {
        total += 20.0 * log10(cfg.outputGain);
    }

    return total;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void Config::compute(const Params & inParams, double inSampleRate)
{
    Params p = inParams;
    p.sanitize();

    sampleRate = (sane(inSampleRate) && inSampleRate > 0.0) ? inSampleRate : 44100.0;

    if (p.bypass) {
        // Not a branch taken in process(): bypass is a target like any other,
        // so what the listener hears is the curve flattening out over
        // kGlideSec rather than a switch.
        for (int i = 0; i < kStages; i++) {
            stage[i] = Biquad();
        }

        outputGain = 1.0;

    } else {
        // Off is a unity stage rather than a skipped one, so that switching the
        // filter in is a coefficient move and the biquad count never changes.
        stage[kStageHighPass1] = (p.hpSlope == 0)
            ? Biquad()
            : design(kHighPass, p.hpFrequency,
                     p.hpSlope == 1 ? kButterworthQ2 : kButterworthQ4Lo, 0.0, sampleRate);

        stage[kStageHighPass2] = (p.hpSlope == 2)
            ? design(kHighPass, p.hpFrequency, kButterworthQ4Hi, 0.0, sampleRate)
            : Biquad();

        // A shelf switched to bell keeps the shelf's bandwidth, as a console's
        // does: the switch is there to concentrate the same broad move, not to
        // turn the band into a third parametric one.
        stage[kStageLowShelf] = design(p.lfBell ? kPeaking : kLowShelf,
                                       p.lfFrequency, kShelfQ, p.lfGain, sampleRate);

        stage[kStageLowMid]   = design(kPeaking, p.lmfFrequency, p.lmfQ, p.lmfGain, sampleRate);
        stage[kStageHighMid]  = design(kPeaking, p.hmfFrequency, p.hmfQ, p.hmfGain, sampleRate);

        stage[kStageHighShelf] = design(p.hfBell ? kPeaking : kHighShelf,
                                        p.hfFrequency, kShelfQ, p.hfGain, sampleRate);

        outputGain = pow(10.0, p.outputGain / 20.0);
    }

    // The fraction of the remaining distance one sample covers, so the
    // approach is exponential with kGlideSec as its time constant whatever the
    // sample rate.
    double tau = kGlideSec * sampleRate;
    glide = (tau > 1.0) ? (1.0 - exp(-1.0 / tau)) : 1.0;
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

Channel::Channel()
{
    memset(m_z, 0, sizeof(m_z));
    snap();
}


void Channel::configure(const Config & cfg)
{
    m_cfg = cfg;
    memset(m_z, 0, sizeof(m_z));
    snap();
}


void Channel::reset()
{
    memset(m_z, 0, sizeof(m_z));
    snap();
}


void Channel::flush()
{
    memset(m_z, 0, sizeof(m_z));
}


bool Channel::retune(const Config & cfg)
{
    m_cfg = cfg;
    m_settled = false;

    return true;
}


void Channel::snap()
{
    for (int i = 0; i < kStages; i++) {
        m_active[i] = m_cfg.stage[i];
    }

    m_gain    = m_cfg.outputGain;
    m_settled = true;
}


void Channel::glideStep()
{
    double g = clampd(m_cfg.glide, 0.0, 1.0);
    if (!(g > 0.0)) g = 1.0;

    // Straight-line interpolation of the coefficients, which is safe rather
    // than merely convenient: the stable region |a2| < 1, |a1| < 1 + a2 is a
    // triangle, so no point between two stable settings is outside it. See the
    // header.
    for (int i = 0; i < kStages; i++) {
        Biquad       & a = m_active[i];
        const Biquad & t = m_cfg.stage[i];

        a.b0 += (t.b0 - a.b0) * g;
        a.b1 += (t.b1 - a.b1) * g;
        a.b2 += (t.b2 - a.b2) * g;
        a.a1 += (t.a1 - a.a1) * g;
        a.a2 += (t.a2 - a.a2) * g;
    }

    m_gain += (m_cfg.outputGain - m_gain) * g;
}


void Channel::checkSettled()
{
    for (int i = 0; i < kStages; i++) {
        const Biquad & a = m_active[i];
        const Biquad & t = m_cfg.stage[i];

        if (fabs(t.b0 - a.b0) > kSettleTolerance ||
            fabs(t.b1 - a.b1) > kSettleTolerance ||
            fabs(t.b2 - a.b2) > kSettleTolerance ||
            fabs(t.a1 - a.a1) > kSettleTolerance ||
            fabs(t.a2 - a.a2) > kSettleTolerance)
        {
            return;
        }
    }

    if (fabs(m_cfg.outputGain - m_gain) > kSettleTolerance) return;

    // Land exactly on the target rather than asymptotically near it, so that
    // repeated retunes cannot accumulate an offset.
    snap();
}


double Channel::runStage(int index, double x)
{
    const Biquad & b = m_active[index];
    double       * z = m_z[index];

    // Transposed direct form II: two state words per stage, and the state is
    // the filter's memory of the input rather than of the output, which is what
    // keeps it well behaved while the coefficients are moving.
    double y = b.b0 * x + z[0];

    z[0] = b.b1 * x - b.a1 * y + z[1];
    z[1] = b.b2 * x - b.a2 * y;

    return y;
}


template<typename Sample>
void Channel::runBlock(Sample * io, size_t frames, size_t stride, bool gliding)
{
    Sample *p = io;

    for (size_t i = 0; i < frames; i++) {
        if (gliding) glideStep();

        double x = (double)*p;

        for (int s = 0; s < kStages; s++) {
            x = runStage(s, x);
        }

        if (!sane(x)) {
            // A non-finite sample would live in the states for the rest of the
            // stream, so it stops here rather than being passed on.
            memset(m_z, 0, sizeof(m_z));
            x = 0.0;
        }

        *p = (Sample)(x * m_gain);
        p += stride;
    }
}


template<typename Sample>
void Channel::process(Sample * io, size_t frames, size_t stride)
{
    if (!io || !stride) return;

    size_t done = 0;

    while (done < frames) {
        Sample *at = io + (done * stride);

        // A settled equaliser - which is what one is almost all the time - runs
        // the whole block through the loop that does no interpolation at all.
        if (m_settled) {
            runBlock(at, frames - done, stride, false);
            return;
        }

        size_t count = frames - done;
        if (count > (size_t)kSettleCheck) count = (size_t)kSettleCheck;

        runBlock(at, count, stride, true);
        checkSettled();

        done += count;
    }
}


template void Channel::process<float>(float *, size_t, size_t);
template void Channel::process<double>(double *, size_t, size_t);

// ---------------------------------------------------------------------------
// Drawing the response
// ---------------------------------------------------------------------------

namespace {

//! Squared magnitude of one section at a point of the table, as a ratio. Kept
//! separate from the dB conversion so the cascade can multiply six of these and
//! take one logarithm instead of six.
inline double powerRatio(const Biquad & b, const double * t)
{
    const double c1 = t[0], s1 = t[1], c2 = t[2], s2 = t[3];

    // H(z) at z = exp(i*w); z^-1 = exp(-i*w), hence the negated imaginary parts.
    const double numRe = b.b0 + b.b1 * c1 + b.b2 * c2;
    const double numIm =      -(b.b1 * s1 + b.b2 * s2);
    const double denRe = 1.0  + b.a1 * c1 + b.a2 * c2;
    const double denIm =      -(b.a1 * s1 + b.a2 * s2);

    const double den = denRe * denRe + denIm * denIm;
    if (!(den > 1e-300)) return 1.0;

    return (numRe * numRe + numIm * numIm) / den;
}

} // anonymous namespace


void curveTrig(const double * frequencyHz, size_t count, double sampleRate,
               double * trig)
{
    if (!frequencyHz || !trig) return;

    const double fs = (sane(sampleRate) && sampleRate > 0.0) ? sampleRate : 44100.0;

    for (size_t i = 0; i < count; i++) {
        // Past Nyquist the response is a mirror of itself rather than anything a
        // reader should be shown, so a point up there is held at Nyquist. A
        // window wider than the audible band is the ordinary way to get here.
        const double f = clampd(sane(frequencyHz[i]) ? frequencyHz[i] : 0.0,
                                0.0, fs * 0.5);
        const double w = 2.0 * kPi * f / fs;

        double * t = trig + i * (size_t)kCurveTrigStride;
        t[0] = cos(w);
        t[1] = sin(w);
        t[2] = cos(2.0 * w);
        t[3] = sin(2.0 * w);
    }
}


void curveDb(const Config & cfg, const double * trig, size_t count, float * outDb)
{
    if (!trig || !outDb) return;

    // The trim is a scalar on the whole cascade, so it is one addition per point
    // rather than anything inside the loop over stages.
    const double trimDb = (cfg.outputGain > 0.0)
                        ? 20.0 * log10(cfg.outputGain)
                        : -600.0;

    for (size_t i = 0; i < count; i++) {
        const double * t = trig + i * (size_t)kCurveTrigStride;

        double power = 1.0;
        for (int s = 0; s < kStages; s++) {
            power *= powerRatio(cfg.stage[s], t);
        }

        // A power ratio, so 10*log10 rather than 20. Six sections of at most
        // 20 dB each cannot take the product anywhere near the limits of a
        // double; the floor is there for the high-pass, which really does go to
        // zero at DC.
        outDb[i] = (float)(10.0 * log10(power + 1e-300) + trimDb);
    }
}


void curveDb(const Biquad & b, const double * trig, size_t count, float * outDb)
{
    if (!trig || !outDb) return;

    for (size_t i = 0; i < count; i++) {
        outDb[i] = (float)(10.0 * log10(powerRatio(b, trig + i * (size_t)kCurveTrigStride)
                                        + 1e-300));
    }
}

} // namespace paraeq
