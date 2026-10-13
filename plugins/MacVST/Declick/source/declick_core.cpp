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

struct Kernels {
    FilterFn    filter;
    AutocorrFn  autocorr;
    const char * name;
};

void filterBase(const double * h, const double * x, double * out,
                int count, int taps) {
    for (int i = 0; i < count; ++i) out[i] = dot(h, x + i, taps);
}

void autocorrBase(const double * x, int n, double * r, int maxLag) {
    for (int d = 0; d <= maxLag; ++d) r[d] = dot(x + d, x, n - d);
}

const Kernels kBaseKernels = {
    filterBase, autocorrBase, DECLICK_SSE2 ? "sse2" : "scalar"
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

const Kernels kAvx2Kernels = { filterAvx2, autocorrAvx2, "avx2" };

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

const Kernels kNeonKernels = { filterNeon, autocorrNeon, "neon" };

#endif // DECLICK_NEON

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
    latency = pad + (int)kBlock;

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

bool bandedInverseDiagonal(const std::vector<double> & chol, int band, int n,
                           std::vector<double> & work, double * diag) {
    if (n <= 0 || diag == NULL) return false;
    if (band >= n) band = n - 1;                  // same clipping as the solve
    const int W = band + 1;
    if (chol.size() < (size_t)n * (size_t)W) return false;
    const double * L = &chol[0];

    // S holds the banded slice of the inverse: S(i,j), 0 <= j-i <= band, at
    // work[i*W + (j-i)]. Rows are filled bottom to top and, within a row,
    // right to left - the j == i entry is last because it is the one that
    // needs the rest of its own row.
    work.assign((size_t)n * (size_t)W, 0.0);
    double * S = &work[0];

    for (int i = n - 1; i >= 0; --i) {
        const double lii = L[(size_t)i * W];
        if (!(lii > 0.0) || !finite(lii)) return false;
        const int khi = (i + band < n - 1) ? (i + band) : (n - 1);

        for (int j = khi; j >= i; --j) {
            double s = (i == j) ? 1.0 / (lii * lii) : 0.0;
            for (int k = i + 1; k <= khi; ++k) {
                const double lki = L[(size_t)k * W + (k - i)] / lii;
                const double skj = (j >= k) ? S[(size_t)k * W + (j - k)]
                                            : S[(size_t)j * W + (k - j)];
                s -= lki * skj;
            }
            S[(size_t)i * W + (j - i)] = s;
        }

        const double v = S[(size_t)i * W];
        if (!(v > 0.0) || !finite(v)) return false;   // G is PD; this must hold
        diag[i] = v;
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
    m_flag.assign(need, 0);
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

    // The Wiener path only runs when wienerAlpha > 0, and its two buffers are
    // the largest things here - at 192 kHz they are 1.06 MB of a 1.3 MB total -
    // so there is no reason to hold them when it cannot run. interpolate()
    // short-circuits on wienerAlpha before it touches either.
    if (cfg.wienerAlpha > 0.0) {
        m_pvar.assign(bm + 1, 0.0);
        m_sinv.assign((bm + 1) * (bo + 1), 0.0);
    } else {
        m_pvar.clear();
        m_sinv.clear();
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

    // The output ring. Between one pull and the next a caller can push maxBlock
    // samples, which emit at most maxBlock + kBlock, on top of the up to kBlock
    // already waiting.
    const size_t ring = (size_t)cfg.maxBlock + 2 * (size_t)kBlock;
    if (m_out.size() != ring) m_out.assign(ring, 0.0);

    reset();
}

size_t Channel::heapBytes() const {
    return (m_win.capacity() + m_dry.capacity() + m_fwd.capacity()
            + m_bwd.capacity() + m_det.capacity() + m_a.capacity()
            + m_ra.capacity() + m_aa.capacity() + m_pvar.capacity() + m_sinv.capacity()
            + m_madRing.capacity() + m_scratch.capacity()
            + m_fitWin.capacity() + m_fitR.capacity() + m_delta.capacity()
            + m_corr.capacity()
            + m_aarev.capacity() + m_rhs.capacity() + m_solve.capacity()
            + m_out.capacity()) * sizeof(double)
           + m_flag.capacity() + m_runs.capacity() * sizeof(int);
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
    std::fill(m_win.begin(), m_win.end(), 0.0);
    std::fill(m_dry.begin(), m_dry.end(), 0.0);
    std::fill(m_flag.begin(), m_flag.end(), (uint8_t)0);
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
    m_silentUntil = 0;
    // Prime with `pad` zeros so the first real sample lands at window index
    // pad and nothing is dropped off the front of the stream.
    m_fill = m_pad;
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
        // but not from prime()'s zeros, or right after priming the median is
        // zero and the first real samples read as one enormous click.
        for (int i = (from > m_silentUntil) ? from : m_silentUntil; i < to; ++i) {
            m_madRing[(size_t)m_madPos] = fabs(m_fwd[i]);
            if (++m_madPos >= (int)m_madRing.size()) { m_madPos = 0; m_madFull = true; }
        }
        const size_t have = m_madFull ? m_madRing.size() : (size_t)m_madPos;
        if (have >= 16) {
            const double med = ringMedian(have) * 1.4826;
            m_scale = dmax(med, 1e-12);
        }
    }

    const double inv = 1.0 / m_scale;
    for (int i = from; i < to; ++i) {
        m_det[i] = dmax(fabs(m_fwd[i]), fabs(m_bwd[i])) * inv;
    }
}

void Channel::interpolate(int from, int to, int reach) {
    const double hi = m_cfg.thresholdHi;
    const double lo = m_cfg.thresholdLo;
    const int dip = m_cfg.dip;
    const double far = lo + m_cfg.dipFar * (hi - lo);

    // Never into the zeros prime() fed: they are not audio, and the first
    // onset after them reads as a click whose backward error reaches back
    // into them - a repair there writes a pre-echo where the stream has to
    // be silent.
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
        bool ok = repairRun(s, e, false);
        if (!ok && (s != s0 || e != e0)) {
            s = s0; e = e0;
            ok = repairRun(s, e, false);
        }
        i = e + 1;
        if (ok && m_cfg.rerepair > 0 &&
            m_nruns + 2 <= (int)m_runs.size()) {
            m_runs[(size_t)m_nruns++] = s;
            m_runs[(size_t)m_nruns++] = e;
        }
    }
}

bool Channel::repairRun(int s, int e, bool again) {
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
    m_rhs.assign((size_t)m, 0.0);
    const double * win = &m_win[0];
    for (int j = 0; j < m; ++j) {
        const int c = s + j;
        double v = 0.0;
        // t = c - order .. s - 1, where aa[c - t] runs backwards.
        const int nl = s - (c - order);
        if (nl > 0) v += dot(&m_aarev[0], win + (c - order), nl);
        // t = e + 1 .. c + order.
        const int nr = c + order - e;
        if (nr > 0) v += dot(&m_aa[(size_t)(e + 1 - c)], win + e + 1, nr);
        m_rhs[(size_t)j] = -v;
    }

    if (!solveBandedToeplitz(&m_aa[0], order, &m_rhs[0], m, m_solve)) return false;

    // Optional per-sample confidence. Under the model's Gaussian
    // innovation the posterior covariance of the interpolated run is
    // sigma^2 * G^-1, and the solve already produced the factor of G, so
    // the diagonal costs one more banded recursion. Off by default; see
    // Config::wienerAlpha for why.
    // The && matters: with wienerAlpha at 0 the two Wiener buffers are not
    // even allocated, and nothing to the right of it is evaluated.
    const bool haveVar =
        m_cfg.wienerAlpha > 0.0 && !m_pvar.empty() &&
        bandedInverseDiagonal(m_solve, order, m, m_sinv, &m_pvar[0]);

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
    const double sigma2 = m_scale * m_scale;
    const double depth = m_cfg.depth;
    const double alpha = m_cfg.wienerAlpha;
    const double wcap = m_cfg.wienerMax;
    int touched = 0;

    for (int j = 0; j < m; ++j) {
        double v = m_rhs[(size_t)j];
        if (v > limit) v = limit;
        else if (v < -limit) v = -limit;

        // A re-repair subtracts from what was actually there, not from the
        // earlier repair - which stays put if this one cannot be done.
        const double x = again ? m_dry[s + j] : m_win[s + j];
        const double dj = x - v;

        // The Wiener gain reduces to 1 wherever the discrepancy dwarfs the
        // estimate's own uncertainty, which on this material is almost
        // everywhere - hence the flat default.
        double w = 1.0;
        if (haveVar) {
            const double P = alpha * sigma2 * m_pvar[(size_t)j];
            const double d2 = dj * dj;
            w = (d2 > P) ? (1.0 - P / d2) : 0.0;
        }
        if (w > wcap) w = wcap;
        w += depth * (1.0 - w);   // at depth 1 this is outright replacement

        if (w > 0.0) {
            m_win[s + j] = x - w * dj;
            ++touched;
        }
        // Flagged either way: the run has been dealt with, and a later
        // pass must not come back and attack it again.
        m_flag[s + j] = 1;
    }
    if (!again) m_repaired += (uint64_t)touched;
    return true;
}

void Channel::rerepairRuns() {
    for (int k = 0; k + 1 < m_nruns; k += 2) {
        repairRun(m_runs[(size_t)k], m_runs[(size_t)k + 1], true);
    }
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

    m_nruns = 0;
    for (int pass = 0; pass < m_cfg.passes; ++pass) {
        if (dTo <= dFrom) break;
        // Refit each pass: once the worst clicks are gone the model fits the
        // music better, which is the whole point of running more than one.
        fitModel(0, m_fill);
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
        fitModel(0, m_fill);
        rerepairRuns();
    }
}

void Channel::pushOne(double v) {
    const int blk = (int)kBlock;
    const int capacity = 2 * m_pad + blk;

    if (!finite(v)) v = 0.0;
    m_win[(size_t)m_fill] = v;
    m_dry[(size_t)m_fill] = v;
    m_flag[(size_t)m_fill] = 0;
    ++m_fill;
    ++m_seen;

    if (m_fill < capacity) return;

    processBlock();

    const double wet = m_cfg.wet;
    const double dry = 1.0 - wet;
    if (m_cfg.delta) {
        // d - (wet * y + dry * d): what the mix below takes out.
        for (int i = 0; i < blk; ++i) {
            const double y = m_win[(size_t)(m_pad + i)];
            const double d = m_dry[(size_t)(m_pad + i)];
            outPush(wet * (d - y));
        }
    } else {
        for (int i = 0; i < blk; ++i) {
            const double y = m_win[(size_t)(m_pad + i)];
            const double d = m_dry[(size_t)(m_pad + i)];
            outPush(wet * y + dry * d);
        }
    }

    // Slide along by one block, keeping the repairs that reached past its end.
    const size_t keep = (size_t)(m_fill - blk);
    memmove(&m_win[0], &m_win[(size_t)blk], sizeof(double) * keep);
    memmove(&m_dry[0], &m_dry[(size_t)blk], sizeof(double) * keep);
    memmove(&m_flag[0], &m_flag[(size_t)blk], sizeof(uint8_t) * keep);
    m_fill -= blk;
    m_fitLen = 0;                 // the next fit is of a different window
    m_silentUntil = (m_silentUntil > blk) ? (m_silentUntil - blk) : 0;
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

void Channel::drain() {
    for (int i = 0; i < m_cfg.latency; ++i) pushOne(0.0);
}

template void Channel::push<float>(const float *, size_t, size_t);
template void Channel::push<double>(const double *, size_t, size_t);
template void Channel::pull<float>(float *, size_t, size_t);
template void Channel::pull<double>(double *, size_t, size_t);

} // namespace declick
