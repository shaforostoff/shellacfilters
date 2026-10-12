/*
 *	ParaEQView.mm
 *	ParaEQ
 *
 *	The editor. See ParaEQView.h for what it is and where it came from; this is
 *	the whole of it, in the same order the window reads: the knob, the curve,
 *	the layout tables, the view, the factory.
 *
 *	Everything that talks to the Audio Unit is in the four helpers at the top.
 *	That is the whole of the difference between this and the AUv3 original: a
 *	v3 view holds an AUParameterTree and asks an AUParameter for its value, its
 *	range and its default, while this holds the opaque AudioUnit and asks
 *	AudioUnitGetParameter, kAudioUnitProperty_ParameterInfo and an
 *	AUEventListener for the same three things. Below those helpers the two files
 *	are the same editor.
 */

#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AudioUnitUtilities.h>

#include "ParaEQControls.h"
#include "ParaEQView.h"

#include <cmath>
#include <cstring>
#include <vector>


static const CGFloat sMargin        = 20;
static const CGFloat sColumnWidth   = 52;
static const CGFloat sSectionGap    = 12;

static const CGFloat sCurveHeight   = 132;
static const CGFloat sCurveGap      = 16;

static const CGFloat sTitleHeight   = 13;
static const CGFloat sLabelHeight   = 12;
static const CGFloat sKnobRowHeight = 42;
static const CGFloat sValueHeight   = 14;
static const CGFloat sSwitchHeight  = 21;

static const CGFloat sLargeKnob     = 40;
static const CGFloat sSmallKnob     = 30;

// Pixels of vertical drag for the full range of a knob.  Roughly the height of
// the window, which is the distance a hand expects to move for "all of it".
static const CGFloat sDragTravel    = 170;

static const double  sCurveMinHz    = 20.0;
static const double  sCurveMaxHz    = 20000.0;
static const double  sCurveMaxDb    = 24.0;


static double sClamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}


// No NSLocalizedString anywhere in this file, and that is deliberate rather
// than an omission.  NSLocalizedString reads the *main* bundle, which in a
// plug-in is the host application - so every string here would be looked up in
// Logic's tables and would quietly come back as whatever Logic has under that
// key.  A plug-in that wants localising uses its own bundle explicitly; this
// one has nothing to localise into, so the strings are literals.


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
// Nothing about a control is written down twice in this file either.  Whether a
// knob is logarithmic is the unit's own DisplayLogarithmic flag, and whether it
// is bipolar is whether its range crosses zero - both read from the same table
// the host's generic view reads, so an editor knob and a host slider cannot
// come to disagree about what a control's travel means.
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


static double sSampleRate(AudioUnit audioUnit)
{
    Float64 rate = 0;
    UInt32  size = sizeof(rate);

    if (AudioUnitGetProperty(audioUnit, kAudioUnitProperty_SampleRate,
                             kAudioUnitScope_Output, 0, &rate, &size) == noErr && rate > 0)
    {
        return rate;
    }

    return 44100;
}


#pragma mark - Knob

@class ShellacFiltersParaEQKnob;

// Gesture, rather than value: the value goes through target/action, the same as
// any other NSControl.  These two bracket a drag so a host records one
// automation move rather than two hundred, which is also one undo step rather
// than two hundred.
@protocol ShellacFiltersParaEQKnobDelegate <NSObject>
- (void) knobDidBeginGesture:(ShellacFiltersParaEQKnob *)knob;
- (void) knobDidEndGesture:(ShellacFiltersParaEQKnob *)knob;
@end


@interface ShellacFiltersParaEQKnob : NSControl

@property (nonatomic) double minValue;
@property (nonatomic) double maxValue;
@property (nonatomic) double defaultValue;
@property (nonatomic) BOOL   logarithmic;
@property (nonatomic) BOOL   bipolar;

@property (nonatomic, weak) id<ShellacFiltersParaEQKnobDelegate> gestureDelegate;

@end


@implementation ShellacFiltersParaEQKnob {
    double  _value;
    double  _dragStartValue;
    CGFloat _dragStartY;
}


- (instancetype) initWithFrame:(NSRect)frameRect
{
    if ((self = [super initWithFrame:frameRect])) {
        _minValue = 0;
        _maxValue = 1;
        [self setContinuous:YES];
    }

    return self;
}


- (double) doubleValue        { return _value; }
- (void) setDoubleValue:(double)v
{
    double clamped = sClamp(v, _minValue, _maxValue);

    if (clamped != _value) {
        _value = clamped;
        [self setNeedsDisplay:YES];
    }
}


#pragma mark - Scale

// A frequency knob has to be logarithmic or its whole bottom half is unusable:
// on a linear 1.5-16 kHz sweep, everything below 3 kHz lives in the first tenth
// of the travel.
//
- (double) _normalizedFromValue:(double)value
{
    if (_logarithmic && _minValue > 0 && _maxValue > _minValue) {
        return log(value / _minValue) / log(_maxValue / _minValue);
    }

    if (_maxValue <= _minValue) return 0;

    return (value - _minValue) / (_maxValue - _minValue);
}


- (double) _valueFromNormalized:(double)normalized
{
    normalized = sClamp(normalized, 0, 1);

    if (_logarithmic && _minValue > 0 && _maxValue > _minValue) {
        return _minValue * pow(_maxValue / _minValue, normalized);
    }

    return _minValue + (normalized * (_maxValue - _minValue));
}


