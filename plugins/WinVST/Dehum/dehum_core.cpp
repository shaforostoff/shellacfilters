/* ========================================
 *  foo_dsp_dehum - portable DSP core
 * ======================================== */

#include "dehum_core.h"

#include <math.h>
#include <string.h>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#define DEHUM_HAVE_MXCSR 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define DEHUM_HAVE_FPCR 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace dehum {

namespace {

const double kPi = 3.14159265358979323846;

//! Renormalise the recursive rotator this often. Its magnitude drifts by about
//! one epsilon per sample, so 1024 keeps the error at 2e-13 - far below
//! anything audible - while costing four operations per thousand samples.
const int kRenormInterval = 1024;

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//! Finite and within a sane range. NaN fails both comparisons, so it is caught
//! here too - which matters because a NaN reaching the notch integrator would
//! stay there for the rest of the stream.
inline bool sane(double v) {
    return v > -1e30 && v < 1e30;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Flush-to-zero
// ---------------------------------------------------------------------------

#if defined(DEHUM_HAVE_MXCSR)
scoped_flush_denormals::scoped_flush_denormals() : m_saved(_mm_getcsr()) {
    // FTZ only. DAZ lives in bit 6, which the earliest SSE2 parts treat as
    // reserved, and writing it there raises a general protection fault.
    _mm_setcsr((m_saved & ~0x8000u) | 0x8000u);
}
scoped_flush_denormals::~scoped_flush_denormals() {
    _mm_setcsr(m_saved);
}

#elif defined(DEHUM_HAVE_FPCR)

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
    _WriteStatusReg(ARM64_FPCR, value);
#else
    // The clobber stops the arithmetic this is meant to govern being scheduled
    // across the write.
    uint64_t wide = value;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(wide) : "memory");
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
// Params / Config
// ---------------------------------------------------------------------------

void Params::sanitize() {
    if (!(sensitivity >= 0.0f)) sensitivity = 0.0f;
    if (sensitivity > 1.0f) sensitivity = 1.0f;

    if (!(bandwidth >= 0.1f)) bandwidth = 0.1f;
    if (bandwidth > 5.0f) bandwidth = 5.0f;

    if (!(searchTo >= 40.0f)) searchTo = 40.0f;
    if (searchTo > (float)kSearchCeil) searchTo = (float)kSearchCeil;

    harmonics = clampi(harmonics, 1, (int)kMaxHarmonics);

    // 0 means "detect automatically"; anything else is pinned, and 10 Hz is the
    // floor because a notch below that overlaps DC.
    if (!(frequency > 0.0f)) frequency = 0.0f;
    else frequency = (float)clampd(frequency, 10.0, 500.0);

    if (!(rumbleHz > 0.0f)) rumbleHz = 0.0f;
    else rumbleHz = (float)clampd(rumbleHz, 10.0, 200.0);

    if (!(dryWet >= 0.0f)) dryWet = 0.0f;
    if (dryWet > 1.0f) dryWet = 1.0f;
}

bool Params::operator==(const Params & o) const {
    return sensitivity == o.sensitivity
        && bandwidth   == o.bandwidth
        && searchTo    == o.searchTo
        && harmonics   == o.harmonics
        && frequency   == o.frequency
        && rumbleHz    == o.rumbleHz
        && dryWet      == o.dryWet;
}

void Config::compute(const Params & pIn, double rate) {
    Params p = pIn;
    p.sanitize();

    sampleRate = (rate >= 1000.0 && rate <= 8.0e6) ? rate : 44100.0;

    // Window length: aim for kBinTargetHz bins, then take whichever neighbouring
    // power of two is the closer ratio.
    const double want = sampleRate / kBinTargetHz;
    int order = (int)kMinFftOrder;
    while ((double)(1 << order) < want && order < (int)kMaxFftOrder) ++order;
    if (order > (int)kMinFftOrder) {
        const double hi = (double)(1 << order);
        const double lo = hi * 0.5;
        if (want / lo < hi / want) --order;
    }
    fftOrder = order;
    fftSize  = 1 << order;

    // Decimate as far as kDecimMinRate allows. The window keeps its length in
    // seconds and its bin width - fftSize is still what sets both - so the only
    // thing that shrinks is the transform. At every rate this project supports
    // that lands on a 4096 point window, or 2048 at 192 kHz.
    decim = 1;
    decimStages = 0;
    while (decimStages < (int)kDecimMaxStages
           && sampleRate / (double)(decim * 2) >= kDecimMinRate
           && fftSize / (decim * 2) >= 256) {
        decim *= 2;
        ++decimStages;
    }
    winSize = fftSize / decim;
    // An eighth of the window. Hopping more often does not make the detector
    // decide any sooner: successive frames overlap more, so they are that much
    // more correlated, and the evidence counter has to be raised in step. It was
    // measured at a sixteenth and bought nothing but FFTs.
    hop      = fftSize / 8;

    const double df = sampleRate / (double)fftSize;
    const int half = fftSize / 2;

    // Two bins below the nominal floor, so a line sitting exactly on it is still
    // an interior bin and can be peak-picked and interpolated.
    binLo = clampi((int)floor((double)kSearchFloor / df) - 2, 1, half - 2);
    binHi = clampi((int)ceil((double)p.searchTo / df), binLo + 2, half - 2);
    baselineBins = clampi((int)(kBaselineHz / df + 0.5), 2, half / 4);

    // The envelope: the widest search range the user can ask for. Sized from the
    // sample rate alone, so a searchTo move never reallocates.
    const int ceilBin = clampi((int)ceil((double)kSearchCeil / df), binLo + 2, half - 2);
    bufFftSize = fftSize;
    bufBins    = ceilBin - binLo + 1;
    bufHistory = (int)kHistory;

    // Sensitivity 0..1 spans 22..10 dB of prominence. The default 0.5 lands on
    // 16 dB, where the reference line holds an 87% duty cycle and the best any
    // control manages is 10% - which the evidence counter turns into 48 against
    // 16. Raising it past about 0.7 lets control material reach the activation
    // score, so that is where false notches start.
    promDb = 22.0 - 12.0 * (double)p.sensitivity;

    halfWidth = (double)p.bandwidth;
    lamNotch  = clampd(2.0 * kPi * halfWidth / sampleRate, 1e-9, 0.5);

    lamCohNarrow = clampd(2.0 * kPi * kCohNarrowHz / sampleRate, 1e-12, 0.5);
    lamCohWide   = clampd(2.0 * kPi * kCohWideHz / sampleRate, 1e-12, 0.5);
    cohBinHi     = clampi((int)ceil(kCohCeilingHz / df), binLo + 2, binHi);
    cohThreshold = kCohThreshold;
    cohSettle    = clampi((int)(kCohWindowSec * sampleRate + 0.5), 1024, 1 << 26);
    // The sums decay rather than reset, so the ratio is a long-window statistic.
    cohSmooth    = exp(-(double)hop / (kCohWindowSec * sampleRate));
    harmonics = p.harmonics;
    manualFreq = (double)p.frequency;
    rumbleHz   = (double)p.rumbleHz;
    wet        = (double)p.dryWet;

    scoreActivate = kScoreActivate;

    trackSamples = clampi((int)(0.25 * sampleRate + 0.5), 256, 1 << 20);
    trackGain    = 0.7;
    trackClampHz = 2.0;
}

// ---------------------------------------------------------------------------
// Median
// ---------------------------------------------------------------------------

double medianInPlace(double * buf, int n) {
    if (n <= 0) return 0.0;
    if (n == 1) return buf[0];
    // Insertion sort. n is at most a few dozen here - the detector history or
    // the baseline span - and at that size it beats anything cleverer.
    for (int i = 1; i < n; ++i) {
        const double v = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; --j; }
        buf[j + 1] = v;
    }
    if (n & 1) return buf[n / 2];
    return 0.5 * (buf[n / 2 - 1] + buf[n / 2]);
}

