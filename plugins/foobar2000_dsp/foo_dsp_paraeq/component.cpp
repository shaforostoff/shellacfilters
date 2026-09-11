/* ========================================
 *  foo_dsp_paraeq - component declaration
 * ======================================== */

#include "stdafx.h"
#include "version.h"

DECLARE_COMPONENT_VERSION(
    "Parametric EQ",
    FOO_DSP_PARAEQ_VERSION_STRING,
    "Parametric EQ - a console channel strip for disc transfers.\n"
    "\n"
    "Five bands in a fixed layout rather than a bank of identical ones, because\n"
    "restoring transfers is repetitive work: the same four moves serve most of a\n"
    "box of records, and what an operator wants is to set frequency and Q once\n"
    "and then reach for four knobs that always mean the same four things. On\n"
    "1926-1949 shellac those are\n"
    "\n"
    "  Bass        60-125 Hz    weight the transfer lost at the bottom\n"
    "  Reverb cut  around 1 kHz the boxy room the horn or the hall adds\n"
    "  Brilliance  4-6 kHz      the brilliance sitting under the surface noise\n"
    "  Hiss cut    around 8 kHz the surface noise itself\n"
    "\n"
    "plus a low cut for rumble and turntable roar under all of it, and an output\n"
    "trim for whatever the bands did. Both shelves switch to bells; the low cut\n"
    "runs at 12 or 24 dB/oct. Every range is chosen to put its target near the\n"
    "middle of the control's travel. The bands are named for what they are for\n"
    "rather than for where they sit; a narrow panel falls back to the console\n"
    "shorthand, HP LF LMF HMF HF Out, and both names are in the context menu.\n"
    "\n"
    "The filters are the biquads from Robert Bristow-Johnson's Audio EQ\n"
    "Cookbook. Moving a control glides the coefficients over 20 ms rather than\n"
    "switching them, so nothing clicks - the shelf/bell switches and the low-cut\n"
    "slope included, since an off stage is a unity biquad rather than a skipped\n"
    "one. Bypass glides the whole curve to flat.\n"
    "\n"
    "The editor is the curve. Drag a handle for frequency and gain, wheel for\n"
    "gain or shift-wheel for Q, right-click for the shape switches and for Q as\n"
    "a list of widths in octaves, double-click to zero a band. Or use the\n"
    "keyboard: left and right move the selected band's frequency, up and down\n"
    "its gain, page up and down its Q, and ctrl with left and right moves the\n"
    "selection from band to band.\n"
    "\n"
    "It is also a Default UI element. Turn on View / Layout / Enable layout\n"
    "editing mode, right-click a panel, and add Parametric EQ from the DSP\n"
    "group; it edits the DSP chain directly, so it and the Preferences dialog\n"
    "always show the same settings.\n"
    "\n"
    "This DSP has zero latency.\n"
);

VALIDATE_COMPONENT_FILENAME("foo_dsp_paraeq.dll");
