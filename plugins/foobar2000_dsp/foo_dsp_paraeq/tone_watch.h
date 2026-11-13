/* ========================================
 *  foo_dsp_paraeq - track tone: measuring the playing track
 *
 *  When track tone is on, every new track whose tags say pre-1954 tango, vals
 *  or milonga is decoded once on a worker thread and measured with
 *  paraeq_tone::Analyzer - the whole track, from the file, so the hint is
 *  there from the first second rather than building up while it plays. A
 *  three-minute track takes well under a second. Results are kept for the
 *  session, so coming back to a track costs nothing.
 *
 *  The measurement is of the decoded file, before any DSP: what the equaliser
 *  is doing is taken into account by paraeq_tone::suggest() instead, so the
 *  hint stays put while a band is being dragged.
 *
 *  Everything here is main thread except the worker, which touches nothing
 *  but its own copy of the path and hands its result back through
 *  fb2k::inMainThread.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_TONE_WATCH_H
#define FOO_DSP_PARAEQ_TONE_WATCH_H

#include "paraeq_core.h"
#include "paraeq_editor.h"

namespace paraeq_tone_watch {

//! The checkbox. Off by default, persisted in foobar2000's configuration.
bool enabled();
void setEnabled(bool on);

//! The hints for the playing track, given the settings the editor is drawing.
void hints(const paraeq::Params & p, double sampleRate, paraeq_editor::ToneHints & out);

//! Told when a measurement lands or the feature is switched. Main thread.
typedef void (*Listener)(void * ctx);
void subscribe(void * ctx, Listener fn);
void unsubscribe(void * ctx);

} // namespace paraeq_tone_watch

#endif // FOO_DSP_PARAEQ_TONE_WATCH_H