namespace {

//! First index of the sorted `buf[0, n)` holding a value not below `v`, which
//! is where `v` belongs and, when `v` is present, where it already sits.
inline int lowerBound(const double * buf, int n, double v) {
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = lo + ((hi - lo) >> 1);
        if (buf[mid] < v) lo = mid + 1; else hi = mid;
    }
    return lo;
}

//! The baseline window centred on bin `i`, sorted, and its median returned.
//! Outside [0, bins) the edge bin repeats, so the window over bin 0 is bin 0
//! `span` times over followed by bins 0 to span.
inline double fillWindow(double * win, int span, const double * med, int bins,
                         int i) {
    const int w = 2 * span + 1;
    for (int k = 0; k < w; ++k) win[k] = med[(size_t)clampi(i + k - span, 0, bins - 1)];
    return medianInPlace(win, w);
}

} // anonymous namespace

void sortedReplaceAt(double * buf, int n, int at, double v) {
    if (v > buf[at]) {
        const int to = at + 1 + lowerBound(buf + at + 1, n - at - 1, v);
        memmove(buf + at, buf + at + 1, (size_t)(to - 1 - at) * sizeof(double));
        buf[to - 1] = v;
    } else {
        const int to = lowerBound(buf, at, v);
        memmove(buf + to + 1, buf + to, (size_t)(at - to) * sizeof(double));
        buf[to] = v;
    }
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

Channel::Channel() {
    m_cfg.compute(Params::defaults(), 44100.0);
}

void Channel::configure(const Config & cfg) {
    m_cfg = cfg;

    const int N = m_cfg.winSize;
    const int M = N / 2;

    designDecimator();

    m_win.assign((size_t)N, 0.0);
    m_taper.assign((size_t)N, 0.0);
    m_fftRe.assign((size_t)M, 0.0);
    m_fftIm.assign((size_t)M, 0.0);
    m_twRe.assign((size_t)(M / 2 > 0 ? M / 2 : 1), 0.0);
    m_twIm.assign((size_t)(M / 2 > 0 ? M / 2 : 1), 0.0);
    m_rev.assign((size_t)M, 0);
    m_mag.assign((size_t)m_cfg.bufBins, 0.0);
    m_hist.assign((size_t)m_cfg.bufHistory * (size_t)m_cfg.bufBins, 0.0);
    m_sorted.assign((size_t)m_cfg.bufBins * (size_t)m_cfg.bufHistory, 0.0);
    m_med.assign((size_t)m_cfg.bufBins, 0.0);
    m_base.assign((size_t)m_cfg.bufBins, 0.0);
    m_baseBuf.assign((size_t)(2 * m_cfg.baselineBins + 1), 0.0);

    // Blackman-Harris, 4 term. -92 dB sidelobes, which is what keeps a loud
    // musical partial from leaking into the baseline around a quiet line.
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    const double denom = (double)(N - 1);
    // Scaled by the decimation factor. A transform sums as many terms as it has
    // points, so decimating would otherwise drop every magnitude by 20*log10 of
    // decim - 24 dB at 44.1 kHz. Nothing downstream would notice, since the
    // detector only ever reads differences between bins, but a diagnostic taken
    // from one build should still mean what it means in the other.
    const double scale = (double)m_cfg.decim;
    for (int n = 0; n < N; ++n) {
        const double t = 2.0 * kPi * (double)n / denom;
        m_taper[(size_t)n] = scale
            * (a0 - a1 * cos(t) + a2 * cos(2.0 * t) - a3 * cos(3.0 * t));
    }

    // FFT twiddles: tw[t] = exp(-2*pi*i*t/M), t < M/2.
    for (int t = 0; t < M / 2; ++t) {
        const double a = 2.0 * kPi * (double)t / (double)M;
        m_twRe[(size_t)t] = cos(a);
        m_twIm[(size_t)t] = -sin(a);
    }

    // Bit reversal for M points.
    int bits = 0;
    while ((1 << bits) < M) ++bits;
    for (int i = 0; i < M; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        m_rev[(size_t)i] = r;
    }

    reset();
}

void Channel::reset() {
    if (!m_win.empty()) memset(&m_win[0], 0, m_win.size() * sizeof(double));
    clearDecimator();
    if (!m_hist.empty()) memset(&m_hist[0], 0, m_hist.size() * sizeof(double));
    m_winPos = 0;
    m_hopAcc = 0;
    m_filled = 0;
    m_histPos = 0;
    m_histFill = 0;
    for (int i = 0; i < (int)kMaxLines; ++i) m_line[i] = Line();
    m_lines = 0;
    memset(m_hpZ, 0, sizeof(m_hpZ));
    m_seen = 0;
    m_confirmations = 0;
    m_dropouts = 0;
    designRumble();
    syncManual();
}

void Channel::flush() {
    // Drop the analysis window and the integrator transients, keep the lines.
    // The cascade is part of the window: its delay lines hold audio from before
    // the seek and would otherwise smear across it.
    if (!m_win.empty()) memset(&m_win[0], 0, m_win.size() * sizeof(double));
    clearDecimator();
    m_winPos = 0;
    m_hopAcc = 0;
    m_filled = 0;
    for (int i = 0; i < m_lines; ++i) {
        for (int h = 0; h < (int)kMaxHarmonics; ++h) {
            Osc & o = m_line[i].osc[h];
            o.wRe = o.wIm = 0.0;
            o.refRe = o.refIm = 0.0;
            o.cosPh = 1.0; o.sinPh = 0.0;
            o.trackAcc = 0;
            o.renorm = 0;
        }
    }
    memset(m_hpZ, 0, sizeof(m_hpZ));
}

//! Drop the detector history. The write position has to go back to zero with the
//! count: rows are filled from zero and the median reads rows [0, fill), so
//! clearing only the count would have the median reading stale rows that the
//! ring has not caught up with yet.
void Channel::clearHistory() {
    m_histPos = 0;
    m_histFill = 0;
}

bool Channel::retune(const Config & cfg) {
    if (!m_cfg.structurallyEquals(cfg)) return false;
    const int oldHi = m_cfg.binHi;
    const double oldManual = m_cfg.manualFreq;
    m_cfg = cfg;
    // The history is indexed from binLo, which never moves, so a searchTo change
    // leaves the columns in place - but any newly exposed column holds stale
    // magnitudes, so the history is dropped rather than trusted.
    if (m_cfg.binHi != oldHi) clearHistory();
    if (m_cfg.manualFreq != oldManual) syncManual();
    else for (int i = 0; i < m_lines; ++i) syncHarmonics(m_line[i]);
    designRumble();
    return true;
}

//! Manual mode holds a single line at the pinned frequency and the detector is
//! not run at all. Switching back to automatic forgets it.
void Channel::syncManual() {
    if (m_cfg.manualFreq > 0.0) {
        const bool fresh = (m_lines != 1) || !m_line[0].manual
                        || m_line[0].detected != m_cfg.manualFreq;
        if (fresh) {
            for (int i = 0; i < (int)kMaxLines; ++i) m_line[i] = Line();
            m_lines = 1;
            m_line[0].detected = m_cfg.manualFreq;
            m_line[0].prom = 0.0;
            m_line[0].score = kScoreCap;
            m_line[0].active = true;
            m_line[0].manual = true;
            startOsc(m_line[0].osc[0], m_cfg.manualFreq);
        }
        syncHarmonics(m_line[0]);
    } else if (m_lines > 0 && m_line[0].manual) {
        // Was a manual line: drop it and hand the job back to the detector.
        for (int i = 0; i < (int)kMaxLines; ++i) m_line[i] = Line();
        m_lines = 0;
        clearHistory();
    }
}

// ---------------------------------------------------------------------------
// Oscillators
// ---------------------------------------------------------------------------

// See the note on the declaration, and on acquisition at the top of the header.
void Channel::adopt(const LineReport * lines, int count) {
    // The user pinning a frequency outranks anything a scout thinks it found
    if (m_cfg.manualFreq > 0.0) return;
    if (!lines || count <= 0) return;

    // The search range in Hz, so a report from a Channel configured at another
    // sample rate - which a scout reading the file's own rate is - still lands in
    // the right place. A line's frequency in Hz does not depend on the rate it
    // was measured at.
    const double binHz = m_cfg.fftSize > 0 ? m_cfg.sampleRate / (double)m_cfg.fftSize : 0.0;
    const double lo = m_cfg.binLo * binHz;
    const double hi = m_cfg.binHi * binHz;

    for (int i = 0; i < count && m_lines < (int)kMaxLines; ++i) {
        const double f = lines[i].frequency;

        if (!(f > 0.0) || f < lo || f > hi) continue;

        // Something within a notch width of it is the same line
        bool held = false;
        for (int k = 0; k < m_lines; ++k) {
            if (fabs(m_line[k].detected - f) < m_cfg.halfWidth) { held = true; break; }
        }
        if (held) continue;

        Line & L = m_line[m_lines++];
        L = Line();
        L.detected = f;
        L.prom     = lines[i].prominence;
        L.active   = true;
        L.manual   = false;
        L.viaCoh   = lines[i].viaCoherence;

        // At the activation threshold rather than at kScoreCap: adopted on
        // somebody else's word, so a line the evidence does not actually support
        // is given up in about the time it took to be believed, instead of the
        // half minute a saturated score would buy it.
        L.score = m_cfg.scoreActivate;
        if (L.viaCoh) L.cohScore = (double)kCohScoreActivate;

        startOsc(L.osc[0], f);
        syncHarmonics(L);
    }
}

void Channel::startOsc(Osc & o, double freq) {
    o.freq = freq;
    o.cosPh = 1.0; o.sinPh = 0.0;
    o.wRe = o.wIm = 0.0;
    o.refRe = o.refIm = 0.0;
    o.wPeak = 0.0;
    o.trackAcc = 0;
    o.renorm = 0;
    o.live = true;
    setOscFreq(o, freq);
}

void Channel::setOscFreq(Osc & o, double freq) {
    o.freq = freq;
    const double inc = 2.0 * kPi * freq / m_cfg.sampleRate;
    o.cosInc = cos(inc);
    o.sinInc = sin(inc);
}

//! Harmonics are locked to h times the fundamental rather than tracked
//! separately. For mains hum that is exactly right, and it is the more robust
//! choice anyway: a harmonic sitting under music has no reliable rotation of its
//! own to measure, so left to itself it would wander.
void Channel::syncHarmonics(Line & line) {
    const double f0 = line.osc[0].freq;
    const double nyq = 0.45 * m_cfg.sampleRate;
    for (int h = 1; h < (int)kMaxHarmonics; ++h) {
        Osc & o = line.osc[h];
        const double f = f0 * (double)(h + 1);
        const bool want = (h < m_cfg.harmonics) && (f > 0.0) && (f < nyq);
        if (!want) { o.live = false; continue; }
        if (!o.live) startOsc(o, f);
        else if (o.freq != f) setOscFreq(o, f);
    }
}

double Channel::runOsc(Osc & o, double x, bool probe) {
    // z = x * conj(e), with e = exp(i*theta)
    const double zr =  x * o.cosPh;
    const double zi = -x * o.sinPh;
    o.wRe += m_cfg.lamNotch * (zr - o.wRe);
    o.wIm += m_cfg.lamNotch * (zi - o.wIm);
    // 2*Re{w*e}: the coherent part of x at this frequency
    const double est = 2.0 * (o.wRe * o.cosPh - o.wIm * o.sinPh);

    if (probe) {
        // The tonality pair. Same heterodyne, two very different bandwidths.
        o.cnRe += m_cfg.lamCohNarrow * (zr - o.cnRe);
        o.cnIm += m_cfg.lamCohNarrow * (zi - o.cnIm);
        o.cwRe += m_cfg.lamCohWide * (zr - o.cwRe);
        o.cwIm += m_cfg.lamCohWide * (zi - o.cwIm);
        o.sumN += o.cnRe * o.cnRe + o.cnIm * o.cnIm;
        o.sumW += o.cwRe * o.cwRe + o.cwIm * o.cwIm;
        if (o.lived < (1 << 30)) ++o.lived;
    }

    const double c = o.cosPh * o.cosInc - o.sinPh * o.sinInc;
    const double s = o.sinPh * o.cosInc + o.cosPh * o.sinInc;
    o.cosPh = c;
    o.sinPh = s;
    if (++o.renorm >= kRenormInterval) {
        o.renorm = 0;
        const double m2 = o.cosPh * o.cosPh + o.sinPh * o.sinPh;
        const double k = 1.5 - 0.5 * m2;   // one Newton step for 1/sqrt near 1
        o.cosPh *= k;
        o.sinPh *= k;
    }
    return x - est;
}

double Channel::runLine(Line & line, double x) {
    double y = x;
    // The fundamental always runs - an inactive line is a probe, and a probe has
    // to be measured before there is anything to decide. Only its output is
    // withheld until the line is confirmed.
    if (line.osc[0].live) {
        const double cleaned = runOsc(line.osc[0], y, true);
        if (line.active) y = cleaned;
    }
    if (line.active) {
        for (int h = 1; h < (int)kMaxHarmonics; ++h) {
            if (line.osc[h].live) y = runOsc(line.osc[h], y, false);
        }
    }

    Osc & f = line.osc[0];
    if (f.live && ++f.trackAcc >= m_cfg.trackSamples) {
        const double m2now = f.wRe * f.wRe + f.wIm * f.wIm;
        const double m2ref = f.refRe * f.refRe + f.refIm * f.refIm;
        const double mag = sqrt(m2now);
        f.wPeak = mag > f.wPeak * m_cfg.trackPeakDecay
                ? mag : f.wPeak * m_cfg.trackPeakDecay;
        const bool worthReading = mag >= m_cfg.trackFloor * f.wPeak;
        if (worthReading && m2now > 1e-26 && m2ref > 1e-26) {
            // angle(w * conj(ref)): how far the estimate has rotated, which is
            // the frequency error times the interval
            const double dot = f.wRe * f.refRe + f.wIm * f.refIm;
            const double crs = f.wIm * f.refRe - f.wRe * f.refIm;
            const double dpsi = atan2(crs, dot);
            double err = dpsi * m_cfg.sampleRate
                       / (2.0 * kPi * (double)f.trackAcc);
            err = clampd(err, -1.0, 1.0);
            double nf = f.freq + m_cfg.trackGain * err;
            nf = clampd(nf, line.detected - m_cfg.trackClampHz,
                            line.detected + m_cfg.trackClampHz);
            if (nf != f.freq) {
                setOscFreq(f, nf);
                syncHarmonics(line);
            }
        }
        f.refRe = f.wRe;
        f.refIm = f.wIm;
        f.trackAcc = 0;
    }
    return y;
}

//! Read each probe's tonality once a hop and turn it into evidence.
//!
//! Confined to lines below kCohCeilingHz: above that a sustained musical note is
//! indistinguishable from a hum by this measure, and scores higher than either
//! real line on the reference material.
void Channel::updateCoherence() {
    for (int i = 0; i < m_lines; ++i) {
        Line & L = m_line[i];
        Osc & f = L.osc[0];
        if (!f.live) continue;

        if (f.sumW > 1e-300) f.coh = sqrt(f.sumN / f.sumW);
        if (f.coh > 1.5) f.coh = 1.5;
        // Forget rather than reset: the ratio is meant to be read over
        // kCohWindowSec, not over one hop. See the note on that constant.
        f.sumN *= m_cfg.cohSmooth;
        f.sumW *= m_cfg.cohSmooth;

        if (L.manual) continue;
        if (f.lived < m_cfg.cohSettle) continue;
        if (L.detected > kCohCeilingHz) continue;

        const double bar = L.active ? m_cfg.cohThreshold - kCohRetainMargin
                                    : m_cfg.cohThreshold;
        if (f.coh >= bar) L.cohScore += 1.0;
        else L.cohScore -= L.active ? kScoreFallActive : kScoreFall;
        if (L.cohScore < 0.0) L.cohScore = 0.0;
        if (L.cohScore > kScoreCap) L.cohScore = kScoreCap;
    }
}

// ---------------------------------------------------------------------------
// Rumble high-pass: 4th order Butterworth, two RBJ biquads
// ---------------------------------------------------------------------------

void Channel::designRumble() {
    m_hpOn = false;
    for (int s = 0; s < 2; ++s) {
        m_hpB[s][0] = 1.0; m_hpB[s][1] = 0.0; m_hpB[s][2] = 0.0;
        m_hpA[s][0] = 0.0; m_hpA[s][1] = 0.0;
    }
    if (m_cfg.rumbleHz <= 0.0) return;

    const double fc = clampd(m_cfg.rumbleHz, 10.0, 0.4 * m_cfg.sampleRate);
    // Butterworth order 4: section Q values 1/(2 cos(pi/8)) and 1/(2 cos(3pi/8))
    const double q[2] = { 1.0 / (2.0 * cos(kPi / 8.0)),
                          1.0 / (2.0 * cos(3.0 * kPi / 8.0)) };
    const double w0 = 2.0 * kPi * fc / m_cfg.sampleRate;
    const double cw = cos(w0), sw = sin(w0);
    for (int s = 0; s < 2; ++s) {
        const double alpha = sw / (2.0 * q[s]);
        const double a0 = 1.0 + alpha;
        m_hpB[s][0] =  (1.0 + cw) * 0.5 / a0;
        m_hpB[s][1] = -(1.0 + cw) / a0;
        m_hpB[s][2] =  (1.0 + cw) * 0.5 / a0;
        m_hpA[s][0] = (-2.0 * cw) / a0;
        m_hpA[s][1] = (1.0 - alpha) / a0;
    }
    m_hpOn = true;
}

double Channel::runRumble(double x) {
    for (int s = 0; s < 2; ++s) {
        // transposed direct form II
        const double y = m_hpB[s][0] * x + m_hpZ[s][0];
        m_hpZ[s][0] = m_hpB[s][1] * x - m_hpA[s][0] * y + m_hpZ[s][1];
        m_hpZ[s][1] = m_hpB[s][2] * x - m_hpA[s][1] * y;
        x = y;
    }
    return x;
}

// ---------------------------------------------------------------------------
// Decimation
// ---------------------------------------------------------------------------

namespace {

//! Modified Bessel function of the first kind, order zero. The series converges
//! in a couple of dozen terms for the beta a 120 dB Kaiser asks for.
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x * 0.5) / (double)k;
        const double add = term * term;
        sum += add;
        if (add < 1e-18 * sum) break;
    }
    return sum;
}

}  // namespace