#pragma mark - Drawing

// Minimum at 7:30, maximum at 4:30, 270 degrees of clockwise travel through
// straight up.  NSBezierPath measures counter-clockwise from the positive x
// axis, hence the subtraction.
static CGFloat sAngleForNormalized(double normalized)
{
    return (CGFloat)(225.0 - (270.0 * normalized));
}


- (void) drawRect:(NSRect)dirtyRect
{
    CGRect  bounds = [self bounds];
    CGPoint center = CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds));
    CGFloat radius = (MIN(bounds.size.width, bounds.size.height) / 2) - 3;

    if (radius <= 2) return;

    double  normalized = [self _normalizedFromValue:_value];
    CGFloat angle      = sAngleForNormalized(normalized);
    double  originN    = _bipolar ? 0.5 : 0.0;

    NSBezierPath *track = [NSBezierPath bezierPath];
    [track appendBezierPathWithArcWithCenter:center radius:radius
                                  startAngle:sAngleForNormalized(0)
                                    endAngle:sAngleForNormalized(1)
                                   clockwise:YES];
    [track setLineWidth:3];
    [track setLineCapStyle:NSLineCapStyleRound];
    [[NSColor tertiaryLabelColor] set];
    [track stroke];

    if (fabs(normalized - originN) > 0.0005) {
        NSBezierPath *filled = [NSBezierPath bezierPath];
        [filled appendBezierPathWithArcWithCenter:center radius:radius
                                       startAngle:sAngleForNormalized(originN)
                                         endAngle:angle
                                        clockwise:(normalized > originN)];
        [filled setLineWidth:3];
        [filled setLineCapStyle:NSLineCapStyleRound];
        [[NSColor controlAccentColor] set];
        [filled stroke];
    }

    CGFloat radians = (CGFloat)(angle * M_PI / 180.0);
    CGFloat dx = cos(radians), dy = sin(radians);

    NSBezierPath *pointer = [NSBezierPath bezierPath];
    [pointer moveToPoint:CGPointMake(center.x + (dx * radius * 0.30),
                                     center.y + (dy * radius * 0.30))];
    [pointer lineToPoint:CGPointMake(center.x + (dx * (radius - 1)),
                                     center.y + (dy * (radius - 1)))];
    [pointer setLineWidth:2];
    [pointer setLineCapStyle:NSLineCapStyleRound];
    [[NSColor labelColor] set];
    [pointer stroke];

    if ([[self window] firstResponder] == self && [[self window] isKeyWindow]) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:CGRectInset(bounds, 1, 1)];
        [ring setLineWidth:2];
        [[NSColor keyboardFocusIndicatorColor] set];
        [ring stroke];
    }
}


#pragma mark - Interaction

- (BOOL) acceptsFirstResponder { return YES; }
- (BOOL) becomeFirstResponder  { [self setNeedsDisplay:YES]; return YES; }
- (BOOL) resignFirstResponder  { [self setNeedsDisplay:YES]; return YES; }


- (void) _commitNormalized:(double)normalized
{
    [self setDoubleValue:[self _valueFromNormalized:normalized]];
    [self sendAction:[self action] to:[self target]];
}


// No nested tracking loop, which is worth keeping from the original for a
// second reason here: an AU editor lives in somebody else's window, and a
// plug-in that runs its own event loop inside a host's is a plug-in that gets
// blamed for the host's next beachball.
//
- (void) mouseDown:(NSEvent *)event
{
    [[self window] makeFirstResponder:self];

    [_gestureDelegate knobDidBeginGesture:self];

    if ([event clickCount] == 2) {
        [self setDoubleValue:_defaultValue];
        [self sendAction:[self action] to:[self target]];
        return;
    }

    _dragStartValue = _value;
    _dragStartY     = [event locationInWindow].y;
}


- (void) mouseDragged:(NSEvent *)event
{
    CGFloat delta = [event locationInWindow].y - _dragStartY;
    double  scale = ([event modifierFlags] & NSEventModifierFlagShift) ? 0.25 : 1.0;

    double normalized = [self _normalizedFromValue:_dragStartValue] +
                        ((delta / sDragTravel) * scale);

    [self _commitNormalized:normalized];
}


- (void) mouseUp:(NSEvent *)event
{
    [_gestureDelegate knobDidEndGesture:self];
}


// The wheel and the arrow keys send bare value changes with no gesture around
// them.  A gesture is a claim that a move has a beginning and an end, and these
// two have neither: there is no moment at which a wheel is finished.
//
- (void) scrollWheel:(NSEvent *)event
{
    CGFloat delta = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas]) delta *= 0.1;

    [self _commitNormalized:[self _normalizedFromValue:_value] + (delta * 0.01)];
}


- (void) keyDown:(NSEvent *)event
{
    NSString *characters = [event charactersIgnoringModifiers];

    if (![characters length]) {
        [super keyDown:event];
        return;
    }

    unichar character = [characters characterAtIndex:0];
    double  step      = ([event modifierFlags] & NSEventModifierFlagShift) ? 0.002 : 0.01;
    double  normalized = [self _normalizedFromValue:_value];

    if (character == NSUpArrowFunctionKey || character == NSRightArrowFunctionKey) {
        [self _commitNormalized:normalized + step];

    } else if (character == NSDownArrowFunctionKey || character == NSLeftArrowFunctionKey) {
        [self _commitNormalized:normalized - step];

    } else {
        [super keyDown:event];
    }
}


