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
    "  LF   60-125 Hz    weight the transfer lost at the bottom\n"
    "  LMF  around 1 kHz the boxy room the horn or the hall adds\n"
    "  HMF  4-6 kHz      the brilliance sitting under the surface noise\n"
    "  HF   around 8 kHz the surface noise itself\n"
    "\n"
    "plus a high-pass for rumble and turntable roar under all of it, and an\n"
    "output trim for whatever the bands did. Both shelves switch to bells; the\n"
    "high-pass runs at 12 or 24 dB/oct. Every range is chosen to put its target\n"
    "near the middle of the control's travel.\n"
    "\n"
    "The filters are the biquads from Robert Bristow-Johnson's Audio EQ\n"
    "Cookbook. Moving a control glides the coefficients over 20 ms rather than\n"
    "switching them, so nothing clicks - the shelf/bell switches and the\n"
    "high-pass slope included, since an off stage is a unity biquad rather than\n"
    "a skipped one. Bypass glides the whole curve to flat.\n"
    "\n"
    "The editor is the curve. Drag a handle for frequency and gain, wheel for\n"
    "gain or shift-wheel for Q, right-click for the shape switches, double-click\n"
    "to zero a band. Or use the keyboard: left and right select a band, up and\n"
    "down move its gain, shift with left and right its frequency, page up and\n"
    "down its Q, and held ctrl makes any of those fine.\n"
    "\n"
    "It is also a Default UI element. Turn on View / Layout / Enable layout\n"
    "editing mode, right-click a panel, and add Parametric EQ from the DSP\n"
    "group; it edits the DSP chain directly, so it and the Preferences dialog\n"
    "always show the same settings.\n"
    "\n"
    "This DSP has zero latency.\n"
);

VALIDATE_COMPONENT_FILENAME("foo_dsp_paraeq.dll");