//! Design the halfband cascade.
//!
//! Stage s runs at rate R and keeps every other sample, so what folds onto the
//! band the detector reads - [0, kDecimPassHz] - is whatever the filter left
//! between R/2 - kDecimPassHz and R/2. Pass to kDecimPassHz, stop from
//! R/2 - kDecimPassHz: a transition centred exactly on R/4, which makes it a
//! halfband, which is why every other coefficient comes out zero and only the
//! rest are stored.
//!
//! The early stages are the cheap ones. Stage 0 needs to pass 560 Hz out of
//! 44100 and stop above 21490, a transition 47% of the band wide, so it is 15
//! taps; by the last stage the same 560 Hz is a fifth of the way to Nyquist and
//! the filter needs 23. Halving the rate each time, the whole cascade costs
//! about twice what its first stage costs.
void Channel::designDecimator() {
    const int stages = m_cfg.decimStages;
    m_hbCoef.clear();
    m_hbTap.clear();
    m_hbZ.clear();
    for (int i = 0; i <= (int)kDecimMaxStages; ++i) { m_hbFirst[i] = 0; m_hbZAt[i] = 0; }
    for (int i = 0; i < (int)kDecimMaxStages; ++i) m_hbZMask[i] = 0;
    if (stages <= 0) { clearDecimator(); return; }

    const double beta = 0.1102 * (kDecimStopDb - 8.7);
    const double i0beta = besselI0(beta);

    double h[kDecimMaxTaps];
    int zTotal = 0;
    for (int st = 0; st < stages; ++st) {
        const double rate = m_cfg.sampleRate / (double)(1 << st);
        // Normalised passband edge, and the transition width that leaves.
        const double fp = kDecimPassHz / rate;
        double trans = 0.5 - 2.0 * fp;
        if (trans < 0.02) trans = 0.02;

        // Kaiser's length estimate, rounded up to 4k+3 - the length a halfband
        // needs for its zeros to land on the even taps.
        int len = (int)ceil((kDecimStopDb - 8.0) / (2.285 * 2.0 * kPi * trans));
        if (len < 7) len = 7;
        while ((len - 3) % 4 != 0) ++len;
        if (len > (int)kDecimMaxTaps) len = (int)kDecimMaxTaps;

        const int half = (len - 1) / 2;
        double dc = 0.0;
        for (int j = 0; j < len; ++j) {
            const int n = j - half;
            double ideal;
            if (n == 0)            ideal = 0.5;
            else if ((n & 1) == 0) ideal = 0.0;   // exactly zero, by halfband
            else                   ideal = sin(kPi * (double)n * 0.5) / (kPi * (double)n);
            const double r = (double)n / (double)half;
            const double w = besselI0(beta * sqrt(1.0 - r * r)) / i0beta;
            h[(size_t)j] = ideal * w;
            dc += h[(size_t)j];
        }
        // Unity at DC, so the magnitudes the detector measures are the ones the
        // signal actually has and prominence reads off the same scale at every
        // sample rate.
        for (int j = 0; j < len; ++j) h[(size_t)j] /= dc;

        m_hbFirst[st] = (int)m_hbCoef.size();
        for (int j = 0; j < len; ++j) {
            if (h[(size_t)j] == 0.0) continue;
            m_hbCoef.push_back(h[(size_t)j]);
            m_hbTap.push_back(j);
        }

        int zlen = 1;
        while (zlen < len) zlen <<= 1;
        m_hbZAt[st]   = zTotal;
        m_hbZMask[st] = zlen - 1;
        zTotal += zlen;
    }
    m_hbFirst[stages] = (int)m_hbCoef.size();
    m_hbZAt[stages]   = zTotal;
    m_hbZ.assign((size_t)zTotal, 0.0);
    clearDecimator();
}

