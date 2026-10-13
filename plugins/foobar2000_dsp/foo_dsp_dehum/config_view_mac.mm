/* ========================================
 *  foo_dsp_dehum - configuration view, macOS
 *
 *  The table that config_dialog.cpp drives on Windows, handed to the shared
 *  Cocoa panel in mac/slider_panel.mm. Labels, ranges, steps and hover text
 *  are the Windows dialog's, so a preset means the same on both platforms and
 *  so does every slider position. Change one, change the other.
 * ======================================== */

#include "stdafx.h"

#include "dehum_preset.h"
#include "slider_panel.h"

using dehum::Params;

namespace {

// Frequency and Rumble have an off position below their range rather than a
// range that reaches zero: automatic detection, and no high-pass.
const slider_panel::row k_rows[] = {
    { "Sensitivity", 1000,  0.0,   1.0, 3, "",    NULL,
      "How far a spectral peak must stand above its surroundings before it counts as a tone. "
      "The default maps to a 16 dB prominence threshold, which the reference hum clears on 87% "
      "of analysis hops while hum-free material manages at most 10%. Past about 0.7, clean "
      "material starts qualifying." },

    { "Bandwidth",    490,  0.1,   5.0, 2, " Hz", NULL,
      "Half width of each notch. 1 Hz costs nothing musically - a partial 5 Hz away loses "
      "0.15 dB - and is wide enough to absorb the detector's own error before the frequency "
      "tracker converges on the line." },

    { "Search to",    460, 40.0, 500.0, 0, " Hz", NULL,
      "Top of the range searched for hum automatically. Hum lives low; searching higher finds "
      "sustained musical notes instead - during calibration a bandoneon E4 at 329 Hz was "
      "detected as hum and duly cancelled." },

    { "Harmonics",      7,  1.0,   8.0, 0, "",    NULL,
      "Multiples of each detected line to cancel as well. Mains buzz has them; the "
      "off-frequency drones on speed-corrected disc transfers usually do not, and the extra "
      "notches land squarely in the musical register." },

    { "Frequency",   4901, 10.0, 500.0, 1, " Hz", "auto",
      "auto detects the line by itself, which needs a few seconds of steady evidence before it "
      "engages. Set a frequency to pin the notch there instead, when you already know what you "
      "are removing." },

    { "Rumble",       191, 10.0, 200.0, 0, " Hz", "off",
      "High-pass corner for broadband turntable rumble - a separate defect that happens to "
      "share the band with hum. Set it to off to leave the low end alone. Higher removes more "
      "rumble, and more of the bass along with it." },

    { "Dry/Wet",     1000,  0.0,   1.0, 3, "",    NULL,
      "Blend of the processed signal against the original. 0 passes the input through "
      "untouched." },
};

const size_t k_count = sizeof(k_rows) / sizeof(k_rows[0]);

void to_values(const Params & p, double * v, bool & delta) {
    v[0] = p.sensitivity;
    v[1] = p.bandwidth;
    v[2] = p.searchTo;
    v[3] = (double)p.harmonics;
    v[4] = p.frequency;
    v[5] = p.rumbleHz;
    v[6] = p.dryWet;
    delta = p.delta;
}

void read(const dsp_preset & in, double * v, bool & delta) {
    to_values(dehum_preset::parse(in), v, delta);
}

void write(const double * v, bool delta, dsp_preset & out) {
    Params p = Params::defaults();
    p.sensitivity = (float)v[0];
    p.bandwidth   = (float)v[1];
    p.searchTo    = (float)v[2];
    p.harmonics   = (int)(v[3] + 0.5);
    p.frequency   = (float)v[4];
    p.rumbleHz    = (float)v[5];
    p.dryWet      = (float)v[6];
    p.delta       = delta;
    dehum_preset::make(p, out);
}

void defaults(double * v, bool & delta) {
    to_values(Params::defaults(), v, delta);
}

const slider_panel::spec k_spec = {
    k_rows, k_count,
    "Output only what is removed (delta, for testing)",
    "Outputs what the notches take out - the input minus the output - instead of the "
    "cleaned audio. For testing: it should be hum and nothing else. Rumble is left out "
    "of it, because what a high-pass removes carries the whole spectrum as phase shift "
    "and sounds like the music. Saved with the preset; untick it when done.",
    "Finds continuous tones by itself and cancels them; it takes a few seconds of a track to "
    "decide. Frequency pins it instead, for a hum you already know. Rumble is a separate "
    "high-pass for broadband low-frequency noise, which is a different defect from hum.",
    read, write, defaults,
};

} // anonymous namespace

service_ptr dehum_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    return slider_panel::make(k_spec, callback);
}
