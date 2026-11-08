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

// Only the controls Params::restrictToControls() keeps: the stages after the
// AR repair were tuned with the rest at their defaults.
const double k_orders[] = { 64.0, 128.0, 256.0 };

const slider_panel::row k_rows[] = {
    { "Sensitivity",  1000,  0.0,   1.0, 3, "",    NULL,
      "How readily a sample is called a click. The default, 0.9, puts the trigger at 2.85 "
      "sigma above the local noise estimate. Detections near the trigger are only partly "
      "repaired - they are as likely noise or music as clicks - and full repair is kept for "
      "clear ones. Past the default, music starts being flagged as damage faster than crackle "
      "is found. On clean records 0.7-0.8 takes less still.", NULL },

    { "Model order",     2, 64.0, 256.0, 0, "",    NULL,
      "Taps in the autoregressive model that predicts what the waveform should have been. 128 "
      "is the balance. 256 models the music better and changes clean records least, but "
      "misses more of the short clicks on heavily crackled transfers. 64 costs about 0.6x the "
      "CPU of 128 and gives up surprisingly little - the one to pick on a slow machine; 256 "
      "costs about 2.4x.", k_orders },
};

const size_t k_count = sizeof(k_rows) / sizeof(k_rows[0]);

// v[2] and v[3] are the Bypass and Extra cautious checkboxes, the panel's
// options (k_options below). Bypass is drawn on the delta checkbox's row,
// ahead of it.
void to_values(const Params & p, bool bypass, double * v, bool & delta) {
    v[0] = p.sensitivity;
    v[1] = (double)p.order;
    v[2] = bypass ? 1.0 : 0.0;
    v[3] = p.cautious ? 1.0 : 0.0;
    delta = p.delta;
}

void read(const dsp_preset & in, double * v, bool & delta) {
    bool bypass = false;
    const Params p = declick_preset::parse(in, &bypass);
    to_values(p, bypass, v, delta);
}

void write(const double * v, bool delta, dsp_preset & out) {
    Params p = Params::defaults();
    p.sensitivity = (float)v[0];
    p.order       = (int)(v[1] + 0.5);
    p.cautious    = (v[3] != 0.0);
    p.delta       = delta;
    declick_preset::make(p, out, v[2] != 0.0);
}

void defaults(double * v, bool & delta) {
    to_values(Params::defaults(), false, v, delta);
}

const slider_panel::option k_options[] = {
    { "Bypass",
      "Passes the audio through untouched, for comparing against the repair. Whatever is "
      "already in the pipeline plays out first, so nothing is skipped or repeated. Saved "
      "with the preset.", true },
    { "Extra cautious: repair the clicks, leave everything else",
      "Repairs only the clicks the detector flags, and turns off every stage that works "
      "on the audio around them: the click-shape fit, the crackle body, top-band and mid "
      "stages, the flank repair and the crackle floor. Those take the carpet of crackle "
      "under the clicks, and on some records a little of the music with it. With this on "
      "the music is left as it is, and more crackle stays. Uses less CPU.", false },
};

const slider_panel::spec k_spec = {
    k_rows, k_count,
    "Delta",
    "Outputs what the repair takes out - the input minus the output - instead of the "
    "repaired audio. For testing: it should be crackle and clicks and nothing else, and "
    "any music heard in it is being damaged. Saved with the preset; untick it when done.",
    NULL,   // no paragraph: the tooltips say it
    read, write, defaults,
    k_options, sizeof(k_options) / sizeof(k_options[0]),
};

} // anonymous namespace

service_ptr declick_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    return slider_panel::make(k_spec, callback);
}