void Channel::clearDecimator() {
    if (!m_hbZ.empty()) memset(&m_hbZ[0], 0, m_hbZ.size() * sizeof(double));
    for (int i = 0; i < (int)kDecimMaxStages; ++i) { m_hbPos[i] = 0; m_hbPhase[i] = 0; }
}

//! One full rate sample in. True when the cascade produced one, which is once
//! every m_cfg.decim calls.
bool Channel::decimate(double x, double * out) {
    const int stages = m_cfg.decimStages;
    double v = x;
    for (int st = 0; st < stages; ++st) {
        const int mask = m_hbZMask[st];
        const int base = m_hbZAt[st];
        const int p = (m_hbPos[st] + 1) & mask;
        m_hbPos[st] = p;
        m_hbZ[(size_t)(base + p)] = v;

        // Half the inputs only fill the line; there is no output to compute for
        // them, and not computing it is the whole point of decimating in stages.
        m_hbPhase[st] ^= 1;
        if (m_hbPhase[st] != 0) return false;

        double acc = 0.0;
        const int lo = m_hbFirst[st], hi = m_hbFirst[st + 1];
        for (int t = lo; t < hi; ++t) {
            const int k = (p - m_hbTap[(size_t)t] + mask + 1) & mask;
            acc += m_hbCoef[(size_t)t] * m_hbZ[(size_t)(base + k)];
        }
        v = acc;
    }
    *out = v;
    return true;
}

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------

