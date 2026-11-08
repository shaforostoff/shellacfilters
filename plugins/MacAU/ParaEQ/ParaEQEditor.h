/*
 *	ParaEQEditor.h
 *	ParaEQ
 *
 *	The editor itself - a response curve over a console strip - with no idea
 *	what it is editing. Two hosts drive it: the Audio Unit in this folder,
 *	through ParaEQView.mm, and foo_dsp_paraeq on foobar2000 for Mac, through
 *	its config_view_mac.mm. What differs between them is where a value is read
 *	from and where a change goes, and that is the whole of the backend protocol
 *	below; the layout, the drawing and the knob behaviour are the one copy.
 *
 *	Objective-C class names are one flat namespace across every bundle in a
 *	process. The Audio Unit keeps the names it always had; foobar2000 builds
 *	this file into a component with FOO_DSP_MAC_CLASS_SUFFIX defined (see
 *	cmake/fb2k_component.cmake there), and every class and protocol here takes
 *	that as a suffix.
 *
 *	Needs ParaEQControls.h, and so paraeq_core.h, and nothing of either host.
 */

#ifndef __ParaEQEditor_h__
#define __ParaEQEditor_h__

#import <Cocoa/Cocoa.h>

#ifdef FOO_DSP_MAC_CLASS_SUFFIX
#define PARAEQ_EDITOR_CAT2(a, b) a##b
#define PARAEQ_EDITOR_CAT(a, b)  PARAEQ_EDITOR_CAT2(a, b)
#define PARAEQ_EDITOR_NAME(n)    PARAEQ_EDITOR_CAT(n##_, FOO_DSP_MAC_CLASS_SUFFIX)
#define ShellacFiltersParaEQBackend       PARAEQ_EDITOR_NAME(ShellacFiltersParaEQBackend)
#define ShellacFiltersParaEQView          PARAEQ_EDITOR_NAME(ShellacFiltersParaEQView)
#define ShellacFiltersParaEQKnob          PARAEQ_EDITOR_NAME(ShellacFiltersParaEQKnob)
#define ShellacFiltersParaEQKnobDelegate  PARAEQ_EDITOR_NAME(ShellacFiltersParaEQKnobDelegate)
#define ShellacFiltersParaEQCurveView     PARAEQ_EDITOR_NAME(ShellacFiltersParaEQCurveView)
#endif


//! What the editor needs to know about a control to draw a knob for it.
typedef enum {
    ParaEQControlUnitGeneric = 0,
    ParaEQControlUnitHertz,
    ParaEQControlUnitDecibels
} ParaEQControlUnit;

typedef struct {
    float             minValue;
    float             maxValue;
    float             defaultValue;
    ParaEQControlUnit unit;
    BOOL              logarithmic;
} ParaEQControlInfo;


//! Everything the editor asks of whatever it is editing. Parameters are the
//! kParam_ indices of ParaEQControls.h. Main thread only, both ways.
@protocol ShellacFiltersParaEQBackend <NSObject>

- (float) valueForParameter:(int)parameter;

//! A change made in the editor, to go wherever changes go.
- (void) setValue:(float)value forParameter:(int)parameter;

//! NO if the control is unknown, in which case the knob runs 0 to 1.
- (BOOL) getInfo:(ParaEQControlInfo *)outInfo forParameter:(int)parameter;

//! Accessibility label for the knob; nil for none.
- (NSString *) nameForParameter:(int)parameter;

//! The rate the curve is drawn at: it decides where Nyquist sits on the plot.
- (double) sampleRate;

//! Whether the host has switched the effect out, which flattens the curve.
- (BOOL) isBypassedByHost;

//! Bracket a knob drag, so a host that records automation or undo records one
//! move rather than two hundred.
- (void) beginGestureForParameter:(int)parameter;
- (void) endGestureForParameter:(int)parameter;

@end


@interface ShellacFiltersParaEQView : NSView

//! The size the layout is drawn at. It is a fixed console strip.
+ (CGSize) fittingSize;

//! The view keeps the backend.
- (instancetype) initWithBackend:(id<ShellacFiltersParaEQBackend>)backend;

//! Picks the values back up after something outside the view changed them -
//! a backend calls this when its host says so.
- (void) reloadData;

//! Every gain and the output trim to 0 dB, leaving frequencies and Q where they
//! are. The counterpart of the VST editor's Flatten, and the right gesture here
//! for the same reason: the setup is worth keeping, the curve is not.
- (void) flatten;

//! Every control back to Params::defaults(), which is what the VST editor's
//! Reset does. A host that offers its own is welcome to; not all of them do.
- (void) restoreDefaults;

@end

#endif
