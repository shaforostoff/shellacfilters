/* ========================================
 *  foo_dsp_declick - component declaration
 * ======================================== */

#include "stdafx.h"
#include "version.h"

DECLARE_COMPONENT_VERSION(
    "Declick",
    FOO_DSP_DECLICK_VERSION_STRING,
    "Declick - removes clicks and crackle from shellac and vinyl transfers.\n"
    "\n"
    "Detects clicks as spikes in an autoregressive prediction residual, then\n"
    "replaces the damaged samples with the least-squares interpolation implied\n"
    "by the model and the surrounding good audio. It reconstructs the waveform\n"
    "rather than smoothing over the damage, and unlike detectors that key on\n"
    "stereo differences it works on mono transfers.\n"
    "\n"
    "Controls:\n"
    "  Sensitivity - the one to reach for. Raise it until the crackle goes,\n"
    "                then back off as soon as the music starts to dull.\n"
    "  Extent      - how far a detection spreads into its own tail. Raise it\n"
    "                if repairs leave a residual tick behind them.\n"
    "  Max repair  - longest single repair. Anything longer is treated as\n"
    "                music and left alone.\n"
    "  Passes      - a second pass catches clicks the first one uncovers.\n"
    "  Model order - higher changes clean records less but misses more short\n"
    "                clicks on heavy crackle; 128 is the balance. CPU grows\n"
    "                with it.\n"
    "  Repair depth - 1 replaces damaged samples outright; lower subtracts\n"
    "                 only part of each click.\n"
    "  Dry/Wet     - 0 bypasses.\n"
    "  Delta       - outputs only what the repair removes, for testing.\n"
    "\n"
    "This DSP is not zero latency; the delay is reported to foobar2000 so\n"
    "visualisations stay in sync.\n"
    "\n"
    "Warm start: after a track change or a seek it holds the first seven\n"
    "seconds and learns the record's surface from them before playing them,\n"
    "a short stall. Advanced Preferences > Playback > Declick turns it off.\n"
);

VALIDATE_COMPONENT_FILENAME("foo_dsp_declick.dll");