//! Magnitude of the windowed spectrum over [binLo, binHi], into m_mag.
//!
//! A real FFT of length N done as a complex FFT of length N/2: the even samples
//! go in the real part and the odd ones in the imaginary part, and the true
//! spectrum is unpacked afterwards. Only the bins in the search range are
//! unpacked, which is a couple of hundred out of tens of thousands.
void Channel::realFftMagnitudes() {
    const int N = m_cfg.winSize;
    const int M = N / 2;
    const int mask = N - 1;

    // Pack the windowed signal, reading the ring oldest-first.
    for (int k = 0; k < M; ++k) {
        const int n0 = 2 * k, n1 = n0 + 1;
        m_fftRe[(size_t)k] = m_win[(size_t)((m_winPos + n0) & mask)]
                           * m_taper[(size_t)n0];
        m_fftIm[(size_t)k] = m_win[(size_t)((m_winPos + n1) & mask)]
                           * m_taper[(size_t)n1];
    }

    for (int i = 0; i < M; ++i) {
        const int r = m_rev[(size_t)i];
        if (r > i) {
            double t = m_fftRe[(size_t)i]; m_fftRe[(size_t)i] = m_fftRe[(size_t)r]; m_fftRe[(size_t)r] = t;
            t = m_fftIm[(size_t)i]; m_fftIm[(size_t)i] = m_fftIm[(size_t)r]; m_fftIm[(size_t)r] = t;
        }
    }

    for (int len = 2; len <= M; len <<= 1) {
        const int h = len >> 1;
        const int step = M / len;
        for (int i = 0; i < M; i += len) {
            for (int j = 0; j < h; ++j) {
                const int t = j * step;
                const double wr = m_twRe[(size_t)t], wi = m_twIm[(size_t)t];
                const int a = i + j, b = a + h;
                const double br = m_fftRe[(size_t)b], bi = m_fftIm[(size_t)b];
                const double xr = br * wr - bi * wi;
                const double xi = br * wi + bi * wr;
                m_fftRe[(size_t)b] = m_fftRe[(size_t)a] - xr;
                m_fftIm[(size_t)b] = m_fftIm[(size_t)a] - xi;
                m_fftRe[(size_t)a] += xr;
                m_fftIm[(size_t)a] += xi;
            }
        }
    }

    // Unpack: X[k] = E[k] + exp(-2*pi*i*k/N) * O[k], where E and O come from
    // Z[k] and conj(Z[M-k]).
    const int lo = m_cfg.binLo, hi = m_cfg.binHi;
    for (int k = lo; k <= hi; ++k) {
        const int kc = (k == 0) ? 0 : (M - k);
        const double ar = m_fftRe[(size_t)(k % M)], ai = m_fftIm[(size_t)(k % M)];
        const double br = m_fftRe[(size_t)(kc % M)], bi = m_fftIm[(size_t)(kc % M)];
        const double er = 0.5 * (ar + br), ei = 0.5 * (ai - bi);
        const double or_ = 0.5 * (ai + bi), oi = -0.5 * (ar - br);
        const double a = 2.0 * kPi * (double)k / (double)N;
        const double c = cos(a), s = sin(a);
        // (c - i s) * (or_ + i oi)
        const double xr = er + c * or_ + s * oi;
        const double xi = ei + c * oi - s * or_;
        m_mag[(size_t)(k - lo)] = sqrt(xr * xr + xi * xi);
    }
}

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------

