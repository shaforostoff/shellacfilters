/* ========================================
 *  foo_dsp_dehum - component declaration
 * ======================================== */

#include "stdafx.h"
#include "version.h"

DECLARE_COMPONENT_VERSION(
    "Dehum",
    FOO_DSP_DEHUM_VERSION_STRING,
    "Dehum - removes hum and other continuous tones from transfers.\n"
    "\n"
    "Finds the offending frequencies itself rather than assuming 50 or 60 Hz,\n"
    "which matters on disc transfers: on the two reference sides used to\n"
    "calibrate it the hum sits at 41.3 Hz, because a speed-corrected transfer\n"
    "moves whatever was on the disc along with the music.\n"
    "\n"
    "A tone is identified by being continuous rather than by being loud - it has\n"
    "to hold its prominence over seconds, which is what separates a hum from a\n"
    "bass note that recurs on every beat. Each one is then removed by cancelling\n"
    "the coherent part at that exact frequency, so a 1 Hz notch takes out the\n"
    "hum and leaves a partial 5 Hz away 0.15 dB down.\n"
    "\n"
    "Controls:\n"
    "  Sensitivity - how prominent a line must be. Raise it if hum survives,\n"
    "                lower it if music is being touched.\n"
    "  Bandwidth   - width of each notch. Wider catches a drifting line at the\n"
    "                cost of more music around it.\n"
    "  Search to   - top of the range searched automatically. Hum lives low;\n"
    "                searching higher finds sustained musical notes instead.\n"
    "  Harmonics   - multiples of each line to cancel as well.\n"
    "  Frequency   - pin the fundamental instead of detecting it. Acts at once,\n"
    "                where detection needs a few seconds of a track.\n"
    "  Rumble      - a separate high-pass for broadband low-frequency noise,\n"
    "                which is a different defect from hum but shares the band.\n"
    "                40 Hz by default; try 60 if rumble survives. 0 turns it\n"
    "                off.\n"
    "  Bypass      - passes the audio through untouched.\n"
    "  Delta       - outputs only what the notches remove, for testing.\n"
    "\n"
    "This DSP has zero latency: the detector reads the signal but does not sit\n"
    "in the path.\n"
);

VALIDATE_COMPONENT_FILENAME("foo_dsp_dehum.dll");