#pragma mark - Accessibility

- (NSAccessibilityRole) accessibilityRole { return NSAccessibilitySliderRole; }
- (id) accessibilityValue                 { return @(_value); }
- (BOOL) isAccessibilityElement           { return YES; }

@end


#pragma mark - Curve

@interface ShellacFiltersParaEQCurveView : NSView
@property (nonatomic) AudioUnit audioUnit;
@end


@implementation ShellacFiltersParaEQCurveView {
    // The frequency axis, the trig table read off it, and the curve read off
    // that.  All three depend on the plot geometry and the sample rate and on
    // nothing a knob can move, which is what makes them worth keeping between
    // redraws -- see -_prepareCurveTableForPlot:rate: below.
    std::vector<double> _curveHz;
    std::vector<double> _curveTrig;
    std::vector<float>  _curveDb;
    CGFloat             _curveWidth;
    double              _curveRate;
}

- (BOOL) isOpaque { return NO; }


static CGFloat sXForFrequency(CGRect plot, double hz)
{
    double normalized = log(hz / sCurveMinHz) / log(sCurveMaxHz / sCurveMinHz);
    return plot.origin.x + (CGFloat)(normalized * plot.size.width);
}


static CGFloat sYForDecibels(CGRect plot, double db)
{
    double normalized = sClamp(db, -sCurveMaxDb, sCurveMaxDb) / sCurveMaxDb;
    return CGRectGetMidY(plot) + (CGFloat)(normalized * (plot.size.height / 2));
}


- (void) _drawGridInPlot:(CGRect)plot
{
    static const double lines[]  = { 20, 30, 50, 100, 200, 300, 500, 1000,
                                     2000, 3000, 5000, 10000, 20000 };
    static const double labels[] = { 100, 1000, 10000 };
    static const double decibels[] = { -20, -10, 0, 10, 20 };

    NSDictionary *attributes = @{
        NSFontAttributeName:            [NSFont systemFontOfSize:9],
        NSForegroundColorAttributeName: [NSColor tertiaryLabelColor]
    };

    [[NSColor separatorColor] set];

    for (int i = 0; i < 13; i++) {
        CGFloat x = round(sXForFrequency(plot, lines[i])) + 0.5;

        NSBezierPath *path = [NSBezierPath bezierPath];
        [path moveToPoint:CGPointMake(x, CGRectGetMinY(plot))];
        [path lineToPoint:CGPointMake(x, CGRectGetMaxY(plot))];
        [path setLineWidth:1];
        [path stroke];
    }

    for (int i = 0; i < 5; i++) {
        CGFloat y = round(sYForDecibels(plot, decibels[i])) + 0.5;

        NSBezierPath *path = [NSBezierPath bezierPath];
        [path moveToPoint:CGPointMake(CGRectGetMinX(plot), y)];
        [path lineToPoint:CGPointMake(CGRectGetMaxX(plot), y)];
        [path setLineWidth:1];

        if (decibels[i] == 0) {
            [[NSColor secondaryLabelColor] set];
            [path stroke];
            [[NSColor separatorColor] set];
        } else {
            [path stroke];

            NSString *text = [NSString stringWithFormat:@"%+.0f", decibels[i]];
            [text drawAtPoint:CGPointMake(CGRectGetMinX(plot) + 3, y + 1) withAttributes:attributes];
        }
    }

    for (int i = 0; i < 3; i++) {
        NSString *text = labels[i] >= 1000 ?
            [NSString stringWithFormat:@"%gk", labels[i] / 1000] :
            [NSString stringWithFormat:@"%g", labels[i]];

        CGSize size = [text sizeWithAttributes:attributes];
        CGFloat x = sXForFrequency(plot, labels[i]) - (size.width / 2);

        [text drawAtPoint:CGPointMake(x, CGRectGetMinY(plot) + 2) withAttributes:attributes];
    }
}


// Brings the frequency axis and its trig table into line with this plot and
// this sample rate, and answers how many points they hold -- 0 when the plot is
// too small to draw a line across.
//
// The table is the expensive half of drawing a curve.  Asking magnitudeDb() for
// one point costs four trig calls per stage and a logarithm per stage, and both
// are per-point constants in disguise: cos(w) and cos(2w) depend on the
// frequency and the sample rate, neither of which a knob can move, and the six
// stage magnitudes are multiplied, so their six logarithms are one logarithm of
// the product.  Built here, a redraw is arithmetic -- about a fifth of the work,
// which is worth having on a drag.  See the note above curveTrig() in
// paraeq_core.h.
- (size_t) _prepareCurveTableForPlot:(CGRect)plot rate:(double)rate
{
    if (!(plot.size.width >= 1) || !(rate > 0)) return 0;

    // The points the drawing loop below walks: one per pixel column from the
    // left edge of the plot to the right.  Normalized against the full width
    // rather than against the last index, so a point lands exactly where
    // sXForFrequency would put its frequency.
    const size_t count = (size_t)floor((double)plot.size.width) + 1;

    if (count != _curveHz.size() ||
        plot.size.width != _curveWidth ||
        rate != _curveRate)
    {
        _curveHz.resize(count);
        _curveTrig.resize(count * (size_t)paraeq::kCurveTrigStride);
        _curveDb.resize(count);

        for (size_t i = 0; i < count; i++) {
            double normalized = (double)i / (double)plot.size.width;
            _curveHz[i] = sCurveMinHz * pow(sCurveMaxHz / sCurveMinHz, normalized);
        }

        paraeq::curveTrig(_curveHz.data(), count, rate, _curveTrig.data());

        _curveWidth = plot.size.width;
        _curveRate  = rate;
    }

    return count;
}


