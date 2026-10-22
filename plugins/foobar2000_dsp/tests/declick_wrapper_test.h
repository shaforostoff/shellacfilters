/* ========================================
 *  declick_wrapper_test.h - what declick_vst_verify and declick_au_verify
 *  share: the fixture signal, the check() they report through, and the
 *  settings the plug-ins open on. Each includes it once.
 * ======================================== */

#ifndef DECLICK_WRAPPER_TEST_H
#define DECLICK_WRAPPER_TEST_H

#include "declick_core.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <vector>

namespace {

//! What the plug-in opens on: the core's defaults, order 128 included. The
//! slider runs 16 .. 256 in eights, and the default position lands on 128.
declick::Params wrapperDefaults() {
    return declick::Params::defaults();
}

int g_failures = 0;

void check(bool ok, const char * what, const char * detail = "") {
    printf("  %-52s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
    if (!ok) ++g_failures;
}

const int kRate = 44100;
const int kFrames = 60000;

//! Deterministic, so a failure is reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    double centred() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (double)s / 4294967296.0 - 0.5; }
};

//! Tones over a surface-noise floor, optionally with clicks at known places,
//! in the sample type the plug-in's process call takes.
//!
//! The noise floor is not decoration. The detector thresholds the prediction
//! residual against a robust estimate of its own scale, so on noiseless
//! synthetic material that estimate collapses towards its 1e-12 floor and
//! everything reads as tens of sigmas. Real transfers have a floor.
template <typename T>
void makeSignal(std::vector<T> & l, std::vector<T> & r,
                std::vector<int> & clickAt, int n, bool withClicks) {
    l.assign((size_t)n, (T)0);
    r.assign((size_t)n, (T)0);
    clickAt.clear();
    Rng rng(2463534242u);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / (double)kRate;
        const double v = 0.30 * sin(6.283185307179586 * 220.0 * t)
                       + 0.15 * sin(6.283185307179586 * 661.0 * t)
                       + 0.07 * sin(6.283185307179586 * 1237.0 * t);
        l[(size_t)i] = (T)(v + rng.centred() * 2.0e-3);       // about -60 dBFS
        r[(size_t)i] = (T)(v * 0.8 + rng.centred() * 2.0e-3);
    }
    if (!withClicks) return;
    for (int i = 4000; i < n - 4000; i += 3001) {
        clickAt.push_back(i);
        for (int k = 0; k < 4; ++k) {
            const T s = (k & 1) ? (T)-0.55 : (T)0.75;
            l[(size_t)(i + k)] += s;
            r[(size_t)(i + k)] += s;
        }
    }
}

} // anonymous namespace

#endif // DECLICK_WRAPPER_TEST_H
