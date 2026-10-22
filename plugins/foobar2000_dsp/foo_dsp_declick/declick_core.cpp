/* ========================================
 *  foo_dsp_declick - portable DSP core
 * ======================================== */

#include "declick_core.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#define DECLICK_HAVE_MXCSR 1
// SSE2 for the double-precision inner products. It is the floor for every
// configuration this project builds; the scalar fallback below is kept
// correct for anything else.
#include <emmintrin.h>
#define DECLICK_SSE2 1
// AVX2 and FMA on top, chosen at run time from CPUID - see pickKernels().
// MSVC compiles AVX2 intrinsics without /arch:AVX2; GCC and clang need the
// target attribute on every function that uses them.
#include <immintrin.h>
#define DECLICK_AVX2 1
#if defined(__GNUC__) || defined(__clang__)
#define DECLICK_AVX2_FN __attribute__((target("avx2,fma")))
#include <cpuid.h>
#else
#define DECLICK_AVX2_FN
#include <intrin.h>
#endif
#elif defined(_M_ARM64) || defined(__aarch64__)
#define DECLICK_HAVE_FPCR 1
#define DECLICK_SSE2 0
#if defined(_MSC_VER)
#include <intrin.h>
#endif
// NEON with double lanes and FMA is part of AArch64 itself, so unlike AVX2
// there is nothing to detect: every AArch64 CPU runs these kernels.
#include <arm_neon.h>
#define DECLICK_NEON 1
#else
#define DECLICK_SSE2 0
#endif
#ifndef DECLICK_AVX2
#define DECLICK_AVX2 0
#endif
#ifndef DECLICK_NEON
#define DECLICK_NEON 0
#endif

namespace declick {

#if defined(DECLICK_HAVE_MXCSR)
// FTZ only. DAZ lives in a bit that early SSE2 parts treat as reserved.
scoped_flush_denormals::scoped_flush_denormals() : m_saved(_mm_getcsr()) {
    if ((m_saved & 0x8000u) == 0u) _mm_setcsr(m_saved | 0x8000u);
}
scoped_flush_denormals::~scoped_flush_denormals() {
    if ((m_saved & 0x8000u) == 0u) _mm_setcsr(m_saved);
}

#elif defined(DECLICK_HAVE_FPCR)

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
    if ((m_saved & kFpcrFlushToZero) == 0u) writeFpcr(m_saved | kFpcrFlushToZero);
}
scoped_flush_denormals::~scoped_flush_denormals() {
    if ((m_saved & kFpcrFlushToZero) == 0u) writeFpcr(m_saved);
}
#endif

namespace {

//! Inner product of two runs of doubles.
//!
//! Every hot loop in this file is one of these - the signal autocorrelation,
//! the forward and backward prediction residuals, the interpolation residual,
//! its right hand side, and the coefficient autocorrelation - so they all go
//! through here.
//!
//! The four accumulators matter more than the vector width. Written the
//! obvious way, `for (k) s += x[k]*y[k]` is a chain of dependent additions and
//! runs at one multiply-add per add-latency (about four cycles), whatever the
//! machine could otherwise sustain. Splitting the sum breaks that chain.
//!
//! Summation order therefore differs from a plain serial loop, which is a
//! change in the last bits. For inner products of hundreds to thousands of
//! terms the partial-sum form is if anything slightly more accurate, and it was
//! checked against the ground-truth corpus: identical scores to two decimals.
inline double dot(const double * x, const double * y, int n) {
    if (n <= 0) return 0.0;
#if DECLICK_SSE2
    __m128d s0 = _mm_setzero_pd(), s1 = _mm_setzero_pd();
    __m128d s2 = _mm_setzero_pd(), s3 = _mm_setzero_pd();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(x + i),     _mm_loadu_pd(y + i)));
        s1 = _mm_add_pd(s1, _mm_mul_pd(_mm_loadu_pd(x + i + 2), _mm_loadu_pd(y + i + 2)));
        s2 = _mm_add_pd(s2, _mm_mul_pd(_mm_loadu_pd(x + i + 4), _mm_loadu_pd(y + i + 4)));
        s3 = _mm_add_pd(s3, _mm_mul_pd(_mm_loadu_pd(x + i + 6), _mm_loadu_pd(y + i + 6)));
    }
    for (; i + 2 <= n; i += 2) {
        s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(x + i), _mm_loadu_pd(y + i)));
    }
    const __m128d t = _mm_add_pd(_mm_add_pd(s0, s1), _mm_add_pd(s2, s3));
    double acc = _mm_cvtsd_f64(_mm_add_sd(t, _mm_unpackhi_pd(t, t)));
    if (i < n) acc += x[i] * y[i];
    return acc;
#else
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += x[i] * y[i];
        a1 += x[i + 1] * y[i + 1];
        a2 += x[i + 2] * y[i + 2];
        a3 += x[i + 3] * y[i + 3];
    }
    double acc = (a0 + a1) + (a2 + a3);
    for (; i < n; ++i) acc += x[i] * y[i];
    return acc;
#endif
}

inline double dmin(double a, double b) { return a < b ? a : b; }
inline double dmax(double a, double b) { return a > b ? a : b; }

//! The largest |x[i]|, or 0 for n <= 0. A maximum is exact, so splitting it
//! changes nothing; the loop it replaced was scalar, and compilers do not
//! vectorise a reduction of fabs() through a compare.
inline double absMax(const double * x, int n) {
    double r = 0.0;
    int i = 0;
#if DECLICK_SSE2
    const __m128d sign = _mm_set1_pd(-0.0);
    __m128d m0 = _mm_setzero_pd(), m1 = _mm_setzero_pd();
    for (; i + 4 <= n; i += 4) {
        m0 = _mm_max_pd(m0, _mm_andnot_pd(sign, _mm_loadu_pd(x + i)));
        m1 = _mm_max_pd(m1, _mm_andnot_pd(sign, _mm_loadu_pd(x + i + 2)));
    }
    m0 = _mm_max_pd(m0, m1);
    r = _mm_cvtsd_f64(_mm_max_sd(m0, _mm_unpackhi_pd(m0, m0)));
#elif DECLICK_NEON
    float64x2_t m0 = vdupq_n_f64(0.0), m1 = vdupq_n_f64(0.0);
    for (; i + 4 <= n; i += 4) {
        m0 = vmaxq_f64(m0, vabsq_f64(vld1q_f64(x + i)));
        m1 = vmaxq_f64(m1, vabsq_f64(vld1q_f64(x + i + 2)));
    }
    r = vmaxvq_f64(vmaxq_f64(m0, m1));
#endif
    for (; i < n; ++i) {
        const double v = fabs(x[i]);
        if (v > r) r = v;
    }
    return r;
}

inline float clampf(float v, float lo, float hi) {
    if (!(v > lo)) return lo;        // negated, so NaN lands on lo
    if (!(v < hi)) return hi;
    return v;
}

const double kAbsurd = 1.0e30;

//! Half-width of the band around the last median that Channel::ringMedian()
//! looks in first, as a fraction of it. Any value gives the same median; this
//! one only decides how often the second pass runs.
const double kMedianBand = 0.125;

inline bool finite(double v) { return v > -kAbsurd && v < kAbsurd; }

// ---------------------------------------------------------------------------
// The hot loops, as two kernels with a baseline and a wide version: AVX2 + FMA
// on x86, NEON on AArch64.
//
// filter:   out[i] = sum_k h[k] * x[i+k], for i < count and k < taps. The
//           forward and backward prediction residuals and the per-click
//           interpolation residual are all this shape.
// autocorr: r[d] = sum_i x[i] * x[i+d], for d <= maxLag. x must be followed by
//           maxLag zeros; the baseline does not read them, the wide versions
//           do, which turns it into a filter of x against itself.
//
// The baseline is exactly the dot() loops these replaced, so a CPU without
// AVX2 produces what it always did, bit for bit.

typedef void (*FilterFn)(const double * h, const double * x, double * out,
                         int count, int taps);
typedef void (*AutocorrFn)(const double * x, int n, double * r, int maxLag);
//! Two radix-2 stages of the body stage's FFT, half-lengths h and 2h, in one
//! sweep over the n points; h >= 4. sgn is -1 forward, +1 inverse.
typedef void (*FftPairFn)(double * re, double * im, int n, int h,
                          const double * stRe, const double * stIm, double sgn);
//! One radix-2 stage of half-length h over the n points; h >= 4. The FFT
//! needs it alone when its stage count is odd, which at the fine stage's 128
//! points it is. sgn is -1 forward, +1 inverse.
typedef void (*FftOneFn)(double * re, double * im, int n, int h,
                         const double * stRe, const double * stIm, double sgn);
//! `count` sorted windows of `n` values laid end to end: in each, replace
//! out[b] with in[b], keeping it sorted. See sortedReplace().
typedef void (*SortedRowFn)(double * rows, int n, int stride, int count,
                            const double * out, const double * in);
//! Two dot()s, each to the bit: *r0 = dot(x0, y0, n0), *r1 = dot(x1, y1, n1).
typedef void (*Dot2Fn)(const double * x0, const double * y0, int n0,
                       const double * x1, const double * y1, int n1,
                       double * r0, double * r1);
//! The flank repair's high-passed correction: acc[t0 + j + u] += hp[u] * d[j]
//! for j < m and u in -Hh .. Hh, j by j, skipping a d[j] of zero, and never
//! past acc[xEnd]. hpP and hpN point at tap 0 of the taps with kFlankPad
//! zeros either side, -0.0 in hpP and +0.0 in hpN; see flankAddAvx2().
typedef void (*FlankAddFn)(double * acc, const double * hpP, const double * hpN,
                           int Hh, const double * d, int t0, int m, int xEnd);
struct QuantilePos;
//! q[b] = the quantile at p of frames[0][b] .. frames[n - 1][b], for b < nb,
//! straight from the frames; false, having done nothing, where p is too far up
//! for it. Null where a kernel set keeps sorted windows instead.
typedef bool (*FrameQuantileFn)(const double * const * frames, int n, int nb,
                                const QuantilePos & p, double * q);
//! The fine and micro stages' spectrum from their packed transform of M
//! points: bins k0 .. k0 + nb - 1 of the N = 2M real samples, as X into xr and
//! xi and |X| into mg. See fineHop().
typedef void (*BinUnpackFn)(const double * re, const double * im, int M, int k0, int nb,
                            const double * twRe, const double * twIm,
                            double * xr, double * xi, double * mg);
//! The crackle floor stage's power over 7 bins, for b in b0 .. b1 - 1:
//! Pb[b] = the sum of P[b - 3] .. P[b + 3] in that order, each index held to
//! -m0 .. nb - 1. See crackleFloorHop().
typedef void (*Smooth7Fn)(const double * P, int nb, int m0, int b0, int b1, double * Pb);
struct FloorGain;
//! The crackle floor stage's gain, bins b0 .. b1 - 1, into G. See FloorGain.
typedef void (*FloorGainFn)(const FloorGain & p, int b0, int b1, double * G);

struct Kernels {
    FilterFn    filter;
    AutocorrFn  autocorr;
    FftPairFn   fftPair;
    FftOneFn    fftOne;
    SortedRowFn sortedRow;
    Dot2Fn      dot2;
    FlankAddFn  flankAdd;
    FrameQuantileFn frameQuantile;
    BinUnpackFn binUnpack;
    Smooth7Fn   smooth7;
    FloorGainFn floorGain;
    const char * name;
};

//! What crackleFloorHop()'s gain is made of: the five frames' 7-bin powers,
//! the floor, and under kept and capped the floors it is held under; Wiener's
//! gain on them against beta x the floor, never below gmin, rising at once and
//! falling back by a, from scratch where first.
struct FloorGain {
    const double * B[5];
    const double * Fl, * Sv, * Wm;
    bool kept, capped, first;
    double beta, gmin, a;
};

enum { kFlankPad = 16 };

void filterBase(const double * h, const double * x, double * out,
                int count, int taps) {
    for (int i = 0; i < count; ++i) out[i] = dot(h, x + i, taps);
}

void dot2Base(const double * x0, const double * y0, int n0,
              const double * x1, const double * y1, int n1,
              double * r0, double * r1) {
    *r0 = dot(x0, y0, n0);
    *r1 = dot(x1, y1, n1);
}

void flankAddBase(double * acc, const double * hpP, const double *, int Hh,
                  const double * d, int t0, int m, int xEnd) {
    for (int j = 0; j < m; ++j) {
        const double dj = d[j];
        if (dj == 0.0) continue;
        const int t = t0 + j;
        const int b = (t + Hh > xEnd) ? (xEnd - t) : Hh;
        for (int u = -Hh; u <= b; ++u) acc[t + u] += hpP[u] * dj;
    }
}

void autocorrBase(const double * x, int n, double * r, int maxLag) {
    for (int d = 0; d <= maxLag; ++d) r[d] = dot(x + d, x, n - d);
}

//! Each group of four points k, k+h, k+2h, k+3h goes through both stages
//! while it is in registers: (0,1) and (2,3) by the stage of length 2h, then
//! (0,2) and (1,3) by the stage of length 4h. Half the passes over the arrays
//! that one stage at a time makes, and the FFT is bound by those, not by its
//! multiplies. Every twiddle comes from the table and every butterfly is the
//! radix-2 one, operation for operation, so the result is what two radix-2
//! passes give, bit for bit; deriving the second stage's odd twiddles as
//! -i times the even ones would save multiplies but round differently.
void fftPairBase(double * re, double * im, int n, int h,
                 const double * stRe, const double * stIm, double sgn) {
    const double * w1r = stRe + h - 1;
    const double * w1i = stIm + h - 1;
    const double * w2r = stRe + 2 * h - 1;
    const double * w2i = stIm + 2 * h - 1;
    const double * w3r = w2r + h;
    const double * w3i = w2i + h;
    for (int i = 0; i < n; i += 4 * h) {
        double * r0 = re + i;
        double * r1 = r0 + h;
        double * r2 = r1 + h;
        double * r3 = r2 + h;
        double * i0 = im + i;
        double * i1 = i0 + h;
        double * i2 = i1 + h;
        double * i3 = i2 + h;
        for (int k = 0; k < h; ++k) {
            const double c1 = w1r[k], s1 = sgn * w1i[k];
            double tr = c1 * r1[k] - s1 * i1[k];
            double ti = c1 * i1[k] + s1 * r1[k];
            const double y1r = r0[k] - tr, y1i = i0[k] - ti;
            const double y0r = r0[k] + tr, y0i = i0[k] + ti;
            tr = c1 * r3[k] - s1 * i3[k];
            ti = c1 * i3[k] + s1 * r3[k];
            const double y3r = r2[k] - tr, y3i = i2[k] - ti;
            const double y2r = r2[k] + tr, y2i = i2[k] + ti;
            const double c2 = w2r[k], s2 = sgn * w2i[k];
            tr = c2 * y2r - s2 * y2i;
            ti = c2 * y2i + s2 * y2r;
            r2[k] = y0r - tr; i2[k] = y0i - ti;
            r0[k] = y0r + tr; i0[k] = y0i + ti;
            const double c3 = w3r[k], s3 = sgn * w3i[k];
            tr = c3 * y3r - s3 * y3i;
            ti = c3 * y3i + s3 * y3r;
            r3[k] = y1r - tr; i3[k] = y1i - ti;
            r1[k] = y1r + tr; i1[k] = y1i + ti;
        }
    }
}

//! Two window places at a time: each takes its neighbour `d` away where the
//! block moves over it, and keeps what it has where it does not. Reading one
//! place past the window at either end is why the windows carry a pad.
inline void sortedStep2(double * m, int i, int a, int b, int d) {
#if DECLICK_SSE2
    const __m128i idx = _mm_set_epi32(0, i + 1, 0, i);
    // a <= i <= b, spread from the 32-bit result over the whole 64-bit lane.
    __m128i act = _mm_andnot_si128(_mm_cmpgt_epi32(_mm_set1_epi32(a), idx),
                                   _mm_cmpgt_epi32(_mm_set1_epi32(b + 1), idx));
    act = _mm_shuffle_epi32(act, _MM_SHUFFLE(2, 2, 0, 0));
    const __m128d cur = _mm_loadu_pd(m);
    const __m128d nxt = _mm_loadu_pd(m + d);
    _mm_storeu_pd(m, _mm_or_pd(_mm_and_pd(_mm_castsi128_pd(act), nxt),
                               _mm_andnot_pd(_mm_castsi128_pd(act), cur)));
#elif DECLICK_NEON
    // 64-bit lanes throughout: vbslq_f64 selects bit by bit, so a mask
    // widened from 32 bits without its sign would take half of each double
    // from either side.
    const int64x2_t idx = { i, i + 1 };
    const uint64x2_t act = vandq_u64(vcgeq_s64(idx, vdupq_n_s64(a)),
                                     vcleq_s64(idx, vdupq_n_s64(b)));
    vst1q_f64(m, vbslq_f64(act, vld1q_f64(m + d), vld1q_f64(m)));
#else
    (void)m; (void)i; (void)a; (void)b; (void)d;
#endif
}

//! Above this many places a window is worked as the sorted array it is - the
//! ranks by binary search, the shift by memmove - rather than by the
//! branch-free passes below, which touch every place whatever moves. The
//! passes win while a window is short, where a search's branches mispredict
//! and a call costs more than the few vectors the whole window takes; they
//! lose badly once it is long, and the crackle floor stage's is 259 places at
//! 44.1 kHz (Config::crackleFloorHist) against the fine stage's 16, for one
//! value in and one out. Only the floor stage is over the line.
enum { kSortedBig = 64 };

//! Where the q quantile of n sorted values lies, as numpy's default (linear)
//! finds it: the values at floor and ceil of q (n - 1), interpolated. It is
//! the same for every window of a row, so a row finds it once. Found once per
//! window it was nearly all of the fine and micro stages' quantile reads, 4% of
//! the run on a Haswell: below SSE4.1 floor() is a call into the CRT.
struct QuantilePos {
    int lo;
    double frac;
    bool interpolate;
};

inline QuantilePos quantilePos(int n, double q) {
    const double pos = q * (double)(n - 1);
    int lo = (int)floor(pos);
    if (lo < 0) lo = 0;
    if (lo > n - 1) lo = n - 1;
    QuantilePos p;
    p.lo = lo;
    p.frac = pos - (double)lo;
    p.interpolate = (p.frac > 0.0) && lo + 1 < n;
    return p;
}

//! The quantile at p of the sorted values at v.
inline double sortedQuantile(const double * v, const QuantilePos & p) {
    if (!p.interpolate) return v[p.lo];
    return v[p.lo] + p.frac * (v[p.lo + 1] - v[p.lo]);
}

//! Where `out` leaves a sorted window of n values and `in` goes in: `out` is
//! at the number of values below it (the first of any equal to it), and `in`
//! at the number below it among the others. A count, not a search. A NaN
//! counts below nothing, so it costs a wrong quantile, not a write outside
//! the window.
//!
//! The vector pass takes the places two at a time and the loop after it the
//! odd one. It used to stand down altogether for an odd n, which left the
//! micro stage - microM is 17 at 44.1 kHz, see Config::microM - counting its
//! windows a place at a time wherever the wide kernels are not in use.
inline void sortedRanks(const double * m, int n, double out, double in,
                        int & jo, int & ji) {
    jo = 0; ji = 0;
    if (n >= (int)kSortedBig) {
        // The window is sorted, so the count of places below a value is where
        // that value belongs: the same number, in log n steps rather than n.
        jo = (int)(std::lower_bound(m, m + n, out) - m);
        ji = (int)(std::lower_bound(m, m + n, in) - m);
        ji -= (out < in);
        if (jo > n - 1) jo = n - 1;
        if (ji > n - 1) ji = n - 1;
        if (ji < 0) ji = 0;
        return;
    }
    int i = 0;
#if DECLICK_SSE2
    {
        const __m128d vo = _mm_set1_pd(out), vi = _mm_set1_pd(in);
        __m128i co = _mm_setzero_si128(), ci = _mm_setzero_si128();
        for (; i + 2 <= n; i += 2) {
            const __m128d v = _mm_loadu_pd(m + i);
            co = _mm_sub_epi64(co, _mm_castpd_si128(_mm_cmplt_pd(v, vo)));
            ci = _mm_sub_epi64(ci, _mm_castpd_si128(_mm_cmplt_pd(v, vi)));
        }
        co = _mm_add_epi64(co, _mm_unpackhi_epi64(co, co));
        ci = _mm_add_epi64(ci, _mm_unpackhi_epi64(ci, ci));
        jo = _mm_cvtsi128_si32(co);
        ji = _mm_cvtsi128_si32(ci);
    }
#endif
    // On AArch64 the loop below is left to the compiler, which vectorises the
    // two counts about as well as hand-written NEON and unrolls them further:
    // an intrinsic pass here measured 1.9% slower over a whole song on an M1.
    for (; i < n; ++i) {            // the odd place, where there is one
        jo += (m[i] < out);
        ji += (m[i] < in);
    }
    ji -= (out < in);               // `out` itself is not among the others
    if (jo > n - 1) jo = n - 1;     // only if `out` was not there after all
    if (ji > n - 1) ji = n - 1;
    if (ji < 0) ji = 0;
}

//! The places that move when `out` at rank jo leaves and `in` goes in at ji,
//! and which way: up by one over jo .. ji - 1, or down by one over
//! ji + 1 .. jo. Equal ranks leave an empty range either way.
inline void sortedMove(double * m, int n, int jo, int ji, double in) {
    if (n >= (int)kSortedBig) {
        // Only the places between the two ranks move, and they all move one
        // the same way: that is a memmove of them, typically a handful, where
        // the passes below would step over every place in the window.
        if (ji >= jo) memmove(m + jo, m + jo + 1, (size_t)(ji - jo) * sizeof(double));
        else          memmove(m + ji + 1, m + ji, (size_t)(jo - ji) * sizeof(double));
        m[ji] = in;
        return;
    }
    // Going with the move - up from the bottom, down from the top - every
    // place is read before the step that writes it.
    const int up = (ji >= jo);
    const int d  = up ? 1 : -1;
    const int a  = up ? jo : ji + 1;
    const int b  = up ? ji - 1 : jo;
#if DECLICK_SSE2 || DECLICK_NEON
    {
        int i = up ? 0 : n - 2;
        for (int c = 0; c + 2 <= n; c += 2, i += 2 * d) sortedStep2(m + i, i, a, b, d);
        if (n & 1) {                     // the odd place left, last either way
            const int t = up ? n - 1 : 0;
            m[t] = (t >= a && t <= b) ? m[t + d] : m[t];
        }
    }
#else
    {
        int i = up ? 0 : n - 1;
        for (int c = 0; c < n; ++c, i += d)
            m[i] = (i >= a && i <= b) ? m[i + d] : m[i];
    }
#endif
    m[ji] = in;
}

//! In a sorted window of n values, replace `out` - which must be one of them,
//! as it is the window's own copy - with `in`, keeping it sorted.
//!
//! Doing the move as the loop it reads as - `for (i = jo; i < ji; ++i)
//! m[i] = m[i + 1]` and its mirror - cost 74 cycles a window on a Haswell and
//! was a third of the fine stage: the trip count is one or two and depends on
//! the data, so each of the two loop guards mispredicted about half the time.
//! So the whole window runs instead, every step predicated: a few more moves,
//! no branches, and the pass vectorises. Which places move is a comparison
//! against the ranks, so the values written are the ones the loops wrote, in
//! the same places.
//!
//! The vector paths load one place either side of the window and discard it,
//! so every window the fine stage keeps has a pad either side - see
//! m_fSorted.
void sortedReplace(double * m, int n, double out, double in) {
    int jo, ji;
    sortedRanks(m, n, out, in, jo, ji);
    sortedMove(m, n, jo, ji, in);
}

//! A row of windows, `stride` apart - which is the window length where they
//! are packed, and more where each carries its own pad, as the crackle floor
//! stage's do. The ranks for a tile of them are counted first and the moves
//! made after: a window's ranks reach its own move through a
//! vector-to-register-to-vector round trip and nothing in that window can
//! start until they do, so one window at a time the row costs that latency
//! rather than its work. Counting the next window covers it.
void sortedRowBase(double * rows, int n, int stride, int count,
                   const double * out, const double * in) {
    enum { kTile = 16 };
    int jo[kTile], ji[kTile];
    for (int b0 = 0; b0 < count; b0 += (int)kTile) {
        const int nt = (count - b0 < (int)kTile) ? count - b0 : (int)kTile;
        for (int t = 0; t < nt; ++t)
            sortedRanks(rows + (size_t)(b0 + t) * (size_t)stride, n,
                        out[b0 + t], in[b0 + t], jo[t], ji[t]);
        for (int t = 0; t < nt; ++t)
            sortedMove(rows + (size_t)(b0 + t) * (size_t)stride, n,
                       jo[t], ji[t], in[b0 + t]);
    }
}

//! The odd stage out, when the FFT has one. The loop fftRadix2() used to
//! carry here, moved so that a wide kernel can take it: at 128 points it is
//! the whole of one of the five stages and it was the only scalar one left.
void fftOneBase(double * re, double * im, int n, int h,
                const double * stRe, const double * stIm, double sgn) {
    const double * wr = stRe + h - 1;
    const double * ws = stIm + h - 1;
    for (int i = 0; i < n; i += 2 * h) {
        double * ar = re + i;
        double * ai = im + i;
        double * br = ar + h;
        double * bi = ai + h;
        for (int k = 0; k < h; ++k) {
            const double c = wr[k], sn = sgn * ws[k];
            const double tr = c * br[k] - sn * bi[k];
            const double ti = c * bi[k] + sn * br[k];
            br[k] = ar[k] - tr;
            bi[k] = ai[k] - ti;
            ar[k] += tr;
            ai[k] += ti;
        }
    }
}

//! X[k] = E + e^(-2 pi i k / N) O, with E and O the transforms of the even and
//! odd samples: (Z[k] + Z*[M-k]) / 2 and (Z[k] - Z*[M-k]) / 2i. The band can
//! reach the Nyquist bin k = M, where both Z[k] and Z[M-k] are Z[0]; the masks
//! fold it there.
void binUnpackBase(const double * re, const double * im, int M, int k0, int nb,
                   const double * twRe, const double * twIm,
                   double * xr, double * xi, double * mg) {
    for (int b = 0; b < nb; ++b) {
        const int k = k0 + b;
        const int kk = k & (M - 1), mk = (M - kk) & (M - 1);
        const double zr = re[kk], zi = im[kk];
        const double cr = re[mk], ci = -im[mk];
        const double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
        const double our = 0.5 * (zi - ci), oui = -0.5 * (zr - cr);
        const double c = twRe[k], sn = twIm[k];
        const double vr = er + c * our + sn * oui;
        const double vi = ei + c * oui - sn * our;
        xr[b] = vr;
        xi[b] = vi;
        mg[b] = sqrt(vr * vr + vi * vi);
    }
}

void smooth7Base(const double * P, int nb, int m0, int b0, int b1, double * Pb) {
    for (int b = b0; b < b1; ++b) {
        double s = 0.0;
        for (int j = -3; j <= 3; ++j) {
            int kb = b + j;
            if (kb + m0 < 0) kb = -m0;
            if (kb > nb - 1) kb = nb - 1;
            s += P[kb];
        }
        Pb[b] = s;
    }
}

void floorGainBase(const FloorGain & p, int b0, int b1, double * G) {
    const double beta = p.beta, gmin = p.gmin, a = p.a;
    for (int b = b0; b < b1; ++b) {
        const double s = (p.B[0][b] + p.B[1][b] + p.B[2][b] + p.B[3][b] + p.B[4][b]) / 35.0;
        double f = p.kept ? dmin(p.Fl[b], p.Sv[b]) : p.Fl[b];
        if (p.capped) f = dmin(f, p.Wm[b]);
        const double nf = f * beta + 1e-30;
        const double x = dmax(s / nf - 1.0, 0.0);
        const double g0v = dmax(x / (1.0 + x), gmin);
        double & g = G[b];
        g = p.first ? g0v : dmax(g0v, a * g + (1.0 - a) * g0v);
    }
}