// The sixteen controls read back as core parameters, which is what a response
// curve is drawn from.  Through the same mapping the wrapper designs its
// filters with -- ParaEQControls.h -- so the curve on screen cannot come to
// disagree with what the audio is doing.
//
// Two bypasses reach this, and either one flattens the curve: the unit's own
// Bypass control, which is a target the core glides to, and the host's
// kAudioUnitProperty_BypassEffect, which stops the unit being asked to render
// at all.  Drawing a curve while a host has the effect switched out would be
// drawing something nothing is doing.
//
- (paraeq::Params) _params
{
    float values[kNumberOfParameters];

    for (int i = 0; i < kNumberOfParameters; i++) {
        values[i] = sGetParameter(_audioUnit, (AudioUnitParameterID)i);
    }

    paraeq::Params params = ParaEQParamsFromControls(values);

    UInt32 bypassed = 0;
    UInt32 size     = sizeof(bypassed);

    if (AudioUnitGetProperty(_audioUnit, kAudioUnitProperty_BypassEffect,
                             kAudioUnitScope_Global, 0, &bypassed, &size) == noErr && bypassed)
    {
        params.bypass = true;
    }

    return params;
}


- (void) drawRect:(NSRect)dirtyRect
{
    CGRect bounds = [self bounds];

    NSBezierPath *background = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:4 yRadius:4];
    [[NSColor controlBackgroundColor] set];
    [background fill];

    CGRect plot = CGRectInset(bounds, 1, 1);

    [NSGraphicsContext saveGraphicsState];
    [background addClip];
    [self _drawGridInPlot:plot];

    if (_audioUnit) {
        double rate = sSampleRate(_audioUnit);

        paraeq::Config config;
        config.compute([self _params], rate);

        const size_t count = [self _prepareCurveTableForPlot:plot rate:rate];

        if (count >= 2) {
            // The whole cascade in one pass over the table, including the
            // output trim.  Reads the targets rather than the coefficients
            // actually in use, so the curve shows where the controls are and
            // not where a glide has got to.
            paraeq::curveDb(config, _curveTrig.data(), count, _curveDb.data());

            NSBezierPath *curve = [NSBezierPath bezierPath];
            NSBezierPath *fill  = [NSBezierPath bezierPath];

            CGFloat zeroY = sYForDecibels(plot, 0);

            for (size_t i = 0; i < count; i++) {
                CGPoint point = CGPointMake(plot.origin.x + (CGFloat)i,
                                            sYForDecibels(plot, _curveDb[i]));

                if (i == 0) {
                    [curve moveToPoint:point];
                    [fill moveToPoint:CGPointMake(point.x, zeroY)];
                    [fill lineToPoint:point];
                } else {
                    [curve lineToPoint:point];
                    [fill lineToPoint:point];
                }
            }

            [fill lineToPoint:CGPointMake(plot.origin.x + (CGFloat)(count - 1), zeroY)];
            [fill closePath];

            [[[NSColor controlAccentColor] colorWithAlphaComponent:0.16] set];
            [fill fill];

            [curve setLineWidth:1.5];
            [[NSColor controlAccentColor] set];
            [curve stroke];
        }
    }

    [NSGraphicsContext restoreGraphicsState];

    [[NSColor separatorColor] set];
    [background setLineWidth:1];
    [background stroke];
}

@end


#pragma mark - Layout table

typedef struct {
    int                  column;
    int                  columns;   //!< span, so a lone knob can centre itself
    AudioUnitParameterID parameter;
    const char          *label;
    BOOL                 large;
} ParaEQKnobSpec;


// Fourteen columns, read left to right as a console strip.  The four gain knobs
// are the large ones because they are the four an operator actually reaches for
// once the session has started; frequency and Q get set once for a box of
// records.  Nothing else here says anything about a control that the unit's own
// parameter table already says -- see sGetParameterInfo above.
//
static const ParaEQKnobSpec sKnobs[] = {
    // Spans both of its section's columns so the slope switch underneath has
    // room for three legible segments.
    {  0, 2, kParam_A, "Freq", NO  },

    {  2, 1, kParam_C, "Gain", YES },
    {  3, 1, kParam_D, "Freq", NO  },

    {  4, 1, kParam_F, "Gain", YES },
    {  5, 1, kParam_G, "Freq", NO  },
    {  6, 1, kParam_H, "Q",    NO  },

    {  7, 1, kParam_I, "Gain", YES },
    {  8, 1, kParam_J, "Freq", NO  },
    {  9, 1, kParam_K, "Q",    NO  },

    { 10, 1, kParam_L, "Gain", YES },
    { 11, 1, kParam_M, "Freq", NO  },

    { 12, 2, kParam_O, "Trim", NO  }
};

static const int sKnobCount = (int)(sizeof(sKnobs) / sizeof(sKnobs[0]));


