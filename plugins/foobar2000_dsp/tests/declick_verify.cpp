/* ========================================
 *  declick_verify - the declicker's linear algebra, against brute force.
 *
 *  The AR machinery is the part where a subtle error produces plausible but
 *  wrong audio rather than an obvious failure, so each piece is checked
 *  against a dense, obviously-correct implementation:
 *
 *    levinson()               vs. solving the Yule-Walker system by
 *                                Gaussian elimination
 *    solveBandedToeplitz()    vs. dense Gaussian elimination
 *
 *  Plus end-to-end properties of the streaming Channel: silence in, silence
 *  out; clean audio is left essentially alone; an injected click is reduced;
 *  and hostile input does not produce anything non-finite.
 * ======================================== */

#include "declick_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

using namespace declick;

namespace {

int g_failures = 0;

void fail(const char * what, double got, double want) {
    printf("  FAIL  %s: got %.9g, want %.9g\n", what, got, want);
    ++g_failures;
}

void checkClose(double got, double want, double tol, const char * what) {
    const double scale = fabs(want) > 1.0 ? fabs(want) : 1.0;
    if (!(fabs(got - want) <= tol * scale)) fail(what, got, want);
}

//! Deterministic, so a failure is reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double uniform() { return (double)(next() >> 8) / 16777216.0; }
    double centred() { return 2.0 * uniform() - 1.0; }
};

//! Dense solve of A x = b by Gaussian elimination with partial pivoting.
bool denseSolve(std::vector<double> A, std::vector<double> & b, int n) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) {
            if (fabs(A[(size_t)r * n + c]) > fabs(A[(size_t)piv * n + c])) piv = r;
        }
        if (fabs(A[(size_t)piv * n + c]) < 1e-300) return false;
        if (piv != c) {
            for (int k = 0; k < n; ++k) {
                const double t = A[(size_t)c * n + k];
                A[(size_t)c * n + k] = A[(size_t)piv * n + k];
                A[(size_t)piv * n + k] = t;
            }
            const double t = b[(size_t)c]; b[(size_t)c] = b[(size_t)piv]; b[(size_t)piv] = t;
        }
        const double d = A[(size_t)c * n + c];
        for (int r = c + 1; r < n; ++r) {
            const double f = A[(size_t)r * n + c] / d;
            if (f == 0.0) continue;
            for (int k = c; k < n; ++k) A[(size_t)r * n + k] -= f * A[(size_t)c * n + k];
            b[(size_t)r] -= f * b[(size_t)c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[(size_t)r];
        for (int k = r + 1; k < n; ++k) s -= A[(size_t)r * n + k] * b[(size_t)k];
        b[(size_t)r] = s / A[(size_t)r * n + r];
    }
    return true;
}

//! An autocorrelation sequence guaranteed positive definite: build it from a
//! random signal rather than from random numbers.
void makeAutocorr(Rng & rng, int order, int n, std::vector<double> & r) {
    std::vector<double> x((size_t)n);
    double prev = 0.0, prev2 = 0.0;
    for (int i = 0; i < n; ++i) {
        // Lightly coloured noise, so the model has something to fit.
        const double v = 0.6 * prev - 0.3 * prev2 + rng.centred();
        x[(size_t)i] = v;
        prev2 = prev; prev = v;
    }
    r.assign((size_t)order + 1, 0.0);
    for (int d = 0; d <= order; ++d) {
        double s = 0.0;
        for (int i = d; i < n; ++i) s += x[(size_t)i] * x[(size_t)(i - d)];
        r[(size_t)d] = s;
    }
    r[0] *= 1.0000001;
}

// -------------------------------------------------------------------------