void Channel::runDetector() {
    if (m_cfg.manualFreq > 0.0) return;

    realFftMagnitudes();

    const int bins = m_cfg.binHi - m_cfg.binLo + 1;
    if (!historyMedian(bins)) return;
    baselineMedian(bins);
    detectPeaks(bins);
}

//! This frame's magnitudes into the history, and the median over the history
//! of every bin into m_med, in dB. One pass, because they are now the same
//! operation: every bin keeps its history sorted alongside it in arrival
//! order, a frame displaces one value in that sorted row rather than calling
//! for a fresh sort of it, and the median is then a read at the middle.
//!
//! False while fewer than four frames are in. Four is enough for the median to
//! mean something, and waiting for the full history would put first detection
//! another four seconds out.
bool Channel::historyMedian(int bins) {
    const int stride = m_cfg.bufBins;
    const int hs     = m_cfg.bufHistory;
    const bool full  = (m_histFill >= hs);
    double * row     = &m_hist[(size_t)m_histPos * (size_t)stride];

    for (int i = 0; i < bins; ++i) {
        const double v = m_mag[(size_t)i];
        double * w = &m_sorted[(size_t)i * (size_t)hs];
        if (!full) {
            // Still filling: rows are taken in order, so this one is new and
            // nothing leaves to make room for it.
            const int at = lowerBound(w, m_histFill, v);
            memmove(w + at + 1, w + at,
                    (size_t)(m_histFill - at) * sizeof(double));
            w[at] = v;
        } else {
            const int at = lowerBound(w, hs, row[i]);
            if (at < hs && w[at] == row[i]) {
                sortedReplaceAt(w, hs, at, v);
            } else {
                // Only reachable if a magnitude is NaN, which process() rules
                // out by zeroing anything not sane on the way in. Kept so that
                // losing that guarantee upstream costs a rebuilt row rather
                // than a write off the end of one.
                row[i] = v;
                for (int h = 0; h < hs; ++h)
                    w[h] = m_hist[(size_t)h * (size_t)stride + (size_t)i];
                medianInPlace(w, hs);
                continue;
            }
        }
        row[i] = v;
    }

    if (++m_histPos >= hs) m_histPos = 0;
    if (m_histFill < hs) ++m_histFill;
    if (m_histFill < 4) return false;

    const int n = m_histFill;
    for (int i = 0; i < bins; ++i) {
        const double * w = &m_sorted[(size_t)i * (size_t)hs];
        const double m = (n & 1) ? w[n / 2] : 0.5 * (w[n / 2 - 1] + w[n / 2]);
        m_med[(size_t)i] = 20.0 * log10(m + 1e-30);
    }
    return true;
}

//! The local baseline of m_med into m_base: the median over kBaselineHz either
//! side of every bin.
//!
//! The window is carried sorted from one bin to the next rather than sorted
//! afresh for each. Stepping one bin along drops one value and admits one, so
//! the work is a binary search and a memmove instead of a sort - 61 values a
//! bin at the default settings, where a sort of the same window is nearer
//! nine hundred operations. This pass was two thirds of the detector's time
//! before that, and five sixths of it with the search range opened up.
//!
//! The result is the same window, so it is the same median: the window length
//! is odd, which leaves no middle pair to break a tie between.
void Channel::baselineMedian(int bins) {
    const int span = m_cfg.baselineBins;
    const int w    = 2 * span + 1;
    double * win   = &m_baseBuf[0];
    const double * med = &m_med[0];

    m_base[0] = fillWindow(win, span, med, bins, 0);

    for (int i = 1; i < bins; ++i) {
        const double gone = med[(size_t)clampi(i - 1 - span, 0, bins - 1)];
        const double came = med[(size_t)clampi(i + span, 0, bins - 1)];
        if (gone == came) { m_base[(size_t)i] = win[span]; continue; }

        const int a = lowerBound(win, w, gone);
        if (a >= w || win[a] != gone) {
            // Only reachable if m_med holds a NaN, which process() rules out by
            // zeroing anything not sane on the way in. Kept so that losing that
            // guarantee upstream costs a rebuilt window rather than a write off
            // the end of this one.
            m_base[(size_t)i] = fillWindow(win, span, med, bins, i);
            continue;
        }
        sortedReplaceAt(win, w, a, came);
        m_base[(size_t)i] = win[span];
    }
}

