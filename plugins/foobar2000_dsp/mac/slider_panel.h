/* ========================================
 *  slider_panel - the macOS configuration view shared by Declick and Dehum
 *
 *  The Cocoa counterpart of the two components' config_dialog.cpp, which are
 *  the same dialog with a different table: a row per parameter - label,
 *  slider, readout - then the delta checkbox, a paragraph of advice and a
 *  Defaults button. Here the machinery is written once and each component
 *  supplies only the table and the conversions to and from its preset.
 *
 *  What foobar2000 for Mac asks a DSP for is not a dialog but a view
 *  controller, which it puts in a window of its own with its own buttons. So
 *  there is no OK or Cancel here, and nothing to restore on Cancel either:
 *  every change goes to the host as it is made, and the host keeps the preset
 *  it opened with.
 *
 *  Plain C++ on the outside, so a component's own file needs no Objective-C
 *  to describe its table. Compiled into each component that uses it, which is
 *  why the Objective-C class inside carries the component's name - see
 *  FOO_DSP_MAC_CLASS_SUFFIX in slider_panel.mm.
 *
 *  Include after stdafx.h; assumes the foobar2000 SDK is visible.
 * ======================================== */

#ifndef FOO_DSP_MAC_SLIDER_PANEL_H
#define FOO_DSP_MAC_SLIDER_PANEL_H

#include <stddef.h>

namespace slider_panel {

//! One row. The slider works in integer steps 0..steps and maps onto the
//! parameter linearly from `lo` to `hi`, as the Windows trackbars do.
//!
//! `offText` marks a slider whose bottom position means off or automatic
//! rather than `lo`: the parameter is 0 there, and the remaining steps cover
//! `lo` to `hi`. NULL if the row has no such position.
//!
//! `help` is the hover text, word for word the Windows tooltip.
struct row {
    const char * label;
    int          steps;
    double       lo, hi;
    int          decimals;   // for the readout
    const char * suffix;
    const char * offText;
    const char * help;
};

//! Everything the panel knows about one component. `values` always has one
//! entry per row, in row order.
struct spec {
    const row *  rows;
    size_t       count;
    const char * deltaLabel;
    const char * deltaHelp;
    const char * hint;

    //! Never fail: a preset that does not parse reads as the defaults.
    void (*read)(const dsp_preset & in, double * values, bool & delta);
    void (*write)(const double * values, bool delta, dsp_preset & out);
    void (*defaults)(double * values, bool & delta);
};

//! The view controller, wrapped for the host. `s` must outlive it - a static
//! table, in practice. Main thread only.
service_ptr make(const spec & s, dsp_preset_edit_callback_v2::ptr callback);

} // namespace slider_panel

#endif // FOO_DSP_MAC_SLIDER_PANEL_H