const Kernels kBaseKernels = {
    filterBase, autocorrBase, fftPairBase, fftOneBase, sortedRowBase, dot2Base, flankAddBase,
    nullptr,                    // frameQuantile: the baseline keeps sorted windows
    binUnpackBase, smooth7Base, floorGainBase,
    DECLICK_SSE2 ? "sse2" : "scalar"
};

#if DECLICK_AVX2

// Every floating-point operation in these functions is an intrinsic, and that
// is load-bearing. Without /arch:AVX2, MSVC encodes the intrinsics as AVX but
// any plain double arithmetic or store as legacy SSE2, and on Haswell - the
// 2014 MacBook Air - each switch between the two with the upper halves of the
// registers dirty costs about 70 cycles. Hence the masked loads instead of a
// scalar tail, _mm_store_sd instead of `out[i] = ...`, and the explicit
// _mm256_zeroupper() before returning to SSE2 code.

const int64_t kLaneMask[8] = { -1, -1, -1, -1, 0, 0, 0, 0 };

//! The first r lanes, r in 0..4.
DECLICK_AVX2_FN inline __m256i laneMask(int r) {
    return _mm256_loadu_si256((const __m256i *)(kLaneMask + 4 - r));
}

//! dot() with FMA, leaving the sum in the low lane of a register.
DECLICK_AVX2_FN inline __m128d dotAvx2(const double * x, const double * y, int n) {
    __m256d s0 = _mm256_setzero_pd(), s1 = _mm256_setzero_pd();
    __m256d s2 = _mm256_setzero_pd(), s3 = _mm256_setzero_pd();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = _mm256_fmadd_pd(_mm256_loadu_pd(x + i),      _mm256_loadu_pd(y + i),      s0);
        s1 = _mm256_fmadd_pd(_mm256_loadu_pd(x + i + 4),  _mm256_loadu_pd(y + i + 4),  s1);
        s2 = _mm256_fmadd_pd(_mm256_loadu_pd(x + i + 8),  _mm256_loadu_pd(y + i + 8),  s2);
        s3 = _mm256_fmadd_pd(_mm256_loadu_pd(x + i + 12), _mm256_loadu_pd(y + i + 12), s3);
    }
    for (; i + 4 <= n; i += 4) {
        s0 = _mm256_fmadd_pd(_mm256_loadu_pd(x + i), _mm256_loadu_pd(y + i), s0);
    }
    if (i < n) {
        const __m256i m = laneMask(n - i);
        s1 = _mm256_fmadd_pd(_mm256_maskload_pd(x + i, m),
                             _mm256_maskload_pd(y + i, m), s1);
    }
    const __m256d t = _mm256_add_pd(_mm256_add_pd(s0, s1), _mm256_add_pd(s2, s3));
    const __m128d u = _mm_add_pd(_mm256_castpd256_pd128(t), _mm256_extractf128_pd(t, 1));
    return _mm_add_sd(u, _mm_unpackhi_pd(u, u));
}

//! One broadcast tap times consecutive x feeds four outputs per register, so
//! a block of outputs shares every load of h. Each accumulator is a dependent
//! chain, one FMA latency per tap, and enough of them side by side cover that
//! latency: eight on x64, four on x86, which has only eight vector registers.
//!
//! The loads of x are shared as well. Accumulator j wants x at 4j + k for tap
//! k, which for tap k + 4 is what accumulator j + 1 wanted for tap k. So taps
//! go in groups of four a stride of four apart - k, k+4, k+8, k+12 - and each
//! load of x feeds up to four of them: 11 loads and 4 broadcasts per 32
//! multiply-adds where one tap at a time took 36. Most of those loads straddle
//! a cache line, which costs Haswell two, and it can only issue two a cycle,
//! so tap at a time left its FMA units waiting on them. x86 runs two taps a
//! group, four accumulators. The taps left over go one at a time, and the
//! outputs left over through dotAvx2().
//!
//! Each accumulator still adds its taps in a fixed order, but no longer in
//! index order, so this differs from one tap at a time in the last bits.
#define DECLICK_FMA(acc, hv, xv) acc = _mm256_fmadd_pd(hv, xv, acc)
DECLICK_AVX2_FN void filterAvx2(const double * h, const double * x, double * out,
                                int count, int taps) {
    int i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    for (; i + 32 <= count; i += 32) {
        const double * xi = x + i;
        __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
        __m256d a2 = _mm256_setzero_pd(), a3 = _mm256_setzero_pd();
        __m256d a4 = _mm256_setzero_pd(), a5 = _mm256_setzero_pd();
        __m256d a6 = _mm256_setzero_pd(), a7 = _mm256_setzero_pd();
        int k = 0;
        for (; k + 16 <= taps; k += 16) {
            for (int r = 0; r < 4; ++r) {
                const __m256d h0 = _mm256_broadcast_sd(h + k + r);
                const __m256d h1 = _mm256_broadcast_sd(h + k + r + 4);
                const __m256d h2 = _mm256_broadcast_sd(h + k + r + 8);
                const __m256d h3 = _mm256_broadcast_sd(h + k + r + 12);
                const double * p = xi + k + r;
                __m256d v;
                v = _mm256_loadu_pd(p);
                DECLICK_FMA(a0, h0, v);
                v = _mm256_loadu_pd(p + 4);
                DECLICK_FMA(a1, h0, v); DECLICK_FMA(a0, h1, v);
                v = _mm256_loadu_pd(p + 8);
                DECLICK_FMA(a2, h0, v); DECLICK_FMA(a1, h1, v); DECLICK_FMA(a0, h2, v);
                v = _mm256_loadu_pd(p + 12);
                DECLICK_FMA(a3, h0, v); DECLICK_FMA(a2, h1, v); DECLICK_FMA(a1, h2, v);
                DECLICK_FMA(a0, h3, v);
                v = _mm256_loadu_pd(p + 16);
                DECLICK_FMA(a4, h0, v); DECLICK_FMA(a3, h1, v); DECLICK_FMA(a2, h2, v);
                DECLICK_FMA(a1, h3, v);
                v = _mm256_loadu_pd(p + 20);
                DECLICK_FMA(a5, h0, v); DECLICK_FMA(a4, h1, v); DECLICK_FMA(a3, h2, v);
                DECLICK_FMA(a2, h3, v);
                v = _mm256_loadu_pd(p + 24);
                DECLICK_FMA(a6, h0, v); DECLICK_FMA(a5, h1, v); DECLICK_FMA(a4, h2, v);
                DECLICK_FMA(a3, h3, v);
                v = _mm256_loadu_pd(p + 28);
                DECLICK_FMA(a7, h0, v); DECLICK_FMA(a6, h1, v); DECLICK_FMA(a5, h2, v);
                DECLICK_FMA(a4, h3, v);
                v = _mm256_loadu_pd(p + 32);
                DECLICK_FMA(a7, h1, v); DECLICK_FMA(a6, h2, v); DECLICK_FMA(a5, h3, v);
                v = _mm256_loadu_pd(p + 36);
                DECLICK_FMA(a7, h2, v); DECLICK_FMA(a6, h3, v);
                v = _mm256_loadu_pd(p + 40);
                DECLICK_FMA(a7, h3, v);
            }
        }
        for (; k < taps; ++k) {
            const __m256d hk = _mm256_broadcast_sd(h + k);
            const double * xk = xi + k;
            a0 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk),      a0);
            a1 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 4),  a1);
            a2 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 8),  a2);
            a3 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 12), a3);
            a4 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 16), a4);
            a5 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 20), a5);
            a6 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 24), a6);
            a7 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 28), a7);
        }
        _mm256_storeu_pd(out + i,      a0);
        _mm256_storeu_pd(out + i + 4,  a1);
        _mm256_storeu_pd(out + i + 8,  a2);
        _mm256_storeu_pd(out + i + 12, a3);
        _mm256_storeu_pd(out + i + 16, a4);
        _mm256_storeu_pd(out + i + 20, a5);
        _mm256_storeu_pd(out + i + 24, a6);
        _mm256_storeu_pd(out + i + 28, a7);
    }
#endif
    for (; i + 16 <= count; i += 16) {
        const double * xi = x + i;
        __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
        __m256d a2 = _mm256_setzero_pd(), a3 = _mm256_setzero_pd();
        int k = 0;
        for (; k + 8 <= taps; k += 8) {
            for (int r = 0; r < 4; ++r) {
                const __m256d h0 = _mm256_broadcast_sd(h + k + r);
                const __m256d h1 = _mm256_broadcast_sd(h + k + r + 4);
                const double * p = xi + k + r;
                __m256d v;
                v = _mm256_loadu_pd(p);
                DECLICK_FMA(a0, h0, v);
                v = _mm256_loadu_pd(p + 4);
                DECLICK_FMA(a1, h0, v); DECLICK_FMA(a0, h1, v);
                v = _mm256_loadu_pd(p + 8);
                DECLICK_FMA(a2, h0, v); DECLICK_FMA(a1, h1, v);
                v = _mm256_loadu_pd(p + 12);
                DECLICK_FMA(a3, h0, v); DECLICK_FMA(a2, h1, v);
                v = _mm256_loadu_pd(p + 16);
                DECLICK_FMA(a3, h1, v);
            }
        }
        for (; k < taps; ++k) {
            const __m256d hk = _mm256_broadcast_sd(h + k);
            const double * xk = xi + k;
            a0 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk),      a0);
            a1 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 4),  a1);
            a2 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 8),  a2);
            a3 = _mm256_fmadd_pd(hk, _mm256_loadu_pd(xk + 12), a3);
        }
        _mm256_storeu_pd(out + i,      a0);
        _mm256_storeu_pd(out + i + 4,  a1);
        _mm256_storeu_pd(out + i + 8,  a2);
        _mm256_storeu_pd(out + i + 12, a3);
    }
    for (; i < count; ++i) _mm_store_sd(out + i, dotAvx2(h, x + i, taps));
    _mm256_zeroupper();
}
#undef DECLICK_FMA

DECLICK_AVX2_FN void autocorrAvx2(const double * x, int n, double * r, int maxLag) {
    // With maxLag zeros after x, r[d] = sum over all n of x[k] * x[k+d].
    filterAvx2(x, x, r, maxLag + 1, n);
}

//! fftPairBase four groups at a time. Multiplies and adds, no FMA, in the
//! scalar version's order, so it gives the same bits; h >= 4, so k runs in
//! whole vectors.
DECLICK_AVX2_FN void fftPairAvx2(double * re, double * im, int n, int h,
                                 const double * stRe, const double * stIm,
                                 double sgn) {
    const __m256d vs = _mm256_set1_pd(sgn);
    const double * w1r = stRe + h - 1;
    const double * w1i = stIm + h - 1;
    const double * w2r = stRe + 2 * h - 1;
    const double * w2i = stIm + 2 * h - 1;
    const double * w3r = w2r + h;
    const double * w3i = w2i + h;
    for (int i = 0; i < n; i += 4 * h) {
        double * r0 = re + i;
        double * r1 = r0 + h;
        double * r2 = r1 + h;
        double * r3 = r2 + h;
        double * i0 = im + i;
        double * i1 = i0 + h;
        double * i2 = i1 + h;
        double * i3 = i2 + h;
        for (int k = 0; k < h; k += 4) {
            const __m256d c1 = _mm256_loadu_pd(w1r + k);
            const __m256d s1 = _mm256_mul_pd(vs, _mm256_loadu_pd(w1i + k));
            __m256d xr = _mm256_loadu_pd(r1 + k);
            __m256d xi = _mm256_loadu_pd(i1 + k);
            __m256d tr = _mm256_sub_pd(_mm256_mul_pd(c1, xr), _mm256_mul_pd(s1, xi));
            __m256d ti = _mm256_add_pd(_mm256_mul_pd(c1, xi), _mm256_mul_pd(s1, xr));
            __m256d ar = _mm256_loadu_pd(r0 + k);
            __m256d ai = _mm256_loadu_pd(i0 + k);
            const __m256d y1r = _mm256_sub_pd(ar, tr), y1i = _mm256_sub_pd(ai, ti);
            const __m256d y0r = _mm256_add_pd(ar, tr), y0i = _mm256_add_pd(ai, ti);
            xr = _mm256_loadu_pd(r3 + k);
            xi = _mm256_loadu_pd(i3 + k);
            tr = _mm256_sub_pd(_mm256_mul_pd(c1, xr), _mm256_mul_pd(s1, xi));
            ti = _mm256_add_pd(_mm256_mul_pd(c1, xi), _mm256_mul_pd(s1, xr));
            ar = _mm256_loadu_pd(r2 + k);
            ai = _mm256_loadu_pd(i2 + k);
            const __m256d y3r = _mm256_sub_pd(ar, tr), y3i = _mm256_sub_pd(ai, ti);
            const __m256d y2r = _mm256_add_pd(ar, tr), y2i = _mm256_add_pd(ai, ti);
            const __m256d c2 = _mm256_loadu_pd(w2r + k);
            const __m256d s2 = _mm256_mul_pd(vs, _mm256_loadu_pd(w2i + k));
            tr = _mm256_sub_pd(_mm256_mul_pd(c2, y2r), _mm256_mul_pd(s2, y2i));
            ti = _mm256_add_pd(_mm256_mul_pd(c2, y2i), _mm256_mul_pd(s2, y2r));
            _mm256_storeu_pd(r2 + k, _mm256_sub_pd(y0r, tr));
            _mm256_storeu_pd(i2 + k, _mm256_sub_pd(y0i, ti));
            _mm256_storeu_pd(r0 + k, _mm256_add_pd(y0r, tr));
            _mm256_storeu_pd(i0 + k, _mm256_add_pd(y0i, ti));
            const __m256d c3 = _mm256_loadu_pd(w3r + k);
            const __m256d s3 = _mm256_mul_pd(vs, _mm256_loadu_pd(w3i + k));
            tr = _mm256_sub_pd(_mm256_mul_pd(c3, y3r), _mm256_mul_pd(s3, y3i));
            ti = _mm256_add_pd(_mm256_mul_pd(c3, y3i), _mm256_mul_pd(s3, y3r));
            _mm256_storeu_pd(r3 + k, _mm256_sub_pd(y1r, tr));
            _mm256_storeu_pd(i3 + k, _mm256_sub_pd(y1i, ti));
            _mm256_storeu_pd(r1 + k, _mm256_add_pd(y1r, tr));
            _mm256_storeu_pd(i1 + k, _mm256_add_pd(y1i, ti));
        }
    }
    _mm256_zeroupper();
}

//! sortedReplace() over a row of windows, four places at a time. The ranks
//! come from the same count, widened; the move is the same predicated pass.
//! A window whose length is not a multiple of four has its last one, two or
//! three places taken by a scalar step either end of the vectors. It used to
//! hand the whole row to sortedRowBase() instead, which left the micro stage
//! - microM is 17 at 44.1 kHz and 18 at 48, see Config::microM - on the
//! scalar kernel at every sample rate.
//!
//! Each window's ranks reach its own move through a vector-to-register-to-
//! vector round trip, and nothing in that window can start until they do: one
//! window at a time, that latency - not the work - was what the row cost, 71
//! cycles a window on a Haswell against the dozen its uops take. So the ranks
//! for a tile of windows are counted first and the moves made after, and each
//! window's latency is covered by the next window's counting.
//! The counts of places below out[t] and below in[t] in each of `count`
//! windows of n places, eight places a step: the ranks sortedRanks() finds by
//! binary search in a window this long, which for a sorted window are the
//! same numbers, NaN aside, as for frameQuantileAvx2(). Branch-free, where the
//! two searches of a 259-place window mispredicted about half of their 18
//! steps.
DECLICK_AVX2_FN void sortedCountAvx2(const double * rows, int n, int stride, int count,
                                     const double * out, const double * in,
                                     int * jo, int * ji) {
    const int nv = n & ~7;
    const int r = n - nv;
    const __m256i k0 = laneMask((r < 4) ? r : 4), k1 = laneMask((r > 4) ? r - 4 : 0);
    for (int t = 0; t < count; ++t) {
        const double * m = rows + (size_t)t * (size_t)stride;
        const __m256d vo = _mm256_broadcast_sd(out + t), vi = _mm256_broadcast_sd(in + t);
        __m256i co0 = _mm256_setzero_si256(), ci0 = co0, co1 = co0, ci1 = co0;
        for (int i = 0; i < nv; i += 8) {
            const __m256d a = _mm256_loadu_pd(m + i), b = _mm256_loadu_pd(m + i + 4);
            co0 = _mm256_sub_epi64(co0, _mm256_castpd_si256(_mm256_cmp_pd(a, vo, _CMP_LT_OQ)));
            ci0 = _mm256_sub_epi64(ci0, _mm256_castpd_si256(_mm256_cmp_pd(a, vi, _CMP_LT_OQ)));
            co1 = _mm256_sub_epi64(co1, _mm256_castpd_si256(_mm256_cmp_pd(b, vo, _CMP_LT_OQ)));
            ci1 = _mm256_sub_epi64(ci1, _mm256_castpd_si256(_mm256_cmp_pd(b, vi, _CMP_LT_OQ)));
        }
        // The last n - nv places, masked; a lane masked off compares 0 < v
        // and is masked again.
        const __m256d a = _mm256_maskload_pd(m + nv, k0), b = _mm256_maskload_pd(m + nv + 4, k1);
        co0 = _mm256_sub_epi64(co0, _mm256_and_si256(k0, _mm256_castpd_si256(_mm256_cmp_pd(a, vo, _CMP_LT_OQ))));
        ci0 = _mm256_sub_epi64(ci0, _mm256_and_si256(k0, _mm256_castpd_si256(_mm256_cmp_pd(a, vi, _CMP_LT_OQ))));
        co1 = _mm256_sub_epi64(co1, _mm256_and_si256(k1, _mm256_castpd_si256(_mm256_cmp_pd(b, vo, _CMP_LT_OQ))));
        ci1 = _mm256_sub_epi64(ci1, _mm256_and_si256(k1, _mm256_castpd_si256(_mm256_cmp_pd(b, vi, _CMP_LT_OQ))));
        const __m256i co = _mm256_add_epi64(co0, co1), ci = _mm256_add_epi64(ci0, ci1);
        __m128i h = _mm_add_epi64(_mm256_castsi256_si128(co), _mm256_extracti128_si256(co, 1));
        jo[t] = _mm_cvtsi128_si32(_mm_add_epi64(h, _mm_unpackhi_epi64(h, h)));
        h = _mm_add_epi64(_mm256_castsi256_si128(ci), _mm256_extracti128_si256(ci, 1));
        ji[t] = _mm_cvtsi128_si32(_mm_add_epi64(h, _mm_unpackhi_epi64(h, h)));
    }
    _mm256_zeroupper();
}

//! sortedRowBase() for windows of kSortedBig places and more - the crackle
//! floor stage's 259 - with the ranks counted by sortedCountAvx2(), a tile of
//! windows at a time, and the moves the base kernel's own memmove. Plain code:
//! the counts end in a vzeroupper, and memmove is SSE. 340 cycles a window of
//! 259 became 270 on a Haswell.
void sortedRowBigAvx2(double * rows, int n, int stride, int count,
                      const double * out, const double * in) {
    enum { kTile = 16 };
    int jo[kTile], ji[kTile];
    for (int b0 = 0; b0 < count; b0 += (int)kTile) {
        const int nt = (count - b0 < (int)kTile) ? count - b0 : (int)kTile;
        sortedCountAvx2(rows + (size_t)b0 * (size_t)stride, n, stride, nt,
                        out + b0, in + b0, jo, ji);
        for (int t = 0; t < nt; ++t) {
            // sortedRanks()'s adjustments to its two searches.
            int a = jo[t], c = ji[t];
            c -= (out[b0 + t] < in[b0 + t]);
            if (a > n - 1) a = n - 1;
            if (c > n - 1) c = n - 1;
            if (c < 0) c = 0;
            sortedMove(rows + (size_t)(b0 + t) * (size_t)stride, n, a, c, in[b0 + t]);
        }
    }
}

DECLICK_AVX2_FN void sortedRowAvx2(double * rows, int n, int stride, int count,
                                   const double * out, const double * in) {
    if (n >= (int)kSortedBig) { sortedRowBigAvx2(rows, n, stride, count, out, in); return; }
    enum { kTile = 16 };
    const int nv = n & ~3;        // the places four at a time
    const int r  = n - nv;        // and the one, two or three left over
    const __m256i lane = _mm256_setr_epi64x(0, 1, 2, 3);
    int jo[kTile], ji[kTile];
    for (int b0 = 0; b0 < count; b0 += (int)kTile) {
        const int nt = (count - b0 < (int)kTile) ? count - b0 : (int)kTile;
        for (int t = 0; t < nt; ++t) {
            const double * m = rows + (size_t)(b0 + t) * (size_t)stride;
            const double vout = out[b0 + t], vin = in[b0 + t];
            const __m256d vo = _mm256_set1_pd(vout);
            const __m256d vi = _mm256_set1_pd(vin);
            __m256i co = _mm256_setzero_si256(), ci = _mm256_setzero_si256();
            for (int i = 0; i < nv; i += 4) {
                const __m256d v = _mm256_loadu_pd(m + i);
                co = _mm256_sub_epi64(co, _mm256_castpd_si256(_mm256_cmp_pd(v, vo, _CMP_LT_OQ)));
                ci = _mm256_sub_epi64(ci, _mm256_castpd_si256(_mm256_cmp_pd(v, vi, _CMP_LT_OQ)));
            }
            __m128i h = _mm_add_epi64(_mm256_castsi256_si128(co), _mm256_extracti128_si256(co, 1));
            int a = _mm_cvtsi128_si32(_mm_add_epi64(h, _mm_unpackhi_epi64(h, h)));
            h = _mm_add_epi64(_mm256_castsi256_si128(ci), _mm256_extracti128_si256(ci, 1));
            int b = _mm_cvtsi128_si32(_mm_add_epi64(h, _mm_unpackhi_epi64(h, h)));
            for (int i = nv; i < n; ++i) {       // the places left over
                a += (m[i] < vout);
                b += (m[i] < vin);
            }
            b -= (vout < vin);
            if (a > n - 1) a = n - 1;
            if (b > n - 1) b = n - 1;
            if (b < 0) b = 0;
            jo[t] = a; ji[t] = b;
        }
        for (int t = 0; t < nt; ++t) {
            double * m = rows + (size_t)(b0 + t) * (size_t)stride;
            const int up = (ji[t] >= jo[t]);
            const int d  = up ? 1 : -1;
            const int lo = up ? jo[t] : ji[t] + 1;
            const int hi = up ? ji[t] - 1 : jo[t];
            const __m256i va  = _mm256_set1_epi64x(lo);
            const __m256i vb1 = _mm256_set1_epi64x(hi + 1);
            // Going with the move - up from the bottom, down from the top -
            // every place is read before the step that writes it. The vectors
            // take nv of them, from place 0 going up and from place r going
            // down; the r the other end are last either way, still with the
            // move, so the rule holds across the two.
            int i = up ? 0 : n - 4;
            for (int c = 0; c < nv; c += 4, i += 4 * d) {
                const __m256i k = _mm256_add_epi64(lane, _mm256_set1_epi64x(i));
                const __m256i act = _mm256_andnot_si256(_mm256_cmpgt_epi64(va, k),
                                                        _mm256_cmpgt_epi64(vb1, k));
                _mm256_storeu_pd(m + i, _mm256_blendv_pd(_mm256_loadu_pd(m + i),
                                                         _mm256_loadu_pd(m + i + d),
                                                         _mm256_castsi256_pd(act)));
            }
            for (int c = 0, j = up ? nv : r - 1; c < r; ++c, j += d)
                m[j] = (j >= lo && j <= hi) ? m[j + d] : m[j];
            _mm_store_sd(m + ji[t], _mm_load_sd(in + b0 + t));
        }
    }
    _mm256_zeroupper();
}

//! fftPairAvx2()'s butterfly, one stage of it. Multiply and add, never FMA:
//! the result has to be the scalar kernel's to the bit.
DECLICK_AVX2_FN void fftOneAvx2(double * re, double * im, int n, int h,
                                const double * stRe, const double * stIm, double sgn) {
    if ((h & 3) != 0) { fftOneBase(re, im, n, h, stRe, stIm, sgn); return; }
    const __m256d vs = _mm256_set1_pd(sgn);
    const double * wr = stRe + h - 1;
    const double * ws = stIm + h - 1;
    for (int i = 0; i < n; i += 2 * h) {
        double * ar = re + i;
        double * ai = im + i;
        double * br = ar + h;
        double * bi = ai + h;
        for (int k = 0; k < h; k += 4) {
            const __m256d c = _mm256_loadu_pd(wr + k);
            const __m256d sn = _mm256_mul_pd(vs, _mm256_loadu_pd(ws + k));
            const __m256d xr = _mm256_loadu_pd(br + k);
            const __m256d xi = _mm256_loadu_pd(bi + k);
            const __m256d tr = _mm256_sub_pd(_mm256_mul_pd(c, xr), _mm256_mul_pd(sn, xi));
            const __m256d ti = _mm256_add_pd(_mm256_mul_pd(c, xi), _mm256_mul_pd(sn, xr));
            const __m256d yr = _mm256_loadu_pd(ar + k);
            const __m256d yi = _mm256_loadu_pd(ai + k);
            _mm256_storeu_pd(br + k, _mm256_sub_pd(yr, tr));
            _mm256_storeu_pd(bi + k, _mm256_sub_pd(yi, ti));
            _mm256_storeu_pd(ar + k, _mm256_add_pd(yr, tr));
            _mm256_storeu_pd(ai + k, _mm256_add_pd(yi, ti));
        }
    }
    _mm256_zeroupper();
}

//! What is left of dot() once its eight-at-a-time loop is done, from its four
//! partial sums: the pairs left over go to s0, and the sums close as dot()
//! closes them. Multiply and add, never FMA, as dot() has it.
DECLICK_AVX2_FN inline __m128d dotFinish(__m128d s0, __m128d s1, __m128d s2, __m128d s3,
                                         const double * x, const double * y, int i, int n) {
    for (; i + 2 <= n; i += 2)
        s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(x + i), _mm_loadu_pd(y + i)));
    const __m128d t = _mm_add_pd(_mm_add_pd(s0, s1), _mm_add_pd(s2, s3));
    __m128d acc = _mm_add_sd(t, _mm_unpackhi_pd(t, t));
    if (i < n) acc = _mm_add_sd(acc, _mm_mul_sd(_mm_load_sd(x + i), _mm_load_sd(y + i)));
    return acc;
}