typedef struct {
    const char          *title;
    int                  firstColumn;
    int                  columns;
    AudioUnitParameterID switchParameter;   //!< kNoSwitch for none
    int                  switchSegments;
    //! Segment titles, comma separated.  Written here rather than taken from
    //! the unit's value strings because none of the three would fit: "12
    //! dB/oct" is what the slope is called everywhere else and it does not go
    //! in a third of a section, and the two shape controls and Bypass are
    //! Boolean parameters, which a host draws as a checkbox and never asks for
    //! strings for.
    const char          *switchLabels;
} ParaEQSectionSpec;

static const AudioUnitParameterID kNoSwitch = kNumberOfParameters;

// The sections, named for the job rather than for the console.  An operator
// reaching for the hiss should not have to know that a strip would have called
// that band HF, and the names say which way the knob usually goes, which is the
// point of a fixed layout: these are four jobs, not four identical bands.  The
// shorthand survives in the parameter names, which is what the host's own
// generic view draws and what a host writes into a saved preset.
//
// The same names the foobar2000 component and the VST editor show, and from the
// same place: the tango transfer literature, which calls them low cut, bass,
// reverb cut, brilliance and hiss cut.
//
// OUTPUT is two columns where the AUv3 original had one, because this unit has
// a Bypass control of its own -- the core glides to it rather than switching,
// which is the whole reason it is a parameter and not just the host's property
// -- and a switch needs the width to say so.
//
static const ParaEQSectionSpec sSections[] = {
    { "LOW CUT",     0, 2, kParam_B,  3, "Off,12,24"  },
    { "BASS",        2, 2, kParam_E,  2, "Shelf,Bell" },
    { "REVERB CUT",  4, 3, kNoSwitch, 0, NULL         },
    { "BRILLIANCE",  7, 3, kNoSwitch, 0, NULL         },
    { "HISS CUT",   10, 2, kParam_N,  2, "Shelf,Bell" },
    { "OUTPUT",     12, 2, kParam_P,  2, "EQ,Bypass"  }
};

static const int sSectionCount = (int)(sizeof(sSections) / sizeof(sSections[0]));
static const int sColumnCount  = 14;


// One entry per control, in parameter order.  These say what each control is
// *for* on a disc transfer, because the layout is only worth having if the
// operator knows which knob is the hiss.
//
// A C table rather than an NSArray of NSStrings, and indexed by the parameter
// rather than by position in a list: this is read with a parameter ID in hand,
// and the comment against each row is what makes the ordering auditable.
//
static const char * const sParameterHelp[kNumberOfParameters] = {

/* kParam_A */ "Corner of the high-pass filter, which is what takes out turntable rumble and "
               "the roar under a disc transfer. Wind it up until the bottom stops wallowing; "
               "past about 120 Hz it is removing the double bass along with the rumble.",

/* kParam_B */ "Steepness of the high-pass. Off leaves the filter out entirely. 24 dB/oct "
               "clears rumble while staying closer to the corner, which is usually what a worn "
               "transfer wants; 12 dB/oct is the gentler slope where the low end is worth "
               "keeping.",

/* kParam_C */ "The bass knob. Cut and boost at the Low frequency, and on transfers from "
               "1926-1949 this is the one that puts back the weight the recording chain never "
               "captured.",

/* kParam_D */ "Where the bass shelf turns over. 60-125 Hz is the band those transfers are "
               "short of; keeping it there leaves the cello and the double bass alone.",

/* kParam_E */ "Shelf lifts or drops everything below the frequency. Bell concentrates the "
               "same move around it, for when only one part of the low end needs the help.",

/* kParam_F */ "The knob that takes the room out. Cut here and the boxiness a recording horn "
               "or a hall adds goes with it - around 1 kHz on most of this material. Narrow the "
               "Q first if what you are removing is a single ring rather than a general "
               "thickness.",

/* kParam_G */ "Centre of the low-mid band. Sweep it with a few dB of boost to find what is "
               "honking, then cut there.",

/* kParam_H */ "How wide the low-mid band is. Low values are broad and musical; high values "
               "are narrow enough to take out a resonance without taking the music either side "
               "of it.",

/* kParam_I */ "The brilliance knob. 4-6 kHz is where the detail on a shellac sits, under the "
               "surface noise rather than above it, so a boost here brings up the instrument "
               "and the noise together - which is what the High band is then for.",

/* kParam_J */ "Centre of the high-mid band. 5 kHz is the middle of the brilliance range and "
               "far enough below the hiss shelf that the two knobs do not fight.",

/* kParam_K */ "How wide the high-mid band is. Broad for brilliance, narrow for a single "
               "piercing partial.",

/* kParam_L */ "The hiss knob. Most shellacs carry little programme above 8 kHz and a great "
               "deal of surface noise, so this generally goes down. Pull it only as far as the "
               "cymbals and the bandoneon reeds allow.",

/* kParam_M */ "Where the high shelf turns over. Lower reaches further down into the noise and "
               "further into the music with it.",

/* kParam_N */ "Shelf drops or lifts everything above the frequency. Bell concentrates the "
               "move around it, which is the better shape when the noise sits in a band rather "
               "than across the whole top.",

/* kParam_O */ "Makeup for whatever the bands did, so the equaliser can be compared against "
               "bypass at the same loudness. Boosting four bands and leaving this at 0 dB is "
               "how an EQ ends up sounding better than it is.",

/* kParam_P */ "Takes the equaliser out, gliding rather than switching: the curve flattens "
               "over about 20 ms instead of the coefficients jumping, so the comparison is "
               "against the same programme rather than against a click."
};


