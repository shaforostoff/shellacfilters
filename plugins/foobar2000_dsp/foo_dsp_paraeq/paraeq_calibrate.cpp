/* ========================================
 *  foo_dsp_paraeq - room calibration for the Reverb cut band
 *
 *  See paraeq_calibrate.h for what is measured and why.
 * ======================================== */

#include "paraeq_calibrate.h"

#include <algorithm>
#include <math.h>
#include <mutex>

namespace paraeq_calib {

namespace {

const double kPi = 3.14159265358979323846;

//! Resampler kernel: zero crossings either side, and table points per
//! crossing. Sixteen crossings under a Blackman window put the stopband past
//! 70 dB, far more than a phone microphone's own noise floor needs.
const int kZeroCrossings = 16;
const int kTablePerCrossing = 512;

//! The search ignores what lies outside this band: footsteps and handling
//! noise below, the resampler's transition above.
const double kSearchBandLoHz = 150.0;
const double kSearchBandHiHz = 3500.0;

//! An output block quieter than this is not compared: the microphone would be
//! hearing the room's own noise. Low, because the digital level says little
//! about the acoustic one - a quiet transfer played loud sits near -50 dBFS
//! RMS after the resampler and is perfectly measurable, and a block that was
//! found in the output at all has music in it.
const double kMinOutputDb = -70.0;

double toDb(double power) { return 10.0 * log10(power + 1e-30); }

// ---------------------------------------------------------------------------
// Fitting a bell to a bump
//
// Subtracting a smooth trend and reading the bump's height and half-height
// width off what is left gets the centre right and the rest wrong: the trend
// is lifted by the bell's own skirts, so a Q 1.5, +6 dB bell measured that way
// comes out at Q 1.9 and +4. Instead the measured curve, two octaves either
// side of the bump, is fitted with the thing that would cancel it - a bell -
// plus a quadratic in log frequency for whatever the loudspeaker and the
// microphone do. The bell's gain enters almost linearly and the quadratic
// exactly, so for each centre and Q those four numbers are one small least
// squares; centre and Q are searched on a grid.
//
// The bell is the analog prototype, which the cookbook design matches well
// below Nyquist and which does not depend on where its centre is, so one
// table of shapes per Q serves every centre. It is smoothed exactly as the
// measurement was, a sixth of an octave either side with Hann weights and
// linearly spaced bins, so a narrow bell is compared with what a narrow bump
// looks like after smoothing rather than with itself.
// ---------------------------------------------------------------------------

const int kFitSpan    = 2 * kGridPerOctave;   //!< fit window, either side
const int kFitShift   = kGridPerOctave / 4;   //!< centre search, either side
const int kSmoothSpan = kGridPerOctave / 6;   //!< the measurement's smoothing
//! Q 0.7 to 8 in sixths of an octave. Not down to the band's 0.5: a bell two
//! and a half octaves wide inside a four-octave window is indistinguishable
//! from the quadratic beside it, and on a flat cable the fit was found
//! turning the card's own low-end slope into one.
const int kFitQSteps  = 21;
const double kFitQMin = 0.7;

double fitQ(int i) { return kFitQMin * pow(2.0, i / 6.0); }

//! Analog peaking EQ, dB, `oct` octaves from its centre.
double bellDb(double oct, double q, double gainDb)
{
    const double w = pow(2.0, oct);
    const double A = pow(10.0, gainDb / 40.0);
    const double d = (1.0 - w * w) * (1.0 - w * w);
    const double num = d + (w * A / q) * (w * A / q);
    const double den = d + (w / (A * q)) * (w / (A * q));
    return 10.0 * log10(num / den);
}

//! Smoothed bell shape over grid offsets [-D, D], divided by its gain so
//! that the fit's coefficient is the gain in dB.
void bellShape(double q, double gainDb, int D, std::vector<double> & out)
{
    out.resize((size_t)(2 * D + 1));
    for (int d = -D; d <= D; ++d) {
        double s = 0.0, sw = 0.0;
        for (int k = -kSmoothSpan; k <= kSmoothSpan; ++k) {
            const double w = (0.5 + 0.5 * cos(kPi * k / (kSmoothSpan + 1))) * pow(2.0, (double)k / kGridPerOctave);
            s  += w * pow(10.0, bellDb((double)(d + k) / kGridPerOctave, q, gainDb) / 10.0);
            sw += w;
        }
        out[(size_t)(d + D)] = 10.0 * log10(s / sw) / gainDb;
    }
}

//! Solves the 4x4 system in place; false if it is singular.
bool solve4(double a[4][5])
{
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 5; ++k) std::swap(a[p][k], a[c][k]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (int k = c; k < 5; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) a[r][4] /= a[r][r];
    return true;
}

struct BellFit {
    int    centre = 0;      //!< grid index
    double q = 1.0;
    double gainDb = 0.0;
    double sse = 1e300;
};

//! The best bell near grid index `jc`, centred no lower than `cLo` and no
//! higher than `cHi` - the band's range, which a suggestion never leaves.
//! False if no positive bell fits.
bool fitBell(const std::vector<double> & resp, int jc, int cLo, int cHi, BellFit & best)
{
    const int n = (int)resp.size();
    const int lo = std::max(0, jc - kFitSpan), hi = std::min(n - 1, jc + kFitSpan);
    const int D = kFitSpan + kFitShift;
    std::vector<double> shape;
    best = BellFit();

    // Twice: the shape of a bell changes a little with its gain, so the
    // second pass uses the first pass's gain, over Q near the first answer.
    double g0 = 6.0;
    int qLo = 0, qHi = kFitQSteps;
    for (int pass = 0; pass < 2; ++pass) {
        BellFit found;
        for (int qi = qLo; qi <= qHi; ++qi) {
            const double q = fitQ(qi);
            bellShape(q, g0, D, shape);
            for (int c = std::max(cLo, jc - kFitShift); c <= std::min(cHi, jc + kFitShift); ++c) {
                double a[4][5] = {};
                for (int j = lo; j <= hi; ++j) {
                    const double u = (double)(j - jc) / kGridPerOctave;
                    const double x[4] = { 1.0, u, u * u, shape[(size_t)(j - c + D)] };
                    for (int p = 0; p < 4; ++p) {
                        for (int k = 0; k < 4; ++k) a[p][k] += x[p] * x[k];
                        a[p][4] += x[p] * resp[(size_t)j];
                    }
                }
                if (!solve4(a)) continue;
                const double g = a[3][4];
                if (!(g > 0.0)) continue;
                double sse = 0.0;
                for (int j = lo; j <= hi; ++j) {
                    const double u = (double)(j - jc) / kGridPerOctave;
                    const double m = a[0][4] + a[1][4] * u + a[2][4] * u * u + g * shape[(size_t)(j - c + D)];
                    sse += (resp[(size_t)j] - m) * (resp[(size_t)j] - m);
                }
                if (sse < found.sse) { found.centre = c; found.q = q; found.gainDb = g; found.sse = sse; }
            }
        }
        if (!(found.sse < 1e300)) return pass > 0;
        best = found;
        g0 = std::max(1.0, std::min(20.0, found.gainDb));
        const int qi = (int)floor(6.0 * log2(found.q / kFitQMin) + 0.5);
        qLo = std::max(0, qi - 3);
        qHi = std::min(kFitQSteps, qi + 3);
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------

void Resampler::reset(double inRate)
{
    m_in = inRate;
    m_buf.clear();
    m_pos = 0.0;
    if (!(inRate > 0.0)) return;

    m_step = inRate / kRate;
    // Cutoff in cycles per input sample: 0.45 of the output rate when
    // decimating, 0.45 of the input rate otherwise.
    const double fc = 0.45 * std::min(1.0, kRate / inRate);
    m_scale = 2.0 * fc;
    m_half  = kZeroCrossings / m_scale;

    const int n = kZeroCrossings * kTablePerCrossing;
    m_table.assign((size_t)n + 2, 0.0f);
    for (int i = 0; i <= n; ++i) {
        const double u = (double)i / kTablePerCrossing;
        const double v = u / kZeroCrossings;
        const double sinc = (i == 0) ? 1.0 : sin(kPi * u) / (kPi * u);
        const double win  = 0.42 + 0.5 * cos(kPi * v) + 0.08 * cos(2.0 * kPi * v);
        m_table[(size_t)i] = (float)(sinc * win);
    }
}

void Resampler::process(const float * in, size_t n, std::vector<float> & out)
{
    if (!(m_in > 0.0) || n == 0) return;
    m_buf.insert(m_buf.end(), in, in + n);

    const double tableScale = m_scale * kTablePerCrossing;
    const size_t tableLast = m_table.size() - 1;
    for (;;) {
        const double c = m_pos;
        const long hi = (long)floor(c + m_half);
        if (hi >= (long)m_buf.size()) break;
        long lo = (long)ceil(c - m_half);
        if (lo < 0) lo = 0;     // zeros before the first sample

        double acc = 0.0;
        for (long i = lo; i <= hi; ++i) {
            const double u = fabs(c - (double)i) * tableScale;
            const size_t k = (size_t)u;
            if (k >= tableLast) continue;
            const double f = u - (double)k;
            acc += (double)m_buf[(size_t)i] * (m_table[k] + f * (m_table[k + 1] - m_table[k]));
        }
        out.push_back((float)(acc * m_scale));
        m_pos += m_step;
    }

    const long drop = (long)floor(m_pos - m_half) - 1;
    if (drop > 8192) {
        m_buf.erase(m_buf.begin(), m_buf.begin() + drop);
        m_pos -= (double)drop;
    }
}

// ---------------------------------------------------------------------------
// Fft
// ---------------------------------------------------------------------------

void Fft::init(unsigned order)
{
    const size_t n = (size_t)1 << order;
    m_cos.resize(n / 2);
    m_sin.resize(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        m_cos[k] = cos(2.0 * kPi * (double)k / (double)n);
        m_sin[k] = sin(2.0 * kPi * (double)k / (double)n);
    }
    m_rev.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = 0;
        for (unsigned b = 0; b < order; ++b) if (i & ((size_t)1 << b)) r |= 1u << (order - 1 - b);
        m_rev[i] = r;
    }
}

void Fft::run(double * re, double * im, bool inverse) const
{
    const size_t n = m_rev.size();
    for (size_t i = 0; i < n; ++i) {
        const size_t j = m_rev[i];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    const double sign = inverse ? 1.0 : -1.0;
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2;
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < half; ++j) {
                const double wr = m_cos[j * step];
                const double wi = sign * m_sin[j * step];
                double * ar = re + i + j,        * ai = im + i + j;
                double * br = re + i + j + half, * bi = im + i + j + half;
                const double tr = *br * wr - *bi * wi;
                const double ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr;      *ai += ti;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Analyzer
// ---------------------------------------------------------------------------

Analyzer::Analyzer(double historySec)
{
    historySec = std::max(1.0, std::min(kMaxHistorySec, historySec));
    m_hist.assign((size_t)(historySec * kRate), 0.0f);
    m_corrFft.init(kCorrOrder);
    m_frameFft.init(11);
    m_re.resize((size_t)1 << kCorrOrder);
    m_im.resize((size_t)1 << kCorrOrder);
    m_window.resize(kFrame);
    for (int i = 0; i < kFrame; ++i) m_window[(size_t)i] = 0.5 - 0.5 * cos(2.0 * kPi * i / kFrame);
    m_sxx.assign(kBins, 0.0);
    m_syy.assign(kBins, 0.0);
    m_mic.reserve(kBlock * 2);
}

void Analyzer::resetMeasurement()
{
    std::fill(m_sxx.begin(), m_sxx.end(), 0.0);
    std::fill(m_syy.begin(), m_syy.end(), 0.0);
    m_blockSpectra.clear();
    m_result = Result();
    m_mic.clear();
    m_micPeak = 0.0f;
    m_micRs.reset(0.0);
}

void Analyzer::resetAll()
{
    resetMeasurement();
    std::fill(m_hist.begin(), m_hist.end(), 0.0f);
    m_histPos = 0;
    m_histTotal = 0;
    m_outRs.reset(0.0);
}

double Analyzer::historySec() const
{
    const uint64_t n = std::min<uint64_t>(m_histTotal, m_hist.size());
    return (double)n / kRate;
}

void Analyzer::feedOutput(const float * mono, size_t n, double rate)
{
    if (rate != m_outRs.inRate()) m_outRs.reset(rate);
    m_scratch.clear();
    m_outRs.process(mono, n, m_scratch);
    const size_t N = m_hist.size();
    for (size_t i = 0; i < m_scratch.size(); ++i) {
        m_hist[m_histPos] = m_scratch[i];
        if (++m_histPos == N) m_histPos = 0;
    }
    m_histTotal += m_scratch.size();
}

bool Analyzer::feedMic(const float * mono, size_t n, double rate)
{
    if (rate != m_micRs.inRate()) m_micRs.reset(rate);
    for (size_t i = 0; i < n; ++i) m_micPeak = std::max(m_micPeak, fabsf(mono[i]));
    m_scratch.clear();
    m_micRs.process(mono, n, m_scratch);
    m_mic.insert(m_mic.end(), m_scratch.begin(), m_scratch.end());

    bool any = false;
    while (m_mic.size() >= (size_t)kBlock) {
        analyseBlock();
        m_mic.erase(m_mic.begin(), m_mic.begin() + kBlock);
        any = true;
    }
    return any;
}

void Analyzer::analyseBlock()
{
    Result & r = m_result;
    ++r.blocks;

    const float * y = m_mic.data();
    double ey = 0.0;
    for (int i = 0; i < kBlock; ++i) ey += (double)y[i] * y[i];
    r.micDb = toDb(ey / kBlock);
    r.micClipped = m_micPeak >= 0.999f;
    m_micPeak = 0.0f;
    r.confidence = 0.0;
    r.lagSec = -1.0;
    r.outDb = -200.0;

    const size_t N = m_hist.size();
    const size_t M = (size_t)1 << kCorrOrder;
    const size_t valid = (size_t)std::min<uint64_t>(m_histTotal, N);
    if (valid < (size_t)kBlock || r.micDb < kMinMicDb) return;

    // History, oldest first, in the real part; the block in the imaginary
    // part. One transform gives both spectra.
    double * re = m_re.data();
    double * im = m_im.data();
    for (size_t i = 0; i < N; ++i) re[i] = m_hist[(m_histPos + i) % N];
    std::fill(re + N, re + M, 0.0);
    for (size_t i = 0; i < (size_t)kBlock; ++i) im[i] = y[i];
    std::fill(im + kBlock, im + M, 0.0);
    m_corrFft.run(re, im, false);

    // GCC-PHAT: the cross spectrum with its magnitude divided out, so every
    // frequency votes equally on the delay and loud bass does not smear the
    // peak. Bins either side of the useful band are left out.
    const size_t kLo = (size_t)ceil(kSearchBandLoHz * M / kRate);
    const size_t kHi = (size_t)floor(kSearchBandHiHz * M / kRate);
    double magSum = 0.0;
    for (size_t k = kLo; k <= kHi; ++k) {
        const size_t kc = M - k;
        const double hr = 0.5 * (re[k] + re[kc]), hi = 0.5 * (im[k] - im[kc]);
        const double yr = 0.5 * (im[k] + im[kc]), yi = -0.5 * (re[k] - re[kc]);
        magSum += sqrt((hr * hr + hi * hi) * (yr * yr + yi * yi));
    }
    if (!(magSum > 0.0)) return;
    const double eps = 1e-3 * magSum / (double)(kHi - kLo + 1);

    re[0] = im[0] = 0.0;
    re[M / 2] = im[M / 2] = 0.0;
    for (size_t k = 1; k < M / 2; ++k) {
        const size_t kc = M - k;
        double gr = 0.0, gi = 0.0;
        if (k >= kLo && k <= kHi) {
            const double hr = 0.5 * (re[k] + re[kc]), hi = 0.5 * (im[k] - im[kc]);
            const double yr = 0.5 * (im[k] + im[kc]), yi = -0.5 * (re[k] - re[kc]);
            const double cr = hr * yr + hi * yi;     // H * conj(Y)
            const double ci = hi * yr - hr * yi;
            const double w = 1.0 / (sqrt((hr * hr + hi * hi) * (yr * yr + yi * yi)) + eps);
            gr = cr * w;
            gi = ci * w;
        }
        re[k] = gr;  im[k] = gi;
        re[kc] = gr; im[kc] = -gi;
    }
    m_corrFft.run(re, im, true);

    // re[d] now holds the correlation of the block with the history from d
    // on. The block must lie inside what has been kept, and at least the
    // minimum delay back from the newest output.
    const size_t dLo = N - valid;
    const size_t minLag = (size_t)(m_minLagSec * kRate);
    if (N < (size_t)kBlock + minLag || N - (size_t)kBlock - minLag < dLo + 1000) return;
    const size_t dHi = N - (size_t)kBlock - minLag;
    size_t best = dLo;
    double bestAbs = -1.0;
    for (size_t d = dLo; d <= dHi; ++d) {
        const double a = fabs(re[d]);
        if (a > bestAbs) { bestAbs = a; best = d; }
    }
    const size_t guard = (size_t)(0.02 * kRate);
    double ss = 0.0;
    size_t cnt = 0;
    for (size_t d = dLo; d <= dHi; ++d) {
        if (d + guard >= best && d <= best + guard) continue;
        ss += re[d] * re[d];
        ++cnt;
    }
    if (cnt < 100 || !(ss > 0.0)) return;
    r.confidence = bestAbs / sqrt(ss / (double)cnt);
    if (r.confidence < kMinConfidence) return;

    r.lagSec = (double)(N - (best + (size_t)kBlock)) / kRate;

    // The matched stretch of output.
    double ex = 0.0;
    for (size_t i = 0; i < (size_t)kBlock; ++i) {
        const double v = m_hist[(m_histPos + best + i) % N];
        ex += v * v;
    }
    r.outDb = toDb(ex / kBlock);
    if (r.outDb < kMinOutputDb) return;

    // Welch spectra of both, summed over the block.
    std::vector<double> bx(kBins, 0.0), by(kBins, 0.0);
    for (int f0 = 0; f0 + kFrame <= kBlock; f0 += kHop) {
        for (int i = 0; i < kFrame; ++i) {
            const double w = m_window[(size_t)i];
            re[i] = w * m_hist[(m_histPos + best + (size_t)(f0 + i)) % N];
            im[i] = w * y[f0 + i];
        }
        m_frameFft.run(re, im, false);
        for (int k = 0; k < kBins; ++k) {
            const int kc = (kFrame - k) & (kFrame - 1);
            const double hr = 0.5 * (re[k] + re[kc]), hi = 0.5 * (im[k] - im[kc]);
            const double yr = 0.5 * (im[k] + im[kc]), yi = -0.5 * (re[k] - re[kc]);
            bx[(size_t)k] += hr * hr + hi * hi;
            by[(size_t)k] += yr * yr + yi * yi;
        }
    }

    // Each block counts once whatever its loudness, so a quiet passage at one
    // end of the room weighs as much as a loud one at the other.
    double tx = 0.0;
    for (int k = 0; k < kBins; ++k) tx += bx[(size_t)k];
    if (!(tx > 0.0)) return;
    for (int k = 0; k < kBins; ++k) {
        m_sxx[(size_t)k] += bx[(size_t)k] / tx;
        m_syy[(size_t)k] += by[(size_t)k] / tx;
    }
    for (int k = 0; k < kBins; ++k) m_blockSpectra.push_back((float)(bx[(size_t)k] / tx));
    for (int k = 0; k < kBins; ++k) m_blockSpectra.push_back((float)(by[(size_t)k] / tx));

    ++r.matched;
    updateResponse();
}

namespace {

//! What one set of summed spectra says: the response, its trend, the bumps.
struct Shape {
    std::vector<double> frequency, responseDb, trendDb;
    int  peaks = 0;
    Peak peak[kMaxPeaks];
};

bool near(const Peak & a, const Peak & b, double octaves)
{
    return fabs(log2(a.frequency / b.frequency)) < octaves;
}

void analyseSpectra(const double * sxx, const double * syy, Shape & r)
{
    const int n = (int)floor(log2(kGridHiHz / kGridLoHz) * kGridPerOctave) + 1;
    r.frequency.resize((size_t)n);
    r.responseDb.resize((size_t)n);
    r.trendDb.resize((size_t)n);

    // A sixth of an octave, Hann-weighted in log frequency, power averaged.
    const double binHz = kRate / kFrame;
    const double halfOct = 1.0 / 6.0;
    for (int j = 0; j < n; ++j) {
        const double f = kGridLoHz * pow(2.0, (double)j / kGridPerOctave);
        const int k0 = std::max(1, (int)ceil(f * pow(2.0, -halfOct) / binHz));
        const int k1 = std::min(kBins - 1, (int)floor(f * pow(2.0, halfOct) / binHz));
        double sx = 0.0, sy = 0.0;
        for (int k = k0; k <= k1; ++k) {
            const double v = log2(k * binHz / f) / halfOct;
            const double w = 0.5 + 0.5 * cos(kPi * v);
            sx += w * sxx[k];
            sy += w * syy[k];
        }
        r.frequency[(size_t)j] = f;
        r.responseDb[(size_t)j] = toDb(sy) - toDb(sx);
    }

    // The trend: Hann smoothing two octaves either side, re-run with bumps
    // weighted down so that the one being looked for does not lift its own
    // baseline. It finds the bumps and is drawn; their size comes from the fit.
    std::vector<double> wt((size_t)n, 1.0);
    const int span = 2 * kGridPerOctave;
    for (int pass = 0; pass < 4; ++pass) {
        for (int j = 0; j < n; ++j) {
            double s = 0.0, sw = 0.0;
            for (int i = std::max(0, j - span); i <= std::min(n - 1, j + span); ++i) {
                const double k = 0.5 + 0.5 * cos(kPi * (double)(i - j) / (span + 1));
                s  += k * wt[(size_t)i] * r.responseDb[(size_t)i];
                sw += k * wt[(size_t)i];
            }
            r.trendDb[(size_t)j] = s / sw;
        }
        for (int i = 0; i < n; ++i) {
            const double e = std::max(0.0, r.responseDb[(size_t)i] - r.trendDb[(size_t)i]);
            wt[(size_t)i] = 1.0 / (1.0 + e * e);
        }
    }

    double off = 0.0;
    int cnt = 0;
    for (int j = 0; j < n; ++j) {
        if (r.frequency[(size_t)j] < kSearchLoHz || r.frequency[(size_t)j] > kSearchHiHz) continue;
        off += r.trendDb[(size_t)j];
        ++cnt;
    }
    if (cnt > 0) off /= cnt;
    for (int j = 0; j < n; ++j) { r.responseDb[(size_t)j] -= off; r.trendDb[(size_t)j] -= off; }

    // The band's range on the grid. Nothing outside it is ever suggested.
    int cLo = n, cHi = -1;
    for (int j = 0; j < n; ++j) {
        if (r.frequency[(size_t)j] < kSearchLoHz || r.frequency[(size_t)j] > kSearchHiHz) continue;
        cLo = std::min(cLo, j);
        cHi = std::max(cHi, j);
    }

    // Bumps: local maxima of the excess inside the band's range.
    std::vector<double> e((size_t)n);
    for (int j = 0; j < n; ++j) e[(size_t)j] = r.responseDb[(size_t)j] - r.trendDb[(size_t)j];

    std::vector<Peak> cands;
    for (int j = std::max(1, cLo); j <= std::min(n - 2, cHi); ++j) {
        const double em = e[(size_t)j - 1], e0 = e[(size_t)j], ep = e[(size_t)j + 1];
        if (!(e0 >= em && e0 > ep) || e0 < 1.0) continue;

        Peak pk;
        pk.frequency = r.frequency[(size_t)j];
        pk.heightDb = e0;

        // Width at half height, in octaves, as a fallback should the fit
        // find no bell; one side stands in for the other if it runs off.
        const double half = 0.5 * pk.heightDb;
        double left = -1.0, right = -1.0;
        for (int i = j; i > 0; --i) {
            if (e[(size_t)i - 1] <= half) {
                const double t = (e[(size_t)i] - half) / (e[(size_t)i] - e[(size_t)i - 1]);
                left = ((double)(j - i) + t) / kGridPerOctave;
                break;
            }
        }
        for (int i = j; i + 1 < n; ++i) {
            if (e[(size_t)i + 1] <= half) {
                const double t = (e[(size_t)i] - half) / (e[(size_t)i] - e[(size_t)i + 1]);
                right = ((double)(i - j) + t) / kGridPerOctave;
                break;
            }
        }
        if (left < 0.0 && right < 0.0) left = right = 1.0;
        else if (left < 0.0) left = right;
        else if (right < 0.0) right = left;
        const double bw = std::max(left + right, 0.05);
        pk.q = 1.0 / (2.0 * sinh(0.5 * log(2.0) * bw));

        // That is only a first guess: a bell's skirts reach well past its
        // half-height points, and the trend follows them, so a broad bump
        // reads low and narrow. The fit asks the question directly.
        // A fitted bell far taller than the bump that can be seen is the fit
        // explaining the trend's curvature rather than the bump, and the
        // first guess stands.
        BellFit fit;
        if (fitBell(r.responseDb, j, cLo, cHi, fit) && fit.gainDb <= 2.0 * e0 + 1.0) {
            pk.frequency = r.frequency[(size_t)fit.centre];
            pk.q = fit.q;
            pk.heightDb = fit.gainDb;
        }
        pk.q = std::max(0.5, std::min(8.0, pk.q));
        pk.q = floor(pk.q * 10.0 + 0.5) / 10.0;
        pk.frequency = std::max(kSearchLoHz, std::min(kSearchHiHz, pk.frequency));
        pk.gainDb = -floor(std::min(pk.heightDb, 12.0) * 2.0 + 0.5) / 2.0;
        cands.push_back(pk);
    }
    std::sort(cands.begin(), cands.end(),
              [](const Peak & a, const Peak & b) { return a.heightDb > b.heightDb; });

    r.peaks = 0;
    for (size_t c = 0; c < cands.size() && r.peaks < kMaxPeaks; ++c) {
        bool dup = false;
        for (int k = 0; k < r.peaks; ++k) if (near(cands[c], r.peak[k], 1.0 / 3.0)) dup = true;
        if (!dup) r.peak[r.peaks++] = cands[c];
    }
}

} // anonymous namespace

void Analyzer::updateResponse()
{
    Result & r = m_result;
    if (r.matched < 3) return;

    Shape full;
    analyseSpectra(m_sxx.data(), m_syy.data(), full);
    r.frequency.swap(full.frequency);
    r.responseDb.swap(full.responseDb);
    r.trendDb.swap(full.trendDb);
    r.peaks = full.peaks;
    for (int i = 0; i < full.peaks; ++i) r.peak[i] = full.peak[i];

    r.sure = true;
    r.options = 0;
    if (r.peaks == 0) return;
    const Peak & top = r.peak[0];

    // Two ways of being unsure which bump is the room's main one. A runner-up
    // nearly as tall: the measurement cannot rank them. Or the two halves of
    // the walk each pointing somewhere else: the answer depends on where in
    // the room one stands, and a static cut has to be a compromise chosen by
    // ear.
    if (r.peaks > 1 && r.peak[1].heightDb >= kAmbiguousRatio * top.heightDb) r.sure = false;

    Peak halfTop[2];
    int halves = 0;
    const size_t blocks = m_blockSpectra.size() / (2 * kBins);
    if ((int)blocks >= kMinBlocks) {
        std::vector<double> sx((size_t)kBins), sy((size_t)kBins);
        for (int h = 0; h < 2; ++h) {
            const size_t b0 = h == 0 ? 0 : blocks / 2, b1 = h == 0 ? blocks / 2 : blocks;
            std::fill(sx.begin(), sx.end(), 0.0);
            std::fill(sy.begin(), sy.end(), 0.0);
            for (size_t b = b0; b < b1; ++b) {
                const float * p = m_blockSpectra.data() + b * 2 * kBins;
                for (int k = 0; k < kBins; ++k) { sx[(size_t)k] += p[k]; sy[(size_t)k] += p[kBins + k]; }
            }
            // Half the data is noisier, and a half on its own has been seen to
            // fit a bump the whole walk does not have. Its main bump counts
            // only if the whole walk found it too, and is then offered with
            // the whole walk's numbers.
            Shape s;
            analyseSpectra(sx.data(), sy.data(), s);
            if (s.peaks == 0 || s.peak[0].heightDb < kMinSuggestDb) continue;
            int match = -1;
            for (int i = 0; i < r.peaks && match < 0; ++i)
                if (near(s.peak[0], r.peak[i], 1.0 / 3.0)) match = i;
            if (match < 0) continue;
            halfTop[halves++] = r.peak[match];
            if (match != 0 && !near(s.peak[0], top, kAgreeOctaves)) r.sure = false;
        }
    }

    r.option[r.options++] = top;
    if (r.sure) return;
    auto offer = [&](const Peak & p) {
        if (r.options >= kMaxPeaks) return;
        for (int k = 0; k < r.options; ++k) if (near(p, r.option[k], 1.0 / 3.0)) return;
        r.option[r.options++] = p;
    };
    // The halves' choices first, then the rest of the whole walk's bumps.
    for (int h = 0; h < halves; ++h) offer(halfTop[h]);
    for (int i = 1; i < r.peaks; ++i) offer(r.peak[i]);
}

// ---------------------------------------------------------------------------
// Tap
// ---------------------------------------------------------------------------

namespace tap {

std::atomic<int> g_open(0);

namespace {
std::mutex           s_lock;
std::vector<Segment> s_pending;

//! If the window stops draining, keep at most this much.
const double kMaxPendingSec = 30.0;
} // anonymous namespace

void open() { g_open.fetch_add(1); }

void close()
{
    if (g_open.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lock(s_lock);
        s_pending.clear();
    }
}

void pushMono(const float * mono, size_t n, unsigned rate)
{
    if (n == 0 || rate == 0) return;
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_pending.empty() || s_pending.back().rate != rate) {
        if (s_pending.size() >= 16) s_pending.erase(s_pending.begin());
        s_pending.push_back(Segment());
        s_pending.back().rate = rate;
        s_pending.back().samples.reserve(rate);
    }
    std::vector<float> & v = s_pending.back().samples;
    v.insert(v.end(), mono, mono + n);
    const size_t cap = (size_t)(kMaxPendingSec * rate);
    if (v.size() > cap) v.erase(v.begin(), v.begin() + (v.size() - cap / 2));
}

void drain(std::vector<Segment> & out)
{
    std::lock_guard<std::mutex> lock(s_lock);
    for (size_t i = 0; i < s_pending.size(); ++i) out.push_back(std::move(s_pending[i]));
    s_pending.clear();
}

} // namespace tap

} // namespace paraeq_calib
