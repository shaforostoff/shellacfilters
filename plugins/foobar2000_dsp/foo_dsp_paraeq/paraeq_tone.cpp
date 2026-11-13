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

} // namespace

Suggestion suggest(const Deviation & d, const paraeq::Params & params, double rate)
{
    Suggestion s;
    if (!d.valid) return s;
    paraeq::Params p = params;
    p.bypass = false;
    p.sanitize();
    for (int m = 0; m < kMeasures; m++) {
        // Two Newton steps on the band's gain, the others held where they are.
        // The response is close to linear in the gain, so two is plenty; the
        // slope is measured rather than assumed because a shelf moves its own
        // band by less than its gain and a bell can reach into the body.
        paraeq::Params q = p;
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
        s.gainDb[m] = g;

        paraeq::Config now;
        now.compute(p, rate);
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