void testLevinson() {
    printf("levinson vs. dense Yule-Walker\n");
    Rng rng(12345u);
    double worst = 0.0;

    for (int trial = 0; trial < 40; ++trial) {
        const int order = 4 + (int)(rng.uniform() * 28.0);
        std::vector<double> r;
        makeAutocorr(rng, order, 8 * order + 64, r);

        std::vector<double> a((size_t)order + 1, 0.0);
        if (!levinson(&r[0], order, &a[0])) {
            printf("  FAIL  levinson refused a valid autocorrelation\n");
            ++g_failures;
            continue;
        }

        // Yule-Walker: R a = -r, with R[i][j] = r[|i-j|], i,j in 1..order.
        std::vector<double> R((size_t)order * order, 0.0), b((size_t)order, 0.0);
        for (int i = 0; i < order; ++i) {
            for (int j = 0; j < order; ++j) R[(size_t)i * order + j] = r[(size_t)abs(i - j)];
            b[(size_t)i] = -r[(size_t)(i + 1)];
        }
        if (!denseSolve(R, b, order)) continue;

        for (int i = 1; i <= order; ++i) {
            const double d = fabs(a[(size_t)i] - b[(size_t)(i - 1)]);
            if (d > worst) worst = d;
        }
    }
    printf("  worst coefficient deviation %.3e\n", worst);
    if (!(worst < 1e-8)) { printf("  FAIL  levinson disagrees with the dense solve\n"); ++g_failures; }
}

void testBandedSolve() {
    printf("solveBandedToeplitz vs. dense elimination\n");
    Rng rng(999u);
    double worst = 0.0;

    for (int trial = 0; trial < 60; ++trial) {
        const int order = 4 + (int)(rng.uniform() * 28.0);
        const int n = 1 + (int)(rng.uniform() * 80.0);

        std::vector<double> r;
        makeAutocorr(rng, order, 8 * order + 64, r);
        std::vector<double> a((size_t)order + 1, 0.0);
        if (!levinson(&r[0], order, &a[0])) continue;

        // G is the autocorrelation of the coefficient vector: PD by construction.
        std::vector<double> d((size_t)order + 1, 0.0);
        for (int k = 0; k <= order; ++k) {
            double acc = 0.0;
            for (int t = 0; t + k <= order; ++t) acc += a[(size_t)t] * a[(size_t)(t + k)];
            d[(size_t)k] = acc;
        }
        d[0] *= 1.0 + 1e-9;

        std::vector<double> rhs((size_t)n);
        for (int i = 0; i < n; ++i) rhs[(size_t)i] = rng.centred();

        std::vector<double> got = rhs, scratch;
        if (!solveBandedToeplitz(&d[0], order, &got[0], n, scratch)) {
            printf("  FAIL  banded solve refused a PD matrix (n=%d order=%d)\n", n, order);
            ++g_failures;
            continue;
        }

        std::vector<double> G((size_t)n * n, 0.0), want = rhs;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const int k = abs(i - j);
                G[(size_t)i * n + j] = (k <= order) ? d[(size_t)k] : 0.0;
            }
        }
        if (!denseSolve(G, want, n)) continue;

        for (int i = 0; i < n; ++i) {
            const double e = fabs(got[(size_t)i] - want[(size_t)i]);
            if (e > worst) worst = e;
        }
    }
    printf("  worst solution deviation %.3e\n", worst);
    if (!(worst < 1e-7)) { printf("  FAIL  banded solve disagrees with dense\n"); ++g_failures; }
}

// -------------------------------------------------------------------------

//! `tweak`, if given, edits the Config before compute() derives the rest of it
//! from `p` and the rate, so a stage it turns off is off in the latency too.
std::vector<double> runChannel(const std::vector<double> & in, const Params & p,
                               double sr, void (*tweak)(Config &) = 0) {
    const scoped_flush_denormals ftz;                // as every host runs the core
    Config cfg;
    if (tweak) tweak(cfg);
    cfg.compute(p, sr);
    Channel ch;
    ch.configure(cfg);

    std::vector<double> out;
    out.reserve(in.size());
    const size_t CHUNK = 257;                      // deliberately not a power of two
    double buf[CHUNK];
    const auto pullAll = [&]() {
        for (size_t avail; (avail = ch.available()) > 0; ) {
            const size_t t = (avail < CHUNK) ? avail : CHUNK;
            ch.pull(buf, t, 1);
            out.insert(out.end(), buf, buf + t);
        }
    };
    for (size_t pos = 0; pos < in.size(); pos += CHUNK) {
        const size_t k = (in.size() - pos < CHUNK) ? (in.size() - pos) : CHUNK;
        ch.push(&in[pos], k, 1);
        pullAll();
    }
    ch.drain();
    pullAll();
    out.resize(in.size(), 0.0);
    return out;
}