#pragma mark - Main view

@interface ShellacFiltersParaEQView () <ShellacFiltersParaEQKnobDelegate>
@end


//! Defined below -_listen, which is the only thing that installs it.
static void sParaEQEventListener(void *inUserData, void *inObject,
                                 const AudioUnitEvent *inEvent, UInt64 inEventHostTime,
                                 AudioUnitParameterValue inParameterValue);


@implementation ShellacFiltersParaEQView {
    AudioUnit          _audioUnit;
    AUEventListenerRef _listener;

    ShellacFiltersParaEQCurveView *_curveView;

    NSMutableArray<ShellacFiltersParaEQKnob *> *_knobs;
    NSMutableArray<NSTextField *>              *_valueFields;
    NSMutableArray<NSSegmentedControl *>       *_switches;
}


+ (CGSize) fittingSize
{
    CGFloat width = (sMargin * 2) + (sColumnCount * sColumnWidth) +
                    ((sSectionCount - 1) * sSectionGap);

    CGFloat height = sMargin + sCurveHeight + sCurveGap + sTitleHeight + 4 +
                     sLabelHeight + 3 + sKnobRowHeight + 2 + sValueHeight + 6 +
                     sSwitchHeight + sMargin;

    return CGSizeMake(width, height);
}


- (instancetype) initWithAudioUnit:(AudioUnit)audioUnit
{
    CGSize size = [[self class] fittingSize];

    if ((self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)])) {
        _audioUnit = audioUnit;

        _knobs       = [NSMutableArray arrayWithCapacity:sKnobCount];
        _valueFields = [NSMutableArray arrayWithCapacity:sKnobCount];
        _switches    = [NSMutableArray arrayWithCapacity:sSectionCount];

        [self _build];
        [self _buildMenu];
        [self _listen];
        [self reloadData];
    }

    return self;
}


- (void) dealloc
{
    if (_listener) AUListenerDispose(_listener);
}


- (BOOL) isFlipped { return YES; }


// Paints its own background rather than borrowing the window's.  The window
// does supply one, so this looks like a formality -- but every colour in here
// is a semantic one, and a semantic colour is only legible against a background
// from the same appearance.  In a host that hands a plug-in an unpainted
// container, white-in-dark-mode text on an unpainted view is invisible.
//
- (void) drawRect:(NSRect)dirtyRect
{
    [[NSColor windowBackgroundColor] set];
    NSRectFill(dirtyRect);
}


#pragma mark - Listening

// Catches everything that changes a value from outside this view: a host
// automating a parameter, a preset load, and the host's own bypass button.
// Values this view sets name it as the sender, so they do not come back round.
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
    ShellacFiltersParaEQView *view = (__bridge ShellacFiltersParaEQView *)inUserData;

    if (!view || !inEvent) return;

    // Both cases end in the same place.  A parameter change could update the
    // one control it names, but reading sixteen parameters is sixteen function
    // calls against a unit in the same address space, which is not the reason
    // anything here is ever slow.
    [view reloadData];
}


#pragma mark - Setting

- (void) _setParameter:(AudioUnitParameterID)parameter to:(float)value
{
    AudioUnitParameter target;

    target.mAudioUnit   = _audioUnit;
    target.mParameterID = parameter;
    target.mScope       = kAudioUnitScope_Global;
    target.mElement     = 0;

    // AUParameterSet rather than AudioUnitSetParameter: the second one sets the
    // value and tells nobody, so a host's own generic view and its automation
    // recording would both go on showing the old one.  Naming this listener and
    // this view as the sender is what keeps the change from arriving back here
    // as news -- the same job the AUv3 original's originator token does.
    AUParameterSet(_listener, (__bridge void *)self, &target, (AudioUnitParameterValue)value, 0);
}


- (void) _sendGesture:(AudioUnitEventType)type forParameter:(AudioUnitParameterID)parameter
{
    AudioUnitEvent event;

    memset(&event, 0, sizeof(event));

    event.mEventType                        = type;
    event.mArgument.mParameter.mAudioUnit   = _audioUnit;
    event.mArgument.mParameter.mParameterID = parameter;
    event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;
    event.mArgument.mParameter.mElement     = 0;

    AUEventListenerNotify(_listener, (__bridge void *)self, &event);
}


- (void) knobDidBeginGesture:(ShellacFiltersParaEQKnob *)knob
{
    NSInteger index = [knob tag];
    if (index < 0 || index >= sKnobCount) return;

    [self _sendGesture:kAudioUnitEvent_BeginParameterChangeGesture
          forParameter:sKnobs[index].parameter];
}


- (void) knobDidEndGesture:(ShellacFiltersParaEQKnob *)knob
{
    NSInteger index = [knob tag];
    if (index < 0 || index >= sKnobCount) return;

    [self _sendGesture:kAudioUnitEvent_EndParameterChangeGesture
          forParameter:sKnobs[index].parameter];
}


#pragma mark - Building

