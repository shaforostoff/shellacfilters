/*
 *	ParaEQView.mm
 *	ParaEQ
 *
 *	The Audio Unit's side of the editor: the backend that connects the shared
 *	view in ParaEQEditor.mm to an AudioUnit, the factory a host asks for the
 *	view, and the property that names the factory. See ParaEQView.h for where
 *	the editor came from.
 *
 *	Everything that talks to the Audio Unit is in this file. A v3 view holds an
 *	AUParameterTree and asks an AUParameter for its value, its range and its
 *	default, while this holds the opaque AudioUnit and asks
 *	AudioUnitGetParameter, kAudioUnitProperty_ParameterInfo and an
 *	AUEventListener for the same three things.
 */

#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AudioUnitUtilities.h>

#include "ParaEQControls.h"
#include "ParaEQEditor.h"
#include "ParaEQView.h"

#include <cstring>


#pragma mark - The unit

// The four things the editor asks the Audio Unit for.  A v3 view would ask an
// AUParameter; these are the v2 spellings of the same questions.

static float sGetParameter(AudioUnit audioUnit, AudioUnitParameterID parameter)
{
    AudioUnitParameterValue value = 0;

    if (AudioUnitGetParameter(audioUnit, parameter, kAudioUnitScope_Global, 0, &value) != noErr) {
        return 0;
    }

    return value;
}


// Range, default, unit and flags in one read.  Worth noting what this buys over
// the v3 original: an AUParameter has no default value, so EmbraceNG carries a
// side protocol for the unit to answer "what does double-click restore?" with.
// A v2 parameter carries its own, so that protocol is gone here.
//
// Nothing about a control is written down twice for the Audio Unit either.
// Whether a knob is logarithmic is the unit's own DisplayLogarithmic flag, and
// whether it is bipolar is whether its range crosses zero - both read from the
// same table the host's generic view reads, so an editor knob and a host slider
// cannot come to disagree about what a control's travel means.
//
static BOOL sGetParameterInfo(AudioUnit audioUnit, AudioUnitParameterID parameter,
                              AudioUnitParameterInfo *outInfo)
{
    UInt32 size = sizeof(AudioUnitParameterInfo);

    memset(outInfo, 0, size);

    return AudioUnitGetProperty(audioUnit, kAudioUnitProperty_ParameterInfo,
                                kAudioUnitScope_Global, parameter, outInfo, &size) == noErr;
}


static NSString *sParameterName(AudioUnitParameterInfo *info)
{
    if ((info->flags & kAudioUnitParameterFlag_HasCFNameString) && info->cfNameString) {
        // CFNameRelease means the name came back +1 and is the caller's to
        // release.  ParaEQ does not set it -- FillInParameterName is called
        // with inShouldRelease false -- but the rule belongs with the read
        // rather than with the one unit this editor happens to know about.
        if (info->flags & kAudioUnitParameterFlag_CFNameRelease) {
            return (__bridge_transfer NSString *)info->cfNameString;
        }

        return (__bridge NSString *)info->cfNameString;
    }

    return @(info->name);
}


#pragma mark - Backend

@interface ShellacFiltersParaEQAUBackend : NSObject <ShellacFiltersParaEQBackend>
- (instancetype) initWithAudioUnit:(AudioUnit)audioUnit;
//! Told when something outside it changes a value. Weak: the view owns this.
@property (nonatomic, weak) ShellacFiltersParaEQView *view;
@end


//! Defined below -_listen, which is the only thing that installs it.
static void sParaEQEventListener(void *inUserData, void *inObject,
                                 const AudioUnitEvent *inEvent, UInt64 inEventHostTime,
                                 AudioUnitParameterValue inParameterValue);


@implementation ShellacFiltersParaEQAUBackend {
    AudioUnit          _audioUnit;
    AUEventListenerRef _listener;
}


- (instancetype) initWithAudioUnit:(AudioUnit)audioUnit
{
    if ((self = [super init])) {
        _audioUnit = audioUnit;
        [self _listen];
    }

    return self;
}


- (void) dealloc
{
    if (_listener) AUListenerDispose(_listener);
}