//! A 330 Hz tone over a floor, with a click every 211 .. 307 samples.
std::vector<double> clickedTone(size_t N, double sr, uint32_t seed) {
    std::vector<double> x(N);
    Rng rng(seed);
    for (size_t i = 0; i < N; ++i) {
        const double t = (double)i / sr;
        x[i] = 0.30 * sin(6.283185307 * 330.0 * t) + 0.01 * rng.centred();
    }
    for (size_t i = 2000; i + 16 < N; i += 211 + (rng.next() % 97)) {
        x[i] += 0.3 * rng.centred();
    }
    return x;
}

double rms(const std::vector<double> & v, size_t from, size_t to) {
    double s = 0.0;
    for (size_t i = from; i < to && i < v.size(); ++i) s += v[i] * v[i];
    const size_t n = (to > from) ? (to - from) : 1;
    return sqrt(s / (double)n);
}

void testStreaming() {
    printf("streaming behaviour\n");
    const double sr = 44100.0;
    const size_t N = 44100;
    const Params p = Params::defaults();
    Config probe;
    probe.compute(p, sr);
    const int lat = probe.latency;

    // config().latency is a buffering delay, not a shift: that many samples
    // must go in before the first block comes out, but the emitted stream is
    // aligned with the input. Pin both halves of that, because everything
    // below compares out[i] against in[i] and would quietly pass if the
    // alignment ever changed.
    {
        Channel ch;
        ch.configure(probe);
        std::vector<double> in((size_t)lat, 0.25);
        ch.push(&in[0], (size_t)lat - 1, 1);
        if (ch.available() != 0) {
            printf("  FAIL  output appeared before latency samples were fed\n");
            ++g_failures;
        }
        ch.push(&in[(size_t)lat - 1], 1, 1);
        if (ch.available() == 0) {
            printf("  FAIL  no output after latency samples were fed\n");
            ++g_failures;
        }
    }

    // Silence in, silence out.
    {
        std::vector<double> in(N, 0.0);
        const std::vector<double> out = runChannel(in, p, sr);
        double peak = 0.0;
        for (size_t i = 0; i < out.size(); ++i) peak = fabs(out[i]) > peak ? fabs(out[i]) : peak;
        if (!(peak == 0.0)) fail("silence stays silent", peak, 0.0);
    }

    // Clean tonal audio should come back essentially unchanged, allowing for
    // the reported latency.
    std::vector<double> clean(N);
    {
        Rng rng(31337u);
        for (size_t i = 0; i < N; ++i) {
            const double t = (double)i / sr;
            clean[i] = 0.30 * sin(6.283185307 * 440.0 * t)
                     + 0.15 * sin(6.283185307 * 1237.0 * t)
                     + 0.01 * rng.centred();
        }
    }
    {
        const std::vector<double> out = runChannel(clean, p, sr);
        double num = 0.0, den = 0.0;
        for (size_t i = 2000; i + 2000 < N; ++i) {
            const double d = out[i] - clean[i];
            num += d * d;
            den += clean[i] * clean[i];
        }
        const double snr = 10.0 * log10((den + 1e-300) / (num + 1e-300));
        printf("  clean-audio SNR through the repair path %.1f dB\n", snr);
        if (!(snr > 35.0)) { printf("  FAIL  clean audio is being damaged\n"); ++g_failures; }
    }

    // Injected clicks must come out smaller than they went in.
    {
        std::vector<double> dirty = clean;
        Rng rng(5150u);
        std::vector<size_t> at;
        for (size_t i = 5000; i + 5000 < N; i += 997) {
            const int len = 2 + (int)(rng.uniform() * 6.0);
            const double amp = 0.25 + 0.5 * rng.uniform();
            for (int k = 0; k < len && i + (size_t)k < N; ++k) {
                dirty[i + (size_t)k] += amp * ((k & 1) ? -1.0 : 1.0)
                                      * (1.0 - (double)k / (double)len);
            }
            at.push_back(i);
        }

        // Click energy left in the -2..14 sample neighbourhood of each one.
        auto reduction = [&](const std::vector<double> & out) {
            double before = 0.0, after = 0.0;
            for (size_t idx = 0; idx < at.size(); ++idx) {
                const size_t i = at[idx];
                if (i + 16 >= N) continue;
                for (int k = -2; k < 14; ++k) {
                    const size_t a = i + (size_t)k;
                    const double b0 = dirty[a] - clean[a];
                    const double a0 = out[a] - clean[a];
                    before += b0 * b0;
                    after += a0 * a0;
                }
            }
            return 10.0 * log10((before + 1e-300) / (after + 1e-300));
        };

        // The default replaces the damaged samples outright, so on a tone the
        // only thing left of a click is the interpolation error - which is
        // small. It measures 20.2 dB without the shape fit and 19.7 with it:
        // these bursts alternate at the Nyquist rate, nothing like a shellac
        // click, and the fit's 1-8 kHz shapes take a part of them. A floor of
        // 19 still catches a repair that has gone partial or stopped being
        // applied (at depth 0 it is 5 dB).
        const double full = reduction(runChannel(dirty, p, sr));
        printf("  injected click energy reduced by %.1f dB at the defaults\n", full);
        if (!(full > 19.0)) {
            printf("  FAIL  the default repair leaves too much of each click\n");
            ++g_failures;
        }

        // At depth 0 the repair subtracts Config::wienerMax of the
        // discrepancy, so with a perfect estimate the click can only shrink by
        // -20*log10(1 - wienerMax) - about 5.2 dB at the calibrated 0.45.
        // Checking against that ceiling rather than against a loose floor is
        // what makes this test mean something: it says the interpolation is
        // accurate enough that the subtraction fraction is the only thing
        // holding it back. So it is the repair alone: these clicks, 44 a
        // second at up to 0.75, open the body key, and the stages after the
        // repair then take more of their top band - the body, fine and micro
        // stages and the flank repair took it from 5.0 to 5.6 dB before the
        // micro stage, and to 5.9 with it.
        Params p0 = p;
        p0.depth = 0.0f;
        const double red = reduction(runChannel(dirty, p0, sr,
                                                [](Config & c) { c.body = 0; c.flank = 0; }));
        Config probe2;
        probe2.compute(p0, sr);
        const double ceiling = -20.0 * log10(1.0 - probe2.wienerMax);
        printf("  at depth 0, reduced by %.1f dB (ceiling for a"
               " %.2f subtraction is %.1f dB)\n", red, probe2.wienerMax, ceiling);
        if (!(red > ceiling - 1.0)) {
            printf("  FAIL  well short of the subtraction ceiling\n");
            ++g_failures;
        }
        if (!(red < ceiling + 0.5)) {
            printf("  FAIL  above the subtraction ceiling, which is impossible\n");
            ++g_failures;
        }

        // The same, for clicks that start in the last few samples of a block
        // and run on into the next one. The clicks above land all over the
        // block and average this case away; it once came out at 0.8 dB.
        std::vector<double> edge = clean;
        std::vector<size_t> edgeAt;
        for (size_t b = 10; (b + 1) * (size_t)kBlock + 16 < N - 5000; b += 7) {
            const size_t i = b * (size_t)kBlock + (size_t)kBlock - 4;
            for (int k = 0; k < 10; ++k) {
                edge[i + (size_t)k] += 0.6 * ((k & 1) ? -1.0 : 1.0) * (1.0 - k / 10.0);
            }
            edgeAt.push_back(i);
        }
        const std::vector<double> edgeOut = runChannel(edge, p, sr);
        double eb = 0.0, ea = 0.0;
        for (size_t idx = 0; idx < edgeAt.size(); ++idx) {
            for (int k = -2; k < 14; ++k) {
                const size_t a = edgeAt[idx] + (size_t)k;
                eb += (edge[a] - clean[a]) * (edge[a] - clean[a]);
                ea += (edgeOut[a] - clean[a]) * (edgeOut[a] - clean[a]);
            }
        }
        const double edgeRed = 10.0 * log10((eb + 1e-300) / (ea + 1e-300));
        printf("  clicks straddling a block edge reduced by %.1f dB\n", edgeRed);
        if (!(edgeRed > 20.0)) {             // measures 33 at the defaults
            printf("  FAIL  a click across a block edge is only partly repaired\n");
            ++g_failures;
        }
    }

    // Hostile input: nothing non-finite may escape, and clean audio afterwards
    // must still come back clean.
    {
        std::vector<double> nasty(N, 0.0);
        for (size_t i = 0; i < N / 2; ++i) {
            switch (i % 6) {
            case 0: nasty[i] = 0.0; break;
            case 1: nasty[i] = 1e30; break;
            case 2: nasty[i] = -1e30; break;
            case 3: nasty[i] = 1e-320; break;         // denormal
            case 4: nasty[i] = (double)(i & 1) - 0.5; break;
            default: nasty[i] = 1.0e9; break;
            }
        }
        for (size_t i = N / 2; i < N; ++i) nasty[i] = clean[i];

        const std::vector<double> out = runChannel(nasty, p, sr);
        for (size_t i = 0; i < out.size(); ++i) {
            if (!(out[i] > -1e35 && out[i] < 1e35)) {
                fail("non-finite output from hostile input", out[i], 0.0);
                break;
            }
        }
        const double tail = rms(out, N - 4000, N);
        if (!(tail > 0.01)) {
            printf("  FAIL  did not recover after hostile input (tail rms %.3g)\n", tail);
            ++g_failures;
        }
    }

    // Dry/wet 0 must be a true bypass, sample for sample after the latency.
    {
        Params bp = Params::defaults();
        bp.dryWet = 0.0f;
        std::vector<double> dirty = clean;
        for (size_t i = 5000; i + 5000 < N; i += 997) dirty[i] += 0.5;
        const std::vector<double> out = runChannel(dirty, bp, sr);
        double worst = 0.0;
        for (size_t i = 0; i < N; ++i) {
            const double d = fabs(out[i] - dirty[i]);
            if (d > worst) worst = d;
        }
        if (!(worst == 0.0)) fail("dry/wet 0 is a bit-exact bypass", worst, 0.0);
    }
}

