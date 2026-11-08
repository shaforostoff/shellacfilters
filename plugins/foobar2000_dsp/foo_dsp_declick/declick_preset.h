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
//! 3 = current layout, with `delta` appended after order, and then
//!     `cautious` (Extra cautious) after that. That one came later without a
//!     version bump, so older builds still read the preset; a version 3
//!     preset without it reads as off.
//! The layout still carries extent, maxLengthMs, depth and passes, so a chain
//! saved here still loads in an older build; both directions write and read
//! them as their defaults (Params::restrictToControls()).
enum { version = 3 };

void make(const declick::Params & params, dsp_preset & out);

//! Never throws: anything unreadable falls back to the defaults. The result
//! has been through Params::restrictToControls().
declick::Params parse(const dsp_preset & in);

} // namespace declick_preset

#ifdef _WIN32
//! Blocking modal configuration dialog. Main thread only.
void declick_config_popup(const dsp_preset & data, HWND parent,
                          dsp_preset_edit_callback & callback);
#else
//! The configuration view foobar2000 for Mac embeds in a window of its own: an
//! NSViewController, wrapped. Changes go to `callback` as they are made. Main
//! thread only.
service_ptr declick_config_view(dsp_preset_edit_callback_v2::ptr callback);
#endif

#endif // FOO_DSP_DECLICK_PRESET_H
