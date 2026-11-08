/* ========================================
 *  foo_dsp_paraeq - the curve editor, macOS
 *
 *  The Cocoa port of paraeq_editor.cpp: the same curve, handles, readout strip
 *  and footer, the same gestures and the same layout rules, drawn with AppKit
 *  instead of GDI. One NSView, used by both places it appears - the window
 *  foobar2000 for Mac opens for the DSP's configuration (config_view_mac.mm)
 *  and the layout element (ui_element_mac.mm). As on Windows, the two hosts
 *  differ only in where a change goes, which is the whole of the host protocol
 *  below.
 *
 *  The band table, the nudges and the drag rates are the Windows editor's and
 *  have to stay that way: a gesture should mean the same on both platforms.
 *  Change one, change the other.
 *
 *  Where the platforms differ, Option stands in for Ctrl, because Ctrl-click
 *  is a right-click on a Mac and Cmd with an arrow belongs to the menus:
 *
 *    Mouse     drag a handle for frequency and gain (Option for fine), scroll
 *              for gain (Option for fine) or Shift-scroll for Q, right-click
 *              or Ctrl-click for the menu, double-click to zero a band. The
 *              readout cells drag vertically.
 *    Keyboard  left and right move the selected band's frequency, up and down
 *              its gain, Option with up and down its Q - page up and down do
 *              the same - and Option with left and right moves the selection.
 *              Shift takes bigger steps. Space switches the shape, Home resets
 *              the band (Option-Home everything), Delete zeroes its gain.
 *
 *  Objective-C++; include from .mm files only, after stdafx.h.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_EDITOR_MAC_H
#define FOO_DSP_PARAEQ_EDITOR_MAC_H

#import <Cocoa/Cocoa.h>

#include "paraeq_core.h"

//! What the editor needs from whoever put it on screen.
@protocol FooDspParaEQEditorHost <NSObject>

//! The user moved something. Already sanitised. Throttled to one call per
//! 25 ms during a drag plus one when it ends.
- (void)editorParamsChanged:(const paraeq::Params &)params;

@optional

//! NO when the equaliser is not in the DSP chain at all, which the editor
//! draws dimmed, with a button to put it there. YES if not implemented.
- (BOOL)editorEngaged;

//! The user asked for it to be put into the chain.
- (void)editorEngage;

@end


@interface FooDspParaEQEditorView : NSView

- (instancetype)initWithFrame:(NSRect)frame
                       params:(const paraeq::Params &)params
                         host:(id<FooDspParaEQEditorHost>)host;

- (paraeq::Params)currentParams;

//! Settings arrived from somewhere else. Ignored mid-drag, so a push of our
//! own coming back round does not pull the handle out from under the pointer.
- (void)takeParams:(const paraeq::Params &)params;

//! Whether the equaliser is in the chain has changed; asks the host again.
- (void)refreshEngaged;

//! Rate to draw the curve for. Only the top of the plot can tell.
- (void)setSampleRate:(double)rate;

//! Sends a change still held back by the throttle.
- (void)flushPush;

@end

#endif // FOO_DSP_PARAEQ_EDITOR_MAC_H