//! Every supported model order, through the real streaming path.
//!
//! The inner products are vectorised with a tail: eight doubles per iteration,
//! then two, then one. `order + 1` taps and `n - d` autocorrelation lengths hit
//! every residue class, so sweeping the order is what covers the tail
//! arithmetic. A mistake there would be silent - a wrong last tap still
//! produces plausible audio - so this checks reconstruction actually works at
//! each order rather than merely that nothing crashed.
void testOrderSweep() {
    printf("every model order through the streaming path\n");
    const double sr = 44100.0;
    const size_t N = 16384;

    std::vector<double> clean(N);
    Rng rng(2024u);
    for (size_t i = 0; i < N; ++i) {
        const double t = (double)i / sr;
        clean[i] = 0.30 * sin(6.283185307 * 392.0 * t)
                 + 0.12 * sin(6.283185307 * 987.0 * t)
                 + 0.01 * rng.centred();
    }
    std::vector<double> dirty = clean;
    std::vector<size_t> at;
    for (size_t i = 3000; i + 3000 < N; i += 611) {
        const int len = 2 + (int)(rng.uniform() * 5.0);
        const double amp = 0.30 + 0.4 * rng.uniform();
        for (int k = 0; k < len && i + (size_t)k < N; ++k) {
            dirty[i + (size_t)k] += amp * ((k & 1) ? -1.0 : 1.0);
        }
        at.push_back(i);
    }

    double worstRed = 1e9;
    int worstOrder = 0, checked = 0;
    for (int order = (int)kMinOrder; order <= (int)kMaxOrder; order += 2) {
        Params p = Params::defaults();
        p.order = order;
        p.sanitize();
        if (p.order != order) continue;          // sanitize() rejected it

        const std::vector<double> out = runChannel(dirty, p, sr);

        for (size_t i = 0; i < out.size(); ++i) {
            if (!(out[i] > -1e35 && out[i] < 1e35)) {
                printf("  FAIL  order %d produced non-finite output\n", order);
                ++g_failures;
                break;
            }
        }

        double before = 0.0, after = 0.0;
        for (size_t idx = 0; idx < at.size(); ++idx) {
            const size_t i = at[idx];
            if (i + 12 >= N) continue;
            for (int k = -2; k < 10; ++k) {
                const size_t a = i + (size_t)k;
                const double b0 = dirty[a] - clean[a];
                const double a0 = out[a] - clean[a];
                before += b0 * b0;
                after += a0 * a0;
            }
        }
        const double red = 10.0 * log10((before + 1e-300) / (after + 1e-300));
        if (red < worstRed) { worstRed = red; worstOrder = order; }
        ++checked;
        if (!(red > 6.0)) {      // full replacement; the worst order measures 10
            printf("  FAIL  order %d reduced clicks by only %.1f dB\n", order, red);
            ++g_failures;
        }
    }
    printf("  %d orders checked, worst click reduction %.1f dB (at order %d)\n",
           checked, worstRed, worstOrder);
    if (!(checked > 100)) {
        printf("  FAIL  expected to sweep far more orders than %d\n", checked);
        ++g_failures;
    }
}

