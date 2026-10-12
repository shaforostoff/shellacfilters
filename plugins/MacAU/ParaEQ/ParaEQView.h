/*
 *	ParaEQView.h
 *	ParaEQ
 *
 *	The editor: a response curve over a console strip, read left to right as
 *	low cut, bass, reverb cut, brilliance, hiss cut, output - the sections being
 *	named for the job rather than for the console. The shorthand a strip would
 *	have used, HP LF LMF HMF HF, survives in the parameter names the host's own
 *	generic view draws, which is where it has to stay.
 *
 *	This is the same editor EmbraceNG carries for the same core, moved from
 *	AUv3 to AUv2: there the view reached an AUParameterTree, here it holds the
 *	opaque AudioUnit and goes through AudioUnitGetParameter, AUParameterSet and
 *	an AUEventListener. The layout, the drawing and the knob behaviour are
 *	unchanged, and both are MIT.
 *
 *	Sixteen controls is a wall of sliders in a host's generic view, which the
 *	clumps only soften. A fixed layout is not sixteen of anything: it is four
 *	gain knobs an operator reaches for and eleven that get set once for a box of
 *	records, and the curve says what the four are doing.
 *
 *	Include order: ParaEQ.cpp includes this after ParaEQ.h, which is what has
 *	already brought in AudioUnitCocoaViewInfo - from Apple's headers in a real
 *	build, from ../au_shim in the verify build. This header deliberately pulls
 *	in neither.
 */

#ifndef __ParaEQView_h__
#define __ParaEQView_h__

#ifdef __cplusplus
extern "C" {
#endif

/*! Fills in kAudioUnitProperty_CocoaUI: where the view class lives and what it
 *  is called. The caller owns both fields and releases them, which is the
 *  property's documented contract. Defined in ParaEQView.mm - and stubbed by
 *  paraeq_au_verify, which links the wrapper without any of AppKit. */
OSStatus ParaEQ_CopyCocoaViewInfo(AudioUnitCocoaViewInfo * outInfo);

#ifdef __cplusplus
}
#endif


#ifdef __OBJC__

#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AUCocoaUIView.h>

/*! The factory a host asks for the view. Named in full rather than prefixed:
 *  Objective-C classes share one flat namespace across every plug-in a host has
 *  loaded, and this name is also the string that travels in the property. */
@interface ShellacFiltersParaEQViewFactory : NSObject <AUCocoaUIBase>
@end


@interface ShellacFiltersParaEQView : NSView

- (instancetype) initWithAudioUnit:(AudioUnit)audioUnit;

//! Picks the values back up after something outside the view changed them. Not
//! normally needed - the event listener catches preset loads and a host
//! automating a parameter on its own - but harmless, and cheap.
- (void) reloadData;

//! Every gain and the output trim to 0 dB, leaving frequencies and Q where they
//! are. The counterpart of the VST editor's Flatten, and the right gesture here
//! for the same reason: the setup is worth keeping, the curve is not.
- (void) flatten;

//! Every control back to Params::defaults(), which is what the VST editor's
//! Reset does. A host that offers its own is welcome to; not all of them do.
- (void) restoreDefaults;

@end

#endif // __OBJC__

#endif