//! Two dot()s at once, each to the bit. dot()'s four two-lane partial sums are
//! two four-lane ones here, s0 and s1 side by side and s2 and s3, which take
//! the same products in the same order: half the adds. dot() is bound by
//! Haswell's one port for adds; two four-lane sums alone would wait on their
//! own adds instead, and two dots at once keep four going, so the port does
//! twice dot()'s work. For solveSpan(), whose right-hand side is two dot()s a
//! sample - 1290 cycles a solve became 930 on a Haswell - and the voice
//! guard's lags.
DECLICK_AVX2_FN void dot2Avx2(const double * x0, const double * y0, int n0,
                              const double * x1, const double * y1, int n1,
                              double * r0, double * r1) {
    __m256d a0 = _mm256_setzero_pd(), b0 = _mm256_setzero_pd();
    __m256d a1 = _mm256_setzero_pd(), b1 = _mm256_setzero_pd();
    // MSVC workaround (VS 2017 15.9, cl 19.16.27051): counted loops, each
    // with one exit test. The natural `for (; i + 8 <= n0 && i + 8 <= n1;)`
    // came out with the sums kept in memory and a vzeroupper inside the loop.
    // A later compiler may not need this: if that form disassembles with the
    // sums in ymm registers and no vzeroupper in the loop, it can come back.
    const int e0 = (n0 > 0) ? (n0 & ~7) : 0;
    const int e1 = (n1 > 0) ? (n1 & ~7) : 0;
    const int ec = (e0 < e1) ? e0 : e1;
    for (int i = 0; i < ec; i += 8) {
        a0 = _mm256_add_pd(a0, _mm256_mul_pd(_mm256_loadu_pd(x0 + i), _mm256_loadu_pd(y0 + i)));
        a1 = _mm256_add_pd(a1, _mm256_mul_pd(_mm256_loadu_pd(x1 + i), _mm256_loadu_pd(y1 + i)));
        b0 = _mm256_add_pd(b0, _mm256_mul_pd(_mm256_loadu_pd(x0 + i + 4), _mm256_loadu_pd(y0 + i + 4)));
        b1 = _mm256_add_pd(b1, _mm256_mul_pd(_mm256_loadu_pd(x1 + i + 4), _mm256_loadu_pd(y1 + i + 4)));
    }
    for (int i = ec; i < e0; i += 8) {
        a0 = _mm256_add_pd(a0, _mm256_mul_pd(_mm256_loadu_pd(x0 + i), _mm256_loadu_pd(y0 + i)));
        b0 = _mm256_add_pd(b0, _mm256_mul_pd(_mm256_loadu_pd(x0 + i + 4), _mm256_loadu_pd(y0 + i + 4)));
    }
    for (int i = ec; i < e1; i += 8) {
        a1 = _mm256_add_pd(a1, _mm256_mul_pd(_mm256_loadu_pd(x1 + i), _mm256_loadu_pd(y1 + i)));
        b1 = _mm256_add_pd(b1, _mm256_mul_pd(_mm256_loadu_pd(x1 + i + 4), _mm256_loadu_pd(y1 + i + 4)));
    }
    // MSVC workaround (VS 2017 15.9, cl 19.16.27051): the sums to memory and
    // the upper halves cleared here, before the close, which is all halves.
    // Taking the halves straight from a0 .. b1, with _mm256_zeroupper() at the
    // end, MSVC moved its vzeroupper ahead of the close, then read the sums
    // back whole from where it had spilled them, and returned to the SSE2
    // caller with the upper halves dirty: Haswell's ~70-cycle state change,
    // twice a call. This store took the right-hand side above from 1760
    // cycles a solve to 960. A later compiler may not need it: if the plain
    // form disassembles with no ymm register used after the last vzeroupper
    // before each ret, the halves can come from the registers.
    double sums[16];
    _mm256_storeu_pd(sums,      a0);
    _mm256_storeu_pd(sums + 4,  b0);
    _mm256_storeu_pd(sums + 8,  a1);
    _mm256_storeu_pd(sums + 12, b1);
    _mm256_zeroupper();
    // dot() of nothing is 0.0, where the close would give 0.0 + 0.0.
    _mm_store_sd(r0, (n0 > 0) ? dotFinish(_mm_loadu_pd(sums), _mm_loadu_pd(sums + 2),
                                          _mm_loadu_pd(sums + 4), _mm_loadu_pd(sums + 6),
                                          x0, y0, e0, n0) : _mm_setzero_pd());
    _mm_store_sd(r1, (n1 > 0) ? dotFinish(_mm_loadu_pd(sums + 8), _mm_loadu_pd(sums + 10),
                                          _mm_loadu_pd(sums + 12), _mm_loadu_pd(sums + 14),
                                          x1, y1, e1, n1) : _mm_setzero_pd());
}

//! flankAddBase() turned round: sixteen outputs at a time held in registers,
//! every sample whose band reaches them added in, then stored. The base loop
//! comes out scalar from MSVC 19.16, which cannot tell acc from the taps, and
//! does each output's add again for every sample of the span, on Haswell's one
//! port for adds; here a sample's taps go into four outputs an add. Each
//! output still takes its terms one at a time in sample order, a multiply then
//! an add, never FMA, so the sums are the base loop's to the bit. 1870 cycles
//! a span became 1190 on a Haswell.
//!
//! A lane whose output the sample's band does not reach reads a pad of the
//! taps, and its product has to change nothing. An added -0 is the one value
//! that changes nothing whatever it is added to, +0 and -0 included, so the
//! pads are zeros signed to make that product: -0 for a positive d, +0 for a
//! negative. Outputs past xEnd are neither read nor written.
//!
//! The tests of d[j] are intrinsics rather than `d[j] == 0.0` and `d[j] < 0`
//! for the reason given above the AVX2 kernels: MSVC encodes plain double code
//! here as legacy SSE.
DECLICK_AVX2_FN void flankAddAvx2(double * acc, const double * hpP, const double * hpN,
                                  int Hh, const double * d, int t0, int m, int xEnd) {
    const int x0 = t0 - Hh;
    const int x1 = (t0 + m - 1 + Hh < xEnd) ? (t0 + m - 1 + Hh) : xEnd;
    const __m128d zero = _mm_setzero_pd();
    for (int xb = x0; xb <= x1; xb += 16) {
        // The outputs in range, by vector: all of them but in the last block.
        const int left = x1 - xb + 1;
        const __m256i k0 = laneMask((left > 4) ? 4 : left);
        const __m256i k1 = laneMask((left > 8) ? 4 : (left > 4 ? left - 4 : 0));
        const __m256i k2 = laneMask((left > 12) ? 4 : (left > 8 ? left - 8 : 0));
        const __m256i k3 = laneMask((left > 16) ? 4 : (left > 12 ? left - 12 : 0));
        double * o = acc + xb;
        __m256d a0 = _mm256_maskload_pd(o,      k0);
        __m256d a1 = _mm256_maskload_pd(o + 4,  k1);
        __m256d a2 = _mm256_maskload_pd(o + 8,  k2);
        __m256d a3 = _mm256_maskload_pd(o + 12, k3);
        // The samples whose band reaches the block: t - Hh <= xb + 15, t + Hh >= xb.
        const int jlo = (xb - Hh - t0 > 0) ? (xb - Hh - t0) : 0;
        const int jhi = (xb + 15 + Hh - t0 < m - 1) ? (xb + 15 + Hh - t0) : (m - 1);
        for (int j = jlo; j <= jhi; ++j) {
            const __m128d dj = _mm_load_sd(d + j);
            if (_mm_movemask_pd(_mm_cmpeq_sd(dj, zero)) & 1) continue;   // d[j] == 0.0
            const double * hp = ((_mm_movemask_pd(dj) & 1) ? hpN : hpP) + (xb - t0 - j);
            const __m256d dv = _mm256_broadcast_sd(d + j);
            a0 = _mm256_add_pd(a0, _mm256_mul_pd(_mm256_loadu_pd(hp),      dv));
            a1 = _mm256_add_pd(a1, _mm256_mul_pd(_mm256_loadu_pd(hp + 4),  dv));
            a2 = _mm256_add_pd(a2, _mm256_mul_pd(_mm256_loadu_pd(hp + 8),  dv));
            a3 = _mm256_add_pd(a3, _mm256_mul_pd(_mm256_loadu_pd(hp + 12), dv));
        }
        _mm256_maskstore_pd(o,      k0, a0);
        _mm256_maskstore_pd(o + 4,  k1, a1);
        _mm256_maskstore_pd(o + 8,  k2, a2);
        _mm256_maskstore_pd(o + 12, k3, a3);
    }
    _mm256_zeroupper();
}

//! The fine and micro stages' quantile without their sorted windows: for four
//! bins at a time, the L + 1 + I smallest of the n frames' values kept in
//! order, each frame's value passed down them by a min and a max a place, and
//! the quantile read off the last one or two. A quantile is an order
//! statistic, and this finds the same values the windows hold, so the result
//! is theirs to the bit: the interpolation is the same multiply and add, never
//! FMA. NaN aside, which min and max would place differently - and the
//! magnitudes are roots of sums of squares of finite values.
//!
//! Keeping the windows sorted costs a count and a predicated move per bin per
//! hop, ~45 cycles a window of 16 on a Haswell however it is written: two
//! rewrites of sortedRowAvx2(), one of them taking four windows at a time
//! transposed, neither saved more than 15%. This is 2 (L + 1 + I) - 1 mins
//! and maxes a frame for four bins: the fine stage's 53 bins went from ~2800
//! cycles a hop to ~1600, the micro stage's 11 from ~850 to ~600. Infinity
//! seeds the places, and lanes past nb read nothing and are not written.
template <int L, bool I>
DECLICK_AVX2_FN void frameSelectAvx2(const double * const * frames, int n, int nb,
                                     double frac, double * q) {
    enum { K = L + 1 + (I ? 1 : 0) };
    const __m256d inf = _mm256_set1_pd(HUGE_VAL);
    const __m256d fr = _mm256_set1_pd(frac);
    for (int b = 0; b < nb; b += 4) {
        const __m256i m = laneMask((nb - b < 4) ? nb - b : 4);
        __m256d s[K];
        for (int i = 0; i < (int)K; ++i) s[i] = inf;
        for (int j = 0; j < n; ++j) {
            __m256d v = _mm256_maskload_pd(frames[j] + b, m);
            for (int i = 0; i < (int)K; ++i) {
                const __m256d lo = _mm256_min_pd(s[i], v);
                v = _mm256_max_pd(s[i], v);
                s[i] = lo;
            }
        }
        __m256d r = s[L];
        if (I) r = _mm256_add_pd(r, _mm256_mul_pd(fr, _mm256_sub_pd(s[L + (I ? 1 : 0)], r)));
        _mm256_maskstore_pd(q + b, m, r);
    }
}

DECLICK_AVX2_FN bool frameQuantileAvx2(const double * const * frames, int n, int nb,
                                       const QuantilePos & p, double * q) {
    switch (p.lo * 2 + (p.interpolate ? 1 : 0)) {
    case 0:  frameSelectAvx2<0, false>(frames, n, nb, p.frac, q); break;
    case 1:  frameSelectAvx2<0, true >(frames, n, nb, p.frac, q); break;
    case 2:  frameSelectAvx2<1, false>(frames, n, nb, p.frac, q); break;
    case 3:  frameSelectAvx2<1, true >(frames, n, nb, p.frac, q); break;
    case 4:  frameSelectAvx2<2, false>(frames, n, nb, p.frac, q); break;
    case 5:  frameSelectAvx2<2, true >(frames, n, nb, p.frac, q); break;
    case 6:  frameSelectAvx2<3, false>(frames, n, nb, p.frac, q); break;
    case 7:  frameSelectAvx2<3, true >(frames, n, nb, p.frac, q); break;
    case 8:  frameSelectAvx2<4, false>(frames, n, nb, p.frac, q); break;
    case 9:  frameSelectAvx2<4, true >(frames, n, nb, p.frac, q); break;
    case 10: frameSelectAvx2<5, false>(frames, n, nb, p.frac, q); break;
    case 11: frameSelectAvx2<5, true >(frames, n, nb, p.frac, q); break;
    default: return false;    // further up: the sorted windows, not more of these
    }
    _mm256_zeroupper();
    return true;
}

//! One group of binUnpackAvx2(): X and |X| for the four bins from zr, zi, cr,
//! cm - Z at the bins and at their mirrors - and the twiddles c and sn.
#define DECLICK_UNPACK4(vr, vi, mv)                                                      \
    const __m256d ci = _mm256_xor_pd(cm, sign);                       /* -im[mk] */     \
    const __m256d er = _mm256_mul_pd(half, _mm256_add_pd(zr, cr));                      \
    const __m256d ei = _mm256_mul_pd(half, _mm256_add_pd(zi, ci));                      \
    const __m256d our = _mm256_mul_pd(half, _mm256_sub_pd(zi, ci));                     \
    const __m256d oui = _mm256_mul_pd(mhalf, _mm256_sub_pd(zr, cr));                    \
    const __m256d vr = _mm256_add_pd(_mm256_add_pd(er, _mm256_mul_pd(c, our)),          \
                                     _mm256_mul_pd(sn, oui));                           \
    const __m256d vi = _mm256_sub_pd(_mm256_add_pd(ei, _mm256_mul_pd(c, oui)),          \
                                     _mm256_mul_pd(sn, our));                           \
    const __m256d mv = _mm256_sqrt_pd(_mm256_add_pd(_mm256_mul_pd(vr, vr),              \
                                                    _mm256_mul_pd(vi, vi)))

//! The bins b .. b + 3 by gathers, lanes past nb neither read nor written.
#define DECLICK_UNPACK4_GATHER(b)                                                        \
    {                                                                                    \
        const int left_ = nb - (b);                                                      \
        const __m256i m = laneMask((left_ < 4) ? left_ : 4);                             \
        const __m256d md = _mm256_castsi256_pd(m);                                       \
        const __m256i kv = _mm256_and_si256(_mm256_add_epi64(_mm256_set1_epi64x(k0 + (b)), lane), wrap); \
        const __m256i mk = _mm256_and_si256(_mm256_sub_epi64(_mm256_set1_epi64x(M), kv), wrap);         \
        const __m256d zr = _mm256_mask_i64gather_pd(zero, re, kv, md, 8);                \
        const __m256d zi = _mm256_mask_i64gather_pd(zero, im, kv, md, 8);                \
        const __m256d cr = _mm256_mask_i64gather_pd(zero, re, mk, md, 8);                \
        const __m256d cm = _mm256_mask_i64gather_pd(zero, im, mk, md, 8);                \
        const __m256d c = _mm256_maskload_pd(twRe + k0 + (b), m);                        \
        const __m256d sn = _mm256_maskload_pd(twIm + k0 + (b), m);                       \
        DECLICK_UNPACK4(vr, vi, mv);                                                     \
        _mm256_maskstore_pd(xr + (b), m, vr);                                            \
        _mm256_maskstore_pd(xi + (b), m, vi);                                            \
        _mm256_maskstore_pd(mg + (b), m, mv);                                            \
    }

//! binUnpackBase() four bins at a time: the same multiplies, adds and square
//! root, never FMA, in the same order, so the same bits. A square root is
//! ~9.7 cycles of the divider apiece as sqrtsd on a Haswell, and vsqrtpd takes
//! four in ~11; the fine stage's unpack went from ~1310 cycles a hop to ~620.
//! Bins k .. k + 3 read Z at kk .. kk + 3 and their mirrors at M - kk ..
//! M - kk - 3, one load and a reversal each, wherever the four stay inside
//! 1 .. M - 1. The group with the DC or the Nyquist bin, which fold onto
//! Z[0], gathers instead, as does the last one when nb is not a multiple of
//! four; those can only be the first group and the last ones.
//!
//! MSVC workaround (VS 2017 15.9, cl 19.16.27051): three loops, each body
//! straight-line, and the gathers' indices in 256-bit integers. One loop that
//! chose between the loads and between the stores by `if` came out with every
//! vector kept in memory and three vzeroupper inside the loop, and the
//! indices' 128-bit integer arithmetic as legacy SSE with the upper halves
//! dirty. A later compiler may manage the one loop: if it disassembles with
//! the vectors in registers and no vzeroupper or non-VEX instruction inside
//! it, it can come back.
DECLICK_AVX2_FN void binUnpackAvx2(const double * re, const double * im, int M, int k0, int nb,
                                   const double * twRe, const double * twIm,
                                   double * xr, double * xi, double * mg) {
    const __m256d half = _mm256_set1_pd(0.5), mhalf = _mm256_set1_pd(-0.5);
    const __m256d sign = _mm256_set1_pd(-0.0);
    const __m256d zero = _mm256_setzero_pd();
    const __m256i lane = _mm256_setr_epi64x(0, 1, 2, 3);
    const __m256i wrap = _mm256_set1_epi64x(M - 1);
    // Groups at b = 4j are loaded whole from bA up to bB: k0 + b >= 1,
    // b + 4 <= nb, and k0 + b + 3 <= M - 1.
    const int bA = (k0 >= 1) ? 0 : 4;
    const int last = (nb - 4 < M - 4 - k0) ? nb - 4 : M - 4 - k0;   // the last b loaded whole
    const int bB = (last >= bA) ? bA + ((last - bA) / 4 + 1) * 4 : bA;
    for (int b = 0; b < bA && b < nb; b += 4) DECLICK_UNPACK4_GATHER(b)
    for (int b = bA; b < bB; b += 4) {
        const int kk = k0 + b;
        const __m256d zr = _mm256_loadu_pd(re + kk);
        const __m256d zi = _mm256_loadu_pd(im + kk);
        // Lanes 0 .. 3 want M - kk .. M - kk - 3: the four from M - kk - 3, reversed.
        const __m256d cr = _mm256_permute4x64_pd(_mm256_loadu_pd(re + (M - kk - 3)), 0x1b);
        const __m256d cm = _mm256_permute4x64_pd(_mm256_loadu_pd(im + (M - kk - 3)), 0x1b);
        const __m256d c = _mm256_loadu_pd(twRe + kk);
        const __m256d sn = _mm256_loadu_pd(twIm + kk);
        DECLICK_UNPACK4(vr, vi, mv);
        _mm256_storeu_pd(xr + b, vr);
        _mm256_storeu_pd(xi + b, vi);
        _mm256_storeu_pd(mg + b, mv);
    }
    for (int b = (bB > bA) ? bB : bA; b < nb; b += 4) DECLICK_UNPACK4_GATHER(b)
    _mm256_zeroupper();
}
#undef DECLICK_UNPACK4_GATHER
#undef DECLICK_UNPACK4

//! smooth7Base()'s sum for the four bins from b by gathers, each index held as
//! the base kernel holds it, lanes past b1 neither read nor written.
#define DECLICK_SMOOTH7_GATHER(b)                                                        \
    {                                                                                    \
        const int left_ = b1 - (b);                                                      \
        const __m256i m = laneMask((left_ < 4) ? left_ : 4);                             \
        const __m256d md = _mm256_castsi256_pd(m);                                       \
        __m256d s = _mm256_setzero_pd();                                                 \
        for (int j = -3; j <= 3; ++j) {                                                  \
            __m256i kb = _mm256_add_epi32(_mm256_set1_epi32((b) + j), lane);             \
            kb = _mm256_max_epi32(kb, lo);                /* if (kb + m0 < 0) kb = -m0 */ \
            kb = _mm256_min_epi32(kb, hi);                /* if (kb > nb - 1) kb = nb - 1 */ \
            s = _mm256_add_pd(s, _mm256_mask_i32gather_pd(_mm256_setzero_pd(), P,        \
                                                          _mm256_castsi256_si128(kb), md, 8)); \
        }                                                                                \
        _mm256_maskstore_pd(Pb + (b), m, s);                                             \
    }

//! smooth7Base() four bins at a time, each the same seven adds from 0.0 in the
//! same order. The bins whose seven are all inside -m0 .. nb - 1 take them as
//! seven loads; the groups either side of those, and the last when the count
//! is not a multiple of four, gather them. The indices are 32-bit lanes of a
//! 256-bit vector - MSVC 19.16 encodes the 128-bit integer intrinsics as
//! legacy SSE here; see binUnpackAvx2() - and the loops are three, each
//! straight-line, for the same reason.
DECLICK_AVX2_FN void smooth7Avx2(const double * P, int nb, int m0, int b0, int b1, double * Pb) {
    if (b1 <= b0) return;
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 0, 0, 0, 0);
    const __m256i lo = _mm256_set1_epi32(-m0), hi = _mm256_set1_epi32(nb - 1);
    // Groups at b0 + 4j take loads from iA up to iB: b - 3 >= -m0 and
    // b + 3 + 3 <= nb - 1, and the whole group before b1.
    const int need = 3 - m0 - b0;
    const int iA = b0 + ((need > 0) ? (need + 3) / 4 * 4 : 0);
    const int last = (b1 - 4 < nb - 7) ? b1 - 4 : nb - 7;
    const int iB = (last >= iA) ? iA + ((last - iA) / 4 + 1) * 4 : iA;
    int b = b0;
    for (; b < iA && b < b1; b += 4) DECLICK_SMOOTH7_GATHER(b)
    for (b = iA; b < iB; b += 4) {
        const double * q = P + b;
        __m256d s = _mm256_setzero_pd();
        s = _mm256_add_pd(s, _mm256_loadu_pd(q - 3));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q - 2));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q - 1));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q + 1));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q + 2));
        s = _mm256_add_pd(s, _mm256_loadu_pd(q + 3));
        _mm256_storeu_pd(Pb + b, s);
    }
    for (b = (iB > iA) ? iB : iA; b < b1; b += 4) DECLICK_SMOOTH7_GATHER(b)
    _mm256_zeroupper();
}
#undef DECLICK_SMOOTH7_GATHER

//! floorGainBase() four bins at a time: the same three divisions, four to a
//! vdivpd, and the same multiplies and adds, never FMA. dmin(a, b) is
//! a < b ? a : b and dmax(a, b) a > b ? a : b, which is what minpd and maxpd
//! do with their operands in that order, NaN and signed zeros included. The
//! flags are masks selected as integers - MSVC 19.16 made a ?: between two
//! vectors a branch with a vzeroupper on one side - and the saved and warm
//! floors are read only under them: a mask of nothing reads nothing, and with
//! the flag down the base kernel never reads them, nor may this.
DECLICK_AVX2_FN void floorGainAvx2(const FloorGain & p, int b0, int b1, double * G) {
    const __m256d one = _mm256_set1_pd(1.0), zero = _mm256_setzero_pd();
    const __m256d k35 = _mm256_set1_pd(35.0), tiny = _mm256_set1_pd(1e-30);
    const __m256d beta = _mm256_set1_pd(p.beta), gmin = _mm256_set1_pd(p.gmin);
    const __m256d a = _mm256_set1_pd(p.a);
    const __m256d ra = _mm256_sub_pd(one, a);                   // 1.0 - a
    const __m256i kept = _mm256_set1_epi64x(p.kept ? -1 : 0);
    const __m256i capped = _mm256_set1_epi64x(p.capped ? -1 : 0);
    const __m256d first = _mm256_castsi256_pd(_mm256_set1_epi64x(p.first ? -1 : 0));
    for (int b = b0; b < b1; b += 4) {
        const __m256i m = laneMask((b1 - b < 4) ? b1 - b : 4);
        __m256d s = _mm256_add_pd(_mm256_maskload_pd(p.B[0] + b, m), _mm256_maskload_pd(p.B[1] + b, m));
        s = _mm256_add_pd(s, _mm256_maskload_pd(p.B[2] + b, m));
        s = _mm256_add_pd(s, _mm256_maskload_pd(p.B[3] + b, m));
        s = _mm256_add_pd(s, _mm256_maskload_pd(p.B[4] + b, m));
        s = _mm256_div_pd(s, k35);
        const __m256i mk = _mm256_and_si256(m, kept), mc = _mm256_and_si256(m, capped);
        __m256d f = _mm256_maskload_pd(p.Fl + b, m);
        f = _mm256_blendv_pd(f, _mm256_min_pd(f, _mm256_maskload_pd(p.Sv + b, mk)),
                             _mm256_castsi256_pd(mk));
        f = _mm256_blendv_pd(f, _mm256_min_pd(f, _mm256_maskload_pd(p.Wm + b, mc)),
                             _mm256_castsi256_pd(mc));
        const __m256d nf = _mm256_add_pd(_mm256_mul_pd(f, beta), tiny);
        const __m256d x = _mm256_max_pd(_mm256_sub_pd(_mm256_div_pd(s, nf), one), zero);
        const __m256d g0v = _mm256_max_pd(_mm256_div_pd(x, _mm256_add_pd(one, x)), gmin);
        const __m256d g = _mm256_maskload_pd(G + b, m);
        const __m256d gr = _mm256_max_pd(g0v, _mm256_add_pd(_mm256_mul_pd(a, g),
                                                            _mm256_mul_pd(ra, g0v)));
        _mm256_maskstore_pd(G + b, m, _mm256_blendv_pd(gr, g0v, first));
    }
    _mm256_zeroupper();
}

const Kernels kAvx2Kernels = { filterAvx2, autocorrAvx2, fftPairAvx2, fftOneAvx2,
                               sortedRowAvx2, dot2Avx2, flankAddAvx2, frameQuantileAvx2,
                               binUnpackAvx2, smooth7Avx2, floorGainAvx2, "avx2" };

//! AVX2 and FMA in the CPU, and YMM state saved by the OS.
bool cpuHasAvx2Fma() {
    unsigned ecx1, ebx7;
#if defined(__GNUC__) || defined(__clang__)
    unsigned a, b, c, d;
    if (__get_cpuid_max(0, 0) < 7) return false;
    __cpuid(1, a, b, c, d);
    ecx1 = c;
    __cpuid_count(7, 0, a, b, c, d);
    ebx7 = b;
#else
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    ecx1 = (unsigned)r[2];
    __cpuidex(r, 7, 0);
    ebx7 = (unsigned)r[1];
#endif
    // FMA is 1.ECX[12], AVX 1.ECX[28], AVX2 7.EBX[5]. OSXSAVE, 1.ECX[27], has
    // to be checked as well before XGETBV, which faults without it.
    const unsigned need1 = (1u << 12) | (1u << 27) | (1u << 28);
    if ((ecx1 & need1) != need1 || !(ebx7 & (1u << 5))) return false;

    // XCR0 bits 1 and 2: the OS saves the SSE and the upper YMM state on a
    // context switch. Without that the CPU has AVX2 but cannot use it.
#if defined(__GNUC__) || defined(__clang__)
    unsigned lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    const unsigned long long xcr0 = ((unsigned long long)hi << 32) | lo;
#else
    const unsigned long long xcr0 = _xgetbv(0);
#endif
    return (xcr0 & 6u) == 6u;
}

#endif // DECLICK_AVX2

#if DECLICK_NEON

// The same filter as filterAvx2(), two doubles per register instead of four.
// AArch64 has 32 vector registers, so there is room for sixteen accumulators,
// which is what an Apple core needs: four FMA pipes at four cycles' latency.
// Each multiply-add needs one load of x. The taps come in two at a time and
// each is used from its lane with fmla-by-element, so one load of h serves
// thirty-two multiply-adds. There is no AVX-SSE style transition penalty
// here, so the leftovers are ordinary code.
//
// The one load of x per multiply-add stays, both ways of cutting it having
// been measured on an M1 over a whole song. The odd tap wants the pair of x a
// place along, which is the top half of the even tap's vector over the bottom
// half of the next one: vextq_f64 of two vectors already in hand, seventeen
// loads for the sixteen accumulators rather than thirty-two. But an extract
// goes to the same pipes as a multiply-add, and fifteen of them cost 1.3% more
// than the loads they save. Pairing the even loads with vld1q_f64_x2 instead,
// which asks nothing of those pipes and brings the loop's thirty-three load
// instructions down to twenty-five, came out level - the loop is not short of
// load slots. What it is short of is chains: sixteen accumulators are sixteen
// dependent chains against four pipes at four cycles' latency, just enough to
// fill them and not one more, and a seventeenth would have to come either from
// a register that is not there or from summing the taps in another order,
// which the output is entitled to to the bit.

//! dot() with FMA, two lanes per register.
inline double dotNeon(const double * x, const double * y, int n) {
    float64x2_t s0 = vdupq_n_f64(0.0), s1 = vdupq_n_f64(0.0);
    float64x2_t s2 = vdupq_n_f64(0.0), s3 = vdupq_n_f64(0.0);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = vfmaq_f64(s0, vld1q_f64(x + i),     vld1q_f64(y + i));
        s1 = vfmaq_f64(s1, vld1q_f64(x + i + 2), vld1q_f64(y + i + 2));
        s2 = vfmaq_f64(s2, vld1q_f64(x + i + 4), vld1q_f64(y + i + 4));
        s3 = vfmaq_f64(s3, vld1q_f64(x + i + 6), vld1q_f64(y + i + 6));
    }
    for (; i + 2 <= n; i += 2) {
        s0 = vfmaq_f64(s0, vld1q_f64(x + i), vld1q_f64(y + i));
    }
    double acc = vaddvq_f64(vaddq_f64(vaddq_f64(s0, s1), vaddq_f64(s2, s3)));
    if (i < n) acc += x[i] * y[i];
    return acc;
}

//! acc += lane LANE of hp times the two x at p. One tap of the filter for one
//! accumulator; LANE has to be a constant, hence a macro.
#define DECLICK_NEON_TAP(acc, p, hp, LANE) \
    acc = vfmaq_laneq_f64(acc, vld1q_f64(p), hp, LANE)