//! The wide kernels - AVX2 or NEON - sum in a different order and fuse the
//! multiply-adds, so they may differ from the baseline in the last bits and
//! nowhere else. Every order, because each changes the tap count and with it
//! which remainder paths of the blocked filter run. Where the CPU has no AVX2
//! this compares the baseline with itself, and says so.
void testKernels() {
    allowWideKernels(true);
    const char * wide = kernelName();
    printf("Wide kernels against the baseline (%s here)\n", wide);
    const double sr = 44100.0;
    const size_t N = 16384;

    std::vector<double> dirty(N);
    Rng rng(77u);
    for (size_t i = 0; i < N; ++i) {
        const double t = (double)i / sr;
        dirty[i] = 0.30 * sin(6.283185307 * 392.0 * t)
                 + 0.12 * sin(6.283185307 * 987.0 * t)
                 + 0.01 * rng.centred();
    }
    // Dense small crackle and some larger clicks, so that both passes and
    // the re-repair have work to do.
    for (size_t i = 2000; i + 16 < N; i += 37 + (rng.next() % 97)) {
        dirty[i] += (rng.uniform() < 0.1 ? 0.4 : 0.03) * rng.centred();
    }

    double worst = 0.0;
    int worstOrder = 0, checked = 0;
    for (int order = (int)kMinOrder; order <= (int)kMaxOrder; order += 2) {
        Params p = Params::defaults();
        p.order = order;
        p.sanitize();
        if (p.order != order) continue;

        allowWideKernels(false);
        const std::vector<double> base = runChannel(dirty, p, sr);
        allowWideKernels(true);
        const std::vector<double> fast = runChannel(dirty, p, sr);

        for (size_t i = 0; i < N; ++i) {
            const double d = fabs(fast[i] - base[i]);
            if (!(d <= worst)) { worst = d; worstOrder = order; }
        }
        ++checked;
    }
    allowWideKernels(true);
    printf("  %d orders, worst difference %.3g (at order %d)\n",
           checked, worst, worstOrder);
    // Rounding is ~1e-16 of the signal. A detection decided differently would
    // be a whole click's worth - far above this.
    if (!(worst < 1e-9)) {
        printf("  FAIL  the kernels disagree by more than rounding\n");
        ++g_failures;
    }
}

