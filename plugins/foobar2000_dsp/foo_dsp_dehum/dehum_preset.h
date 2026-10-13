/* ========================================
 *  foo_dsp_dehum - preset (de)serialisation and dialog entry point
 *
 *  Include after stdafx.h; assumes the foobar2000 SDK is visible.
 * ======================================== */

#ifndef FOO_DSP_DEHUM_PRESET_H
#define FOO_DSP_DEHUM_PRESET_H

#include "dehum_core.h"

namespace dehum_preset {

//! Identity of this DSP in the stored chain configuration. Never change it.
GUID guid();

//! 1 = pre-delta layout (still read, delta off).
//! 2 = current layout, with `delta` appended after harmonics.
enum { version = 2 };

void make(const dehum::Params & params, dsp_preset & out);

//! Never throws: anything unreadable falls back to the defaults.
dehum::Params parse(const dsp_preset & in);

} // namespace dehum_preset

#ifdef _WIN32
//! Blocking modal configuration dialog. Main thread only.
void dehum_config_popup(const dsp_preset & data, HWND parent,
                        dsp_preset_edit_callback & callback);
#else
//! The configuration view foobar2000 for Mac embeds in a window of its own: an
//! NSViewController, wrapped. Changes go to `callback` as they are made. Main
//! thread only.
service_ptr dehum_config_view(dsp_preset_edit_callback_v2::ptr callback);
#endif

#endif // FOO_DSP_DEHUM_PRESET_H