void filterNeon(const double * h, const double * x, double * out,
                int count, int taps) {
    int i = 0;
    for (; i + 32 <= count; i += 32) {
        // Spelled out rather than an array, which a compiler may leave in
        // memory.
        const double * xi = x + i;
        float64x2_t a0 = vdupq_n_f64(0.0), a1 = vdupq_n_f64(0.0);
        float64x2_t a2 = vdupq_n_f64(0.0), a3 = vdupq_n_f64(0.0);
        float64x2_t a4 = vdupq_n_f64(0.0), a5 = vdupq_n_f64(0.0);
        float64x2_t a6 = vdupq_n_f64(0.0), a7 = vdupq_n_f64(0.0);
        float64x2_t a8 = vdupq_n_f64(0.0), a9 = vdupq_n_f64(0.0);
        float64x2_t aa = vdupq_n_f64(0.0), ab = vdupq_n_f64(0.0);
        float64x2_t ac = vdupq_n_f64(0.0), ad = vdupq_n_f64(0.0);
        float64x2_t ae = vdupq_n_f64(0.0), af = vdupq_n_f64(0.0);
#define DECLICK_NEON_TAP16(xk, hp, LANE)              \
        DECLICK_NEON_TAP(a0, (xk),      hp, LANE);    \
        DECLICK_NEON_TAP(a1, (xk) + 2,  hp, LANE);    \
        DECLICK_NEON_TAP(a2, (xk) + 4,  hp, LANE);    \
        DECLICK_NEON_TAP(a3, (xk) + 6,  hp, LANE);    \
        DECLICK_NEON_TAP(a4, (xk) + 8,  hp, LANE);    \
        DECLICK_NEON_TAP(a5, (xk) + 10, hp, LANE);    \
        DECLICK_NEON_TAP(a6, (xk) + 12, hp, LANE);    \
        DECLICK_NEON_TAP(a7, (xk) + 14, hp, LANE);    \
        DECLICK_NEON_TAP(a8, (xk) + 16, hp, LANE);    \
        DECLICK_NEON_TAP(a9, (xk) + 18, hp, LANE);    \
        DECLICK_NEON_TAP(aa, (xk) + 20, hp, LANE);    \
        DECLICK_NEON_TAP(ab, (xk) + 22, hp, LANE);    \
        DECLICK_NEON_TAP(ac, (xk) + 24, hp, LANE);    \
        DECLICK_NEON_TAP(ad, (xk) + 26, hp, LANE);    \
        DECLICK_NEON_TAP(ae, (xk) + 28, hp, LANE);    \
        DECLICK_NEON_TAP(af, (xk) + 30, hp, LANE)
        int k = 0;
        for (; k + 2 <= taps; k += 2) {
            const float64x2_t hp = vld1q_f64(h + k);
            DECLICK_NEON_TAP16(xi + k,     hp, 0);
            DECLICK_NEON_TAP16(xi + k + 1, hp, 1);
        }
        if (k < taps) {
            const float64x2_t hp = vdupq_n_f64(h[k]);
            DECLICK_NEON_TAP16(xi + k, hp, 0);
        }
#undef DECLICK_NEON_TAP16
        vst1q_f64(out + i,      a0);
        vst1q_f64(out + i + 2,  a1);
        vst1q_f64(out + i + 4,  a2);
        vst1q_f64(out + i + 6,  a3);
        vst1q_f64(out + i + 8,  a4);
        vst1q_f64(out + i + 10, a5);
        vst1q_f64(out + i + 12, a6);
        vst1q_f64(out + i + 14, a7);
        vst1q_f64(out + i + 16, a8);
        vst1q_f64(out + i + 18, a9);
        vst1q_f64(out + i + 20, aa);
        vst1q_f64(out + i + 22, ab);
        vst1q_f64(out + i + 24, ac);
        vst1q_f64(out + i + 26, ad);
        vst1q_f64(out + i + 28, ae);
        vst1q_f64(out + i + 30, af);
    }
    for (; i + 8 <= count; i += 8) {
        const double * xi = x + i;
        float64x2_t a0 = vdupq_n_f64(0.0), a1 = vdupq_n_f64(0.0);
        float64x2_t a2 = vdupq_n_f64(0.0), a3 = vdupq_n_f64(0.0);
#define DECLICK_NEON_TAP4(xk, hp, LANE)               \
        DECLICK_NEON_TAP(a0, (xk),     hp, LANE);     \
        DECLICK_NEON_TAP(a1, (xk) + 2, hp, LANE);     \
        DECLICK_NEON_TAP(a2, (xk) + 4, hp, LANE);     \
        DECLICK_NEON_TAP(a3, (xk) + 6, hp, LANE)
        int k = 0;
        for (; k + 2 <= taps; k += 2) {
            const float64x2_t hp = vld1q_f64(h + k);
            DECLICK_NEON_TAP4(xi + k,     hp, 0);
            DECLICK_NEON_TAP4(xi + k + 1, hp, 1);
        }
        if (k < taps) {
            const float64x2_t hp = vdupq_n_f64(h[k]);
            DECLICK_NEON_TAP4(xi + k, hp, 0);
        }
#undef DECLICK_NEON_TAP4
        vst1q_f64(out + i,     a0);
        vst1q_f64(out + i + 2, a1);
        vst1q_f64(out + i + 4, a2);
        vst1q_f64(out + i + 6, a3);
    }
    for (; i < count; ++i) out[i] = dotNeon(h, x + i, taps);
}

#undef DECLICK_NEON_TAP

void autocorrNeon(const double * x, int n, double * r, int maxLag) {
    // With maxLag zeros after x, r[d] = sum over all n of x[k] * x[k+d].
    filterNeon(x, x, r, maxLag + 1, n);
}

// The scalar FFT pass vectorises as it stands, two doubles to a register.
const Kernels kNeonKernels = { filterNeon, autocorrNeon, fftPairBase, fftOneBase,
                               sortedRowBase, dot2Base, flankAddBase, nullptr,
                               binUnpackBase, smooth7Base, floorGainBase, "neon" };

#endif // DECLICK_NEON

//! In-place radix-2 complex FFT of n points (n >= 4), for the body stage.
//! The input must already be in bit-reversed order: the callers write it
//! there, through the bit reversal table, as they fill it, which saves the
//! permuting pass. st holds each stage's twiddles end to end, cos and sin of
//! 2 pi k / len for k < len/2 at st[len/2 - 1 + k], so a stage reads them in
//! order. The first two stages need no multiplies and go together in one
//! pass; the rest go two at a time through pair, a stage on its own through
//! one first when their count is odd. Forward is e^-i; the inverse is not scaled.
void fftRadix2(double * re, double * im, int n, const double * stRe,
               const double * stIm, bool inverse, FftPairFn pair, FftOneFn one) {
    for (int i = 0; i < n; i += 4) {
        const double a0r = re[i] + re[i + 1], a0i = im[i] + im[i + 1];
        const double a1r = re[i] - re[i + 1], a1i = im[i] - im[i + 1];
        const double a2r = re[i + 2] + re[i + 3], a2i = im[i + 2] + im[i + 3];
        const double a3r = re[i + 2] - re[i + 3], a3i = im[i + 2] - im[i + 3];
        re[i] = a0r + a2r; im[i] = a0i + a2i;
        re[i + 2] = a0r - a2r; im[i + 2] = a0i - a2i;
        // The second butterfly's twiddle is -i forward, +i inverse.
        const double tr = inverse ? -a3i : a3i;
        const double ti = inverse ? a3r : -a3r;
        re[i + 1] = a1r + tr; im[i + 1] = a1i + ti;
        re[i + 3] = a1r - tr; im[i + 3] = a1i - ti;
    }
    const double sgn = inverse ? 1.0 : -1.0;
    int stages = 0;
    for (int len = 8; len <= n; len <<= 1) ++stages;
    int len = 8;
    if (stages & 1) {
        one(re, im, n, len >> 1, stRe, stIm, sgn);
        len <<= 1;
    }
    for (; len <= n; len <<= 2) pair(re, im, n, len >> 1, stRe, stIm, sgn);
}

//! Median of three.
inline double median3(double a, double b, double c) {
    return dmax(dmin(a, b), dmin(dmax(a, b), c));
}

//! The modified Bessel function I0, by its series: for the flank repair's
//! Kaiser window, built once in configure().
static double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double h = x / (2.0 * (double)k);
        term *= h * h;
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

//! The flank repair's long-term spectrum: 512-point transforms at any rate.
enum { kWideFft = 512 };

const Kernels * pickKernels(bool allowWide) {
#if DECLICK_AVX2
    if (allowWide && cpuHasAvx2Fma()) return &kAvx2Kernels;
#endif
#if DECLICK_NEON
    if (allowWide) return &kNeonKernels;
#endif
    (void)allowWide;
    return &kBaseKernels;
}

//! Chosen once, when the module loads.
const Kernels * g_kernels = pickKernels(true);

} // anonymous namespace

const char * kernelName() { return g_kernels->name; }

void allowWideKernels(bool allow) { g_kernels = pickKernels(allow); }

// ---------------------------------------------------------------------------

Params Params::fromSliders(const float s[7]) {
    float v[7];
    for (int i = 0; i < 7; ++i) v[i] = (s[i] > 0.0f) ? ((s[i] < 1.0f) ? s[i] : 1.0f) : 0.0f;   // NaN to 0
    Params p = defaults();
    p.sensitivity = v[0];
    p.extent      = v[1];
    p.maxLengthMs = v[2] * 20.0f;                  // clamped to 0.2 below
    p.depth       = v[3];
    p.passes      = 1 + (int)(v[4] * 2.999f);
    p.order       = 16 + 8 * (int)(v[5] * 30.999f);
    p.dryWet      = v[6];
    p.sanitize();
    return p;
}

void Params::sanitize() {
    sensitivity = clampf(sensitivity, 0.0f, 1.0f);
    extent      = clampf(extent, 0.0f, 1.0f);
    depth       = clampf(depth, 0.0f, 1.0f);
    maxLengthMs = clampf(maxLengthMs, 0.2f, 20.0f);
    dryWet      = clampf(dryWet, 0.0f, 1.0f);
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    if (order < (int)kMinOrder) order = (int)kMinOrder;
    if (order > (int)kMaxOrder) order = (int)kMaxOrder;
    order &= ~1;                                   // keep it even
}

bool Params::operator==(const Params & o) const {
    return sensitivity == o.sensitivity && extent == o.extent
        && maxLengthMs == o.maxLengthMs && depth == o.depth && passes == o.passes
        && order == o.order && dryWet == o.dryWet && delta == o.delta;
}

// ---------------------------------------------------------------------------

int Config::bodyDelay(int n) {
    const int b = (int)kBlock;
    return b * ((n + n / 2 - 1 + b - 1) / b);
}

// Frames per second at hop n/4, and from it the window as the prototype
// computes it, operation for operation, so the two agree on its length.
int Config::fineWindow(int n) const {
    const double fs = rate / (double)(n / 4);
    return (int)(24.0 * 1e-3 * fs);
}

int Config::fineDelay(int n) const {
    const int w = fineWindow(n);
    return (w > fineM) ? w : fineM;
}

double Config::warmSeconds() const {
    if (crackleFloor) return 1.0 + crackleFloorWin;
    return (double)(carpetMin + 1);
}

void Config::compute(const Params & pIn, double sampleRate) {
    Params p = pIn;
    p.sanitize();

    if (!(sampleRate >= 1000.0)) sampleRate = 44100.0;
    if (!(sampleRate <= 20000000.0)) sampleRate = 20000000.0;

    order  = p.order;
    passes = p.passes;
    wet    = p.dryWet;
    delta  = p.delta;
    depth  = p.depth;

    // Sensitivity 0..1 maps onto the trigger threshold, in robust sigmas.
    // 6.0 catches only unmistakable clicks; 2.5 is aggressive enough to start
    // costing real music.
    thresholdHi = 6.0 - 3.5 * (double)p.sensitivity;
    // The hysteresis floor: how far a detection spreads into its own tail.
    thresholdLo = dmin(3.0 - 2.0 * (double)p.extent, thresholdHi);

    maxRun = (int)(p.maxLengthMs * 1e-3 * sampleRate);
    if (maxRun < 2) maxRun = 2;
    if (maxRun > 2048) maxRun = 2048;

    // The robust noise estimate must span much more than the average gap
    // between clicks, or it ends up measuring the crackle instead of the
    // music and the detector loses its contrast.
    madWindow = (int)(0.030 * sampleRate);
    if (madWindow < 256) madWindow = 256;
    if (madWindow > 1 << 16) madWindow = 1 << 16;

    // Context either side of a block: enough for the longest repair plus the
    // interpolation window, which itself needs at least `order` on each side
    // for the normal equations to stay Toeplitz.
    pad = maxRun + 3 * order;

    // The body stage's transform spans ~46 ms at any rate: the power of two
    // nearest 2048 * rate / 44.1 kHz. A sample is final once the frame two
    // hops (N/2) after the last one covering it has arrived, 1.5 N - 1 samples
    // later, and the stage delays every sample by the same amount, rounded up
    // to whole blocks. Whole blocks because `latency` is held to two things:
    // exactly that many samples go in before the first comes out, and that
    // many zeros run every real sample out. With output arriving from the
    // repair a block at a time, a delay of whole blocks is the only kind that
    // keeps both.
    rate = sampleRate;
    {
        const double want = 2048.0 * sampleRate / 44100.0;
        int n = 2048;
        while (n < 16384 && want >= 1.5 * (double)n) n *= 2;
        while (n > 256 && want <= 0.75 * (double)n) n /= 2;
        bodyN = n;
    }
    bodyLatency = body ? bodyDelay(bodyN) : 0;

    // The flank repair's pad and high-pass scale with the rate, as times: 8
    // samples and 127 taps at 44.1 kHz. It holds each repaired block back by
    // one more block before the body stage, so the next one's flanks can
    // still reach into it - a whole block, for the same two reasons the body
    // stage's delay is whole blocks. The pad and the filter's half length
    // have to fit inside it; at any rate they do by a factor of four.
    if (sampleRate < 32000.0) flank = 0;
    flankPad = (int)floor(flankPadMs * 1e-3 * sampleRate + 0.5);
    if (flankPad < 0) flankPad = 0;
    flankHalf = (int)floor(63.0 * sampleRate / 44100.0 + 0.5);
    if (flankHalf < 1) flankHalf = 1;
    if (flankPad + flankHalf >= (int)kBlock) flank = 0;
    flankStage = flank ? (int)kBlock : 0;

    // The crackle floor stage's frames span ~23 ms at any rate: the power of
    // two nearest 0.0232 * rate. Its cut frame trails the newest by two hops,
    // as the body stage's does, so its delay is that stage's for its length. It
    // runs on the finished output, so the delay adds to the rest.
    {
        const double want = 0.0232 * sampleRate;
        int n = 64;
        while (n < 32768 && want >= 1.4142135623730951 * (double)n) n *= 2;
        crackleFloorN = n;
        crackleFloorHist = (int)(crackleFloorWin * sampleRate / (double)(n / 4) / (double)4) | 1;
        if (crackleFloorHist < 1) crackleFloorHist = 1;
        if (crackleFloorHist > 16383) crackleFloorHist = 16383;
        crackleFloorQ = dmin(dmax(crackleFloorQ, 0.0), 1.0);
    }
    crackleFloorLatency = crackleFloor ? bodyDelay(crackleFloorN) : 0;
    latency = pad + (int)kBlock + bodyLatency + flankStage + crackleFloorLatency;

    // The carpet key's ring is a fixed array of kCarpetMax seconds.
    if (carpetWin < 1) carpetWin = 1;
    if (carpetWin > (int)kCarpetMax) carpetWin = (int)kCarpetMax;
    if (carpetMin < 1) carpetMin = 1;
    if (carpetMin > carpetWin) carpetMin = carpetWin;
    carpetQ = dmin(dmax(carpetQ, 0.0), 1.0);
    if (!(carpetFirst > 0.0)) carpetFirst = 0.0;
    // The Poisson key's bins are ~11.6 ms: a block at 44.1 and 48 kHz, two at
    // 88.2 and 96.
    fanoBlocks = (int)floor(sampleRate / 44100.0 + 0.5);
    if (fanoBlocks < 1) fanoBlocks = 1;

    // The fine stage's frames span ~2.9 ms at any rate: the power of two
    // nearest 0.0029 * rate. A sample is final once the frame D hops after
    // the last one covering it has arrived, D * N/4 + N - 1 samples later, D
    // the larger of fineM and the voice guard's 24 ms. That has to fit inside
    // the body stage's delay, as the two share it. It does at every rate, by
    // a factor of two or more; the loop is a guard.
    {
        const double want = 0.0029 * sampleRate;
        int n = 16;
        while (n < 4096 && want >= 1.4142135623730951 * (double)n) n *= 2;
        if (fineM < 1) fineM = 1;
        while (n > 16 && fineDelay(n) * (n / 4) + n - 1 > bodyDelay(bodyN)) n /= 2;
        fineN = n;
    }

    // The micro stage's frames are a quarter of the fine stage's, at hop N/2,
    // and its background spans microMs either side, in hops as the prototype
    // rounds it. It starts on a sample once the fine stage's removal of it is
    // final, microLag() on, and its own is final microM hops and a frame after
    // that: all of it within the body stage's delay, as for the fine stage, or
    // it stays off. It does fit at every rate, by about half the delay.
    {
        microN = fineN / 4;
        if (microN < 4) microN = 4;
        microM = (int)floor(microMs * 1e-3 * sampleRate / (double)(microN / 2) + 0.5);
        if (microM < 1) microM = 1;
        if (microLag() + microM * (microN / 2) + microN - 1 > bodyDelay(bodyN)) micro = 0;
    }

    // The envelope the buffers are sized from. Same clamps as maxRun above, but
    // at Params::sanitize()'s 20 ms ceiling and kMaxOrder, so no parameter
    // setting at this sample rate can outgrow them.
    bufOrder  = (int)kMaxOrder;
    bufMaxRun = (int)(20.0 * 1e-3 * sampleRate);
    if (bufMaxRun < 2) bufMaxRun = 2;
    if (bufMaxRun > 2048) bufMaxRun = 2048;
    if (bufMaxRun < maxRun) bufMaxRun = maxRun;      // belt and braces
    bufPad = bufMaxRun + 3 * bufOrder;

    if (maxBlock < (int)kBlock) maxBlock = (int)kBlock;
}

// ---------------------------------------------------------------------------

bool levinson(const double * r, int order, double * a) {
    a[0] = 1.0;
    for (int i = 1; i <= order; ++i) a[i] = 0.0;
    double err = r[0];
    if (!(err > 0.0)) return false;

    // sum_j a[j] * r[i-j] walks r backwards, which as a plain loop is one long
    // chain of dependent additions - at order 128 it was a sixth of the whole
    // run time. With r reversed it is a forward inner product, and dot()
    // splits the chain.
    double rrevBuf[kMaxOrder + 1];
    const bool rev = order <= (int)kMaxOrder;
    if (rev) for (int i = 0; i <= order; ++i) rrevBuf[order - i] = r[i];

    for (int i = 1; i <= order; ++i) {
        double acc = r[i];
        if (rev) acc += dot(&a[1], &rrevBuf[order - i + 1], i - 1);
        else for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const double k = -acc / err;
        if (!(k > -1.0 && k < 1.0)) return i > 1;   // lost positive definiteness
        // Symmetric update; the second half must read pre-update values.
        int j = 1;
#if DECLICK_SSE2
        // Two from each end at a time, the far pair swapped into step - the
        // same products and sums as the loop below, so the same bits.
        const __m128d kk = _mm_set1_pd(k);
        for (; 2 * j + 2 < i; j += 2) {
            const __m128d lo = _mm_loadu_pd(a + j);
            const __m128d hi = _mm_shuffle_pd(_mm_loadu_pd(a + i - j - 1),
                                              _mm_loadu_pd(a + i - j - 1), 1);
            _mm_storeu_pd(a + j, _mm_add_pd(lo, _mm_mul_pd(kk, hi)));
            const __m128d nh = _mm_add_pd(hi, _mm_mul_pd(kk, lo));
            _mm_storeu_pd(a + i - j - 1, _mm_shuffle_pd(nh, nh, 1));
        }
#endif
        for (; j <= i / 2; ++j) {
            const double aj = a[j];
            const double ai = a[i - j];
            a[j]     = aj + k * ai;
            a[i - j] = ai + k * aj;
        }
        a[i] = k;
        err *= (1.0 - k * k);
        if (!(err > 0.0)) return i > 1;
    }
    return true;
}