//! Delta mode must output exactly what normal mode takes out, so that what is
//! heard through it is what the repair removes and nothing else.
void testDelta() {
    printf("delta output\n");
    const double sr = 44100.0;
    const size_t N = 32768;
    const std::vector<double> dirty = clickedTone(N, sr, 91u);

    const float wets[2] = { 1.0f, 0.6f };
    for (int k = 0; k < 2; ++k) {
        Params p = Params::defaults();
        p.dryWet = wets[k];
        const std::vector<double> out = runChannel(dirty, p, sr);
        p.delta = true;
        const std::vector<double> del = runChannel(dirty, p, sr);

        double worst = 0.0, removed = 0.0;
        for (size_t i = 0; i < N; ++i) {
            const double d = fabs(del[i] - (dirty[i] - out[i]));
            if (d > worst) worst = d;
            removed += del[i] * del[i];
        }
        printf("  wet %.1f: worst difference from input - output %.3g\n",
               (double)wets[k], worst);
        // At wet 1 the normal mix is y + 0*d, so the two are the same sum;
        // below it they differ by rounding only.
        if (!(k == 0 ? worst == 0.0 : worst < 1e-12)) {
            printf("  FAIL  delta is not what the repair removes\n");
            ++g_failures;
        }
        if (!(removed > 0.0)) {
            printf("  FAIL  delta is silent; nothing was repaired\n");
            ++g_failures;
        }
    }

    // A live toggle must not reset the pipeline.
    Config a, b;
    Params p = Params::defaults();
    a.compute(p, sr);
    p.delta = true;
    b.compute(p, sr);
    Channel ch;
    ch.configure(a);
    if (!ch.retune(b)) {
        printf("  FAIL  switching delta on needs a reconfigure\n");
        ++g_failures;
    }
}