// Catches everything that changes a value from outside the view: a host
// automating a parameter, a preset load, and the host's own bypass button.
// Values the view sets name this backend as the sender, so they do not come
// back round.
//
// On the run loop this is created on, which is the main one -- a view is built
// on the main thread -- so the callback may touch AppKit directly.  The AUv3
// original could not: a parameter observer there is called on whatever thread
// set the value, the render thread included, and has to hop.
//
- (void) _listen
{
    if (AUEventListenerCreate(sParaEQEventListener, (__bridge void *)self,
                              CFRunLoopGetCurrent(), kCFRunLoopDefaultMode,
                              0.05f, 0.05f, &_listener) != noErr)
    {
        _listener = NULL;
        return;
    }

    AudioUnitEvent event;
    memset(&event, 0, sizeof(event));

    event.mEventType                      = kAudioUnitEvent_ParameterValueChange;
    event.mArgument.mParameter.mAudioUnit = _audioUnit;
    event.mArgument.mParameter.mScope     = kAudioUnitScope_Global;
    event.mArgument.mParameter.mElement   = 0;

    for (int i = 0; i < kNumberOfParameters; i++) {
        event.mArgument.mParameter.mParameterID = (AudioUnitParameterID)i;
        AUEventListenerAddEventType(_listener, (__bridge void *)self, &event);
    }

    // A preset load restores the parameters behind the host's back and then
    // says so once, with this: AUBase::RestoreState sets the values into the
    // scope directly and follows them with one PresentPreset property change.
    // Without this the editor would still be showing the old setting.
    event.mEventType                     = kAudioUnitEvent_PropertyChange;
    event.mArgument.mProperty.mAudioUnit = _audioUnit;
    event.mArgument.mProperty.mPropertyID = kAudioUnitProperty_PresentPreset;
    event.mArgument.mProperty.mScope     = kAudioUnitScope_Global;
    event.mArgument.mProperty.mElement   = 0;
    AUEventListenerAddEventType(_listener, (__bridge void *)self, &event);

    // The host's bypass button, which flattens the curve.
    event.mArgument.mProperty.mPropertyID = kAudioUnitProperty_BypassEffect;
    AUEventListenerAddEventType(_listener, (__bridge void *)self, &event);

    // A rate change moves every frequency on the plot relative to Nyquist, and
    // it is also what the trig table was built for.
    event.mArgument.mProperty.mPropertyID = kAudioUnitProperty_StreamFormat;
    event.mArgument.mProperty.mScope      = kAudioUnitScope_Output;
    AUEventListenerAddEventType(_listener, (__bridge void *)self, &event);
}


static void sParaEQEventListener(void *inUserData, void *inObject,
                                 const AudioUnitEvent *inEvent, UInt64 inEventHostTime,
                                 AudioUnitParameterValue inParameterValue)
{
    ShellacFiltersParaEQAUBackend *backend = (__bridge ShellacFiltersParaEQAUBackend *)inUserData;

    if (!backend || !inEvent) return;

    // Both cases end in the same place.  A parameter change could update the
    // one control it names, but reading sixteen parameters is sixteen function
    // calls against a unit in the same address space, which is not the reason
    // anything here is ever slow.
    [[backend view] reloadData];
}


- (float) valueForParameter:(int)parameter
{
    return sGetParameter(_audioUnit, (AudioUnitParameterID)parameter);
}


- (void) setValue:(float)value forParameter:(int)parameter
{
    AudioUnitParameter target;

    target.mAudioUnit   = _audioUnit;
    target.mParameterID = (AudioUnitParameterID)parameter;
    target.mScope       = kAudioUnitScope_Global;
    target.mElement     = 0;

    // AUParameterSet rather than AudioUnitSetParameter: the second one sets the
    // value and tells nobody, so a host's own generic view and its automation
    // recording would both go on showing the old one.  Naming this listener and
    // this backend as the sender is what keeps the change from arriving back
    // here as news -- the same job the AUv3 original's originator token does.
    AUParameterSet(_listener, (__bridge void *)self, &target, (AudioUnitParameterValue)value, 0);
}


- (BOOL) getInfo:(ParaEQControlInfo *)outInfo forParameter:(int)parameter
{
    AudioUnitParameterInfo info;

    if (!sGetParameterInfo(_audioUnit, (AudioUnitParameterID)parameter, &info)) return NO;

    outInfo->minValue     = info.minValue;
    outInfo->maxValue     = info.maxValue;
    outInfo->defaultValue = info.defaultValue;
    outInfo->logarithmic  = (info.flags & kAudioUnitParameterFlag_DisplayLogarithmic) ? YES : NO;

    switch (info.unit) {
    case kAudioUnitParameterUnit_Hertz:    outInfo->unit = ParaEQControlUnitHertz;    break;
    case kAudioUnitParameterUnit_Decibels: outInfo->unit = ParaEQControlUnitDecibels; break;
    default:                               outInfo->unit = ParaEQControlUnitGeneric;  break;
    }

    return YES;
}


