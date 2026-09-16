/* ========================================
 *  dehum_rt_verify - the audio thread must not touch the heap.
 *
 *  A real-time audio callback cannot allocate: malloc may take a lock, and a
 *  lock held by a lower-priority thread is a dropout. This is more of a hazard
 *  for dehum than for the other two components, because its processing path
 *  contains a whole FFT-based detector that runs every hop - a megabyte or two
 *  of working buffers, a per-bin history, and a peak list that changes size as
 *  lines come and go. All of it is sized in configure() from the sample rate
 *  alone, so this test replaces global operator new and requires the count to be
 *  zero across processing, every parameter change, flush(), reset() and a
 *  second configure() at a rate the channel has already been sized for.
 *
 *  The parameter case is the interesting one: the search range, the number of
 *  harmonics and the notch bandwidth all change what the detector and the
 *  cancellers do, yet none of them may resize anything. That is why Config sizes
 *  the history for kSearchCeil rather than for the range actually in use, and
 *  why the oscillators are a fixed array rather than a vector.
 * ======================================== */

#include "dehum_core.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

using namespace dehum;

// ---------------------------------------------------------------------------
// Counting global allocator. Only armed inside the regions under test, because
// the harness itself - and printf - are entitled to allocate.

namespace {

size_t g_count = 0, g_bytes = 0, g_biggest = 0;
bool   g_armed = false;

int g_failures = 0;

} // anonymous namespace

void * operator new(size_t n) {
    if (g_armed) {
        ++g_count;
        g_bytes += n;
        if (n > g_biggest) g_biggest = n;
    }
    void * p = malloc(n ? n : 1);
    if (p == NULL) throw std::bad_alloc();
    return p;
}
void * operator new[](size_t n) { return operator new(n); }
void operator delete(void * p) throw() { free(p); }
void operator delete[](void * p) throw() { free(p); }
void operator delete(void * p, size_t) throw() { free(p); }
void operator delete[](void * p, size_t) throw() { free(p); }

namespace {

void arm() { g_count = g_bytes = g_biggest = 0; g_armed = true; }

void requireQuiet(const char * what) {
    g_armed = false;
    const bool ok = (g_count == 0);
    printf("  %-54s %-4s %zu allocs, %.1f kB\n", what, ok ? "ok" : "FAIL",
           g_count, (double)g_bytes / 1024.0);
    if (!ok) ++g_failures;
}

const double kPi = 3.14159265358979323846;

//! Hum plus noise, so the detector has something to find and the line
//! bookkeeping - create, confirm, track, merge, forget - actually runs.
void fill(std::vector<double> & buf, double sr, double f, uint64_t & seed) {
    for (size_t i = 0; i < buf.size(); ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const double n = (double)((int64_t)(seed >> 11)) / (double)(1ULL << 52) - 1.0;
        buf[i] = 0.05 * sin(2.0 * kPi * f * (double)i / sr) + 0.01 * n;
    }
}

} // anonymous namespace