//! A stream that starts or stops mid-note - a seek, or a gapless change of
//! track, where the wrapper resets and drains the pipeline - must come out
//! with its edges alone. The step from or to the silence either side used to
//! read as a click: the first and last 10 ms were rebuilt from the silence,
//! changed by -10 to -25 dB, and at the default order the step opened the
//! body key, whose stages then cut this steady tone by 25 dB for two seconds.
void testEdges() {
    printf("stream edges\n");
    const double sr = 44100.0;
    const size_t N = (size_t)(3.0 * sr), W = (size_t)(0.010 * sr), T = (size_t)(2.0 * sr);
    const int orders[3] = { 32, 128, 256 };
    for (int a = 0; a < 2; ++a) {
        const double amp = a ? 0.5 : 0.1;
        std::vector<double> x(N);
        Rng rng(5u);
        for (size_t i = 0; i < N; ++i) {
            const double t = (double)i / sr;
            x[i] = amp * (sin(6.283185307 * 220.0 * t + 1.3) + 0.5 * sin(6.283185307 * 1330.0 * t + 0.4))
                 + 1e-3 * rng.centred();
        }
        for (int o = 0; o < 3; ++o) {
            Params p = Params::defaults();
            p.order = orders[o];
            const std::vector<double> y = runChannel(x, p, sr);
            std::vector<double> d(N);
            for (size_t i = 0; i < N; ++i) d[i] = y[i] - x[i];
            const double first = 20.0 * log10(rms(d, 0, W) / rms(x, 0, W) + 1e-30);
            const double last = 20.0 * log10(rms(d, N - W, N) / rms(x, N - W, N) + 1e-30);
            const double opening = 20.0 * log10(rms(d, 0, T) / rms(x, 0, T) + 1e-30);
            printf("  amplitude %.1f, order %3d: first 10 ms %6.1f dB, last 10 ms %6.1f dB,"
                   " first 2 s %6.1f dB\n", amp, orders[o], first, last, opening);
            if (!(first < -40.0 && last < -40.0 && opening < -40.0)) {
                printf("  FAIL  the edges of the stream were repaired as clicks\n");
                ++g_failures;
            }
        }
    }
}

//! What the wrappers rely on when a slider moves under a running stream:
//! retune() takes effect from the next sample out, without a reset, and
//! refuses whatever would need other buffers.
void testRetune() {
    printf("live retune\n");
    const double sr = 44100.0;
    const size_t N = 65536;
    const std::vector<double> dirty = clickedTone(N, sr, 17u);

    Params p = Params::defaults();
    Config a;
    a.compute(p, sr);
    Channel ch;
    ch.configure(a);

    // Half the stream wet, then Dry/Wet to 0 between two pulls.
    std::vector<double> out(N, 0.0);
    size_t got = 0;
    ch.push(&dirty[0], N / 2, 1);
    got = ch.available();
    ch.pull(&out[0], got, 1);
    p.dryWet = 0.0f;
    Config b;
    b.compute(p, sr);
    if (!ch.retune(b)) {
        printf("  FAIL  Dry/Wet needs a reconfigure\n");
        ++g_failures;
        return;
    }
    ch.push(&dirty[N / 2], N - N / 2, 1);
    ch.drain();
    ch.pull(&out[got], N - got, 1);

    double before = 0.0, after = 0.0;
    for (size_t i = 0; i < got; ++i) before = fmax(before, fabs(out[i] - dirty[i]));
    for (size_t i = got; i < N; ++i) after = fmax(after, fabs(out[i] - dirty[i]));
    printf("  wet until sample %zu: worst change %.3g; dry after it: %.3g\n", got, before, after);
    if (!(before > 0.01)) {
        printf("  FAIL  nothing was repaired before the retune\n");
        ++g_failures;
    }
    if (after != 0.0) {
        printf("  FAIL  Dry/Wet 0 is not the input, bit for bit, from the next sample out\n");
        ++g_failures;
    }

    // Max repair sizes the pipeline: a live move has to be refused.
    p.maxLengthMs = 2.0f * Params::defaults().maxLengthMs;
    Config c;
    c.compute(p, sr);
    if (c.maxRun == a.maxRun || ch.retune(c)) {
        printf("  FAIL  retune() took a Max repair move (maxRun %d -> %d)\n", a.maxRun, c.maxRun);
        ++g_failures;
    }
}

} // anonymous namespace

int main() {
    testLevinson();
    testBandedSolve();
    testStreaming();
    testOrderSweep();
    testKernels();
    testDelta();
    testEdges();
    testRetune();

    if (g_failures == 0) { printf("\nOK\n"); return 0; }
    printf("\n%d failure(s)\n", g_failures);
    return 1;
}
