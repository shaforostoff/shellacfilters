/* ========================================
 *  foo_dsp_declick - preset (de)serialisation and dialog entry point
 *
 *  Include after stdafx.h; assumes the foobar2000 SDK is visible.
 * ======================================== */

#ifndef FOO_DSP_DECLICK_PRESET_H
#define FOO_DSP_DECLICK_PRESET_H

#include "declick_core.h"

namespace declick_preset {

//! Identity of this DSP in the stored chain configuration. Never change it.
GUID guid();

//! 1 = pre-depth layout (still read, depth falls back to its default).
//! 2 = adds `depth` between maxLengthMs and dryWet (still read, delta off).
//! 3 = current layout, with `delta` appended after order.
enum { version = 3 };

void make(const declick::Params & params, dsp_preset & out);

//! Never throws: anything unreadable falls back to the defaults.
declick::Params parse(const dsp_preset & in);

} // namespace declick_preset

#ifdef _WIN32
//! Blocking modal configuration dialog. Main thread only.
void declick_config_popup(const dsp_preset & data, HWND parent,
                          dsp_preset_edit_callback & callback);
#endif

#endif // FOO_DSP_DECLICK_PRESET_H
