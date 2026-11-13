/* ========================================
 *  foo_dsp_paraeq - tone profile of a whole track. See paraeq_tone.h.
 * ======================================== */

#include "paraeq_tone.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace paraeq_tone {

namespace {

const double kPi = 3.14159265358979323846;
const double kQ4Lo = 0.54119610014619698, kQ4Hi = 1.30656296487637652;

Section section(bool highPass, double f, double q, double rate)
{
    const double w = 2.0 * kPi * f / rate;
    const double c = cos(w), alpha = sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Section s;
    if (highPass) {
        s.b0 = (1.0 + c) / 2.0 / a0; s.b1 = -(1.0 + c) / a0; s.b2 = s.b0;
    } else {
        s.b0 = (1.0 - c) / 2.0 / a0; s.b1 = (1.0 - c) / a0; s.b2 = s.b0;
    }
    s.a1 = -2.0 * c / a0;
    s.a2 = (1.0 - alpha) / a0;
    s.z1 = s.z2 = 0.0;
    return s;
}

inline double run(Section & s, double x)
{
    const double y = s.b0 * x + s.z1;
    s.z1 = s.b1 * x - s.a1 * y + s.z2;
    s.z2 = s.b2 * x - s.a2 * y;
    return y;
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

} // namespace

bool Analyzer::start(double rate, unsigned channels)
{
    m_energy.clear();
    m_inFrame = 0;
    m_channels = channels;
    m_ok = channels > 0 && rate >= kBandHi[kHiss] / 0.45;
    m_frameLen = (size_t)(rate * 0.05 + 0.5);
    for (int f = 0; f < kFilters; f++) {
        m_acc[f] = 0.0;
        if (!m_ok) continue;
        const double lo = f < kMeasures ? kBandLo[f] : kBodyLo;
        const double hi = f < kMeasures ? kBandHi[f] : kBodyHi;
        m_f[f][0] = section(true,  lo, kQ4Lo, rate);
        m_f[f][1] = section(true,  lo, kQ4Hi, rate);
        m_f[f][2] = section(false, hi, kQ4Lo, rate);
        m_f[f][3] = section(false, hi, kQ4Hi, rate);
    }

    m_spec.clear();
    m_fill = 0;
    if (m_ok) {
        unsigned order = 10;
        while ((double)((size_t)1 << order) < rate * 0.17) order++;
        m_fft.init(order);
        const size_t n = m_fft.size();
        m_window.resize(n);
        for (size_t i = 0; i < n; i++) m_window[i] = 0.5 - 0.5 * cos(2.0 * kPi * (double)i / (double)n);
        m_block.assign(n, 0.0);
        m_re.resize(n);
        m_im.resize(n);
        const double binHz = rate / (double)n;
        for (int k = 0; k <= kSpecBands; k++) {
            const double lo = k < kSpecBands ? specCentre(k) * pow(2.0, -1.0 / 6.0) : kBodyLo;
            const double hi = k < kSpecBands ? specCentre(k) * pow(2.0,  1.0 / 6.0) : kBodyHi;
            m_binLo[k] = (int)ceil(lo / binHz);
            m_binHi[k] = std::max(m_binLo[k] + 1, (int)ceil(hi / binHz));
        }
    }
    return m_ok;
}

template<typename T>
void Analyzer::feedT(const T * in, size_t frames)
{
    if (!m_ok) return;
    const double norm = 1.0 / (double)m_channels;
    for (size_t i = 0; i < frames; i++) {
        double x = 0.0;
        for (unsigned c = 0; c < m_channels; c++) x += (double)in[i * m_channels + c];
        x *= norm;
        for (int f = 0; f < kFilters; f++) {
            double y = x;
            for (int s = 0; s < kSections; s++) y = run(m_f[f][s], y);
            m_acc[f] += y * y;
        }
        if (++m_inFrame == m_frameLen) endFrame();
        m_block[m_fill] = x;
        if (++m_fill == m_block.size()) endBlock();
    }
}

void Analyzer::feed(const float * in, size_t frames)  { feedT(in, frames); }
void Analyzer::feed(const double * in, size_t frames) { feedT(in, frames); }

void Analyzer::endFrame()
{
    for (int f = 0; f < kFilters; f++) {
        m_energy.push_back((float)m_acc[f]);
        m_acc[f] = 0.0;
    }
    m_inFrame = 0;
}

void Analyzer::endBlock()
{
    const size_t n = m_block.size();
    for (size_t i = 0; i < n; i++) { m_re[i] = m_block[i] * m_window[i]; m_im[i] = 0.0; }
    m_fft.run(m_re.data(), m_im.data(), false);
    for (int k = 0; k <= kSpecBands; k++) {
        double e = 0.0;
        for (int b = m_binLo[k]; b < m_binHi[k]; b++) e += m_re[b] * m_re[b] + m_im[b] * m_im[b];
        m_spec.push_back((float)e);
    }
    // Half overlapped: the second half becomes the first.
    std::copy(m_block.begin() + (ptrdiff_t)(n / 2), m_block.end(), m_block.begin());
    m_fill = n / 2;
}

Profile Analyzer::finish()
{
    Profile p;
    const size_t n = m_energy.size() / kFilters;
    if (!m_ok || n < 200) return p;   // 10 s

    std::vector<double> body(n);
    for (size_t i = 0; i < n; i++) body[i] = m_energy[i * kFilters + kMeasures];
    const double gate = percentile(body, 0.30);

    double sum[kFilters] = {};
    int used = 0;
    for (size_t i = 0; i < n; i++) {
        if (body[i] < gate || body[i] <= 0.0) continue;
        for (int f = 0; f < kFilters; f++) sum[f] += m_energy[i * kFilters + f];
        used++;
    }
    if (used < 100 || sum[kMeasures] <= 0.0) return p;
    for (int m = 0; m < kMeasures; m++)
        p.db[m] = (float)(10.0 * log10(std::max(sum[m], 1e-30) / sum[kMeasures]));
    p.frames = used;
    p.valid = true;

    // The spectrum, gated the same way on its own blocks' body.
    const int per = kSpecBands + 1;
    const size_t nb = m_spec.size() / per;
    if (nb >= 40) {
        std::vector<double> sbody(nb);
        for (size_t i = 0; i < nb; i++) sbody[i] = m_spec[i * per + kSpecBands];
        const double sgate = percentile(sbody, 0.30);
        std::vector<double> ssum(per, 0.0);
        int sused = 0;
        for (size_t i = 0; i < nb; i++) {
            if (sbody[i] < sgate || sbody[i] <= 0.0) continue;
            for (int k = 0; k < per; k++) ssum[k] += m_spec[i * per + k];
            sused++;
        }
        if (sused >= 20 && ssum[kSpecBands] > 0.0) {
            for (int k = 0; k < kSpecBands; k++)
                p.spec[k] = (float)(10.0 * log10(std::max(ssum[k], 1e-30) / ssum[kSpecBands]));
            p.specValid = true;
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// The reference
// ---------------------------------------------------------------------------

// Measured by tools/paraeq_tone_cli over C:\TangoTunes on 2026-11-13: every
// file tagged tango, vals or milonga, dated before 1954 and with "TangoTunes"
// in its album, less the "(decrackle)" copies - 1250 tracks. Those transfers
// are the tone the user wants; the Todos (tangotimetravel) ones, which the user
// plays with more bass, are left out. Over the 704 other tracks the medians sit
// 2.1 dB lower in bass, 0.5 dB in brilliance and 0.9 dB in hiss. On the 126
// recordings the library has from both, Todos carries 3.8 dB less bass and
// 3.4 dB less hiss, with the same brilliance; dehumming first moves bass by
// 0.2 dB on the median track, so the bass gap is the transfer, not hum. The
// spread is the 10th and 90th percentiles of the same 1250 tracks, which flag
// each band on one track in five.
const Profile & reference()
{
    static const Profile ref = [] {
        Profile p;
        p.valid = true;
        p.db[kBass]       = -6.80f;
        p.db[kBrilliance] = -14.75f;
        p.db[kHiss]       = -30.70f;
        p.frames = 0;
        // The same 1250 tracks' third-octave medians, 40 Hz to 10 kHz.
        static const float spec[kSpecBands] = {
            -33.06f, -32.40f, -21.84f, -18.79f, -14.33f, -13.56f, -15.23f,
            -12.71f,  -8.93f,  -7.19f,  -5.77f,  -5.79f,  -6.16f,  -6.94f,
             -8.62f,  -9.59f, -10.48f, -12.31f, -12.66f, -13.94f, -18.43f,
            -24.06f, -28.87f, -32.20f, -35.75f,
        };
        for (int k = 0; k < kSpecBands; k++) p.spec[k] = spec[k];
        p.specValid = true;
        return p;
    }();
    return ref;
}

const Spread & spread()
{
    static const Spread s = {
        { -3.73f, -2.95f, -4.75f },
        {  3.46f,  3.12f,  5.17f },
    };
    return s;
}

Deviation deviation(const Profile & track)
{
    Deviation d;
    const Profile & ref = reference();
    if (!track.valid || !ref.valid) return d;
    for (int m = 0; m < kMeasures; m++) d.db[m] = track.db[m] - ref.db[m];
    d.valid = true;
    if (track.specValid && ref.specValid) {
        for (int k = 0; k < kSpecBands; k++) d.spec[k] = track.spec[k] - ref.spec[k];
        d.specValid = true;
    }
    return d;
}

namespace {

//! Mean level of the curve over [lo, hi], log-spaced, minus the same over the
//! body. That is what the analyser would see change if this curve were run
//! over the track before it.
double effectDb(const paraeq::Config & cfg, double lo, double hi)
{
    const int n = 12;
    double band = 0.0, body = 0.0;
    for (int i = 0; i < n; i++) {
        const double t = (i + 0.5) / n;
        band += paraeq::magnitudeDb(cfg, lo * pow(hi / lo, t));
        body += paraeq::magnitudeDb(cfg, kBodyLo * pow(kBodyHi / kBodyLo, t));
    }
    return (band - body) / n;
}

float & gainOf(paraeq::Params & p, int m)
{
    return m == kBass ? p.lfGain : (m == kBrilliance ? p.hmfGain : p.hfGain);
}

float & freqOf(paraeq::Params & p, int m)
{
    return m == kBass ? p.lfFrequency : (m == kBrilliance ? p.hmfFrequency : p.hfFrequency);
}

// ---------------------------------------------------------------------------
// The frequency fit. See suggest() in the header for what it is and why.
// ---------------------------------------------------------------------------

//! Library half-spread per third-octave step (half the 10th-90th percentile
//! distance of the same 1250 tracks); each step is weighted by its inverse
//! square.
const float kSpecSpread[kSpecBands] = {
    8.63f, 7.14f, 8.33f, 6.87f, 5.88f, 5.04f, 4.84f, 4.08f, 3.64f, 2.97f,
    2.78f, 2.08f, 1.70f, 1.61f, 1.38f, 2.02f, 2.92f, 2.62f, 3.78f, 3.57f,
    3.75f, 3.68f, 4.67f, 5.03f, 7.44f,
};

//! Steps each band is fitted over: LF 40-400 Hz, HMF and HF 2-10 kHz.
const int kFitLo[kMeasures] = {  0, 17, 17 };
const int kFitHi[kMeasures] = { 10, 24, 24 };

//! Where each band's frequency may be suggested. Inside the band's own range,
//! and narrower: an LF shelf below 40 Hz would be fitting the rumble the
//! high-pass is for, and one above 250 Hz the cello.
const double kFitFreqLo[kMeasures] = {   40.0, 2000.0,  4000.0 };
const double kFitFreqHi[kMeasures] = {  250.0, 8000.0, 10000.0 };

//! Cost of moving away from the default frequency, in dB^2 of weighted mean
//! square misfit per octave squared.
const double kFreqPrior = 2.0;

//! Third-octave steps of the gap are smoothed with a five-tap Hann, about an
//! octave wide, so a single instrument's step does not pull the band.
void smoothSpec(const float * in, double * out)
{
    static const double w[5] = { 0.25, 0.75, 1.0, 0.75, 0.25 };
    for (int k = 0; k < kSpecBands; k++) {
        double sum = 0.0;
        for (int j = -2; j <= 2; j++) {
            const int i = std::max(0, std::min(kSpecBands - 1, k + j));
            sum += w[j + 2] * in[i];
        }
        out[k] = sum / 3.0;
    }
}

//! The band's stage as Config::compute() would design it at f and g.
paraeq::Biquad bandStage(const paraeq::Params & p, int m, double f, double g, double rate)
{
    using namespace paraeq;
    if (m == kBass)      return design(p.lfBell ? kPeaking : kLowShelf, f, kShelfQ, g, rate);
    if (m == kBrilliance) return design(kPeaking, f, p.hmfQ, g, rate);
    return design(p.hfBell ? kPeaking : kHighShelf, f, kShelfQ, g, rate);
}

//! The points the fit evaluates the curve at, with their cosines and sines
//! worked out once: 13 across the body, 7 across each third-octave step. A
//! fit evaluates some 40000 of them, so this is most of its cost.
struct Grid {
    enum { kBody = 13, kPerStep = 7, kPoints = kBody + kSpecBands * kPerStep };
    double c1[kPoints], s1[kPoints], c2[kPoints], s2[kPoints];

    explicit Grid(double rate) {
        for (int i = 0; i < kPoints; i++) {
            const double f = i < kBody
                ? kBodyLo * pow(kBodyHi / kBodyLo, i / 12.0)
                : specCentre((i - kBody) / kPerStep) * pow(2.0, ((i - kBody) % kPerStep - 3) / 18.0);
            const double w = 2.0 * kPi * f / rate;
            c1[i] = cos(w); s1[i] = sin(w);
            c2[i] = cos(2.0 * w); s2[i] = sin(2.0 * w);
        }
    }

    //! paraeq::magnitudeDb() at point i.
    double db(const paraeq::Biquad & b, int i) const {
        const double nr = b.b0 + b.b1 * c1[i] + b.b2 * c2[i], ni = b.b1 * s1[i] + b.b2 * s2[i];
        const double dr = 1.0 + b.a1 * c1[i] + b.a2 * c2[i],  di = b.a1 * s1[i] + b.a2 * s2[i];
        const double den = dr * dr + di * di;
        if (!(den > 1e-300)) return 0.0;
        return 10.0 * log10((nr * nr + ni * ni) / den + 1e-300);
    }
};

//! What some stages do to third-octave steps k0..k1 as the analyser sees it:
//! the mean over each step minus the mean over the body.
struct StepEffect {
    double db[kSpecBands];
};

void stepEffect(const Grid & grid, const paraeq::Biquad * stages, int n, int k0, int k1, StepEffect & out)
{
    double body = 0.0;
    for (int i = 0; i < Grid::kBody; i++)
        for (int j = 0; j < n; j++) body += grid.db(stages[j], i);
    body /= Grid::kBody;
    for (int k = k0; k <= k1; k++) {
        double sum = 0.0;
        for (int i = 0; i < Grid::kPerStep; i++)
            for (int j = 0; j < n; j++) sum += grid.db(stages[j], Grid::kBody + k * Grid::kPerStep + i);
        out.db[k] = sum / Grid::kPerStep - body;
    }
}

void stageEffect(const Grid & grid, const paraeq::Biquad & b, int k0, int k1, StepEffect & out)
{
    stepEffect(grid, &b, 1, k0, k1, out);
}

//! Best frequency and gain for band m against `target` (what is heard with
//! every other band where it is, before this band), over the band's steps.
void fitBand(const Grid & grid, const paraeq::Params & p, int m, const double * target,
             double rate, double fDefault, float & fOut, float & gOut)
{
    const int k0 = kFitLo[m], k1 = kFitHi[m];
    const double fMax = std::min(kFitFreqHi[m], rate * paraeq::kMaxFreqFraction);
    double wsum = 0.0, w[kSpecBands];
    for (int k = k0; k <= k1; k++) { w[k] = 1.0 / ((double)kSpecSpread[k] * kSpecSpread[k]); wsum += w[k]; }

    double bestErr = 1e300;
    for (int i = 0; ; i++) {
        const double f = kFitFreqLo[m] * pow(2.0, i / 12.0);
        if (f > fMax) break;
        // A first guess from the unit-gain shape, then two Newton steps on the
        // real curve: a shelf's shape changes a little with its gain.
        StepEffect e1;
        stageEffect(grid, bandStage(p, m, f, 1.0, rate), k0, k1, e1);
        double num = 0.0, den = 0.0;
        for (int k = k0; k <= k1; k++) { num += w[k] * target[k] * e1.db[k]; den += w[k] * e1.db[k] * e1.db[k]; }
        if (!(den > 1e-9)) continue;
        double g = std::max(-(double)paraeq::kGainMaxDb, std::min((double)paraeq::kGainMaxDb, -num / den));
        StepEffect e, ed;
        for (int it = 0; it < 2; it++) {
            stageEffect(grid, bandStage(p, m, f, g, rate), k0, k1, e);
            stageEffect(grid, bandStage(p, m, f, g + 0.5, rate), k0, k1, ed);
            num = den = 0.0;
            for (int k = k0; k <= k1; k++) {
                const double slope = (ed.db[k] - e.db[k]) / 0.5;
                num += w[k] * (target[k] + e.db[k]) * slope;
                den += w[k] * slope * slope;
            }
            if (!(den > 1e-9)) break;
            g = std::max(-(double)paraeq::kGainMaxDb, std::min((double)paraeq::kGainMaxDb, g - num / den));
        }
        stageEffect(grid, bandStage(p, m, f, g, rate), k0, k1, e);
        double err = 0.0;
        for (int k = k0; k <= k1; k++) { const double r = target[k] + e.db[k]; err += w[k] * r * r; }
        const double oct = log2(f / fDefault);
        err = err / wsum + kFreqPrior * oct * oct;
        if (err < bestErr) { bestErr = err; fOut = (float)f; gOut = (float)g; }
    }
}

//! Replaces s.freqHz and s.gainDb with the fit. LF on its own; HMF and HF
//! share 2-10 kHz, so they are fitted in turn, three rounds, each against the
//! other's latest.
void fitFrequencies(const Deviation & d, const paraeq::Params & params, double rate, Suggestion & s)
{
    double gap[kSpecBands];
    smoothSpec(d.spec, gap);

    const Grid grid(rate);
    paraeq::Params q = params;
    const paraeq::Params def = paraeq::Params::defaults();
    const double fDef[kMeasures] = { def.lfFrequency, def.hmfFrequency, def.hfFrequency };

    const int order[] = { kBass, kBrilliance, kHiss, kBrilliance, kHiss, kBrilliance, kHiss };
    for (int m : order) {
        // What is heard with every band where q has it, less this band.
        paraeq::Config cfg;
        cfg.compute(q, rate);
        StepEffect all, own;
        stepEffect(grid, cfg.stage, paraeq::kStages, 0, kSpecBands - 1, all);
        stageEffect(grid, bandStage(q, m, freqOf(q, m), gainOf(q, m), rate), 0, kSpecBands - 1, own);
        double target[kSpecBands];
        for (int k = 0; k < kSpecBands; k++) target[k] = gap[k] + all.db[k] - own.db[k];

        float f = freqOf(q, m), g = gainOf(q, m);
        fitBand(grid, q, m, target, rate, fDef[m], f, g);
        freqOf(q, m) = f;
        gainOf(q, m) = g;
    }
    for (int m = 0; m < kMeasures; m++) {
        s.freqHz[m] = freqOf(q, m);
        s.gainDb[m] = gainOf(q, m);
    }
}

} // namespace

Suggestion suggest(const Deviation & d, const paraeq::Params & params, double rate)
{
    Suggestion s;
    if (!d.valid) return s;
    paraeq::Params p = params;
    p.bypass = false;
    p.sanitize();

    // Frequencies from the spectrum, and its gains as a starting point.
    paraeq::Params q = p;
    if (d.specValid) {
        fitFrequencies(d, p, rate, s);
        for (int m = 0; m < kMeasures; m++) { freqOf(q, m) = s.freqHz[m]; gainOf(q, m) = s.gainDb[m]; }
    }

    // The gains then come from the three bands, the same measure that decides
    // whether a dot shows, so following a dot makes it go away. Two Newton
    // steps per band, two rounds because the high-mid bell and the high shelf
    // reach into each other's band. The response is close to linear in the
    // gain, so two steps is plenty; the slope is measured rather than assumed
    // because a shelf moves its own band by less than its gain and a bell can
    // reach into the body.
    for (int round = 0; round < 2; round++) {
        for (int m = 0; m < kMeasures; m++) {
            float & g = gainOf(q, m);
            for (int it = 0; it < 2; it++) {
                paraeq::Config a, b;
                a.compute(q, rate);
                const double e0 = effectDb(a, kBandLo[m], kBandHi[m]);
                paraeq::Params q1 = q;
                gainOf(q1, m) += 1.0f;
                b.compute(q1, rate);
                const double k = effectDb(b, kBandLo[m], kBandHi[m]) - e0;
                if (!(k > 0.05)) break;
                const double residual = d.db[m] + e0;   // what the listener hears
                g = (float)std::max(-(double)paraeq::kGainMaxDb,
                                    std::min((double)paraeq::kGainMaxDb, g - residual / k));
            }
        }
    }

    paraeq::Config now;
    now.compute(p, rate);
    for (int m = 0; m < kMeasures; m++) {
        s.gainDb[m] = gainOf(q, m);
        s.freqHz[m] = freqOf(q, m);
        const double heard = d.db[m] + effectDb(now, kBandLo[m], kBandHi[m]);
        s.show[m] = heard < spread().below[m] || heard > spread().above[m];
    }
    s.valid = true;
    return s;
}

namespace {

bool containsWord(const char * hay, const char * needle)
{
    const size_t n = strlen(needle);
    for (const char * p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && tolower((unsigned char)p[k]) == needle[k]) k++;
        if (k == n) return true;
    }
    return false;
}

} // namespace

bool eligible(const char * genre, const char * date)
{
    if (genre == NULL || date == NULL) return false;
    if (!containsWord(genre, "tango") && !containsWord(genre, "vals") &&
        !containsWord(genre, "milonga"))
        return false;
    // The first four-digit run is the year: "1941", "1941-05-12", "05/1941".
    for (const char * p = date; *p; p++) {
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3])) {
            const int year = atoi(std::string(p, 4).c_str());
            return year >= 1890 && year < 1954;
        }
    }
    return false;
}

} // namespace paraeq_tone