//! Credit for one sighting: one, plus a bonus for how far past the threshold the
//! prominence is. Strong evidence should count for more than marginal evidence,
//! and on the reference material that is exactly what tells the two apart - the
//! hum sits about 3 dB past the threshold and the music peaks that reach it
//! barely clear it.
double Channel::scoreFor(double prom) const {
    double bonus = (prom - m_cfg.promDb) * kScoreBonusPerDb;
    if (bonus < 0.0) bonus = 0.0;
    if (bonus > kScoreBonusMax) bonus = kScoreBonusMax;
    return 1.0 + bonus;
}

void Channel::detectPeaks(int bins) {
    const double df = m_cfg.sampleRate / (double)m_cfg.fftSize;

    // Every peak clearing the retain threshold - the weaker bar - so an engaged
    // line can be matched in a frame where it would not have been worth
    // creating. The list is longer than kMaxLines so a transient music peak
    // cannot crowd a real line out of it before the lines have had their pick.
    const double retainDb = m_cfg.promDb - kRetainMarginDb;
    enum { kCandidates = 4 * kMaxLines };
    double candF[kCandidates];
    double candP[kCandidates];
    bool   candTaken[kCandidates];
    int nc = 0;

    for (int i = 1; i < bins - 1; ++i) {
        const double prom = m_med[(size_t)i] - m_base[(size_t)i];
        if (prom < retainDb) continue;
        if (m_med[(size_t)i] < m_med[(size_t)(i - 1)]) continue;
        if (m_med[(size_t)i] < m_med[(size_t)(i + 1)]) continue;

        // Quadratic interpolation on the log magnitude places the line inside
        // the bin. Without it the notch would sit up to half a bin out, and at
        // 0.67 Hz bins half a bin costs about 10 dB of removal.
        const double a = m_med[(size_t)(i - 1)];
        const double b = m_med[(size_t)i];
        const double c = m_med[(size_t)(i + 1)];
        const double d = a - 2.0 * b + c;
        double off = (fabs(d) < 1e-12) ? 0.0 : 0.5 * (a - c) / d;
        off = clampd(off, -0.5, 0.5);
        const double f = ((double)(m_cfg.binLo + i) + off) * df;

        int at = nc;
        if (nc < (int)kCandidates) ++nc;
        else {
            int worst = 0;
            for (int k = 1; k < nc; ++k) if (candP[k] < candP[worst]) worst = k;
            if (candP[worst] >= prom) continue;
            at = worst;
        }
        candF[at] = f;
        candP[at] = prom;
    }

    // Second nomination pass, for the coherence route: the loudest local maxima
    // in the low band, prominent or not. This is how a line standing at the
    // level of the pedestal it sits on gets a probe at all - it will never clear
    // a prominence threshold, so nothing above decides it is worth looking at.
    {
        const int cohBins = m_cfg.cohBinHi - m_cfg.binLo + 1;
        for (int n = 0; n < (int)kNominees; ++n) {
            int best = -1;
            for (int i = 1; i < cohBins - 1 && i < bins - 1; ++i) {
                if (m_med[(size_t)i] < m_med[(size_t)(i - 1)]) continue;
                if (m_med[(size_t)i] < m_med[(size_t)(i + 1)]) continue;
                const double fi = (double)(m_cfg.binLo + i) * df;
                bool taken = false;
                for (int c = 0; c < nc; ++c) {
                    if (fabs(candF[c] - fi) < 1.5 * df) { taken = true; break; }
                }
                if (taken) continue;
                if (best < 0 || m_med[(size_t)i] > m_med[(size_t)best]) best = i;
            }
            if (best < 0 || nc >= (int)kCandidates) break;
            const double a = m_med[(size_t)(best - 1)];
            const double b = m_med[(size_t)best];
            const double c = m_med[(size_t)(best + 1)];
            const double d = a - 2.0 * b + c;
            double off = (fabs(d) < 1e-12) ? 0.0 : 0.5 * (a - c) / d;
            off = clampd(off, -0.5, 0.5);
            candF[nc] = ((double)(m_cfg.binLo + best) + off) * df;
            candP[nc] = m_med[(size_t)best] - m_base[(size_t)best];
            ++nc;
        }
    }

    for (int c = 0; c < nc; ++c) candTaken[c] = false;

    const double tol = 1.5 * df;

    // Existing lines get first refusal on the peaks, so a line already being
    // cancelled is never displaced by a louder newcomer.
    for (int i = 0; i < m_lines; ++i) {
        int best = -1;
        for (int c = 0; c < nc; ++c) {
            if (candTaken[c]) continue;
            if (fabs(m_line[i].detected - candF[c]) >= tol) continue;
            if (best < 0 || fabs(m_line[i].detected - candF[c])
                          < fabs(m_line[i].detected - candF[best])) best = c;
        }
        // A line that has not engaged yet still has to clear the full threshold
        // to make progress; only an engaged one gets the discount.
        if (best >= 0 && !m_line[i].active && candP[best] < m_cfg.promDb) best = -1;

        if (best < 0) {
            m_line[i].score -= m_line[i].active ? kScoreFallActive : kScoreFall;
            if (m_line[i].score < 0.0) m_line[i].score = 0.0;
            continue;
        }
        candTaken[best] = true;
        // Slew where the detector believes the line is. This is the slow half of
        // frequency tracking: it follows drift that the notch's own tracker is
        // clamped out of, and it re-centres that clamp window. Only the notch's
        // own tracker moves the oscillator; this just re-centres its limits.
        m_line[i].detected += 0.25 * (candF[best] - m_line[i].detected);
        m_line[i].prom = candP[best];
        // Prominence evidence only accrues from prominent sightings; a nominee
        // that is merely a local maximum keeps the line alive as a probe but
        // does not argue that it is hum.
        if (candP[best] >= m_cfg.promDb) {
            m_line[i].score += scoreFor(candP[best]);
            if (m_line[i].score > kScoreCap) m_line[i].score = kScoreCap;
        }
    }

    // Whatever is left over, strongest first, can start a new line. A prominent
    // peak earns prominence evidence immediately; a bare nominee starts at zero
    // and has kCohSettleSec to prove itself by coherence instead.
    while (m_lines < (int)kMaxLines) {
        int best = -1;
        for (int c = 0; c < nc; ++c) {
            if (candTaken[c]) continue;
            if (best < 0 || candP[c] > candP[best]) best = c;
        }
        if (best < 0) break;
        candTaken[best] = true;
        // Not within tol of a line already held.
        bool clash = false;
        for (int i = 0; i < m_lines; ++i) {
            if (fabs(m_line[i].detected - candF[best]) < tol) { clash = true; break; }
        }
        if (clash) continue;
        const int at = m_lines++;
        m_line[at] = Line();
        m_line[at].detected = candF[best];
        m_line[at].prom = candP[best];
        m_line[at].score = (candP[best] >= m_cfg.promDb) ? scoreFor(candP[best]) : 0.0;
        // The probe starts now, not at activation: the coherence measurement
        // needs the frequency tracker to have locked first, and restarting the
        // oscillator when the line engages would throw that lock away.
        startOsc(m_line[at].osc[0], m_line[at].detected);
    }

    for (int i = 0; i < m_lines; ++i) {
        if (m_line[i].active) continue;
        const bool byProm = m_line[i].score >= m_cfg.scoreActivate;
        const bool byCoh  = m_line[i].cohScore >= (double)kCohScoreActivate;
        if (!byProm && !byCoh) continue;
        m_line[i].active = true;
        m_line[i].viaCoh = !byProm;
        ++m_confirmations;
        if (!m_line[i].osc[0].live) startOsc(m_line[i].osc[0], m_line[i].detected);
        syncHarmonics(m_line[i]);
    }

    // Two candidates half a bin apart on the shoulders of one line both get
    // pulled onto it by their trackers, and then notch it twice. Once they land
    // within a notch width of each other they are the same line, so the weaker
    // evidence is discarded.
    const double merge = 2.0 * m_cfg.halfWidth;
    for (int i = 0; i < m_lines; ++i) {
        if (m_line[i].osc[0].live == false) continue;
        const double fi = m_line[i].osc[0].freq;
        for (int j = i + 1; j < m_lines; ++j) {
            if (!m_line[j].osc[0].live) continue;
            const double fj = m_line[j].osc[0].freq;
            if (fabs(fi - fj) >= merge) continue;
            const double ei = m_line[i].score + m_line[i].cohScore;
            const double ej = m_line[j].score + m_line[j].cohScore;
            const int drop = (ei >= ej) ? j : i;
            m_line[drop].score = 0.0;
            m_line[drop].cohScore = 0.0;
            m_line[drop].osc[0].lived = m_cfg.cohSettle;  // no grace on a merge
            m_line[drop].active = false;   // a merge is not a dropout
            if (drop == i) break;
        }
    }

    // Forget the ones whose evidence has run out, keeping the array packed. A
    // young probe is spared: it is still being measured, and dropping it before
    // kCohSettleSec would mean the coherence route never got to answer.
    int w = 0;
    for (int i = 0; i < m_lines; ++i) {
        const bool spent = m_line[i].score <= 0.0 && m_line[i].cohScore <= 0.0;
        const bool young = m_line[i].osc[0].lived < m_cfg.cohSettle;
        if (spent && !young) {
            if (m_line[i].active) ++m_dropouts;
            continue;
        }
        if (w != i) m_line[w] = m_line[i];
        ++w;
    }
    for (int i = w; i < m_lines; ++i) m_line[i] = Line();
    m_lines = w;
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------

template<typename Sample>
void Channel::process(Sample * io, size_t frames, size_t stride) {
    const int N = m_cfg.winSize;
    const double wet = m_cfg.wet;

    for (size_t i = 0; i < frames; ++i) {
        double x = (double)io[i * stride];
        if (!sane(x)) x = 0.0;

        // The detector reads the input, never the output. Feeding it the cleaned
        // signal would be a loop: the line would vanish, its prominence would
        // fall below threshold, the notch would be dropped and the hum would
        // come back.
        double dec;
        if (decimate(x, &dec)) {
            m_win[(size_t)m_winPos] = dec;
            if (++m_winPos >= N) m_winPos = 0;
            if (m_filled < N) ++m_filled;
        }
        // Still counted at full rate: hop is a multiple of decim, so a hop
        // boundary is always a decimated sample boundary too, and cohSmooth was
        // derived from hop in full rate samples.
        if (++m_hopAcc >= m_cfg.hop) {
            m_hopAcc = 0;
            updateCoherence();                 // probes are read every hop
            if (m_filled >= N) runDetector();  // nomination needs a full window
        }

        double y = x;
        // Every line runs, active or not: an inactive one is a probe being
        // measured, and it must see the same signal a confirmed line would.
        for (int L = 0; L < m_lines; ++L) y = runLine(m_line[L], y);
        if (m_hpOn) y = runRumble(y);

        // wet == 0 returns x untouched, bit for bit.
        io[i * stride] = (Sample)(x + wet * (y - x));
    }
    m_seen += frames;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

int Channel::lineCount() const {
    int n = 0;
    for (int i = 0; i < m_lines; ++i) if (m_line[i].active) ++n;
    return n;
}

void Channel::report(LineReport * out, int max, int * count) const {
    int n = 0;
    for (int i = 0; i < m_lines && n < max; ++i) {
        if (!m_line[i].active) continue;
        const Osc & o = m_line[i].osc[0];
        out[n].frequency  = o.freq;
        out[n].detected   = m_line[i].detected;
        out[n].prominence = m_line[i].prom;
        out[n].amplitude  = 2.0 * sqrt(o.wRe * o.wRe + o.wIm * o.wIm);
        out[n].coherence  = o.coh;
        out[n].viaCoherence = m_line[i].viaCoh;
        int h = 0;
        for (int k = 0; k < (int)kMaxHarmonics; ++k) if (m_line[i].osc[k].live) ++h;
        out[n].harmonics = h;
        ++n;
    }
    if (count) *count = n;
}

size_t Channel::heapBytes() const {
    size_t b = 0;
    b += m_win.capacity() * sizeof(double);
    b += m_taper.capacity() * sizeof(double);
    b += m_hbCoef.capacity() * sizeof(double);
    b += m_hbZ.capacity() * sizeof(double);
    b += m_hbTap.capacity() * sizeof(int);
    b += m_fftRe.capacity() * sizeof(double);
    b += m_fftIm.capacity() * sizeof(double);
    b += m_twRe.capacity() * sizeof(double);
    b += m_twIm.capacity() * sizeof(double);
    b += m_rev.capacity() * sizeof(int);
    b += m_mag.capacity() * sizeof(double);
    b += m_hist.capacity() * sizeof(double);
    b += m_med.capacity() * sizeof(double);
    b += m_base.capacity() * sizeof(double);
    b += m_sorted.capacity() * sizeof(double);
    b += m_baseBuf.capacity() * sizeof(double);
    return b;
}

template void Channel::process<float>(float *, size_t, size_t);
template void Channel::process<double>(double *, size_t, size_t);

} // namespace dehum