static CGFloat sXForColumn(int column)
{
    CGFloat x = sMargin;

    for (int i = 0; i < sSectionCount; i++) {
        if (column < sSections[i].firstColumn + sSections[i].columns) {
            return x + ((column - sSections[i].firstColumn) * sColumnWidth);
        }

        x += (sSections[i].columns * sColumnWidth) + sSectionGap;
    }

    return x;
}


static NSTextField *sMakeLabel(NSString *string, CGRect frame, CGFloat fontSize,
                               NSColor *color, NSFontWeight weight)
{
    NSTextField *label = [NSTextField labelWithString:string];

    [label setFrame:frame];
    [label setAlignment:NSTextAlignmentCenter];
    [label setFont:[NSFont systemFontOfSize:fontSize weight:weight]];
    [label setTextColor:color];
    [[label cell] setLineBreakMode:NSLineBreakByClipping];

    return label;
}


// Flatten and Restore Defaults live on a contextual menu rather than on
// buttons.  An AU editor is a rectangle inside somebody else's window with no
// toolbar of its own, and two buttons' worth of chrome across the bottom would
// cost more room than the gestures are worth.
//
- (void) _buildMenu
{
    NSMenu *menu = [[NSMenu alloc] init];

    NSMenuItem *flatten = [menu addItemWithTitle:@"Flatten Gains"
                                          action:@selector(flatten)
                                   keyEquivalent:@""];
    [flatten setTarget:self];

    NSMenuItem *defaults = [menu addItemWithTitle:@"Restore Defaults"
                                           action:@selector(restoreDefaults)
                                    keyEquivalent:@""];
    [defaults setTarget:self];

    [self setMenu:menu];
}


- (void) _build
{
    CGSize  size = [[self class] fittingSize];
    CGFloat contentWidth = size.width - (sMargin * 2);

    CGFloat y = sMargin;

    _curveView = [[ShellacFiltersParaEQCurveView alloc] initWithFrame:
        NSMakeRect(sMargin, y, contentWidth, sCurveHeight)];
    [_curveView setAudioUnit:_audioUnit];
    [self addSubview:_curveView];

    y += sCurveHeight + sCurveGap;

    CGFloat titleY  = y;
    CGFloat labelY  = titleY + sTitleHeight + 4;
    CGFloat knobY   = labelY + sLabelHeight + 3;
    CGFloat valueY  = knobY  + sKnobRowHeight + 2;
    CGFloat switchY = valueY + sValueHeight + 6;

    for (int i = 0; i < sSectionCount; i++) {
        const ParaEQSectionSpec *section = &sSections[i];

        CGFloat x     = sXForColumn(section->firstColumn);
        CGFloat width = section->columns * sColumnWidth;

        // titleLabel rather than title: the segment loop below has a title of
        // its own, and one shadowing the other is a way to read this wrong.
        NSTextField *titleLabel = sMakeLabel(@(section->title),
            NSMakeRect(x, titleY, width, sTitleHeight), 10,
            [NSColor secondaryLabelColor], NSFontWeightSemibold);

        [self addSubview:titleLabel];

        if (section->switchParameter == kNoSwitch) continue;

        // A segmented control rather than a popup: a popup opens a modal menu
        // loop, and a plug-in that runs one inside a host's event loop is a
        // plug-in that gets blamed for the host's next beachball.
        NSSegmentedControl *control = [[NSSegmentedControl alloc] initWithFrame:
            NSMakeRect(x + 2, switchY, width - 4, sSwitchHeight)];

        [control setSegmentCount:section->switchSegments];
        [control setSegmentStyle:NSSegmentStyleRounded];
        [control setControlSize:NSControlSizeSmall];
        [control setFont:[NSFont systemFontOfSize:9]];
        [control setTag:(NSInteger)section->switchParameter];
        [control setTarget:self];
        [control setAction:@selector(_handleSwitch:)];
        [control setToolTip:[self _toolTipForParameter:section->switchParameter]];

        NSArray<NSString *> *titles =
            [@(section->switchLabels) componentsSeparatedByString:@","];

        for (int segment = 0; segment < section->switchSegments; segment++) {
            NSString *title = (segment < (int)[titles count]) ?
                [titles objectAtIndex:segment] : @"";

            [control setLabel:title forSegment:segment];
            [control setWidth:((width - 4) / section->switchSegments) forSegment:segment];
        }

        [self addSubview:control];
        [_switches addObject:control];
    }

    for (int i = 0; i < sKnobCount; i++) {
        const ParaEQKnobSpec *spec = &sKnobs[i];

        AudioUnitParameterInfo info;
        BOOL                   known = sGetParameterInfo(_audioUnit, spec->parameter, &info);

        NSString *toolTip = [self _toolTipForParameter:spec->parameter];

        CGFloat columnX  = sXForColumn(spec->column);
        CGFloat columnW  = spec->columns * sColumnWidth;
        CGFloat diameter = spec->large ? sLargeKnob : sSmallKnob;

        NSTextField *label = sMakeLabel(@(spec->label),
            NSMakeRect(columnX, labelY, columnW, sLabelHeight), 10,
            [NSColor tertiaryLabelColor], NSFontWeightRegular);
        [label setToolTip:toolTip];
        [self addSubview:label];

        ShellacFiltersParaEQKnob *knob = [[ShellacFiltersParaEQKnob alloc] initWithFrame:NSMakeRect(
            columnX + ((columnW - diameter) / 2),
            knobY + ((sKnobRowHeight - diameter) / 2),
            diameter, diameter)];

        [knob setMinValue:known ? info.minValue : 0];
        [knob setMaxValue:known ? info.maxValue : 1];
        [knob setDefaultValue:known ? info.defaultValue : 0];
        [knob setLogarithmic:(known && (info.flags & kAudioUnitParameterFlag_DisplayLogarithmic)) ? YES : NO];
        [knob setBipolar:(known && info.minValue < 0 && info.maxValue > 0) ? YES : NO];
        [knob setTag:i];
        [knob setTarget:self];
        [knob setAction:@selector(_handleKnob:)];
        [knob setGestureDelegate:self];
        [knob setToolTip:toolTip];
        if (known) [knob setAccessibilityLabel:sParameterName(&info)];

        [self addSubview:knob];
        [_knobs addObject:knob];

        NSTextField *valueField = sMakeLabel(@"",
            NSMakeRect(columnX - 4, valueY, columnW + 8, sValueHeight), 10,
            [NSColor labelColor], NSFontWeightRegular);
        [valueField setToolTip:toolTip];
        [self addSubview:valueField];
        [_valueFields addObject:valueField];
    }
}