bool solveBandedToeplitz(const double * d, int band, double * b, int n,
                         std::vector<double> & scratch) {
    if (n <= 0) return false;
    if (band >= n) band = n - 1;
    const int W = band + 1;
    scratch.assign((size_t)n * (size_t)W, 0.0);
    double * L = scratch.empty() ? NULL : &scratch[0];
    if (L == NULL) return false;

    // L(i, j) lives at L[i*W + (i-j)], valid for 0 <= i-j <= band.
    for (int i = 0; i < n; ++i) {
        const int jlo = (i - band > 0) ? (i - band) : 0;
        for (int j = jlo; j <= i; ++j) {
            const int dij = i - j;
            double s = d[dij];
            for (int k = jlo; k < j; ++k) {
                s -= L[(size_t)i * W + (i - k)] * L[(size_t)j * W + (j - k)];
            }
            if (j == i) {
                if (!(s > 0.0) || !finite(s)) return false;
                L[(size_t)i * W] = sqrt(s);
            } else {
                L[(size_t)i * W + dij] = s / L[(size_t)j * W];
            }
        }
    }

    for (int i = 0; i < n; ++i) {                       // forward substitution
        double s = b[i];
        const int jlo = (i - band > 0) ? (i - band) : 0;
        for (int j = jlo; j < i; ++j) s -= L[(size_t)i * W + (i - j)] * b[j];
        b[i] = s / L[(size_t)i * W];
    }
    for (int i = n - 1; i >= 0; --i) {                  // back substitution
        double s = b[i];
        const int jhi = (i + band < n - 1) ? (i + band) : (n - 1);
        for (int j = i + 1; j <= jhi; ++j) s -= L[(size_t)j * W + (j - i)] * b[j];
        b[i] = s / L[(size_t)i * W];
    }
    for (int i = 0; i < n; ++i) {
        if (!finite(b[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

//! Allocation-free, deliberately: configure() is what sizes the buffers, and
//! every caller builds a Channel only to configure it on the very next line.
//! Running configure() here as well meant each Channel first allocated a whole
//! 44.1 kHz envelope - 2.1 MB of it - before the config it would actually run
//! at was known. dsp_declick builds its Channels inside on_chunk(), so that was
//! 2.1 MB per channel of allocation on the audio thread, which is the one thing
//! the envelope in Config exists to prevent.
Channel::Channel() {
    m_cfg.compute(Params::defaults(), 44100.0);
}

//! A real transform of N samples packed into M = N/2 points (or, for the
//! wide spectrum, M complex points): the stage twiddles and bit reversal of
//! M, and nTw twiddles of N for unpacking.
static void fftTables(int M, int N, int nTw, double * stRe, double * stIm, int * rev,
                      double * twRe, double * twIm) {
    const double twoPi = 6.283185307179586;
    for (int k = 0; k < nTw; ++k) {
        twRe[k] = cos(twoPi * (double)k / (double)N);
        twIm[k] = sin(twoPi * (double)k / (double)N);
    }
    for (int half = 1; half < M; half <<= 1) {
        for (int k = 0; k < half; ++k) {
            const double a = twoPi * (double)k / (double)(2 * half);
            stRe[half - 1 + k] = cos(a);
            stIm[half - 1 + k] = sin(a);
        }
    }
    int bits = 0;
    while ((1 << bits) < M) ++bits;
    for (int i = 0; i < M; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev[i] = r;
    }
}

//! Periodic Hann of N, or its square root; returns the sum of its squares
//! every hop samples, the overlap-add's normalisation.
static double hannTable(int N, bool root, int hop, double * w) {
    const double twoPi = 6.283185307179586;
    for (int i = 0; i < N; ++i) {
        const double v = 0.5 - 0.5 * cos(twoPi * (double)i / (double)N);
        w[i] = root ? sqrt(v) : v;
    }
    double norm = 0.0;
    for (int i = 0; i < N; i += hop) norm += w[i] * w[i];
    return (norm > 0.0) ? norm : 1.0;
}

//! Adds V = vr + i vi, bin k of a real transform of N = 2M, to the M-point
//! spectrum its inverse takes: bin k takes V + i V e^(2 pi i k/N), and its
//! mirror N - k the conjugates of both. The Nyquist bin is its own mirror,
//! and real for real input; it folds onto Z[0]. `c`, `sn`: e^(2 pi i k/N).
static inline void packBin(double * re, double * im, const int * rev, int M, int k,
                           double vr, double vi, double c, double sn) {
    const double ur = vr * c - vi * sn, ui = vr * sn + vi * c;
    const int kr = rev[k & (M - 1)];      // see fftRadix2
    re[kr] += vr - ui;
    im[kr] += vi + ur;
    if (k != 2 * M - k) {
        const int mr = rev[M - k];
        re[mr] += vr + ui;
        im[mr] += ur - vi;
    }
}

//! MSVC note (VS 2017 15.9, cl 19.16.27051): of the loops here that fill the
//! windows and twiddles from sin() and cos(), MSVC vectorises some, calling
//! __vdecl_sin2 and __vdecl_cos2, which can differ from the scalar functions
//! in the last bit - and which loops it vectorises moves with edits elsewhere
//! in this function. So an edit here that changes no arithmetic can still move
//! an entry by an ulp, and the output in its last bits: building the flank
//! taps in a local std::vector rather than in m_flHpP took 15 of the 23 calls
//! away, and moved one sample in 7.4 million of a 48 kHz record by a float
//! ulp. To tell that from a real change, compare builds with /fp:strict, under
//! which it vectorises none of them.
void Channel::configure(const Config & cfg) {
    m_cfg = cfg;
    m_pad = cfg.pad;

    // Everything below is sized from cfg's buf* envelope rather than from the
    // parameters in force, so that reconfiguring after a Max repair or Model
    // order change reassigns each vector to the size it already has. See the
    // note on Config::bufOrder: the audio thread must not allocate, and this is
    // what buys that. The algorithm still runs at cfg.order / cfg.maxRun; these
    // only decide how much room is standing by.
    const size_t bo   = (size_t)cfg.bufOrder;
    const size_t bm   = (size_t)cfg.bufMaxRun;
    const size_t need = (size_t)(2 * cfg.bufPad + (int)kBlock) + 8;

    m_win.assign(need, 0.0);
    m_dry.assign(need, 0.0);
    m_full.assign(need, 0.0);
    m_flag.assign(need, 0);
    m_strong.assign(need, 0);
    m_fwd.assign(need, 0.0);
    m_bwd.assign(need, 0.0);
    m_det.assign(need, 0.0);
    // bufOrder zeros after the model, for the wide autocorrelation of it.
    m_a.assign(2 * bo + 1, 0.0);
    m_ra.assign(bo + 1, 0.0);
    m_arev.assign(bo + 1, 0.0);
    m_aa.assign(bo + 1, 0.0);
    m_aarev.assign(bo + 1, 0.0);
    m_madRing.assign((size_t)cfg.madWindow, 0.0);
    // updateAutocorr() reads up to `order` before the last fit's taper, so it
    // is kept behind bufOrder zeros.
    m_fitLead = cfg.bufOrder;
    m_fitWin.assign(need + bo, 0.0);
    m_fitR.assign(bo + 1, 0.0);
    m_delta.assign(need, 0.0);
    m_corr.assign(2 * (bo + 1), 0.0);
    m_runs.assign(2 * (size_t)kBlock, 0);
    m_runGain.assign((size_t)kBlock, 1.0);

    // The fit always spans the whole sliding window, so the Hann taper is the
    // same every block. Build it once here - sized to the envelope like
    // everything else, so a later reconfigure does not allocate.
    m_hann.assign(need, 0.0);
    m_hannLen = 2 * cfg.pad + (int)kBlock;
    if (m_hannLen > 1 && (size_t)m_hannLen <= need) {
        const double twoPi = 6.283185307179586;
        for (int i = 0; i < m_hannLen; ++i) {
            m_hann[(size_t)i] =
                0.5 - 0.5 * cos(twoPi * (double)i / (double)(m_hannLen - 1));
        }
    } else {
        m_hannLen = 0;                            // fitModel falls back to cos()
    }

    // Per-block and per-click scratch, at the worst case interpolate() and
    // fitModel() can ask for. They are still assign()ed and resize()d to the
    // exact length needed each time, which is what zeroes them; reserving here
    // is what stops that from ever reallocating.
    // need + bo: the fit window, and the bufOrder zeros the wide
    // autocorrelation reads past its end.
    m_scratch.reserve(need + bo > (size_t)cfg.madWindow ? need + bo
                                                        : (size_t)cfg.madWindow);
    m_rhs.reserve(bm + 1);                    // m
    m_solve.reserve((bm + 1) * (bo + 1));     // n * (band+1)

    // The body stage, sized from bodyN alone - so from the sample rate - and
    // allocated whether or not it is on, like the rest of the envelope.
    {
        const int N = cfg.bodyN;
        const int H = N / 4;
        m_bN = N;
        m_bH = H;
        // Rings, a power of two that holds the delay and the sample being
        // pushed - the oldest frame being cut starts inside that. Sized from
        // N, not from whether the stage is on, like the rest of the envelope.
        size_t ring = 1;
        while (ring < (size_t)Config::bodyDelay(N) + 1) ring <<= 1;
        m_bMask = ring - 1;
        const double binHz = cfg.rate / (double)N;
        m_bHalf = (int)floor(8.0 * (double)N / 2048.0 + 0.5);
        if (m_bHalf < 1) m_bHalf = 1;
        m_bK0 = (int)ceil(cfg.bodyLo / binHz);
        m_bK1 = (int)floor(cfg.bodyHi / binHz);
        if (m_bK0 < m_bHalf + 1) m_bK0 = m_bHalf + 1;
        // Below N/4, so a bin and the mirror the packed inverse folds it onto
        // never meet; that is 11 kHz at 44.1 kHz.
        if (m_bK1 > N / 4 - m_bHalf - 1) m_bK1 = N / 4 - m_bHalf - 1;
        if (m_bK1 < m_bK0) m_bK1 = m_bK0 - 1;            // an empty band: no cut
        m_bM0 = m_bK0 - m_bHalf;
        m_bM1 = m_bK1 + m_bHalf;
        const int nb = (m_bM1 >= m_bM0) ? (m_bM1 - m_bM0 + 1) : 0;
        const int nk = (m_bK1 >= m_bK0) ? (m_bK1 - m_bK0 + 1) : 0;

        m_bY.assign(ring, 0.0);
        m_bD.assign(ring, 0.0);
        m_bR.assign(ring, 0.0);
        m_bWin.assign((size_t)N, 0.0);
        // The transforms are of N real samples, packed two to a complex one:
        // N/2 points, with the twiddles of N for unpacking.
        m_bRe.assign((size_t)(N / 2), 0.0);
        m_bIm.assign((size_t)(N / 2), 0.0);
        m_bTwRe.assign((size_t)(N / 2), 0.0);
        m_bTwIm.assign((size_t)(N / 2), 0.0);
        m_bStRe.assign((size_t)(N / 2), 0.0);
        m_bStIm.assign((size_t)(N / 2), 0.0);
        m_bRev.assign((size_t)(N / 2), 0);
        m_bMag.assign((size_t)kBodyFrames * (size_t)nb, 0.0);
        m_bXRe.assign((size_t)kBodyFrames * (size_t)nk, 0.0);
        m_bXIm.assign((size_t)kBodyFrames * (size_t)nk, 0.0);
        // One place of pad either side: sortedReplace()'s vector paths load
        // one past each end of the window and discard it.
        m_bMed.assign((size_t)(2 * m_bHalf + 1) + 2, 0.0);

        m_bNorm = hannTable(N, false, H, &m_bWin[0]);
        fftTables(N / 2, N, N / 2, &m_bStRe[0], &m_bStIm[0], &m_bRev[0], &m_bTwRe[0], &m_bTwIm[0]);

        const double blockSec = (double)kBlock / cfg.rate;
        int nRate = (int)floor(cfg.bodyRateWin / blockSec + 0.5);
        int nHold = (int)floor(cfg.bodyHold / blockSec + 0.5);
        int nRelease = (int)floor(cfg.bodyRelease / blockSec + 0.5);
        if (nRate < 1) nRate = 1;
        if (nHold < 1) nHold = 1;
        if (nRelease < 1) nRelease = 1;
        m_bCount.assign((size_t)nRate, 0);
        m_bRate.assign((size_t)nHold, 0.0);
        m_bKey.assign((size_t)nRelease, 0.0);
    }

    // The fine stage, sized from fineN and fineM - so from the sample rate -
    // and allocated whether or not it is on, like the body stage.
    {
        const int N = cfg.fineN;
        m_fN = N;
        m_fH = N / 4;
        const double binHz = cfg.rate / (double)N;
        m_fK0 = (int)ceil(cfg.fineLo / binHz);
        if (m_fK0 < 1) m_fK0 = 1;
        int k1 = (int)floor(cfg.fineHi / binHz);
        if (k1 > N / 2) k1 = N / 2;
        m_fNb = (m_fK0 <= k1) ? (k1 - m_fK0 + 1) : 0;   // an empty band: no cut
        int kMid = (int)ceil(cfg.fineMid / binHz);
        m_fNbLow = kMid - m_fK0;
        if (m_fNbLow < 0) m_fNbLow = 0;
        if (m_fNbLow > m_fNb) m_fNbLow = m_fNb;
        // The mid stage's bins, from midLo up to the fine band, go first; of
        // them, those whose centre is below bodyHi are in the body stage's band.
        m_dNb = 0;
        m_dYieldNb = 0;
        if (cfg.mid && m_fNb > 0) {
            int kd = (int)ceil(cfg.midLo / binHz);
            if (kd < 1) kd = 1;
            if (kd < m_fK0) m_dNb = m_fK0 - kd;
            for (int b = 0; b < m_dNb; ++b)
                if (cfg.body && (double)(kd + b) * binHz < cfg.bodyHi) m_dYieldNb = b + 1;
        }
        m_fHShift = 0;
        while ((1 << m_fHShift) < m_fH) ++m_fHShift;
        // The voice guard's window and lags, as the prototype computes them:
        // +-24 ms, and periods of 2.5 to 12 ms, in frames.
        const double fs = cfg.rate / (double)m_fH;
        m_fW = cfg.fineWindow(N);
        m_fLmin = (int)(0.0025 * fs);
        if (m_fLmin < 2) m_fLmin = 2;
        m_fLmax = (int)(0.012 * fs);
        if (m_fLmax > 2 * m_fW) m_fLmax = 2 * m_fW;
        m_fD = cfg.fineDelay(N);
        m_fSlots = m_fD + 2;
        m_fQSlots = m_fD + 2;
        m_fESlots = m_fD + m_fW + 2;
        m_fWin.assign((size_t)N, 0.0);
        // The transforms are of N real samples, packed two to a complex one:
        // N/2 points, with the twiddles of N for unpacking. The band can
        // reach the Nyquist bin, N/2, so that table has one place more.
        m_fRe.assign((size_t)(N / 2), 0.0);
        m_fIm.assign((size_t)(N / 2), 0.0);
        m_fStRe.assign((size_t)(N / 2), 0.0);
        m_fStIm.assign((size_t)(N / 2), 0.0);
        m_fTwRe.assign((size_t)(N / 2 + 1), 0.0);
        m_fTwIm.assign((size_t)(N / 2 + 1), 0.0);
        m_fRev.assign((size_t)(N / 2), 0);
        const size_t nbAll = (size_t)(m_dNb + m_fNb);
        m_fMag.assign((size_t)m_fSlots * nbAll, 0.0);
        m_fXRe.assign((size_t)m_fSlots * nbAll, 0.0);
        m_fXIm.assign((size_t)m_fSlots * nbAll, 0.0);
        // One place of pad either side of the windows: the vector paths of
        // sortedReplace() load one past each end and discard it.
        m_fSorted.assign(nbAll * (size_t)cfg.fineM + 2, 0.0);
        m_fQ.assign((size_t)m_fQSlots * nbAll, 0.0);
        m_fE.assign((size_t)m_fESlots, 0.0);
        m_dE.assign((size_t)m_fESlots, 0.0);
        m_fSeg.assign((size_t)(2 * m_fW + 1), 0.0);
        // The mid stage's run flags by hop: back to the hop before the cut
        // frame, which trails the newest by m_fD hops, with room to spare.
        {
            size_t slots = 1;
            while (slots < (size_t)(2 * (m_fD + N / m_fH + 4))) slots <<= 1;
            m_dRun.assign(slots, 0);
            m_dRunMask = slots - 1;
        }

        m_fNorm = hannTable(N, true, m_fH, &m_fWin[0]);
        fftTables(N / 2, N, N / 2 + 1, &m_fStRe[0], &m_fStIm[0], &m_fRev[0], &m_fTwRe[0], &m_fTwIm[0]);
        m_fR.assign(m_bY.size(), 0.0);
    }

    // The micro stage, sized from microN and microM - so from the sample rate -
    // and allocated whether or not it is on, like the fine stage. Its band, its
    // floor and its guard's split are the fine stage's.
    {
        const int N = cfg.microN;
        m_mN = N;
        m_mH = N / 2;
        m_mLag = (uint64_t)cfg.microLag();
        const double binHz = cfg.rate / (double)N;
        m_mK0 = (int)ceil(cfg.fineLo / binHz);
        if (m_mK0 < 1) m_mK0 = 1;
        int k1 = (int)floor(cfg.fineHi / binHz);
        if (k1 > N / 2) k1 = N / 2;
        m_mNb = (m_mK0 <= k1) ? (k1 - m_mK0 + 1) : 0;   // an empty band: no cut
        int kMid = (int)ceil(cfg.fineMid / binHz);
        m_mNbLow = kMid - m_mK0;
        if (m_mNbLow < 0) m_mNbLow = 0;
        if (m_mNbLow > m_mNb) m_mNbLow = m_mNb;
        m_mSlots = cfg.microM + 2;
        m_mQSlots = cfg.microM + 2;
        m_mWin.assign((size_t)N, 0.0);
        m_mRe.assign((size_t)(N / 2), 0.0);
        m_mIm.assign((size_t)(N / 2), 0.0);
        m_mStRe.assign((size_t)(N / 2), 0.0);
        m_mStIm.assign((size_t)(N / 2), 0.0);
        m_mTwRe.assign((size_t)(N / 2 + 1), 0.0);
        m_mTwIm.assign((size_t)(N / 2 + 1), 0.0);
        m_mRev.assign((size_t)(N / 2), 0);
        m_mMag.assign((size_t)m_mSlots * (size_t)m_mNb, 0.0);
        m_mXRe.assign((size_t)m_mSlots * (size_t)m_mNb, 0.0);
        m_mXIm.assign((size_t)m_mSlots * (size_t)m_mNb, 0.0);
        m_mSorted.assign((size_t)m_mNb * (size_t)cfg.microM + 2, 0.0);
        m_mQ.assign((size_t)m_mQSlots * (size_t)m_mNb, 0.0);
        // The fine stage's guard by frame, back as far as a micro frame being
        // cut can reach: the fine frames between it and the newest the fine
        // stage has cut by then, with room to spare.
        {
            const int fH = cfg.fineN / 4;
            const int back = (N / 2 + cfg.microM * m_mH + cfg.fineN) / (fH > 0 ? fH : 1) + 8;
            size_t slots = 1;
            while (slots < (size_t)(2 * back)) slots <<= 1;
            m_fKeep.assign(slots, 0.0);
            m_fKeepAt.assign(slots, 0);
            m_fKeepMask = slots - 1;
        }

        m_mNorm = hannTable(N, true, m_mH, &m_mWin[0]);
        fftTables(N / 2, N, N / 2 + 1, &m_mStRe[0], &m_mStIm[0], &m_mRev[0], &m_mTwRe[0], &m_mTwIm[0]);
    }

    // The flank repair: a run list for the block, the corrections aligned with
    // the window, and the high-pass - a unit impulse less a Kaiser-windowed
    // sinc low-pass at flankHz, normalised to unity at DC. Allocated whether
    // or not it is on, like the stages above.
    {
        m_flRun.assign(2 * (size_t)kBlock, 0);
        m_flGain.assign((size_t)kBlock, 1.0);
        m_flOrder.assign((size_t)kBlock, 0);
        // Window index i is m_flAcc[i + kBlock]: the block held back sits just
        // below the window, at -kBlock .. -1.
        m_flAcc.assign(need + (size_t)kBlock, 0.0);
        m_stY.assign((size_t)kBlock, 0.0);
        m_stD.assign((size_t)kBlock, 0.0);
        m_stS.assign((size_t)kBlock, 0);
        const int Hh = cfg.flankHalf;
        m_flHpP.assign((size_t)(2 * Hh + 1 + 2 * kFlankPad), -0.0);
        m_flHpN.assign((size_t)(2 * Hh + 1 + 2 * kFlankPad), 0.0);
        double * hp = &m_flHpP[(size_t)kFlankPad];
        const double pi = 3.141592653589793;
        const double fc = cfg.flankHz / cfg.rate;           // cycles per sample
        const double beta = 6.0;
        double sum = 0.0;
        for (int j = -Hh; j <= Hh; ++j) {
            const double x = 2.0 * pi * fc * (double)j;
            const double sinc = (j == 0) ? 2.0 * fc : sin(x) / (pi * (double)j);
            const double r = (double)j / (double)Hh;
            const double w = besselI0(beta * sqrt(dmax(0.0, 1.0 - r * r))) / besselI0(beta);
            hp[(size_t)(j + Hh)] = sinc * w;
            sum += sinc * w;
        }
        for (int j = 0; j <= 2 * Hh; ++j) hp[(size_t)j] = -hp[(size_t)j] / sum;
        hp[(size_t)Hh] += 1.0;
        std::copy(hp, hp + 2 * Hh + 1, m_flHpN.begin() + kFlankPad);
    }

    // The long-term spectrum for the edge: a 512-point complex transform of
    // 512 real samples, the imaginary part zero, so every bin to Nyquist is
    // there to read without unpacking. Tables as the fine stage builds them.
    {
        const int M = kWideFft;
        m_wSpec.assign((size_t)(M / 2 + 1), 0.0);
        m_wWin.assign((size_t)M, 0.0);
        m_wRe.assign((size_t)M, 0.0);
        m_wIm.assign((size_t)M, 0.0);
        m_wStRe.assign((size_t)M, 0.0);
        m_wStIm.assign((size_t)M, 0.0);
        m_wRev.assign((size_t)M, 0);
        hannTable(M, false, M, &m_wWin[0]);
        fftTables(M, M, 0, &m_wStRe[0], &m_wStIm[0], &m_wRev[0], nullptr, nullptr);
    }

    // The crackle floor stage, sized from crackleFloorN, crackleFloorLo,
    // crackleFloorHi and crackleFloorHist, and - unlike the stages above -
    // only held while it is on: its floor history is ~0.4 MB a channel at
    // 44.1 kHz (~2 MB with no upper edge), and it is off unless asked for. Turning it on or
    // off is a structural change, so it goes through configure().
    if (cfg.crackleFloor) {
        const int N = cfg.crackleFloorN;
        const int H = N / 4, M = N / 2;
        m_nN = N;
        m_nH = H;
        size_t ring = 1;
        while (ring < (size_t)Config::bodyDelay(N) + 1) ring <<= 1;
        m_nMask = ring - 1;
        const double binHz = cfg.rate / (double)N;
        // The band ramps in over the half octave below crackleFloorLo: the
        // first bin is the first above crackleFloorLo / sqrt 2. Kept from four
        // below it, for the 7-bin sums of the bin below the band, whose gain
        // the 3-bin average at its edge reads.
        const double lo = cfg.crackleFloorLo / 1.4142135623730951;
        int k0 = (int)floor(lo / binHz) + 1;
        if (k0 < 1) k0 = 1;
        if (k0 > M) k0 = M;
        m_nK0 = k0;
        m_nM0 = (k0 - 4 > 0) ? k0 - 4 : 0;
        // The per-bin cut fades out over the half octave below crackleFloorHi
        // and is none from its first bin kHi up. Its floor and gain are
        // followed to four bins past kHi, so the 7-bin sums of the bins whose
        // gains the last 3-bin average reads are whole.
        const bool hasHi = cfg.crackleFloorHi > 0.0;
        int kTop = M;
        if (hasHi) {
            int kHi = (int)ceil(cfg.crackleFloorHi / binHz);
            if (kHi < k0) kHi = k0;
            kTop = (kHi + 4 < M) ? kHi + 4 : M;
        }
        m_nNb = kTop - m_nM0 + 1;
        m_nTop = m_nNb;
        const size_t nb = (size_t)m_nNb, nt = (size_t)m_nTop, W = (size_t)cfg.crackleFloorHist;
        m_nY.assign(ring, 0.0);
        m_nD.assign(ring, 0.0);
        m_nR.assign(ring, 0.0);
        m_nWin.assign((size_t)N, 0.0);
        m_nRe.assign((size_t)M, 0.0);
        m_nIm.assign((size_t)M, 0.0);
        m_nStRe.assign((size_t)M, 0.0);
        m_nStIm.assign((size_t)M, 0.0);
        m_nTwRe.assign((size_t)M + 1, 0.0);
        m_nTwIm.assign((size_t)M + 1, 0.0);
        m_nRev.assign((size_t)M, 0);
        m_nP.assign((size_t)kFloorFrames * nb, 0.0);
        m_nPb.assign((size_t)kFloorFrames * nb, 0.0);
        m_nXRe.assign((size_t)kFloorFrames * nb, 0.0);
        m_nXIm.assign((size_t)kFloorFrames * nb, 0.0);
        m_nRing.assign(nt * W, 0.0);
        m_nSorted.assign(nt * (W + 2), 0.0);
        m_nFloor.assign(nt, 0.0);
        m_nIn.assign(nt, 0.0);
        m_nOut.assign(nt, 0.0);
        m_nSaved.assign(nt, 0.0);
        m_nWarm.assign(nt, 0.0);
        m_nAt.assign(W, 0);
        m_nG.assign(nt, 1.0);
        m_nWf.assign(nb, 0.0);
        const double hiFade = hasHi ? cfg.crackleFloorHi / 1.4142135623730951 : 0.0;
        for (int b = 0; b < m_nNb; ++b) {
            const double f = (double)(m_nM0 + b) * binHz;
            const double w = log2(dmax(f, 1.0) / lo) / 0.5;
            const double in = dmin(dmax(w, 0.0), 1.0);
            const double s = hasHi ? dmin(dmax(log2(dmax(f, 1.0) / hiFade) / 0.5, 0.0), 1.0) : 0.0;
            m_nWf[(size_t)b] = in * (1.0 - s);
        }
        m_nNorm = hannTable(N, false, H, &m_nWin[0]);
        fftTables(M, N, M + 1, &m_nStRe[0], &m_nStIm[0], &m_nRev[0], &m_nTwRe[0], &m_nTwIm[0]);
        m_nRel = (cfg.crackleFloorRelease > 0.0) ? exp(-(double)H / cfg.rate / cfg.crackleFloorRelease) : 0.0;
    } else {
        m_nN = 0;
        m_nNb = 0;
        m_nTop = 0;
        m_nY.clear(); m_nD.clear(); m_nR.clear(); m_nWin.clear();
        m_nRe.clear(); m_nIm.clear(); m_nStRe.clear(); m_nStIm.clear();
        m_nTwRe.clear(); m_nTwIm.clear(); m_nRev.clear();
        m_nP.clear(); m_nPb.clear(); m_nXRe.clear(); m_nXIm.clear();
        m_nRing.clear(); m_nSorted.clear(); m_nFloor.clear(); m_nSaved.clear(); m_nWarm.clear(); m_nG.clear();
        m_nAt.clear();
        m_nWf.clear();
    }

    // The output ring. Between one pull and the next a caller can push maxBlock
    // samples, which emit at most maxBlock + kBlock, on top of the up to kBlock
    // already waiting.
    const size_t ring = (size_t)cfg.maxBlock + 2 * (size_t)kBlock;
    if (m_out.size() != ring) m_out.assign(ring, 0.0);

    reset();
}

size_t Channel::heapBytes() const {
    return (m_win.capacity() + m_dry.capacity() + m_full.capacity() + m_fwd.capacity()
            + m_bwd.capacity() + m_det.capacity() + m_a.capacity()
            + m_ra.capacity() + m_aa.capacity() + m_arev.capacity() + m_hann.capacity()
            + m_madRing.capacity() + m_scratch.capacity()
            + m_fitWin.capacity() + m_fitR.capacity() + m_delta.capacity()
            + m_corr.capacity() + m_runGain.capacity()
            + m_aarev.capacity() + m_rhs.capacity() + m_solve.capacity()
            + m_out.capacity()
            + m_bY.capacity() + m_bD.capacity() + m_bR.capacity() + m_bWin.capacity()
            + m_bRe.capacity() + m_bIm.capacity() + m_bTwRe.capacity() + m_bTwIm.capacity()
            + m_bStRe.capacity() + m_bStIm.capacity()
            + m_bMag.capacity() + m_bXRe.capacity() + m_bXIm.capacity()
            + m_bMed.capacity() + m_bRate.capacity() + m_bKey.capacity()
            + m_fWin.capacity() + m_fRe.capacity() + m_fIm.capacity()
            + m_fStRe.capacity() + m_fStIm.capacity()
            + m_fTwRe.capacity() + m_fTwIm.capacity() + m_fMag.capacity()
            + m_fXRe.capacity() + m_fXIm.capacity()
            + m_fSorted.capacity() + m_fQ.capacity()
            + m_fE.capacity() + m_dE.capacity() + m_fSeg.capacity()
            + m_fR.capacity() + m_fKeep.capacity() + m_fKeepAt.capacity()
            + m_mWin.capacity() + m_mRe.capacity() + m_mIm.capacity()
            + m_mStRe.capacity() + m_mStIm.capacity()
            + m_mTwRe.capacity() + m_mTwIm.capacity() + m_mMag.capacity()
            + m_mXRe.capacity() + m_mXIm.capacity()
            + m_mSorted.capacity() + m_mQ.capacity()
            + m_flGain.capacity() + m_flAcc.capacity() + m_flHpP.capacity() + m_flHpN.capacity()
            + m_stY.capacity() + m_stD.capacity()
            + m_wSpec.capacity() + m_wWin.capacity() + m_wRe.capacity() + m_wIm.capacity()
            + m_wStRe.capacity() + m_wStIm.capacity()
            + m_nY.capacity() + m_nD.capacity() + m_nR.capacity() + m_nWin.capacity()
            + m_nRe.capacity() + m_nIm.capacity() + m_nStRe.capacity() + m_nStIm.capacity()
            + m_nTwRe.capacity() + m_nTwIm.capacity()
            + m_nP.capacity() + m_nPb.capacity() + m_nXRe.capacity() + m_nXIm.capacity()
            + m_nRing.capacity() + m_nSorted.capacity()
            + m_nFloor.capacity() + m_nSaved.capacity() + m_nWarm.capacity() + m_nAt.capacity()
            + m_nG.capacity() + m_nWf.capacity() + m_nIn.capacity() + m_nOut.capacity())
             * sizeof(double)
           + m_flag.capacity() + m_strong.capacity() + m_stS.capacity() + m_dRun.capacity()
           + m_runs.capacity() * sizeof(int)
           + (m_bRev.capacity() + m_bCount.capacity() + m_fRev.capacity()
              + m_mRev.capacity() + m_flRun.capacity() + m_flOrder.capacity() + m_wRev.capacity()
              + m_nRev.capacity()) * sizeof(int);
}

bool Channel::retune(const Config & cfg) {
    if (!m_cfg.structurallyEquals(cfg)) return false;
    // Everything left is read fresh per block, so there is nothing to rebuild:
    // thresholds and depth are picked up by the next detect()/interpolate(),
    // wet and delta by the next emit, passes by the next processBlock().
    m_cfg = cfg;
    return true;
}

void Channel::reset() {
    resetPath();
    resetCarpet();
    resetFloorHistory();
    m_warming = false;
    m_warmBlocks = 0;
    m_nWarmT = 0;
}

void Channel::beginWarm() {
    reset();
    m_warming = true;
}

//! What the warm pass counted for the carpet key stays, and stands still for
//! the m_warmBlocks repair blocks it covers. Its crackle floor caps the
//! stage's for the m_nWarmT samples it saw; the stage's history starts again.
void Channel::endWarm() {
    if (!m_warming) return;
    m_warming = false;
    m_warmBlocks = m_blocks;
    m_nWarmT = (m_nHave > 0) ? m_nT : 0;       // no history, no ceiling
    std::copy(m_nFloor.begin(), m_nFloor.end(), m_nWarm.begin());
    resetPath();
    resetFloorHistory();
}

//! What endWarm() keeps: the carpet key's state, its m_warmBlocks, and the
//! floor ceiling. The caller has checked that the ceilings are one size.
template <class To, class From>
void Channel::copyWarm(To & to, const From & w) {
    to.m_c = w.m_c;
    to.m_warmBlocks = w.m_warmBlocks;
    to.m_nWarmT = w.m_nWarmT;
    std::copy(w.m_nWarm.begin(), w.m_nWarm.end(), to.m_nWarm.begin());
}

//! "Taken nothing since" is m_seen, which resetPath() zeroes and every
//! sample pushed counts.
bool Channel::saveWarm(WarmState & s) const {
    s.m_valid = false;
    if (m_warming || m_seen != 0) return false;
    if (s.m_nWarm.size() != m_nWarm.size()) s.m_nWarm.assign(m_nWarm.size(), 0.0);
    copyWarm(s, *this);
    s.m_cfg = m_cfg;
    s.m_valid = true;
    return true;
}

bool Channel::adoptWarm(const WarmState & s) {
    reset();
    if (!s.m_valid || !m_cfg.structurallyEquals(s.m_cfg)
        || m_nWarm.size() != s.m_nWarm.size()) return false;
    copyWarm(*this, s);
    return true;
}

void Channel::resetPath() {
    std::fill(m_win.begin(), m_win.end(), 0.0);
    std::fill(m_dry.begin(), m_dry.end(), 0.0);
    std::fill(m_full.begin(), m_full.end(), 0.0);
    std::fill(m_flag.begin(), m_flag.end(), (uint8_t)0);
    std::fill(m_strong.begin(), m_strong.end(), (uint8_t)0);
    std::fill(m_madRing.begin(), m_madRing.end(), 0.0);
    m_madPos = 0;
    m_madFull = false;
    m_nruns = 0;
    m_aaValid = false;
    m_scale = 1e-6;
    m_medHint = 0.0;
    m_fitLen = 0;
    // m_out.size() is the ring's capacity, so this empties it without giving
    // the storage back - clear() would leave a zero-length ring.
    m_outHead = 0;
    m_outCount = 0;
    m_repaired = 0;
    m_seen = 0;
    m_clamped = 0;
    m_reclamped = 0;
    m_tailFrom = 1 << 30;
    m_blocks = 0;
    std::fill(m_bY.begin(), m_bY.end(), 0.0);
    std::fill(m_bD.begin(), m_bD.end(), 0.0);
    std::fill(m_bR.begin(), m_bR.end(), 0.0);
    std::fill(m_bCount.begin(), m_bCount.end(), 0);
    std::fill(m_bRate.begin(), m_bRate.end(), 0.0);
    std::fill(m_bKey.begin(), m_bKey.end(), 0.0);
    m_bT = 0;
    m_bNextHop = (uint64_t)m_bN;
    m_bHist = 0;
    m_bSlot = 0;
    m_bCountPos = 0;
    m_bRatePos = 0;
    m_bRateHave = 0;
    m_bStrong = 0;
    m_bKeyPos = 0;
    m_bBeta = 0.0;
    m_cBlk = 0;
    m_cOpen = 0;
    std::fill(m_nY.begin(), m_nY.end(), 0.0);
    std::fill(m_nD.begin(), m_nD.end(), 0.0);
    std::fill(m_nR.begin(), m_nR.end(), 0.0);
    std::fill(m_nG.begin(), m_nG.end(), 1.0);
    std::fill(m_nSaved.begin(), m_nSaved.end(), 0.0);
    m_nT = 0;
    m_nNextHop = (uint64_t)m_nN;
    m_nHist = 0;
    m_nSlot = 0;
    m_nCutCount = 0;
    m_nShutHops = 0;
    m_nSinceOpen = 0;
    m_nOpen = false;
    m_nHasSaved = false;
    m_nKeep = false;
    m_nCuts = 0;
    std::fill(m_dRun.begin(), m_dRun.end(), (uint8_t)0);
    m_dCuts = 0;
    m_fNextHop = (uint64_t)m_fN;
    m_fHist = 0;
    m_fFrame = 0;
    std::fill(m_fR.begin(), m_fR.end(), 0.0);
    std::fill(m_fKeep.begin(), m_fKeep.end(), 0.0);
    std::fill(m_fKeepAt.begin(), m_fKeepAt.end(), (uint64_t)0);
    m_mNextHop = (uint64_t)m_mN;
    m_mHist = 0;
    m_mFrame = 0;
    m_mCuts = 0;
    std::fill(m_flAcc.begin(), m_flAcc.end(), 0.0);
    m_flN = 0;
    m_flEnd = -(1 << 30);
    m_flSince = 1 << 30;
    m_bBetaPrev = 0.0;
    m_stHave = false;
    m_flSpans = 0;
    m_flSkipped = 0;
    std::fill(m_wSpec.begin(), m_wSpec.end(), 0.0);
    m_wBlock = 0;
    m_wFrames = 0.0;
    m_wEdge = 0.0;
    m_flP = 0;
    m_flPOpen = 0;
    // Prime with `pad` zeros so the first real sample lands at window index
    // pad and nothing is dropped off the front of the stream. They are not
    // audio, as prime()'s are not: see detect().
    m_fill = m_pad;
    m_silentUntil = m_pad;
}

void Channel::resetCarpet() {
    m_c = Carpet();
}

void Channel::resetFloorHistory() {
    std::fill(m_nFloor.begin(), m_nFloor.end(), 0.0);
    std::fill(m_nAt.begin(), m_nAt.end(), (uint64_t)0);
    m_nHave = 0;
    m_nPos = 0;
    m_nTaken = 0;
    m_nEarly = 0;
}

void Channel::setUnitModel() {
    const int order = m_cfg.order;
    m_a[0] = 1.0;
    for (int i = 1; i <= order; ++i) m_a[(size_t)i] = 0.0;
    for (int i = 0; i <= order; ++i) m_arev[(size_t)i] = m_a[(size_t)(order - i)];
    m_aaValid = false;
}

void Channel::fitModel(int from, int to) {
    const int order = m_cfg.order;
    const int n = to - from;
    if (n < 4 * order + 4) {
        setUnitModel();
        m_fitLen = 0;
        return;
    }

    // Guard against gross outliers only - a dropout, a splice, a digital
    // full-scale spike. It does NOT protect the fit from crackle, despite
    // looking like it should: on 78 rpm material the threshold lands near
    // -8 dBFS while the clicks themselves sit at -40 to -50 dBFS, so not one
    // of them is touched. That turns out not to matter. Fitting on the true
    // clean signal instead - the best any robust estimator could ever do - is
    // worth at most +0.3 dB of reconstruction SNR, measured against
    // injected-click ground truth at contamination from 0.7% to 5.2%.
    // Crackle is dense but tiny, and a sum over a thousand lag products
    // barely notices it. See the README.
    // `order` zeros past the end: the wide autocorrelation reads them.
    m_scratch.resize((size_t)n + (size_t)order);
    for (int i = n; i < n + order; ++i) m_scratch[(size_t)i] = 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += fabs(m_win[from + i]);
    const double lim = 6.0 * (acc / (double)n) + 1e-12;

    // Hann, so the autocorrelation stays positive definite. Precomputed in
    // configure() unless the fit length is not the one it was built for.
    const bool haveHann = (m_hannLen == n);
    const double twoPi = 6.283185307179586;
    for (int i = 0; i < n; ++i) {
        double v = m_win[from + i];
        if (v > lim) v = lim;
        else if (v < -lim) v = -lim;
        const double w = haveHann
            ? m_hann[(size_t)i]
            : 0.5 - 0.5 * cos(twoPi * (double)i / (double)(n - 1));
        m_scratch[i] = v * w;
    }

    m_ra.assign((size_t)order + 1, 0.0);
    if (m_fitLen != n || !updateAutocorr(n, order)) {
        g_kernels->autocorr(&m_scratch[0], n, &m_ra[0], order);
    }
    // Kept, unridged, for the next pass to update rather than redo.
    memcpy(&m_fitWin[(size_t)m_fitLead], &m_scratch[0], sizeof(double) * (size_t)n);
    memcpy(&m_fitR[0], &m_ra[0], sizeof(double) * ((size_t)order + 1));
    m_fitLen = n;
    if (!(m_ra[0] > 0.0)) {
        setUnitModel();
        return;
    }
    m_ra[0] *= 1.0000001;                       // ridge, keeps Levinson stable
    if (!levinson(&m_ra[0], order, &m_a[0])) {
        // Partial model is fine; levinson() leaves what it managed to build.
    }
    for (int i = 0; i <= order; ++i) {
        if (!finite(m_a[i])) {
            setUnitModel();
            return;
        }
    }
    for (int i = 0; i <= order; ++i) m_arev[(size_t)i] = m_a[(size_t)(order - i)];
    m_aaValid = false;
}

//! The autocorrelation of the tapered window in m_scratch, into m_ra, from the
//! last fit's - m_fitWin and m_fitR - and the samples that differ between them.
//!
//! Every pass after the first fits the same window again, which differs from
//! the last fit's only where that pass repaired it: a few dozen samples of
//! some 1600 at the defaults. With w the old taper, w' the new and
//! delta = w' - w,
//!
//!     r'[d] = r[d] + sum_p delta[p] * (w'[p+d] + w[p-d]),
//!
//! which over each stretch of changed samples is two filters of `order` + 1
//! outputs, against the window's length for the full product. A quarter of the
//! fits' time at the defaults; the sums are taken in a different order, so it
//! differs from the full product in the last bits.
//!
//! False, leaving m_ra alone, where so much changed that the full product is
//! as cheap.
bool Channel::updateAutocorr(int n, int order) {
    const double * w1 = &m_scratch[0];
    const double * w0 = &m_fitWin[(size_t)m_fitLead];
    double * delta = &m_delta[0];
    int changed = 0;
    for (int i = 0; i < n; ++i) {
        delta[i] = w1[i] - w0[i];
        changed += (delta[i] != 0.0);
    }
    if (changed > n / 4) return false;

    double * r = &m_ra[0];
    for (int d = 0; d <= order; ++d) r[d] = m_fitR[(size_t)d];
    double * fwd = &m_corr[0];
    double * bwd = &m_corr[(size_t)order + 1];
    int i = 0;
    while (i < n) {
        if (delta[i] == 0.0) { ++i; continue; }
        // A stretch of changes, with gaps of a few unchanged samples bridged:
        // a zero tap costs less than another pair of filter calls.
        const int s = i;
        int e = i, gap = 0;
        for (++i; i < n && gap < 8; ++i) {
            if (delta[i] != 0.0) { e = i; gap = 0; } else ++gap;
        }
        i = e + 1;
        const int len = e - s + 1;
        // fwd[d] = sum_t delta[s+t] * w'[s+t+d], reading into the `order`
        // zeros after w'. bwd[order-d] = sum_t delta[s+t] * w[s+t-d], reading
        // into the m_fitLead zeros before w.
        g_kernels->filter(delta + s, w1 + s, fwd, order + 1, len);
        g_kernels->filter(delta + s, w0 + s - order, bwd, order + 1, len);
        for (int d = 0; d <= order; ++d) r[d] += fwd[d] + bwd[order - d];
    }
    return true;
}

//! The median of the first `have` entries of the noise ring: the value
//! nth_element over a copy of all of them would pick, and the same one bit for
//! bit - an order statistic does not depend on how it is found.
//!
//! nth_element over the whole ring was a seventh of the run time. But from one
//! block to the next the median moves little, so one branch-free pass keeps only
//! the entries within a band either side of the last one and counts those below
//! it, and nth_element then has a tenth as many to look at. Where the median has
//! left the band, a second pass keeps the side it went to instead.
double Channel::ringMedian(size_t have) {
    const size_t mid = have / 2;
    const double * v = &m_madRing[0];
    m_scratch.resize(have);
    double * keep = &m_scratch[0];
    size_t n = 0, k = mid;

    if (m_medHint > 0.0) {
        const double lo = m_medHint * (1.0 - kMedianBand);
        const double hi = m_medHint * (1.0 + kMedianBand);
        size_t below = 0;
        for (size_t i = 0; i < have; ++i) {
            const double x = v[i];
            keep[n] = x;
            below += (size_t)(x < lo);
            n += (size_t)((x >= lo) & (x <= hi));
        }
        if (mid < below) {
            n = 0;
            for (size_t i = 0; i < have; ++i) {
                keep[n] = v[i];
                n += (size_t)(v[i] < lo);
            }
        } else if (mid >= below + n) {
            k = mid - below - n;
            n = 0;
            for (size_t i = 0; i < have; ++i) {
                keep[n] = v[i];
                n += (size_t)(v[i] > hi);
            }
        } else {
            k = mid - below;
        }
    } else {
        for (size_t i = 0; i < have; ++i) keep[i] = v[i];
        n = have;
    }

    std::nth_element(keep, keep + k, keep + n);
    m_medHint = keep[k];
    return keep[k];
}

void Channel::detect(int from, int to, int pass) {
    const int order = m_cfg.order;

    // Forward and backward prediction error. A click blows up the forward
    // error at its onset and the backward error at its offset; taking the max
    // of the two captures the whole burst instead of just its leading edge.
    // sum_k a[k]*win[i-k] walks the window backwards, so it uses the reversed
    // coefficients and reads forward from i-order; the backward residual
    // already walks forward and uses m_a directly.
    if (to > from) {
        const int taps = order + 1;
        g_kernels->filter(&m_arev[0], &m_win[(size_t)(from - order)],
                          &m_fwd[(size_t)from], to - from, taps);
        g_kernels->filter(&m_a[0], &m_win[(size_t)from],
                          &m_bwd[(size_t)from], to - from, taps);
    }

    if (pass == 0) {
        // Refresh the robust noise estimate from this block's forward error -
        // but not from the zeros either side of the audio, or right after a
        // reset the median is zero and the first real samples read as one
        // enormous click.
        const int end = (to < m_tailFrom) ? to : m_tailFrom;
        for (int i = (from > m_silentUntil) ? from : m_silentUntil; i < end; ++i) {
            m_madRing[(size_t)m_madPos] = fabs(m_fwd[i]);
            if (++m_madPos >= (int)m_madRing.size()) { m_madPos = 0; m_madFull = true; }
        }
        const size_t have = m_madFull ? m_madRing.size() : (size_t)m_madPos;
        if (have >= 16) {
            const double med = ringMedian(have) * 1.4826;
            m_scale = dmax(med, 1e-12);
        }
    }

    // Nothing is detected where the residual reads those zeros: within
    // `order` of the first real sample, or of the last before drain()'s. The
    // music stepping from or to silence is a click as far as the model can
    // tell, and repairing it rebuilt the edge from the silence - the first and
    // last 10 ms of every seek and every gapless change of track, and at the
    // default order the step opened the body key for its two second release.
    const double inv = 1.0 / m_scale;
    const int live0 = m_silentUntil + order;
    const int live1 = (m_tailFrom < (1 << 30)) ? m_tailFrom - order : to;
    for (int i = from; i < to; ++i) {
        m_det[i] = (i >= live0 && i < live1) ? dmax(fabs(m_fwd[i]), fabs(m_bwd[i])) * inv : 0.0;
    }
}

void Channel::interpolate(int from, int to, int reach) {
    const double hi = m_cfg.thresholdHi;
    const double lo = m_cfg.thresholdLo;
    const int dip = m_cfg.dip;
    const double far = lo + m_cfg.dipFar * (hi - lo);

    // Never into the zeros reset() or prime() fed: they are not audio, and the
    // first onset after them reads as a click whose backward error reaches
    // back into them - a repair there writes a pre-echo where the stream has
    // to be silent.
    if (from < m_silentUntil) from = m_silentUntil;

    int i = from;
    while (i < to) {
        if (m_det[i] <= hi || m_flag[i]) { ++i; continue; }

        // Hysteresis: walk outwards while the detection function is still
        // above the low threshold. The end may run past `to`, into the margin
        // processBlock() detected over: a burst that straddles the block edge
        // has to be repaired whole, or its unrepaired tail is taken as clean
        // context for the half that is.
        // It steps over a dip of up to Config::dip samples, as long as the
        // detection function is well back above the floor on the far side:
        // inside a click the error rings through zero, which is no reason
        // to end the repair halfway.
        int s = i, e = i;
        while (s - 1 >= from && m_det[s - 1] > lo && !m_flag[s - 1]) --s;
        while (e + 1 < reach && m_det[e + 1] > lo && !m_flag[e + 1]) ++e;
        int s0 = s, e0 = e;
        // Only a run that triggered hard may step over a dip: one that
        // barely crossed the trigger is as likely noise or music, and
        // stretching it only takes more of the music.
        double peak = 0.0;
        for (int t = s; t <= e; ++t) if (m_det[t] > peak) peak = m_det[t];
        const bool mayDip = peak >= m_cfg.dipPeak;
        // And only one that triggered hard is repaired in full; one at the
        // trigger loses softMin of its discrepancy. See Config::softMin.
        double gain = 1.0;
        if (m_cfg.softSpan > 0.0) {
            const double u = (peak - hi) / m_cfg.softSpan;
            gain = m_cfg.softMin + (1.0 - m_cfg.softMin) * dmin(dmax(u, 0.0), 1.0);
        }
        while (mayDip) {
            int j = s - 1;
            while (j >= s - 1 - dip && j >= from && !m_flag[j] && m_det[j] <= lo) --j;
            if (j < s - 1 - dip || j < from || m_flag[j]) break;
            if (j < s - 1 && m_det[j] <= far) break;
            s = j;
        }
        while (mayDip) {
            int j = e + 1;
            while (j <= e + 1 + dip && j < reach && !m_flag[j] && m_det[j] <= lo) ++j;
            if (j > e + 1 + dip || j >= reach || m_flag[j]) break;
            if (j > e + 1 && m_det[j] <= far) break;
            e = j;
        }
        // One sample of slack either side; a repair that stops exactly at the
        // damage boundary tends to leave a step.
        if (s > from) --s;
        if (e < reach - 1) ++e;
        if (s0 > from) --s0;
        if (e0 < reach - 1) ++e0;

        // A run the dips stretched past what repairRun() will take - longer
        // than maxRun, or out to the edge of the window - falls back to the
        // plain one, so stepping over dips can only ever add to a repair.
        // A huge click on a quiet floor rings above it for a long way.
        bool ok = repairRun(s, e, false, gain);
        if (!ok && (s != s0 || e != e0)) {
            s = s0; e = e0;
            ok = repairRun(s, e, false, gain);
        }
        i = e + 1;
        if (ok && peak >= m_cfg.bodyPeak) ++m_bStrong;   // the body stage's key
        if (ok && peak >= m_cfg.carpetPeak) ++m_cBlk;    // the carpet key
        if (ok && m_dNb > 0 && peak >= m_cfg.midPeak)    // the mid stage's gate
            memset(&m_strong[(size_t)s], 1, (size_t)(e - s + 1));
        if (ok && m_cfg.flankStage > 0 && m_flN < (int)m_flGain.size()) {
            m_flRun[(size_t)(2 * m_flN)] = s;
            m_flRun[(size_t)(2 * m_flN + 1)] = e;
            m_flGain[(size_t)m_flN] = gain;
            ++m_flN;
        }
        if (ok && m_cfg.rerepair > 0 &&
            m_nruns + 2 <= (int)m_runs.size()) {
            m_runGain[(size_t)(m_nruns / 2)] = gain;
            m_runs[(size_t)m_nruns++] = s;
            m_runs[(size_t)m_nruns++] = e;
        }
    }
}

bool Channel::solveSpan(int s, int e) {
    const int order = m_cfg.order;
    const int m = e - s + 1;
    // G is symmetric banded Toeplitz with first column aa[0..order],
    // where aa is the autocorrelation of the AR coefficients. That holds
    // exactly as long as ctx >= order, which it is. m_a has `order` zeros
    // after the model, which the wide autocorrelation reads.
    if (!m_aaValid) {
        g_kernels->autocorr(&m_a[0], order + 1, &m_aa[0], order);
        m_aa[0] *= 1.0 + 1e-9;
        for (int d = 0; d <= order; ++d) m_aarev[(size_t)d] = m_aa[(size_t)(order - d)];
        m_aaValid = true;
    }

    // The right hand side is -A'A applied to the window with the run zeroed,
    // on the run's columns: the prediction error of the m + order rows that
    // touch the run, multiplied back through the model. It used to be built
    // that way, a filter of m + order rows by order + 1 taps for every run,
    // and that was the larger part of every repair. But A'A is the Toeplitz
    // matrix of aa, so the same thing is
    //
    //     rhs[j] = -sum over known t within `order` of c of aa[|t - c|] * x[t]
    //
    // for column c = s + j: 2*order - m + 1 terms per sample, an eighth of the
    // work for a click of a few samples. No row is clipped by the window, since
    // ctx >= order. aa[0] carries the ridge, but lag 0 is the sample itself,
    // which is never a known one. The sums run in a different order, so this
    // differs from the filter in the last bits.
    // The known samples come from m_full, not m_win: a neighbour's half
    // repair is no context to solve against. See m_full.
    m_rhs.assign((size_t)m, 0.0);
    const double * win = &m_full[0];
    for (int j = 0; j < m; ++j) {
        const int c = s + j;
        double v = 0.0;
        // t = c - order .. s - 1, where aa[c - t] runs backwards; and
        // t = e + 1 .. c + order. The two sums are independent, and taken
        // together the wide kernel overlaps them; see dot2Avx2().
        const int nl = s - (c - order);
        const int nr = c + order - e;
        double l, r;
        g_kernels->dot2(&m_aarev[0], win + (c - order), nl,
                        (nr > 0) ? &m_aa[(size_t)(e + 1 - c)] : &m_aa[0], win + e + 1, nr,
                        &l, &r);
        if (nl > 0) v += l;
        if (nr > 0) v += r;
        m_rhs[(size_t)j] = -v;
    }

    return solveBandedToeplitz(&m_aa[0], order, &m_rhs[0], m, m_solve);
}

bool Channel::repairRun(int s, int e, bool again, double gain) {
    const int order = m_cfg.order;
    const int ctx = 3 * order;
    const int m = e - s + 1;
    if (m <= 0 || m > m_cfg.maxRun) return false;      // too long: that is music

    const int lo0 = s - ctx;
    const int hi0 = e + ctx;
    if (lo0 < 0 || hi0 >= m_fill) return false;

    const int L = hi0 - lo0 + 1;
    const int rows = L - order;
    if (rows <= m) return false;

    if (!solveSpan(s, e)) return false;

    // Never let a repair leave the local dynamic range - of the input. Taken
    // from m_win it compounded: on dense runs each repair read the last one
    // as context, could reach 1.5x it, and an ill-conditioned model (an MP3's
    // empty top octave) walked a side channel from 0.17 to 728 inside 4 ms.
    const double localMax = dmax(absMax(&m_dry[(size_t)lo0], s - lo0),
                                 absMax(&m_dry[(size_t)(e + 1)], hi0 - e));
    const double limit = 1.5 * dmax(localMax, 1e-9);

    // Subtractive repair. A click ADDS to the music, so the damaged
    // samples still carry the signal underneath: what to remove is the
    // discrepancy d = x - v, not the sample. Subtracting all of d means
    // trusting the estimate completely, and over a hole of any size the
    // estimate is not that good - 78 rpm material interpolates at about
    // 30 dB SNR over 3 samples, 22 dB over 6 and 8 dB over 16, while the
    // click itself sits only ~18 dB below the music. Removing a fixed
    // fraction keeps most of the click while leaving the underlying signal
    // where the estimate would have done more harm than good.
    const double depth = m_cfg.depth;
    const double wcap = dmin(m_cfg.wienerMax, 1.0);
    int touched = 0;
    int clamped = 0;   // see clampedSamples()

    for (int j = 0; j < m; ++j) {
        double v = m_rhs[(size_t)j];
        clamped += fabs(v) > limit;
        if (v > limit) v = limit;
        else if (v < -limit) v = -limit;

        // A re-repair subtracts from what was actually there, not from the
        // earlier repair - which stays put if this one cannot be done.
        const double x = again ? m_dry[s + j] : m_win[s + j];
        const double xf = again ? m_dry[s + j] : m_full[s + j];
        const double dj = x - v;

        double w = wcap;
        w += depth * (1.0 - w);   // at depth 1 this is outright replacement
        if (w > 0.0) m_full[s + j] = xf - w * (xf - v);
        w *= gain;

        if (w > 0.0) {
            m_win[s + j] = x - w * dj;
            ++touched;
        }
        // Flagged either way: the run has been dealt with, and a later
        // pass must not come back and attack it again.
        m_flag[s + j] = 1;
    }
    if (!again) m_repaired += (uint64_t)touched;
    if (again) m_reclamped += (uint64_t)clamped;
    else m_clamped += (uint64_t)clamped;
    return true;
}

void Channel::rerepairRuns() {
    for (int k = 0; k + 1 < m_nruns; k += 2) {
        repairRun(m_runs[(size_t)k], m_runs[(size_t)k + 1], true,
                  m_runGain[(size_t)(k / 2)]);
    }
}

//! The block's runs, widened by the pad in force, in start order, merged where
//! they meet; each span interpolated again and its high-passed difference from
//! the output added to m_flAcc. A span starts after the last one, this block's
//! or the one before, and far enough in that the high-pass reaches back no
//! further than the samples still held for the body stage. See Config::flank.
void Channel::flankPass() {
    const int n = m_flN;
    m_flN = 0;
    if (m_bBeta > 0.0) m_flSince = 0;
    else if (m_flSince < (1 << 30)) ++m_flSince;
    // Where the key is open, at full strength and with flankWide's pad on a
    // full-band transfer; where it is shut, only as far as the transfer is
    // band-limited, fading over flankHold seconds from full to none.
    const bool open = m_bBeta > 0.0;
    const int P = open ? m_flPOpen : m_flP;
    double strength = open ? 1.0 : 0.0;
    if (!open && m_cfg.flankHold > 0.0) {
        const double t = (double)m_flSince * (double)kBlock / m_cfg.rate;
        strength = dmax(strength, 1.0 - t / m_cfg.flankHold);
    }
    // And at least the carpet key; see Config::carpet. With the key shut that
    // is 0, which changes nothing.
    strength = dmax(strength, m_c.key);
    if (n == 0 || P <= 0 || !(strength > 0.0)) return;
    int * ord = &m_flOrder[0];
    for (int k = 0; k < n; ++k) {             // a few dozen at most, mostly in order
        int j = k;
        const int sk = m_flRun[(size_t)(2 * k)];
        while (j > 0 && m_flRun[(size_t)(2 * ord[j - 1])] > sk) { ord[j] = ord[j - 1]; --j; }
        ord[j] = k;
    }
    // Each run on its own, with its own gain: widened into the samples
    // between it and its neighbours, never into a neighbour, and the gap
    // between two runs going to the first. Merged into one span, a cluster on
    // dense crackle became one long interpolation that lost the music's
    // 3-4.5 kHz, and took it with the strongest run's gain.
    const int lowest = m_pad - (int)kBlock + m_cfg.flankHalf;
    int prev = m_flEnd;
    for (int k = 0; k < n; ++k) {
        const int r = ord[k];
        const int s0 = m_flRun[(size_t)(2 * r)], e0 = m_flRun[(size_t)(2 * r + 1)];
        int s = s0 - P, e = e0 + P;
        if (s < lowest) s = lowest;
        if (s <= prev) s = prev + 1;
        if (k + 1 < n) {
            const int next = m_flRun[(size_t)(2 * ord[k + 1])];
            if (e >= next) e = next - 1;
        }
        if (s > s0 || e < e0) continue;       // no room either side: nothing new
        if (s == s0 && e == e0) continue;
        flankSpan(s, e, m_flGain[(size_t)r], strength);
        prev = e;
    }
}

void Channel::flankSpan(int s, int e, double gain, double strength) {
    const int ctx = 3 * m_cfg.order;
    const int m = e - s + 1;
    if (m > m_cfg.maxRun || s - ctx < 0 || e + ctx >= m_fill || !solveSpan(s, e)) {
        ++m_flSkipped;
        return;
    }
    // The same limit as repairRun()'s, from the input either side.
    const double localMax = dmax(absMax(&m_dry[(size_t)(s - ctx)], ctx),
                                 absMax(&m_dry[(size_t)(e + 1)], ctx));
    const double limit = 1.5 * dmax(localMax, 1e-9);
    // Each estimate becomes the correction to its sample, in place.
    double * d = &m_rhs[0];
    for (int j = 0; j < m; ++j) {
        double v = d[j];
        if (v > limit) v = limit;
        else if (v < -limit) v = -limit;
        const int t = s + j;
        const double x = m_dry[(size_t)t];
        d[j] = strength * ((x - gain * (x - v)) - m_win[(size_t)t]);
    }
    // High-passed into acc[i], window index i from -kBlock: never below it,
    // see `lowest`, and never past the last sample in.
    const size_t tap0 = (size_t)(kFlankPad + m_cfg.flankHalf);
    g_kernels->flankAdd(&m_flAcc[(size_t)kBlock], &m_flHpP[tap0], &m_flHpN[tap0],
                        m_cfg.flankHalf, d, s, m, m_fill - 1);
    m_flEnd = e;
    ++m_flSpans;
}

//! Every fourth block, the power spectrum of its input into a long-term
//! average: the mean of every transform for the first ten seconds, then an
//! exponential one over ten. After three seconds the -40 dB edge - the highest
//! bin within 40 dB of the 2-5 kHz level, in log terms, above 5 kHz - sets the
//! flank repair's pad: full at flankEdge0, none at flankEdge1.
void Channel::bandwidthBlock() {
    if (++m_wBlock < 4) return;
    m_wBlock = 0;
    const int M = kWideFft;
    double * re = &m_wRe[0];
    double * im = &m_wIm[0];
    const double * x = &m_dry[(size_t)m_pad];
    for (int i = 0; i < M; ++i) {
        re[m_wRev[(size_t)i]] = x[i] * m_wWin[(size_t)i];
        im[m_wRev[(size_t)i]] = 0.0;
    }
    fftRadix2(re, im, M, &m_wStRe[0], &m_wStIm[0], false, g_kernels->fftPair, g_kernels->fftOne);
    const double hop = 4.0 * (double)kBlock;
    const double tenS = 10.0 * m_cfg.rate / hop;
    m_wFrames += 1.0;
    const double a = (m_wFrames < tenS) ? 1.0 / m_wFrames : 1.0 / tenS;
    double * P = &m_wSpec[0];
    for (int k = 0; k <= M / 2; ++k) P[k] += a * (re[k] * re[k] + im[k] * im[k] - P[k]);
    if (m_wFrames < 3.0 * m_cfg.rate / hop) {
        m_wEdge = 0.0;
        m_flP = 0;
        m_flPOpen = 0;
        return;
    }
    const double binHz = m_cfg.rate / (double)M;
    const int k2 = (int)ceil(2000.0 / binHz), k5 = (int)floor(5000.0 / binHz);
    double lsum = 0.0;
    for (int k = k2; k <= k5; ++k) lsum += log(P[k] + 1e-30);
    const double floorP = exp(lsum / (double)(k5 - k2 + 1)) * 1e-4;    // -40 dB
    int edge = k5;
    for (int k = M / 2; k > k5; --k) {
        if (P[k] > floorP) { edge = k; break; }
    }
    m_wEdge = (double)edge * binHz;
    const double span = m_cfg.flankEdge1 - m_cfg.flankEdge0;
    double f = (span > 0.0) ? (m_cfg.flankEdge1 - m_wEdge) / span
                            : (m_wEdge <= m_cfg.flankEdge0 ? 1.0 : 0.0);
    f = dmin(dmax(f, 0.0), 1.0);
    m_flP = (int)floor((double)m_cfg.flankPad * f + 0.5);
    const double w = dmin(dmax(m_cfg.flankWide, 0.0), 1.0);
    m_flPOpen = (int)floor((double)m_cfg.flankPad * (w + (1.0 - w) * f) + 0.5);
}

void Channel::processBlock() {
    const int order = m_cfg.order;
    const int blk = (int)kBlock;

    // Detect over the block plus a margin, so a burst that starts near the end
    // of the block can still be followed into the next one.
    const int from = m_pad;
    const int to = m_pad + blk + m_cfg.maxRun;
    const int dFrom = (from - order > 0) ? (from - order) : order;
    const int dTo = (to < m_fill - order) ? to : (m_fill - order);

    // The fit stops where drain()'s zeros start: tapered, a window that is half
    // silence is a poor model of the half that is music.
    const int fitTo = (m_tailFrom < m_fill) ? m_tailFrom : m_fill;

    m_nruns = 0;
    m_flN = 0;
    for (int pass = 0; pass < m_cfg.passes; ++pass) {
        if (dTo <= dFrom) break;
        // Refit each pass: once the worst clicks are gone the model fits the
        // music better, which is the whole point of running more than one.
        fitModel(0, fitTo);
        if (pass > 0 && m_cfg.rerepair > 0) rerepairRuns();
        // The margin below the block only feeds the noise estimate, which
        // pass 0 alone refreshes; interpolate() never reads below m_pad.
        detect(pass == 0 ? dFrom : from, dTo, pass);
        // Only commit repairs whose burst STARTS inside this block; anything
        // starting later belongs to the next block, which will see it with
        // full context on both sides.
        interpolate(m_pad, (m_pad + blk < dTo) ? (m_pad + blk) : dTo, dTo);
    }
    if (m_cfg.rerepair > 1 && m_nruns > 0) {
        fitModel(0, fitTo);
        rerepairRuns();
    }
}

void Channel::pushOne(double v) {
    const int blk = (int)kBlock;
    const int capacity = 2 * m_pad + blk;

    if (!finite(v)) v = 0.0;
    m_win[(size_t)m_fill] = v;
    m_dry[(size_t)m_fill] = v;
    m_full[(size_t)m_fill] = v;
    m_flag[(size_t)m_fill] = 0;
    m_strong[(size_t)m_fill] = 0;
    m_flAcc[(size_t)m_fill + kBlock] = 0.0;
    ++m_fill;
    ++m_seen;

    if (m_fill < capacity) return;

    m_bStrong = 0;
    m_cBlk = 0;
    processBlock();
    // After a warm start, the carpet key stands still over the blocks the
    // warm pass counted; see beginWarm().
    const bool fresh = ++m_blocks > m_warmBlocks;
    // A warm pass with the crackle floor stage off needs nothing past the
    // repair: the carpet key is all it keeps.
    const bool back = !m_warming || m_cfg.crackleFloor;

    // The body stage's key: strong runs in this block, their rate over the
    // last bodyRateWin seconds, the history of that rate, and of the key.
    if (!m_bCount.empty()) {
        m_bCount[(size_t)m_bCountPos] = m_bStrong;
        if (++m_bCountPos >= (int)m_bCount.size()) m_bCountPos = 0;
        int sum = 0;
        for (size_t k = 0; k < m_bCount.size(); ++k) sum += m_bCount[k];
        m_bRate[(size_t)m_bRatePos] =
            (double)sum * m_cfg.rate / ((double)m_bCount.size() * (double)blk);
        if (++m_bRatePos >= (int)m_bRate.size()) m_bRatePos = 0;
        if (m_bRateHave < (int)m_bRate.size()) ++m_bRateHave;
        m_bKey[(size_t)m_bKeyPos] = bodyKey();
        if (++m_bKeyPos >= (int)m_bKey.size()) m_bKeyPos = 0;
        m_bBeta = bodyBeta();
    }
    if (m_cfg.carpet) {
        if (fresh) carpetBlock();
        if (m_c.key > 0.0) ++m_cOpen;
    }

    const int Q = m_cfg.flankStage;
    if (Q > 0 && back) {
        // The flank repair, which can reach back into the block held back;
        // then that block goes to the body stage with its corrections, under
        // the key as it stood when it was repaired, so the body and fine
        // stages see what they would have seen without the hold, a block
        // later. This block is held in its place.
        bandwidthBlock();
        flankPass();
        const double beta = m_bBeta;
        if (m_stHave) {
            m_bBeta = m_bBetaPrev;
            const double * acc = &m_flAcc[(size_t)m_pad];   // window index pad - kBlock
            for (int i = 0; i < blk; ++i)
                bodyPush(m_stY[(size_t)i] + acc[i], m_stD[(size_t)i], m_stS[(size_t)i]);
            m_bBeta = beta;
        }
        for (int i = 0; i < blk; ++i) {
            m_stY[(size_t)i] = m_win[(size_t)(m_pad + i)];
            m_stD[(size_t)i] = m_dry[(size_t)(m_pad + i)];
            m_stS[(size_t)i] = m_strong[(size_t)(m_pad + i)];
        }
        m_stHave = true;
        m_bBetaPrev = beta;
    } else if (back) {
        for (int i = 0; i < blk; ++i) {
            bodyPush(m_win[(size_t)(m_pad + i)], m_dry[(size_t)(m_pad + i)],
                     m_strong[(size_t)(m_pad + i)]);
        }
    }

    // Slide along by one block, keeping the repairs that reached past its end.
    const size_t keep = (size_t)(m_fill - blk);
    memmove(&m_win[0], &m_win[(size_t)blk], sizeof(double) * keep);
    memmove(&m_dry[0], &m_dry[(size_t)blk], sizeof(double) * keep);
    memmove(&m_full[0], &m_full[(size_t)blk], sizeof(double) * keep);
    memmove(&m_flag[0], &m_flag[(size_t)blk], sizeof(uint8_t) * keep);
    memmove(&m_strong[0], &m_strong[(size_t)blk], sizeof(uint8_t) * keep);
    if (Q > 0) {
        memmove(&m_flAcc[0], &m_flAcc[(size_t)blk], sizeof(double) * (keep + (size_t)blk));
        m_flEnd = (m_flEnd > -(1 << 30) + blk) ? m_flEnd - blk : -(1 << 30);
    }
    m_fill -= blk;
    m_fitLen = 0;                 // the next fit is of a different window
    m_silentUntil = (m_silentUntil > blk) ? (m_silentUntil - blk) : 0;
    if (m_tailFrom < (1 << 30)) m_tailFrom -= blk;
}

void Channel::emitMixed(double y, double d) {
    if (m_warming) return;      // a warm pass's output is held back
    const double wet = m_cfg.wet;
    if (m_cfg.delta) {
        outPush(wet * (d - y));             // d - (wet * y + dry * d)
    } else {
        outPush(wet * y + (1.0 - wet) * d);
    }
}

//! Every sample comes out Config::bodyLatency samples after it went in. The
//! removal for it is final by then: the last frame covering it is cut when
//! the frame two hops later arrives.
void Channel::bodyPush(double y, double d, uint8_t strong) {
    if (!m_cfg.body) {
        finishSample(y, d);
        return;
    }
    const uint64_t t = m_bT++;
    const size_t i = (size_t)t & m_bMask;
    m_bY[i] = y;
    m_bD[i] = d;
    m_bR[i] = 0.0;              // no frame has reached it yet
    m_fR[i] = 0.0;
    {                           // the mid stage's run flag for this hop
        uint8_t & r = m_dRun[(size_t)(t >> m_fHShift) & m_dRunMask];
        r = ((t & (uint64_t)(m_fH - 1)) == 0) ? strong : (uint8_t)(r | strong);
    }
    const uint64_t N = (uint64_t)m_bN;
    if (t + 1 == m_bNextHop) {
        m_bNextHop += (uint64_t)m_bH;
        bodyHop(t + 1 - N);
    }
    if (t + 1 == m_fNextHop) {
        m_fNextHop += (uint64_t)m_fH;
        if (m_cfg.fine) fineHop(t + 1 - (uint64_t)m_fN);
        else m_fHist = 0;
    }
    // The micro stage runs m_mLag behind: on the samples whose fine removal
    // is final. Its count advances whether or not it is on, as the others do.
    if (t >= m_mLag) {
        const uint64_t tm = t - m_mLag;
        if (tm + 1 == m_mNextHop) {
            m_mNextHop += (uint64_t)m_mH;
            if (m_cfg.micro && m_cfg.fine) microHop(tm + 1 - (uint64_t)m_mN);
            else m_mHist = 0;
        }
    }
    const uint64_t D = (uint64_t)m_cfg.bodyLatency;
    if (t >= D) {
        const size_t u = (size_t)(t - D) & m_bMask;
        finishSample(m_bY[u] - m_bR[u], m_bD[u]);
    }
}

void Channel::finishSample(double y, double d) {
    if (m_cfg.crackleFloor) crackleFloorPush(y, d);
    else emitMixed(y, d);
}

//! Every sample comes out Config::crackleFloorLatency samples after it went in,
//! as in bodyPush(): the last frame covering it is cut two hops later.
void Channel::crackleFloorPush(double y, double d) {
    const uint64_t t = m_nT++;
    const size_t i = (size_t)t & m_nMask;
    m_nY[i] = y;
    m_nD[i] = d;
    m_nR[i] = 0.0;
    if (t + 1 == m_nNextHop) {
        m_nNextHop += (uint64_t)m_nH;
        crackleFloorHop(t + 1 - (uint64_t)m_nN);
    }
    const uint64_t D = (uint64_t)m_cfg.crackleFloorLatency;
    if (t >= D) {
        const size_t u = (size_t)(t - D) & m_nMask;
        emitMixed(m_nY[u] - m_nR[u], m_nD[u]);
    }
}

//! Takes `out` from the n sorted values at m, which close up behind it.
static void sortedTake(double * m, int n, double out) {
    const int j = (int)(std::lower_bound(m, m + n, out) - m);
    memmove(m + j, m + j + 1, (size_t)(n - 1 - j) * sizeof(double));
}

//! The newest frame starts at sample `start` of the stage's input; the one cut
//! is two hops before it, the middle of the five whose power the gain is
//! averaged over. Transforms are packed as in fineHop(), and the band reaches
//! the Nyquist bin as that stage's can. Under the key the fine stage follows.
//! Where it is shut nothing is cut, and the output is the input to the bit.
//! In the stream's first two seconds nothing is transformed there either: the
//! floor's history waits, and the five frames start again when the key opens,
//! so a record's start is cut as it was. From then on the history goes on
//! under a shut key at a quarter of the cost: of every kShutCycle hops the
//! first five are transformed and their power taken once, standing for the
//! kShutCycle / kFloorDec values the open key would have taken (most of the
//! cost is the sorted history, not the transforms). Only what is taken out is
//! transformed back. See Config::crackleFloor.
void Channel::crackleFloorHop(uint64_t start) {
    const double key = dmax(m_bBeta, m_c.key);
    const bool open = key > 0.0;
    const bool late = (double)start >= 2.0 * m_cfg.rate;
    const int W = m_cfg.crackleFloorHist;
    const int nt = m_nTop;
    bool shutTake = false;
    if (!open) {
        if (m_nOpen) {
            // The floor as the cut last used it, for the next opening.
            if (m_nHave > 0) {
                const bool kept = m_nKeep && m_nSinceOpen < (uint64_t)W;
                for (int b = 0; b < nt; ++b) {
                    const double f = m_nFloor[(size_t)b];
                    m_nSaved[(size_t)b] = kept ? dmin(f, m_nSaved[(size_t)b]) : f;
                }
                m_nHasSaved = true;
            }
            m_nOpen = false;
            m_nShutHops = 0;
        }
        const uint64_t ph = late ? m_nShutHops++ % (uint64_t)kShutCycle : (uint64_t)kShutCycle;
        if (ph >= (uint64_t)kFloorFrames) {
            m_nHist = 0;           // the frames have a hole; start them again
            return;
        }
        shutTake = ph == (uint64_t)kFloorFrames - 1;
    } else if (!m_nOpen) {
        m_nOpen = true;
        m_nKeep = late && m_nHasSaved;
        m_nSinceOpen = 0;
    }
    const int N = m_nN, H = m_nH, M = N / 2, nb = m_nNb, m0 = m_nM0;
    const size_t mask = m_nMask, nbs = (size_t)nb;
    double * re = &m_nRe[0];
    double * im = &m_nIm[0];
    const double * win = &m_nWin[0];
    const int * rev = &m_nRev[0];
    const double * twRe = &m_nTwRe[0];
    const double * twIm = &m_nTwIm[0];
    {
        const double * y = &m_nY[0];
        for (int i = 0; i < M; ++i) {
            const size_t at = (size_t)(start + 2 * (uint64_t)i);
            re[rev[i]] = y[at & mask] * win[2 * i];
            im[rev[i]] = y[(at + 1) & mask] * win[2 * i + 1];
        }
    }
    fftRadix2(re, im, M, &m_nStRe[0], &m_nStIm[0], false, g_kernels->fftPair, g_kernels->fftOne);
    const int slot = m_nSlot;
    double * P = &m_nP[(size_t)slot * nbs];
    double * xr = &m_nXRe[(size_t)slot * nbs];
    double * xi = &m_nXIm[(size_t)slot * nbs];
    for (int b = 0; b < nb; ++b) {
        // X[k] = E + e^(-2 pi i k / N) O, as in fineHop(); the Nyquist bin
        // folds onto Z[0].
        const int k = m0 + b;
        const int kk = k & (M - 1), mk = (M - kk) & (M - 1);
        const double zr = re[kk], zi = im[kk];
        const double cr = re[mk], ci = -im[mk];
        const double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
        const double our = 0.5 * (zi - ci), oui = -0.5 * (zr - cr);
        const double c = twRe[k], sn = twIm[k];
        const double vr = er + c * our + sn * oui;
        const double vi = ei + c * oui - sn * our;
        xr[b] = vr;
        xi[b] = vi;
        P[b] = vr * vr + vi * vi;
    }
    // The power over 7 bins, nearest at the Nyquist end (and at DC, which only
    // a very low rate reaches); from the bin below the band, whose gain the
    // 3-bin average at the band's edge reads, to the last followed bin.
    {
        double * Pb = &m_nPb[(size_t)slot * nbs];
        const int g0 = (m_nK0 - 1 - m0 > 0) ? m_nK0 - 1 - m0 : 0;
        g_kernels->smooth7(P, nb, m0, g0, nt, Pb);
    }
    m_nSlot = (slot + 1) % (int)kFloorFrames;
    if (m_nHist < (int)kFloorFrames) {
        ++m_nHist;
        if (m_nHist < (int)kFloorFrames) return;
    }

    // The cut frame c is the middle of the five: slot (slot - 2) mod 5.
    const double * Pq[kFloorFrames];
    const double * Bq[kFloorFrames];
    double frameEnergy = 0.0;
    for (int q = 0; q < (int)kFloorFrames; ++q) {
        const int sl = (slot - q + (int)kFloorFrames) % (int)kFloorFrames;
        Pq[q] = &m_nP[(size_t)sl * nbs];
        Bq[q] = &m_nPb[(size_t)sl * nbs];
    }
    const int cs = (slot - 2 + (int)kFloorFrames) % (int)kFloorFrames;
    const double * cxr = &m_nXRe[(size_t)cs * nbs];
    const double * cxi = &m_nXIm[(size_t)cs * nbs];
    for (int b = 0; b < nb; ++b) frameEnergy += Pq[2][b];

    // The floor's history: every fourth cut frame (under a shut key, the fifth
    // of every kShutCycle hops), each bin's power averaged over the five
    // frames, into the bin's ring and its sorted copy - unless the frame is
    // silent, which carries no floor (a host's pre-roll is). The window leaves
    // out the stream's first second, and before two seconds are in, the first
    // half of what has been taken: a record that fades in - the TangoTunes
    // transfers do, over ~0.5 s from 30 dB under their crackle - would
    // otherwise hold the quantile under the floor until the fade was a tenth
    // of the window, 5 s in, and the stage would start that late. Once
    // crackleFloorHist takes follow that second, it is the last
    // crackleFloorHist. Takes are counted as the open key makes them, so a
    // value taken under a shut key stands for four; and only what was taken
    // in the first two seconds is ever left out.
    const bool take = (open ? (m_nCutCount % (uint64_t)kFloorDec) == 0 : shutTake) && frameEnergy > 0.0;
    const int g0 = (m_nK0 - 1 - m0 > 0) ? m_nK0 - 1 - m0 : 0;
    if (take) {
        m_nTaken += open ? 1 : (uint64_t)(kShutCycle / kFloorDec);
        if (!late) m_nEarly = m_nTaken;
        if (open) ++m_nSinceOpen;
        const int have = m_nHave, pos = m_nPos;
        const uint64_t perSec = (uint64_t)(m_cfg.rate / (double)(m_nH * (int)kFloorDec));
        uint64_t drop = (m_nTaken / 2 < perSec) ? m_nTaken / 2 : perSec;
        if (drop > m_nEarly) drop = m_nEarly;
        int L = (m_nTaken - drop > (uint64_t)W) ? W : (int)(m_nTaken - drop);
        if (L < 1) L = 1;
        // The values that fall out of the window, oldest first. Under the open
        // key that is none while the window grows, else one, as it was before
        // a shut key's values could be in it.
        const uint64_t after = m_nTaken - (uint64_t)L;
        const int old = (pos - have + W) % W;
        int out = 0;
        while (out < have && m_nAt[(size_t)((old + out) % W)] <= after) ++out;
        const int n = have - out + 1;
        int rank = (int)((double)n * m_cfg.crackleFloorQ);
        if (rank > n - 1) rank = n - 1;
        // One value leaves and one goes in, the same count of places held in
        // every bin: that is the tiled row kernel's case, and the row's own
        // pad makes its stride W + 2. Taking the windows one at a time cost
        // each of them the rank-to-move round trip - see sortedRowBase() -
        // over a window of crackleFloorHist places, 259 at 44.1 kHz.
        if (out == 1) {
            double * inv = &m_nIn[0];
            double * outv = &m_nOut[0];
            for (int b = g0; b < nt; ++b) {
                inv[b] = (Pq[0][b] + Pq[1][b] + Pq[2][b] + Pq[3][b] + Pq[4][b]) / 5.0;
                outv[b] = m_nRing[(size_t)b * (size_t)W + (size_t)old];
            }
            g_kernels->sortedRow(&m_nSorted[(size_t)g0 * (size_t)(W + 2) + 1],
                                 have, W + 2, nt - g0, outv + g0, inv + g0);
            for (int b = g0; b < nt; ++b) {
                m_nRing[(size_t)b * (size_t)W + (size_t)pos] = inv[b];
                m_nFloor[(size_t)b] =
                    m_nSorted[(size_t)b * (size_t)(W + 2) + 1 + (size_t)rank];
            }
        } else
        for (int b = g0; b < nt; ++b) {
            const double in = (Pq[0][b] + Pq[1][b] + Pq[2][b] + Pq[3][b] + Pq[4][b]) / 5.0;
            double * ring = &m_nRing[(size_t)b * (size_t)W];
            double * srt = &m_nSorted[(size_t)b * (size_t)(W + 2) + 1];   // a pad either side
            int held = have;
            for (int i = 0; i + 1 < out; ++i) sortedTake(srt, held--, ring[(old + i) % W]);
            if (out == 0) {
                int j = 0;
                while (j < held && srt[j] < in) ++j;
                memmove(srt + j + 1, srt + j, (size_t)(held - j) * sizeof(double));
                srt[j] = in;
            } else {
                sortedReplace(srt, held, ring[(old + out - 1) % W], in);
            }
            ring[pos] = in;
            m_nFloor[(size_t)b] = srt[rank];
        }
        m_nAt[(size_t)pos] = m_nTaken;
        m_nPos = (pos + 1 < W) ? pos + 1 : 0;
        m_nHave = n;
    }
    if (!open) return;     // a shut key's frames only feed the history
    if (m_warming) {       // and so do a warm pass's, counted as the cut counts them
        ++m_nCutCount;
        return;
    }

    // After a shut stretch, until the window has been renewed under the open
    // key, the floor is no higher than it stood when the key shut: one taken
    // under the open key is the crackle's, the shut stretch's may be the
    // music's. Not in the first two seconds, which are cut as before.
    const bool kept = m_nKeep && m_nSinceOpen < (uint64_t)W;
    // And after a warm start, over the seconds the warm pass saw, no higher
    // than the floor that pass ended with; see beginWarm().
    const bool capped = start + (uint64_t)N <= m_nWarmT;
    const double * Fl = &m_nFloor[0];
    const double * Sv = &m_nSaved[0];
    const double * Wm = &m_nWarm[0];
    // The gain: Wiener's, on the power over 7 bins x 5 frames against beta x
    // the floor, never below crackleFloorMin; it rises at once and falls back
    // with the release.
    {
        const FloorGain p = { { Bq[0], Bq[1], Bq[2], Bq[3], Bq[4] }, Fl, Sv, Wm,
                              kept, capped, m_nCutCount == 0,
                              m_cfg.crackleFloorBeta, m_cfg.crackleFloorMin, m_nRel };
        g_kernels->floorGain(p, g0, nt, m_nG.data());
    }
    ++m_nCutCount;

    // The cut, scaled by the key, where its weight reaches.
    memset(re, 0, (size_t)M * sizeof(double));
    memset(im, 0, (size_t)M * sizeof(double));
    bool any = false;
    const double * G = &m_nG[0];
    const double * Wf = &m_nWf[0];
    for (int b = m_nK0 - m0; b < nb; ++b) {
        double cut = 0.0;
        if (Wf[b] > 0.0) {
            // The gain averaged over 3 bins, nearest at the last followed bin.
            const double gs = (G[b - 1] + G[b] + G[(b + 1 < nt) ? b + 1 : b]) / 3.0;
            cut = (1.0 - gs) * Wf[b];
        }
        cut *= key;
        if (!(cut > 0.0)) continue;
        const int k = m0 + b;
        packBin(re, im, rev, M, k, cut * cxr[b], cut * cxi[b], twRe[k], twIm[k]);
        any = true;
    }
    if (!any) return;
    ++m_nCuts;
    fftRadix2(re, im, M, &m_nStRe[0], &m_nStIm[0], true, g_kernels->fftPair, g_kernels->fftOne);
    const double g = 1.0 / ((double)N * m_nNorm);
    double * r = &m_nR[0];
    const uint64_t from = start - 2 * (uint64_t)H;
    for (int i = 0; i < M; ++i) {
        const size_t at = (size_t)(from + 2 * (uint64_t)i);
        r[at & mask]       += re[i] * win[2 * i] * g;
        r[(at + 1) & mask] += im[i] * win[2 * i + 1] * g;
    }
}

double Channel::bodyBeta() const {
    double beta = 0.0;
    for (size_t k = 0; k < m_bKey.size(); ++k) beta = dmax(beta, m_bKey[k]);
    return beta;
}

double Channel::bodyKey() const {
    // A stream's first half second has less history than the hold, and the
    // ring's zeros would keep the key shut through it - often the opening
    // note of a record. So it is the minimum over what has been seen, once
    // that is a whole rate window; the counts themselves start empty.
    if (m_bRateHave < (int)m_bCount.size()) return 0.0;
    double key = kAbsurd;
    for (int i = 0; i < m_bRateHave; ++i) {
        int k = m_bRatePos - 1 - i;
        if (k < 0) k += (int)m_bRate.size();
        key = dmin(key, m_bRate[(size_t)k]);
    }
    const double span = m_cfg.bodyRate1 - m_cfg.bodyRate0;
    if (!(span > 0.0)) return key >= m_cfg.bodyRate1 ? 1.0 : 0.0;
    return dmin(dmax((key - m_cfg.bodyRate0) / span, 0.0), 1.0);
}

//! This block's runs at carpetPeak sigma into the second being counted. Once
//! that has spanned a second of samples, its rate per 44.1k samples goes into
//! the ring, and the key is the ring's carpetQ quantile, linear between the two
//! values either side, on the ramp. Twenty numbers sorted once a second, on the
//! stack.
void Channel::carpetBlock() {
    m_c.runs += m_cBlk;
    m_c.samp += (double)kBlock;
    m_c.totRuns += m_cBlk;
    m_c.totSamp += (double)kBlock;
    // The Poisson key's bins: each finished bin's count into the sums, the
    // second's and the stream's.
    if (m_cfg.carpetFano) {
        m_c.bin += (double)m_cBlk;
        if (++m_c.binBlk >= m_cfg.fanoBlocks) {
            m_c.s1 += m_c.bin;
            m_c.s2 += m_c.bin * m_c.bin;
            m_c.binN += 1.0;
            m_c.totS1 += m_c.bin;
            m_c.totS2 += m_c.bin * m_c.bin;
            m_c.totBinN += 1.0;
            m_c.bin = 0.0;
            m_c.binBlk = 0;
        }
    }
    // Before carpetMin seconds are in, from carpetFirst on: the rate over
    // everything seen so far, against the thresholds at their warmest; and
    // from fanoFirst on, the runs' Fano factor over it too.
    if (m_cfg.carpetFirst > 0.0 && m_c.have < m_cfg.carpetMin
        && m_c.totSamp >= m_cfg.carpetFirst * m_cfg.rate) {
        const double K = m_c.totRuns * 44100.0 / m_c.totSamp;
        const double wf = 1.0 + m_cfg.carpetWarm;
        double key = carpetRamp(K, wf);
        if (m_c.totSamp >= m_cfg.fanoFirst * m_cfg.rate)
            key = dmax(key, fanoKey(K, fanoOf(m_c.totS1, m_c.totS2, m_c.totBinN), wf));
        m_c.key = key;
    }
    if (m_c.samp < m_cfg.rate) return;
    const int W = m_cfg.carpetWin;
    m_c.ring[m_c.pos] = m_c.runs * 44100.0 / m_c.samp;
    m_c.fRing[m_c.pos] = fanoOf(m_c.s1, m_c.s2, m_c.binN);
    if (++m_c.pos >= W) m_c.pos = 0;
    if (m_c.have < W) ++m_c.have;
    m_c.runs = 0.0;
    m_c.samp = 0.0;
    m_c.s1 = m_c.s2 = m_c.binN = 0.0;
    if (m_c.have < m_cfg.carpetMin) return;
    double v[kCarpetMax];
    for (int k = 0; k < m_c.have; ++k) v[k] = m_c.ring[k];
    std::sort(v, v + m_c.have);
    const double qp = m_cfg.carpetQ * (double)(m_c.have - 1);
    const int lo = (int)floor(qp);
    const double fr = qp - lo;
    const double K = (lo + 1 < m_c.have) ? v[lo] + fr * (v[lo + 1] - v[lo]) : v[lo];
    // Less history, more evidence asked: the thresholds start carpetWarm
    // higher and come down to their own as the ring fills.
    const double wf = 1.0 + m_cfg.carpetWarm * (double)(W - m_c.have) / (double)W;
    double key = carpetRamp(K, wf);
    if (m_cfg.carpetFano) {
        // The median of the seconds' Fano factors, as K is taken of the rates.
        for (int k = 0; k < m_c.have; ++k) v[k] = m_c.fRing[k];
        std::sort(v, v + m_c.have);
        const double fp = 0.5 * (double)(m_c.have - 1);
        const int fl = (int)floor(fp);
        const double ff = fp - fl;
        const double F = (fl + 1 < m_c.have) ? v[fl] + ff * (v[fl + 1] - v[fl]) : v[fl];
        key = dmax(key, fanoKey(K, F, wf));
    }
    m_c.key = key;
}

double Channel::carpetRamp(double K, double wf) const {
    const double span = wf * (m_cfg.carpetRate1 - m_cfg.carpetRate0);
    if (!(span > 0.0)) return K >= wf * m_cfg.carpetRate1 ? 1.0 : 0.0;
    return dmin(dmax((K - wf * m_cfg.carpetRate0) / span, 0.0), 1.0);
}

//! A second with no runs at all has no Fano factor; it reads as far from
//! Poisson, which keeps the key shut on silence.
double Channel::fanoOf(double s1, double s2, double n) {
    const double mu = (n > 0.0) ? s1 / n : 0.0;
    return (mu > 0.0) ? (s2 / n - mu * mu) / mu : 1e9;
}

double Channel::fanoKey(double K, double F, double wf) const {
    if (!m_cfg.carpetFano) return 0.0;
    const double ps = wf * (m_cfg.fanoRate1 - m_cfg.fanoRate0);
    const double pr = (ps > 0.0) ? dmin(dmax((K - wf * m_cfg.fanoRate0) / ps, 0.0), 1.0)
                                 : (K >= wf * m_cfg.fanoRate1 ? 1.0 : 0.0);
    const double fs = m_cfg.fano0 - m_cfg.fano1;
    const double pf = (fs > 0.0) ? dmin(dmax((m_cfg.fano0 - F) / fs, 0.0), 1.0)
                                 : (F <= m_cfg.fano1 ? 1.0 : 0.0);
    return pr * pf;
}

//! The newest frame starts at sample `start` of the stage's input, and the
//! one cut here two hops before it. The cut frame's median over
//! time needs the two frames either side, which is why the newest is
//! transformed now and the old one cut: the rings hold all five.
//!
//! The N real samples are transformed as N/2 complex ones, even samples in the
//! real part and odd in the imaginary, and only the bins the cut reads are
//! unpacked. Back the other way the band and its mirror are packed into N/2
//! points the same way.
//!
//! Only the band is touched, and only what is taken out is transformed back,
//! so where the key is shut the output is the input to the bit.
void Channel::bodyHop(uint64_t start) {
    const int N = m_bN, H = m_bH, M = N / 2;
    const size_t mask = m_bMask;
    const int nb = m_bM1 - m_bM0 + 1;
    const int nk = m_bK1 - m_bK0 + 1;
    const double beta = (nk > 0) ? m_bBeta : 0.0;
    const double * twRe = &m_bTwRe[0];
    const double * twIm = &m_bTwIm[0];

    if (beta <= 0.0) {
        m_bHist = 0;           // the history has a hole; start it again
    } else {
        // The newest frame.
        double * re = &m_bRe[0];
        double * im = &m_bIm[0];
        const double * y = &m_bY[0];
        const double * win = &m_bWin[0];
        const int * rev = &m_bRev[0];
        for (int i = 0; i < M; ++i) {
            const size_t at = (size_t)(start + 2 * (uint64_t)i);
            re[rev[i]] = y[at & mask] * win[2 * i];
            im[rev[i]] = y[(at + 1) & mask] * win[2 * i + 1];
        }
        fftRadix2(re, im, M, &m_bStRe[0], &m_bStIm[0], false, g_kernels->fftPair, g_kernels->fftOne);
        // X[k] = E + e^(-2 pi i k / N) O, with E and O the transforms of the
        // even and odd samples: (Z[k] + Z*[M-k]) / 2 and (Z[k] - Z*[M-k]) / 2i.
        const int slot = m_bSlot;
        double * mag = &m_bMag[(size_t)slot * (size_t)nb];
        double * xr = &m_bXRe[(size_t)slot * (size_t)nk];
        double * xi = &m_bXIm[(size_t)slot * (size_t)nk];
        for (int k = m_bM0; k <= m_bM1; ++k) {
            const double zr = re[k], zi = im[k];
            const double cr = re[M - k], ci = -im[M - k];
            const double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
            const double our = 0.5 * (zi - ci), oui = -0.5 * (zr - cr);
            const double c = twRe[k], sn = twIm[k];
            const double vr = er + c * our + sn * oui;
            const double vi = ei + c * oui - sn * our;
            mag[k - m_bM0] = sqrt(vr * vr + vi * vi);
            if (k >= m_bK0 && k <= m_bK1) { xr[k - m_bK0] = vr; xi[k - m_bK0] = vi; }
        }
        m_bSlot = (slot + 1) % (int)kBodyFrames;
        if (m_bHist < (int)kBodyFrames) ++m_bHist;

        if (m_bHist >= (int)kBodyFrames) {
            // Frame j - q lives at (slot - q) mod 5; the one cut is q = 2.
            const double * f[kBodyFrames];
            for (int q = 0; q < (int)kBodyFrames; ++q) {
                const int sl = (slot - q + (int)kBodyFrames) % (int)kBodyFrames;
                f[q] = &m_bMag[(size_t)sl * (size_t)nb];
            }
            const int cs = (slot - 2 + (int)kBodyFrames) % (int)kBodyFrames;
            const double * cr = &m_bXRe[(size_t)cs * (size_t)nk];
            const double * ci = &m_bXIm[(size_t)cs * (size_t)nk];
            const double cap = 1.0 - m_cfg.bodyFloor;
            const int w = 2 * m_bHalf + 1;
            // memset, not a loop: a zero double is all zero bits, and this
            // is a thousand of them a hop.
            memset(re, 0, (size_t)M * sizeof(double));
            memset(im, 0, (size_t)M * sizeof(double));
            bool any = false;
            for (int k = m_bK0; k <= m_bK1; ++k) {
                const int b = k - m_bM0;
                // Tonal: the louder of the medians before and after.
                const double h = dmax(median3(f[4][b], f[3][b], f[2][b]),
                                      median3(f[2][b], f[1][b], f[0][b]));
                // Non-tonal: the median across ~370 Hz of the frame cut. The
                // window slides one bin a step, so it is kept sorted and one
                // value displaced - the same median a fresh selection finds,
                // for a quarter of the time. sortedReplace() finds where the
                // value leaving sits by counting what is below it, which is
                // exact: the window holds its own copies of these magnitudes.
                double * m = &m_bMed[1];        // a pad either side
                if (k == m_bK0) {
                    for (int t = 0; t < w; ++t) m[t] = f[2][b - m_bHalf + t];
                    std::sort(m, m + w);
                } else {
                    sortedReplace(m, w, f[2][b - 1 - m_bHalf], f[2][b + m_bHalf]);
                }
                const double p = m[m_bHalf];
                const double h2 = h * h, p2 = p * p;
                double cut = p2 / (h2 + p2 + 1e-30);
                if (cut > cap) cut = cap;
                cut *= beta;
                if (!(cut > 0.0)) continue;
                packBin(re, im, rev, M, k, cut * cr[k - m_bK0], cut * ci[k - m_bK0],
                        twRe[k], twIm[k]);
                any = true;
            }
            if (any) {
                fftRadix2(re, im, M, &m_bStRe[0], &m_bStIm[0], true, g_kernels->fftPair, g_kernels->fftOne);
                const double g = 1.0 / ((double)N * m_bNorm);
                double * r = &m_bR[0];
                const uint64_t from = start - 2 * (uint64_t)H;
                for (int i = 0; i < M; ++i) {
                    const size_t at = (size_t)(from + 2 * (uint64_t)i);
                    r[at & mask]       += re[i] * win[2 * i] * g;
                    r[(at + 1) & mask] += im[i] * win[2 * i + 1] * g;
                }
            }
        }
    }

}

enum { kFrameMax = 64 };

//! The quantile at p of each of nb bins over the n frames ending at the one in
//! `slot` of a ring of S frames of nb values each: straight from the frames
//! where the kernels can, into q. False where they cannot, which leaves it to
//! the sorted windows. See frameSelectAvx2().
static bool frameQuantile(const std::vector<double> & ring, int S, size_t slot, int n,
                          int nb, const QuantilePos & p, double * q) {
    if (!g_kernels->frameQuantile || n > (int)kFrameMax) return false;
    const double * fr[kFrameMax];
    size_t s = slot;
    for (int j = 0; j < n; ++j) {           // back a frame a step, not a remainder
        fr[j] = &ring[s * (size_t)nb];
        s = (s == 0) ? (size_t)S - 1 : s - 1;
    }
    return g_kernels->frameQuantile(fr, n, nb, p, q);
}

//! The quantile at p of each of nb bins over the n frames ending at frame f,
//! in `slot` of a ring of S frames, into q: by frameQuantile() where the
//! kernels can, else from `srt`, each bin's window kept sorted. That is
//! rebuilt when the frames start again (`restart`) or the kernels had the
//! last one, and otherwise moved on a frame by sortedRow(); `live` says
//! whether it holds the window that ended at the frame before.
static void windowQuantile(const std::vector<double> & ring, int S, size_t slot, uint64_t f,
                           int n, int nb, const QuantilePos & p, bool restart,
                           bool & live, double * srt, double * q) {
    const size_t nbs = (size_t)nb;
    if (frameQuantile(ring, S, slot, n, nb, p, q)) {
        live = false;                   // the windows have fallen behind
    } else if (restart || !live) {
        live = true;
        for (int j = 0; j < n; ++j) {
            const double * pj = &ring[(size_t)((f - (uint64_t)j) % (uint64_t)S) * nbs];
            for (int b = 0; b < nb; ++b) srt[(size_t)b * (size_t)n + (size_t)j] = pj[b];
        }
        for (int b = 0; b < nb; ++b) {
            double * w = srt + (size_t)b * (size_t)n;
            std::sort(w, w + n);
            q[b] = sortedQuantile(w, p);
        }
    } else {
        const double * gone = &ring[(size_t)((f - (uint64_t)n) % (uint64_t)S) * nbs];
        g_kernels->sortedRow(srt, n, n, nb, gone, &ring[slot * nbs]);
        for (int b = 0; b < nb; ++b)
            q[b] = sortedQuantile(srt + (size_t)b * (size_t)n, p);
    }
}

//! The newest frame starts at sample `start` of the stage's input, and the
//! one cut here m_fD hops before it: its background needs the fineM frames
//! either side, and the voice guard the m_fW either side. As in bodyHop(),
//! the N real samples are transformed as N/2 complex ones, even samples in
//! the real part and odd in the imaginary, and only the bins the cut reads
//! are unpacked; back the other way the band and its mirror are packed into
//! N/2 points the same way. Only the band is read back, and only what is
//! taken out is transformed back and added to m_bR, so where the key is shut
//! the output is the input to the bit. See Config::fine.
//!
//! The background of the cut frame c is the louder of two quantiles, over
//! frames c - fineM .. c - 1 and c + 1 .. c + fineM. Both are windows of
//! fineM frames, fineM + 1 apart, so a hop ranks one window, the one ending at
//! the newest frame: each bin's last fineM magnitudes are kept sorted, the
//! oldest moved out and the newest in, and the quantile read off and kept
//! until the cut frame comes round to it. The same values a fresh selection
//! over both finds.
//! How strongly an envelope ring repeats around frame c: the normalised
//! autocorrelation of c - W .. c + W less its mean, at its best lag in
//! lmin .. lmax, scaled for the lag's shorter overlap, as the prototype has
//! it. `seg` holds 2 W + 1. The window is consecutive in the ring, so one
//! remainder finds its start and the rest step: `slots` is not a power of
//! two, and the remainder of a uint64 by it is a hardware divide - at ~25
//! cycles and one per frame of the window, that was the largest single cost
//! in the fine stage's guard, and most of its 100 cycles a sample.
static double envPeriodicity(const double * ring, size_t slots, uint64_t c, int W,
                             int lmin, int lmax, double * seg) {
    const int n = 2 * W + 1;
    double mean = 0.0;
    size_t ei = (size_t)((c - (uint64_t)W) % (uint64_t)slots);
    for (int i = 0; i < n; ++i) {
        seg[i] = ring[ei];
        mean += seg[i];
        if (++ei == slots) ei = 0;
    }
    mean /= (double)n;
    double den = 0.0;
    for (int i = 0; i < n; ++i) { seg[i] -= mean; den += seg[i] * seg[i]; }
    den += 1e-30;
    double best = -1.0;
    int lag = lmin;
    for (; lag + 1 <= lmax; lag += 2) {        // two lags at a time; see dot2Avx2()
        double d0, d1;
        g_kernels->dot2(seg, seg + lag, n - lag, seg, seg + lag + 1, n - lag - 1, &d0, &d1);
        const double v0 = d0 / den * (double)n / (double)(n - lag);
        if (v0 > best) best = v0;
        const double v1 = d1 / den * (double)n / (double)(n - lag - 1);
        if (v1 > best) best = v1;
    }
    if (lag <= lmax) {
        const double v = dot(seg, seg + lag, n - lag) / den * (double)n / (double)(n - lag);
        if (v > best) best = v;
    }
    return best;
}

void Channel::fineHop(uint64_t start) {
    const int N = m_fN, H = m_fH, S = m_fSlots, Mf = m_cfg.fineM;
    // The band: the mid stage's nd bins, then the fine stage's own.
    const int nd = m_dNb, nb = m_dNb + m_fNb, k0 = m_fK0 - m_dNb;
    const int D = m_fD, W = m_fW;
    const size_t mask = m_bMask;
    // The carpet key opens this stage, and only this one, where the body key
    // is shut; see Config::carpet.
    const double beta = (m_fNb > 0) ? dmax(m_bBeta, m_c.key) : 0.0;
    if (beta <= 0.0) {
        m_fHist = 0;           // the history has a hole; start it again
        return;
    }
    const int M = N / 2;
    double * re = &m_fRe[0];
    double * im = &m_fIm[0];
    const double * y = &m_bY[0];
    const double * win = &m_fWin[0];
    const int * rev = &m_fRev[0];
    const double * twRe = &m_fTwRe[0];
    const double * twIm = &m_fTwIm[0];
    for (int i = 0; i < M; ++i) {
        const size_t at = (size_t)(start + 2 * (uint64_t)i);
        re[rev[i]] = y[at & mask] * win[2 * i];
        im[rev[i]] = y[(at + 1) & mask] * win[2 * i + 1];
    }
    fftRadix2(re, im, M, &m_fStRe[0], &m_fStIm[0], false, g_kernels->fftPair, g_kernels->fftOne);
    const uint64_t f = m_fFrame++;
    const size_t nbs = (size_t)nb;
    const size_t slot = (size_t)(f % (uint64_t)S), sl = slot * nbs;
    {
        double * mg = &m_fMag[sl];
        double * xr = &m_fXRe[sl];
        double * xi = &m_fXIm[sl];
        double e = 0.0, ed = 0.0;
        g_kernels->binUnpack(re, im, M, k0, nb, twRe, twIm, xr, xi, mg);
        for (int b = 0; b < nb; ++b) {
            if (b < nd) ed += mg[b] * mg[b];
            else if (b - nd < m_fNbLow) e += mg[b] * mg[b];
        }
        // The guards' envelopes: the root of the energy below fineMid, and of
        // the mid stage's band.
        m_fE[(size_t)(f % (uint64_t)m_fESlots)] = sqrt(e);
        if (nd > 0) m_dE[(size_t)(f % (uint64_t)m_fESlots)] = sqrt(ed);
    }
    if (m_fHist < 2 * D + 1) ++m_fHist;
    if (m_fHist < Mf) return;

    // The window of the last fineM frames, f - fineM + 1 .. f, and its
    // quantile, into the row of frame f.
    const QuantilePos qp = quantilePos(Mf, m_cfg.fineQ);
    const uint64_t QS = (uint64_t)m_fQSlots;
    double * qNew = &m_fQ[(size_t)(f % QS) * nbs];
    windowQuantile(m_fMag, S, slot, f, Mf, nb, qp, m_fHist == Mf, m_fSortedLive,
                   &m_fSorted[1], qNew);       // one place of pad either side
    if (m_fHist < 2 * D + 1) return;

    // The cut frame c = f - D. After it, the window ending at c + fineM;
    // before it, the one that ended at c - 1.
    const uint64_t c = f - (uint64_t)D;
    const size_t cs = (size_t)(c % (uint64_t)S) * nbs;
    const double * cmag = &m_fMag[cs];
    const double * cxr = &m_fXRe[cs];
    const double * cxi = &m_fXIm[cs];
    const double * qBefore = &m_fQ[(size_t)((c - 1) % QS) * nbs];
    const double * qAfter = &m_fQ[(size_t)((c + (uint64_t)Mf) % QS) * nbs];

    // The voice guard: how strongly the envelope below fineMid repeats, over
    // c - W .. c + W, at its best lag - the normalised autocorrelation of the
    // window less its mean, scaled for the lag's shorter overlap, as the
    // prototype has it. The back-off is full at finePeriodic0, gone at 1.
    double keep = 0.0;          // how much of the cut below fineMid is withheld
    if (m_fNbLow > 0) {
        const double best = envPeriodicity(&m_fE[0], (size_t)m_fESlots, c, W, m_fLmin, m_fLmax, &m_fSeg[0]);
        const double span = m_cfg.finePeriodic1 - m_cfg.finePeriodic0;
        keep = (span > 0.0) ? (best - m_cfg.finePeriodic0) / span
                            : (best >= m_cfg.finePeriodic1 ? 1.0 : 0.0);
        keep = dmin(dmax(keep, 0.0), 1.0);
    }
    // The mid stage cuts only a frame that holds, or neighbours, a run
    // repaired at midPeak sigma or more: the hops from the one before the
    // frame to the one after it. Then it backs off as its band's envelope
    // repeats, as the fine stage does below fineMid. See Config::mid.
    bool gate = false;
    double keepD = 1.0;
    if (nd > 0) {
        const uint64_t from = start - (uint64_t)D * (uint64_t)H;
        const uint64_t h0 = from >> m_fHShift, h1 = h0 + (uint64_t)(N >> m_fHShift);
        for (uint64_t h = (h0 > 0) ? h0 - 1 : 0; h <= h1 && !gate; ++h)
            gate = m_dRun[(size_t)h & m_dRunMask] != 0;
        if (gate) {
            const double best = envPeriodicity(&m_dE[0], (size_t)m_fESlots, c, W, m_fLmin, m_fLmax, &m_fSeg[0]);
            const double span = m_cfg.midPeriodic1 - m_cfg.midPeriodic0;
            keepD = (span > 0.0) ? (best - m_cfg.midPeriodic0) / span
                                 : (best >= m_cfg.midPeriodic1 ? 1.0 : 0.0);
            keepD = dmin(dmax(keepD, 0.0), 1.0);
        }
    }
    // In the body stage's band it yields to that stage by the body key.
    const double yieldD = m_cfg.midYield ? 1.0 - m_bBeta : 1.0;
    // The micro stage's frames near this one back off as it does. Kept by the
    // cut frame's place on the hop grid, not by its number: frames are only
    // numbered while the key is open.
    {
        const uint64_t gpos = (start - (uint64_t)D * (uint64_t)H) / (uint64_t)H;
        m_fKeep[(size_t)gpos & m_fKeepMask] = keep;
        m_fKeepAt[(size_t)gpos & m_fKeepMask] = gpos + 1;
    }

    const double alpha = m_cfg.fineAlpha, floorG = m_cfg.fineFloor;
    memset(re, 0, (size_t)M * sizeof(double));
    memset(im, 0, (size_t)M * sizeof(double));
    bool any = false, anyD = false;
    for (int b = 0; b < nb; ++b) {
        if (b < nd && !gate) continue;
        const double h = dmax(qBefore[b], qAfter[b]);
        double g = alpha * h / (cmag[b] + 1e-20);
        if (g > 1.0) g = 1.0;
        if (g < floorG) g = floorG;
        double cut;
        if (b < nd) {
            g = 1.0 - (1.0 - keepD) * (1.0 - g);
            cut = (1.0 - g) * beta * ((b < m_dYieldNb) ? yieldD : 1.0);
        } else {
            if (b - nd < m_fNbLow) g = 1.0 - (1.0 - keep) * (1.0 - g);
            cut = (1.0 - g) * beta;
        }
        if (!(cut > 0.0)) continue;
        const int k = k0 + b;
        packBin(re, im, rev, M, k, cut * cxr[b], cut * cxi[b], twRe[k], twIm[k]);
        any = true;
        if (b < nd) anyD = true;
    }
    if (anyD) ++m_dCuts;
    if (!any) return;
    fftRadix2(re, im, M, &m_fStRe[0], &m_fStIm[0], true, g_kernels->fftPair, g_kernels->fftOne);
    // The packed bins above are twice what the inverse of M points wants, so
    // the 1/M it is missing is a 1/N here, as in bodyHop().
    const double g = 1.0 / ((double)N * m_fNorm);
    double * r = &m_bR[0];
    double * fr = &m_fR[0];
    const uint64_t from = start - (uint64_t)D * (uint64_t)H;
    for (int i = 0; i < M; ++i) {
        const size_t at = (size_t)(from + 2 * (uint64_t)i);
        const double v0 = re[i] * win[2 * i] * g, v1 = im[i] * win[2 * i + 1] * g;
        r[at & mask]        += v0;
        r[(at + 1) & mask]  += v1;
        fr[at & mask]       += v0;
        fr[(at + 1) & mask] += v1;
    }
}

//! The newest frame starts at sample `start` of the micro stage's count, the
//! body stage's less m_mLag, where the fine stage's removal is final; the one
//! cut here microM hops before it, between its two background windows. The
//! input is the fine stage's output, m_bY less m_fR, and what this takes out
//! goes to m_bR with the rest. Transforms, quantiles and the cut are as in
//! fineHop(), at a quarter of the frame and hop N/2; the guard is the fine
//! stage's, for the frame of its own nearest this one's centre, and where the
//! fine stage has not cut that frame this one is not cut either. See
//! Config::micro.
void Channel::microHop(uint64_t start) {
    const int N = m_mN, H = m_mH, nb = m_mNb, S = m_mSlots, Mm = m_cfg.microM;
    const int D = Mm;
    const size_t mask = m_bMask;
    const double beta = (nb > 0) ? dmax(m_bBeta, m_c.key) : 0.0;
    if (beta <= 0.0) {
        m_mHist = 0;
        return;
    }
    const int M = N / 2;
    double * re = &m_mRe[0];
    double * im = &m_mIm[0];
    const double * y = &m_bY[0];
    const double * fr = &m_fR[0];
    const double * win = &m_mWin[0];
    const int * rev = &m_mRev[0];
    const double * twRe = &m_mTwRe[0];
    const double * twIm = &m_mTwIm[0];
    for (int i = 0; i < M; ++i) {
        const size_t at = (size_t)(start + 2 * (uint64_t)i);
        re[rev[i]] = (y[at & mask] - fr[at & mask]) * win[2 * i];
        im[rev[i]] = (y[(at + 1) & mask] - fr[(at + 1) & mask]) * win[2 * i + 1];
    }
    fftRadix2(re, im, M, &m_mStRe[0], &m_mStIm[0], false, g_kernels->fftPair, g_kernels->fftOne);
    const uint64_t f = m_mFrame++;
    const size_t nbs = (size_t)nb;
    const size_t slot = (size_t)(f % (uint64_t)S), sl = slot * nbs;
    {
        double * mg = &m_mMag[sl];
        double * xr = &m_mXRe[sl];
        double * xi = &m_mXIm[sl];
        g_kernels->binUnpack(re, im, M, m_mK0, nb, twRe, twIm, xr, xi, mg);
    }
    if (m_mHist < 2 * D + 1) ++m_mHist;
    if (m_mHist < Mm) return;

    const QuantilePos qp = quantilePos(Mm, m_cfg.microQ);
    const uint64_t QS = (uint64_t)m_mQSlots;
    double * qNew = &m_mQ[(size_t)(f % QS) * nbs];
    windowQuantile(m_mMag, S, slot, f, Mm, nb, qp, m_mHist == Mm, m_mSortedLive,
                   &m_mSorted[1], qNew);       // one place of pad either side
    if (m_mHist < 2 * D + 1) return;

    const uint64_t c = f - (uint64_t)D;
    const uint64_t from = start - (uint64_t)D * (uint64_t)H;
    // The guard: the fine frame whose centre is nearest this frame's.
    double keep = 1.0;
    if (m_mNbLow > 0) {
        const int fN = m_fN, fH = m_fH;
        const uint64_t centre = from + (uint64_t)(N / 2);
        if (centre < (uint64_t)(fN / 2)) return;
        const uint64_t cf = (centre - (uint64_t)(fN / 2) + (uint64_t)(fH / 2)) / (uint64_t)fH;
        if (m_fKeepAt[(size_t)cf & m_fKeepMask] != cf + 1) return;   // the fine stage did not cut it
        keep = m_fKeep[(size_t)cf & m_fKeepMask];
    }
    const size_t cs = (size_t)(c % (uint64_t)S) * nbs;
    const double * cmag = &m_mMag[cs];
    const double * cxr = &m_mXRe[cs];
    const double * cxi = &m_mXIm[cs];
    const double * qBefore = &m_mQ[(size_t)((c - 1) % QS) * nbs];
    const double * qAfter = &m_mQ[(size_t)((c + (uint64_t)Mm) % QS) * nbs];

    const double alpha = m_cfg.microAlpha, floorG = m_cfg.fineFloor;
    memset(re, 0, (size_t)M * sizeof(double));
    memset(im, 0, (size_t)M * sizeof(double));
    bool any = false;
    for (int b = 0; b < nb; ++b) {
        const double h = dmax(qBefore[b], qAfter[b]);
        double g = alpha * h / (cmag[b] + 1e-20);
        if (g > 1.0) g = 1.0;
        if (g < floorG) g = floorG;
        if (b < m_mNbLow) g = 1.0 - (1.0 - keep) * (1.0 - g);
        const double cut = (1.0 - g) * beta;
        if (!(cut > 0.0)) continue;
        const int k = m_mK0 + b;
        packBin(re, im, rev, M, k, cut * cxr[b], cut * cxi[b], twRe[k], twIm[k]);
        any = true;
    }
    if (!any) return;
    ++m_mCuts;
    fftRadix2(re, im, M, &m_mStRe[0], &m_mStIm[0], true, g_kernels->fftPair, g_kernels->fftOne);
    const double g = 1.0 / ((double)N * m_mNorm);
    double * r = &m_bR[0];
    for (int i = 0; i < M; ++i) {
        const size_t at = (size_t)(from + 2 * (uint64_t)i);
        r[at & mask]       += re[i] * win[2 * i] * g;
        r[(at + 1) & mask] += im[i] * win[2 * i + 1] * g;
    }
}

template<typename Sample>
void Channel::push(const Sample * in, size_t frames, size_t stride) {
    for (size_t i = 0; i < frames; ++i) pushOne((double)in[i * stride]);
}

void Channel::outPush(double v) {
    if (m_outCount >= m_out.size()) growOut();
    size_t w = m_outHead + m_outCount;
    if (w >= m_out.size()) w -= m_out.size();
    m_out[w] = v;
    ++m_outCount;
}

//! Only reachable by pushing more than Config::maxBlock between pulls. Doubling
//! rather than growing by a block keeps it from happening repeatedly: the ring
//! ends up big enough for whatever the caller is actually doing and then stays
//! that way for the life of the Channel.
void Channel::growOut() {
    const size_t was = m_out.size();
    std::vector<double> bigger(was ? was * 2 : (size_t)kBlock * 4, 0.0);
    for (size_t i = 0; i < m_outCount; ++i) {
        size_t r = m_outHead + i;
        if (r >= was) r -= was;
        bigger[i] = m_out[r];
    }
    m_out.swap(bigger);
    m_outHead = 0;
}

template<typename Sample>
void Channel::pull(Sample * out, size_t frames, size_t stride) {
    const size_t cap = m_out.size();
    const size_t give = (frames < m_outCount) ? frames : m_outCount;
    for (size_t i = 0; i < give; ++i) {
        out[i * stride] = (Sample)m_out[m_outHead];
        if (++m_outHead >= cap) m_outHead = 0;
    }
    m_outCount -= give;
    for (size_t i = give; i < frames; ++i) out[i * stride] = (Sample)0;
}

void Channel::feedZeros() {
    for (int i = 0; i < m_cfg.latency; ++i) pushOne(0.0);
}

void Channel::drain() {
    if (m_tailFrom > m_fill) m_tailFrom = m_fill;
    feedZeros();
    // Every real sample is out. Whatever comes next follows silence.
    m_tailFrom = 1 << 30;
    m_silentUntil = m_fill;
}

template void Channel::push<float>(const float *, size_t, size_t);
template void Channel::push<double>(const double *, size_t, size_t);
template void Channel::pull<float>(float *, size_t, size_t);
template void Channel::pull<double>(double *, size_t, size_t);

} // namespace declick