- (NSString *) nameForParameter:(int)parameter
{
    AudioUnitParameterInfo info;

    if (!sGetParameterInfo(_audioUnit, (AudioUnitParameterID)parameter, &info)) return nil;

    return sParameterName(&info);
}


- (double) sampleRate
{
    Float64 rate = 0;
    UInt32  size = sizeof(rate);

    if (AudioUnitGetProperty(_audioUnit, kAudioUnitProperty_SampleRate,
                             kAudioUnitScope_Output, 0, &rate, &size) == noErr && rate > 0)
    {
        return rate;
    }

    return 44100;
}


- (BOOL) isBypassedByHost
{
    UInt32 bypassed = 0;
    UInt32 size     = sizeof(bypassed);

    return AudioUnitGetProperty(_audioUnit, kAudioUnitProperty_BypassEffect,
                                kAudioUnitScope_Global, 0, &bypassed, &size) == noErr && bypassed;
}


- (void) _sendGesture:(AudioUnitEventType)type forParameter:(int)parameter
{
    AudioUnitEvent event;

    memset(&event, 0, sizeof(event));

    event.mEventType                        = type;
    event.mArgument.mParameter.mAudioUnit   = _audioUnit;
    event.mArgument.mParameter.mParameterID = (AudioUnitParameterID)parameter;
    event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;
    event.mArgument.mParameter.mElement     = 0;

    AUEventListenerNotify(_listener, (__bridge void *)self, &event);
}


- (void) beginGestureForParameter:(int)parameter
{
    [self _sendGesture:kAudioUnitEvent_BeginParameterChangeGesture forParameter:parameter];
}


- (void) endGestureForParameter:(int)parameter
{
    [self _sendGesture:kAudioUnitEvent_EndParameterChangeGesture forParameter:parameter];
}

@end


#pragma mark - Factory

@implementation ShellacFiltersParaEQViewFactory

- (unsigned) interfaceVersion { return 0; }

// The name a host shows in its "choose a view" menu when a unit publishes more
// than one.  This one publishes one, but a host with an empty string there
// draws an empty menu item.
- (NSString *) description { return @"ParaEQ"; }

- (NSView *) uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
    // inPreferredSize is what the host has room for, and it is advice.  The
    // layout is a console strip at a fixed size -- fourteen columns wide and
    // one row of knobs tall -- so there is nothing here that could use a
    // different one, and a host that cannot fit it scrolls or clips.
    ShellacFiltersParaEQAUBackend *backend =
        [[ShellacFiltersParaEQAUBackend alloc] initWithAudioUnit:inAudioUnit];

    ShellacFiltersParaEQView *view = [[ShellacFiltersParaEQView alloc] initWithBackend:backend];
    [backend setView:view];

    return view;
}

@end


#pragma mark - The property

OSStatus ParaEQ_CopyCocoaViewInfo(AudioUnitCocoaViewInfo * outInfo)
{
    if (outInfo == NULL) return kAudioUnitErr_InvalidPropertyValue;

    @autoreleasepool {
        // -bundleForClass: rather than CFBundleGetBundleWithIdentifier with the
        // identifier from the Info.plist: the identifier would be a copy of a
        // string that lives somewhere else, and a second place for it to be
        // wrong.  This asks which bundle the class was loaded from, which is
        // the question actually being answered.
        //
        // That bundle is the component itself.  The view class is in the same
        // binary as the unit rather than in a nested one, so there is one
        // thing to sign and one thing to notarise.
        NSBundle *bundle = [NSBundle bundleForClass:[ShellacFiltersParaEQViewFactory class]];
        NSURL    *url    = [bundle bundleURL];

        if (url == nil) return kAudioUnitErr_InvalidProperty;

        // Both fields come back +1: the caller owns them and releases them,
        // which is what kAudioUnitProperty_CocoaUI documents.
        outInfo->mCocoaAUViewBundleLocation = (CFURLRef)CFBridgingRetain(url);
        outInfo->mCocoaAUViewClass[0] =
            (CFStringRef)CFBridgingRetain(NSStringFromClass([ShellacFiltersParaEQViewFactory class]));
    }

    return noErr;
}