- (NSString *) _toolTipForParameter:(AudioUnitParameterID)parameter
{
    if (parameter >= kNumberOfParameters) return nil;

    return [NSString stringWithFormat:@"%s\n\nDouble-click a knob to restore its default value.",
            sParameterHelp[parameter]];
}


#pragma mark - Formatting

- (NSString *) _stringForParameter:(AudioUnitParameterID)parameter
{
    AudioUnitParameterInfo info;

    if (!sGetParameterInfo(_audioUnit, parameter, &info)) return @"";

    float value = sGetParameter(_audioUnit, parameter);

    switch (info.unit) {
    case kAudioUnitParameterUnit_Hertz:
        if (value >= 1000) {
            double kilohertz = value / 1000.0;
            NSString *number = (fabs(kilohertz - round(kilohertz)) < 0.05) ?
                [NSString stringWithFormat:@"%.0f", kilohertz] :
                [NSString stringWithFormat:@"%.1f", kilohertz];

            return [NSString stringWithFormat:@"%@ kHz", number];
        }

        return [NSString stringWithFormat:@"%.0f Hz", value];

    case kAudioUnitParameterUnit_Decibels:
        // Signed, because on a gain knob "5" and "-5" are the whole question
        // and the reader should not have to look at the knob to tell which.
        if (fabs(value) < 0.05) return @"0.0 dB";
        return [NSString stringWithFormat:@"%+.1f dB", value];

    default:
        return [NSString stringWithFormat:@"%.2f", value];
    }
}


#pragma mark - Actions

- (void) _handleKnob:(ShellacFiltersParaEQKnob *)knob
{
    NSInteger index = [knob tag];
    if (index < 0 || index >= sKnobCount) return;

    AudioUnitParameterID parameter = sKnobs[index].parameter;

    [self _setParameter:parameter to:(float)[knob doubleValue]];

    [[_valueFields objectAtIndex:index] setStringValue:[self _stringForParameter:parameter]];
    [_curveView setNeedsDisplay:YES];
}


// The segment index is the parameter's value, not an index into a list of them:
// all three switched controls start at 0 and step by 1, which is what lets the
// slope read 0/1/2 and the two shapes and Bypass read 0/1.
//
- (void) _handleSwitch:(NSSegmentedControl *)control
{
    AudioUnitParameterID parameter = (AudioUnitParameterID)[control tag];

    [self _setParameter:parameter to:(float)[control selectedSegment]];

    [_curveView setNeedsDisplay:YES];
}


#pragma mark - Public Methods

- (void) reloadData
{
    for (int i = 0; i < sKnobCount; i++) {
        AudioUnitParameterID parameter = sKnobs[i].parameter;

        [[_knobs objectAtIndex:i] setDoubleValue:sGetParameter(_audioUnit, parameter)];
        [[_valueFields objectAtIndex:i] setStringValue:[self _stringForParameter:parameter]];
    }

    for (NSSegmentedControl *control in _switches) {
        AudioUnitParameterID parameter = (AudioUnitParameterID)[control tag];
        NSInteger            segment   = (NSInteger)lround(sGetParameter(_audioUnit, parameter));

        if (segment >= 0 && segment < [control segmentCount]) {
            [control setSelectedSegment:segment];
        }
    }

    [_curveView setNeedsDisplay:YES];
    [self setNeedsDisplay:YES];
}


- (void) flatten
{
    static const AudioUnitParameterID gains[] = {
        kParam_C, kParam_F, kParam_I, kParam_L, kParam_O
    };

    for (int i = 0; i < 5; i++) {
        [self _setParameter:gains[i] to:0];
    }

    [self reloadData];
}


- (void) restoreDefaults
{
    for (int i = 0; i < kNumberOfParameters; i++) {
        AudioUnitParameterInfo info;

        if (sGetParameterInfo(_audioUnit, (AudioUnitParameterID)i, &info)) {
            [self _setParameter:(AudioUnitParameterID)i to:info.defaultValue];
        }
    }

    [self reloadData];
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
    return [[ShellacFiltersParaEQView alloc] initWithAudioUnit:inAudioUnit];
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
