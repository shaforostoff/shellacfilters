/* ========================================
 *  foo_dsp_paraeq - preset (de)serialisation and dialog entry point
 *
 *  Include after stdafx.h; assumes the foobar2000 SDK is visible.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_PRESET_H
#define FOO_DSP_PARAEQ_PRESET_H

#include "paraeq_core.h"

namespace paraeq_preset {

//! Identity of this DSP in the stored chain configuration. Never change it:
//! it is what a saved DSP chain names, and what the UI element looks the
//! equaliser up by.
GUID guid();

enum { version = 1 };

void make(const paraeq::Params & params, dsp_preset & out);

//! Never throws: anything unreadable falls back to the defaults.
paraeq::Params parse(const dsp_preset & in);

//! The equaliser's settings as the DSP chain currently holds them, and whether
//! it is in the chain at all. Both halves are what the UI element needs to draw
//! itself, and it needs them together - a curve is a different picture when
//! nothing is running it.
//!
//! Main thread only, as everything on dsp_config_manager is.
bool core_read(paraeq::Params & out);

//! Writes the settings back into the DSP chain, adding the equaliser to it if
//! it is not there yet. `insertIfAbsent` false leaves an absent equaliser
//! absent, which is what a redraw wants and what an edit does not.
//!
//! Main thread only. Cheap when nothing changed: the SDK helper compares before
//! it writes, so a drag that lands on the same value costs a chain copy and no
//! notification.
void core_write(const paraeq::Params & params, bool insertIfAbsent);

//! Takes the equaliser out of the DSP chain entirely.
void core_remove();

} // namespace paraeq_preset

//! Blocking modal configuration dialog. Main thread only.
void paraeq_config_popup(const dsp_preset & data, HWND parent,
                         dsp_preset_edit_callback & callback);

#endif // FOO_DSP_PARAEQ_PRESET_H