int main() {
    printf("dehum_rt_verify\n");

    const double sr = 44100.0;
    Params p = Params::defaults();
    Config cfg;
    cfg.compute(p, sr);

    Channel ch;
    ch.configure(cfg);          // allowed to allocate; everything after is not
    printf("  footprint after configure(): %zu kB\n", ch.heapBytes() / 1024);

    uint64_t seed = 12345;
    std::vector<double> buf(4096, 0.0);
    std::vector<double> big(65536, 0.0);

    // --- steady processing, long enough to detect, engage and track ---------
    {
        scoped_flush_denormals ftz;
        arm();
        for (int k = 0; k < 400; ++k) {          // ~37 s
            fill(buf, sr, 41.28, seed);
            ch.process(&buf[0], buf.size(), 1);
        }
        requireQuiet("400 blocks of 4096 (detect, engage, track)");
    }

    // --- a single push far larger than any host would make -----------------
    {
        scoped_flush_denormals ftz;
        arm();
        fill(big, sr, 41.28, seed);
        ch.process(&big[0], big.size(), 1);
        requireQuiet("one 65536-sample block");
    }

    // --- one sample at a time, the worst case for the hop bookkeeping ------
    {
        scoped_flush_denormals ftz;
        arm();
        fill(buf, sr, 41.28, seed);
        for (size_t i = 0; i < buf.size(); ++i) ch.process(&buf[i], 1, 1);
        requireQuiet("4096 single-sample calls");
    }

    // --- every parameter, moved live ---------------------------------------
    {
        const float sens[3]  = { 0.0f, 0.5f, 1.0f };
        const float bw[3]    = { 0.1f, 1.0f, 5.0f };
        const float top[3]   = { 40.0f, 150.0f, 500.0f };
        const int   harm[3]  = { 1, 4, 8 };
        const float freq[3]  = { 0.0f, 50.0f, 500.0f };
        const float rumb[3]  = { 0.0f, 20.0f, 200.0f };
        const float wet[3]   = { 0.0f, 0.5f, 1.0f };

        scoped_flush_denormals ftz;
        arm();
        for (int a = 0; a < 3; ++a) {
            Params q = p;
            q.sensitivity = sens[a];
            q.bandwidth   = bw[a];
            q.searchTo    = top[a];
            q.harmonics   = harm[a];
            q.frequency   = freq[a];
            q.rumbleHz    = rumb[a];
            q.dryWet      = wet[a];
            Config c2;
            c2.compute(q, sr);
            if (!ch.retune(c2)) {
                printf("  retune refused a same-rate config - FAIL\n");
                ++g_failures;
            }
            fill(buf, sr, 41.28, seed);
            ch.process(&buf[0], buf.size(), 1);
        }
        requireQuiet("21 parameter changes with audio in between");
    }

    // --- flush and reset ----------------------------------------------------
    {
        scoped_flush_denormals ftz;
        arm();
        ch.flush();
        fill(buf, sr, 41.28, seed);
        ch.process(&buf[0], buf.size(), 1);
        ch.reset();
        ch.process(&buf[0], buf.size(), 1);
        requireQuiet("flush() and reset()");
    }

    // --- the same again in manual mode, where the detector does not run -----
    {
        Params q = Params::defaults();
        q.frequency = 50.0f;
        q.harmonics = 8;
        Config c2;
        c2.compute(q, sr);
        Channel m;
        m.configure(c2);
        scoped_flush_denormals ftz;
        arm();
        for (int k = 0; k < 50; ++k) {
            fill(buf, sr, 50.0, seed);
            m.process(&buf[0], buf.size(), 1);
        }
        requireQuiet("manual mode, 8 harmonics, 50 blocks");
    }

    // --- configure() again, on a channel that already holds its buffers -----
    //
    // The first configure() is entitled to allocate; a later one at a rate the
    // channel has already been sized for is not. Every buffer is assigned the
    // length it already has, which a vector serves out of the capacity it is
    // still holding, so the only way this fails is a working buffer taken from
    // the heap instead of kept - which is what designDecimator() did with the
    // taps it designs, three allocations a call for two kilobytes that never
    // outlived it. Hosts reconfigure whenever the format changes, and not all
    // of them do it from a thread that may wait on malloc.
    {
        arm();
        for (int k = 0; k < 4; ++k) ch.configure(cfg);
        requireQuiet("4 reconfigures at the same rate");
    }

    // --- the same, with the parameters moved ------------------------------
    //
    // configure() rather than retune(), so the pipeline is rebuilt rather than
    // swapped; the envelope is sized from the sample rate alone, so rebuilding
    // it may not resize anything either.
    {
        arm();
        for (int a = 0; a < 3; ++a) {
            Params q = p;
            q.searchTo  = 40.0f + 230.0f * (float)a;
            q.harmonics = 1 + 3 * a;
            q.bandwidth = 0.1f + 2.0f * (float)a;
            Config c2;
            c2.compute(q, sr);
            ch.configure(c2);
        }
        requireQuiet("3 reconfigures with the parameters moved");
    }

    printf("  footprint at the end:        %zu kB\n", ch.heapBytes() / 1024);
    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
