/* ========================================
 *  foo_dsp_declick - configuration view, macOS
 *
 *  The table that config_dialog.cpp drives on Windows, handed to the shared
 *  Cocoa panel in mac/slider_panel.mm. Labels, ranges, steps and hover text
 *  are the Windows dialog's, so a preset means the same on both platforms and
 *  so does every slider position. Change one, change the other.
 * ======================================== */

#include "stdafx.h"

#include "declick_preset.h"
#include "slider_panel.h"

using declick::Params;

namespace {

const slider_panel::row k_rows[] = {
    { "Sensitivity",  1000,  0.0,   1.0, 3, "",    NULL,
      "How readily a sample is called a click. The default, 0.9, puts the trigger at 2.85 "
      "sigma above the local noise estimate. Detections near the trigger are only partly "
      "repaired - they are as likely noise or music as clicks - and full repair is kept for "
      "clear ones. Past the default, music starts being flagged as damage faster than crackle "
      "is found. On clean records 0.7-0.8 takes less still." },

    { "Extent",       1000,  0.0,   1.0, 3, "",    NULL,
      "How far each detection spreads outwards into the tail of the click. Higher repairs "
      "more of the decay, but it lowers the floor for every repair, so too high starts "
      "replacing good audio either side. A strong click whose error rings through zero is "
      "repaired whole without it. Lower it (0.3) on clean records." },

    { "Max repair",    198,  0.2,  20.0, 1, " ms", NULL,
      "The longest single stretch that may be reconstructed. Damage longer than this is left "
      "alone rather than guessed at." },

    { "Passes",          2,  1.0,   3.0, 0, "",    NULL,
      "How many times the detector sweeps each block. A second pass catches clicks that the "
      "first pass's repairs uncover." },

    { "Model order",   124,  8.0, 256.0, 0, "",    NULL,
      "Taps in the autoregressive model that predicts what the waveform should have been. "
      "Higher orders model the music better, so they change clean records least - 256 is the "
      "gentlest - but they also miss more of the short clicks on heavily crackled transfers, "
      "where 64-128 catches more. 128 is the balance. Any value works, not only powers of "
      "two; CPU and latency grow with it (256 costs about 2.4x what 128 does, 64 about 0.6x)." },

    { "Repair depth", 1000,  0.0,   1.0, 3, "",    NULL,
      "How much of the estimated click is subtracted. 1, the default, replaces the damaged "
      "samples outright. 0 subtracts 45% of each click - the least added error by whole-file "
      "measure, but it leaves every click no more than 5 dB quieter." },

    { "Dry/Wet",      1000,  0.0,   1.0, 3, "",    NULL,
      "Blend of the repaired signal against the original. 0 passes the input through "
      "untouched." },
};

const size_t k_count = sizeof(k_rows) / sizeof(k_rows[0]);

void to_values(const Params & p, double * v, bool & delta) {
    v[0] = p.sensitivity;
    v[1] = p.extent;
    v[2] = p.maxLengthMs;
    v[3] = (double)p.passes;
    v[4] = (double)p.order;
    v[5] = p.depth;
    v[6] = p.dryWet;
    delta = p.delta;
}

void read(const dsp_preset & in, double * v, bool & delta) {
    to_values(declick_preset::parse(in), v, delta);
}

void write(const double * v, bool delta, dsp_preset & out) {
    Params p = Params::defaults();
    p.sensitivity = (float)v[0];
    p.extent      = (float)v[1];
    p.maxLengthMs = (float)v[2];
    p.passes      = (int)(v[3] + 0.5);
    p.order       = (int)(v[4] + 0.5);
    p.depth       = (float)v[5];
    p.dryWet      = (float)v[6];
    p.delta       = delta;
    declick_preset::make(p, out);
}

void defaults(double * v, bool & delta) {
    to_values(Params::defaults(), v, delta);
}

const slider_panel::spec k_spec = {
    k_rows, k_count,
    "Output only what is removed (delta, for testing)",
    "Outputs what the repair takes out - the input minus the output - instead of the "
    "repaired audio. For testing: it should be crackle and clicks and nothing else, and "
    "any music heard in it is being damaged. Saved with the preset; untick it when done.",
    "Sensitivity is the one to reach for: raise it until the crackle goes, then back off as "
    "soon as the music dulls. Repair depth 0 subtracts the fraction of each click that adds "
    "the least error of its own; raise it to strip more, at the cost of more guesswork.",
    read, write, defaults,
};

} // anonymous namespace

service_ptr declick_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    return slider_panel::make(k_spec, callback);
}
