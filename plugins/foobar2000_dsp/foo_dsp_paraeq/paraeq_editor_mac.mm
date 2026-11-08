/* ========================================
 *  foo_dsp_paraeq - the curve editor, macOS
 *
 *  See paraeq_editor_mac.h. The comments in paraeq_editor.cpp say why each
 *  thing is the way it is; this file repeats a reason only where the Mac
 *  version differs.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_editor_mac.h"

#include <math.h>
#include <vector>

using namespace paraeq;

namespace {

// ---------------------------------------------------------------------------
// The bands - the same table as paraeq_editor.cpp
// ---------------------------------------------------------------------------

enum { kHP = 0, kLF, kLMF, kHMF, kHF, kOut, kBandCount };
enum { kRowFreq = 0, kRowGain, kRowShape, kRowCount };

struct BandSpec {
    NSString * name;
    NSString * abbr;
    float fMin, fMax;       //!< both zero when the band has no frequency
    bool  hasGain;
    bool  hasQ;
    bool  hasShelf;         //!< shelf that can be switched to a bell
};

const BandSpec kSpec[kBandCount] = {
    { @"Low cut",    @"HP",  kHpFreqMin,  kHpFreqMax,  false, false, false },
    { @"Bass",       @"LF",  kLfFreqMin,  kLfFreqMax,  true,  false, true  },
    { @"Reverb cut", @"LMF", kLmfFreqMin, kLmfFreqMax, true,  true,  false },
    { @"Brilliance", @"HMF", kHmfFreqMin, kHmfFreqMax, true,  true,  false },
    { @"Hiss cut",   @"HF",  kHfFreqMin,  kHfFreqMax,  true,  false, true  },
    { @"Output",     @"Out", 0.0f,        0.0f,        true,  false, false },
};

NSString * const kRowName[kRowCount] = { @"Freq", @"Gain", @"Shape" };

double bandFreq(const Params & p, int b) {
    switch (b) {
    case kHP:  return p.hpFrequency;
    case kLF:  return p.lfFrequency;
    case kLMF: return p.lmfFrequency;
    case kHMF: return p.hmfFrequency;
    case kHF:  return p.hfFrequency;
    default:   return 0.0;
    }
}

void setBandFreq(Params & p, int b, double hz) {
    switch (b) {
    case kHP:  p.hpFrequency  = (float)hz; break;
    case kLF:  p.lfFrequency  = (float)hz; break;
    case kLMF: p.lmfFrequency = (float)hz; break;
    case kHMF: p.hmfFrequency = (float)hz; break;
    case kHF:  p.hfFrequency  = (float)hz; break;
    default: break;
    }
}

double bandGain(const Params & p, int b) {
    switch (b) {
    case kLF:  return p.lfGain;
    case kLMF: return p.lmfGain;
    case kHMF: return p.hmfGain;
    case kHF:  return p.hfGain;
    case kOut: return p.outputGain;
    default:   return 0.0;
    }
}

void setBandGain(Params & p, int b, double db) {
    switch (b) {
    case kLF:  p.lfGain     = (float)db; break;
    case kLMF: p.lmfGain    = (float)db; break;
    case kHMF: p.hmfGain    = (float)db; break;
    case kHF:  p.hfGain     = (float)db; break;
    case kOut: p.outputGain = (float)db; break;
    default: break;
    }
}

double bandQ(const Params & p, int b) {
    if (b == kLMF) return p.lmfQ;
    if (b == kHMF) return p.hmfQ;
    return 0.0;
}

void setBandQ(Params & p, int b, double q) {
    if (b == kLMF) p.lmfQ = (float)q;
    if (b == kHMF) p.hmfQ = (float)q;
}

Biquad bandSection(const Config & cfg, int b) {
    switch (b) {
    case kHP:  return cfg.stage[kStageHighPass1];
    case kLF:  return cfg.stage[kStageLowShelf];
    case kLMF: return cfg.stage[kStageLowMid];
    case kHMF: return cfg.stage[kStageHighMid];
    case kHF:  return cfg.stage[kStageHighShelf];
    default:   return Biquad();
    }
}

const double kQPreset[] = { 0.5, 0.71, 1.0, 1.41, 2.0, 3.0, 4.5, 8.0 };
const int    kQPresetCount = (int)(sizeof(kQPreset) / sizeof(kQPreset[0]));

double octavesForQ(double q) {
    if (!(q > 0.0)) return 0.0;
    const double v = 1.0 / (2.0 * q);
    return 2.0 * log(v + sqrt(v * v + 1.0)) / log(2.0);
}

// ---------------------------------------------------------------------------
// Plot geometry
// ---------------------------------------------------------------------------

const double kAxisLoHz = 14.0;
const double kAxisHiHz = 22000.0;
const double kAxisDb   = 24.0;

const double kGridHz[] = { 20, 30, 50, 100, 200, 300, 500, 1000,
                           2000, 3000, 5000, 10000, 20000 };
const int    kGridHzCount = (int)(sizeof(kGridHz) / sizeof(kGridHz[0]));

const double kLabelHz[] = { 100, 1000, 10000 };
NSString * const kLabelHzText[] = { @"100", @"1k", @"10k" };
const int    kLabelHzCount = 3;

const double kGridDb[] = { -18, -12, -6, 0, 6, 12, 18 };
const int    kGridDbCount = (int)(sizeof(kGridDb) / sizeof(kGridDb[0]));

const NSTimeInterval kPushInterval = 0.025;

enum { kBtnEngage = 0, kBtnBypass, kBtnFlatten, kBtnReset, kBtnCount };

NSString * const kBtnText[kBtnCount] = {
    @"Add to DSP chain", @"Bypass", @"Flatten", @"Reset"
};

// Context menu commands, as NSMenuItem tags.
enum { kIdOff = 100, kId12, kId24, kIdShelf, kIdBell,
       kIdQNarrow, kIdQWide,
       kIdResetBand = 200, kIdFlatten, kIdResetAll, kIdBypass,
       kIdQFirst = 300 };

// Sizes in points, which on a Mac are what the Windows editor's 96-dpi pixels
// are: the unit the eye judges a control by.
const CGFloat kPad          = 4.0;
const CGFloat kHandleR      = 4.0;
const CGFloat kHandleRHot   = 6.0;
const CGFloat kHandleReach  = 9.0;
const CGFloat kTrimReach    = 4.0;
const CGFloat kCurveMin     = 60.0;

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

NSString * formatHz(double hz) {
    if (hz >= 10000.0) return [NSString stringWithFormat:@"%.1fk", hz / 1000.0];
    if (hz >= 1000.0)  return [NSString stringWithFormat:@"%.2fk", hz / 1000.0];
    return [NSString stringWithFormat:@"%.0f", hz];
}

//! The colours, resolved for the appearance being drawn in. Everything but
//! the background, the text and the accent is blended from them, as on Windows,
//! which is what keeps the editor right in dark mode without a branch for it.
struct Palette {
    NSColor * back;
    NSColor * text;
    NSColor * grid;
    NSColor * zero;
    NSColor * dim;
    NSColor * fill;
    NSColor * sel;
};

NSColor * resolved(NSColor * c) {
    NSColor * r = [c colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return r != nil ? r : c;
}

NSColor * blend(NSColor * a, NSColor * b, CGFloat t) {
    NSColor * r = [a blendedColorWithFraction:t ofColor:b];
    return r != nil ? r : a;
}

bool emptyRect(NSRect r) {
    return r.size.width <= 0.0 || r.size.height <= 0.0;
}

} // anonymous namespace


@implementation FooDspParaEQEditorView {
    __weak id<FooDspParaEQEditorHost> _host;

    Params _params;
    Params _dragStart;
    double _sampleRate;

    NSFont * _font;
    CGFloat  _rowH;

    // Layout - see paraeq_editor.h for what each piece is.
    NSRect  _curve;
    NSRect  _strip;
    NSRect  _foot;
    CGFloat _cellX[kBandCount + 1];
    bool    _hasStrip;
    NSRect  _button[kBtnCount];
    CGFloat _nameW[kBandCount];
    CGFloat _abbrW[kBandCount];

    // The frequency axis, its trig table and the curve read off them. More
    // points than there are points across, on a Retina screen, so the curve is
    // as smooth as the pixels allow.
    std::vector<double> _hz;
    std::vector<double> _trig;
    std::vector<float>  _db;
    std::vector<float>  _dbBand;
    std::vector<float>  _dbBand2;
    bool _curveStale;

    int     _selected;
    int     _hover;
    int     _dragBand;
    int     _dragRow;
    int     _footHot;
    NSPoint _dragFrom;
    bool    _dragging;
    bool    _engaged;
    CGFloat _wheelRest;
    int     _menuBand;

    bool           _pushDirty;
    bool           _pushScheduled;
    NSTimeInterval _lastPush;

    NSTrackingArea * _tracking;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

- (instancetype)initWithFrame:(NSRect)frame
                       params:(const Params &)params
                         host:(id<FooDspParaEQEditorHost>)host
{
    if ((self = [super initWithFrame:frame])) {
        _host = host;
        _params = params;
        _params.sanitize();
        _dragStart = _params;
        _sampleRate = 44100.0;

        _selected = -1;
        _hover    = -1;
        _dragBand = -1;
        _dragRow  = -1;
        _footHot  = -1;
        _menuBand = -1;
        _curveStale = true;
        _lastPush = 0.0;

        // Monospaced digits so the readouts do not shuffle sideways as they
        // change, at the size AppKit uses for secondary text.
        _font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                                 weight:NSFontWeightRegular];
        _rowH = ceil(_font.ascender - _font.descender + _font.leading) + 2.0;

        _engaged = [self askEngaged];

        [self relayout];
        [self rebuildTable];
    }
    return self;
}

- (void)dealloc {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

- (BOOL)isFlipped { return YES; }    // y down, as the Windows geometry is written
- (BOOL)isOpaque  { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (BOOL)becomeFirstResponder {
    [self setNeedsDisplay:YES];
    return YES;
}

- (BOOL)resignFirstResponder {
    [self setNeedsDisplay:YES];
    return YES;
}

// No focus ring. Round a whole panel it is a heavy frame for little news: the
// selected band's handle and its readout column are already marked only while
// the keys would reach them - see showSelection.
- (NSFocusRingType)focusRingType { return NSFocusRingTypeNone; }

- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityLabel { return @"Parametric EQ curve"; }

- (BOOL)askEngaged {
    id<FooDspParaEQEditorHost> host = _host;
    if (host != nil && [host respondsToSelector:@selector(editorEngaged)]) {
        return [host editorEngaged];
    }
    return YES;
}

- (BOOL)hasFocus {
    return self.window != nil && self.window.firstResponder == self;
}

- (BOOL)showSelection {
    return _selected >= 0 && [self hasFocus];
}

// ---------------------------------------------------------------------------
// Told from outside
// ---------------------------------------------------------------------------

- (Params)currentParams { return _params; }

- (void)takeParams:(const Params &)p {
    if (_dragging) return;
    Params next = p;
    next.sanitize();
    if (next == _params) return;
    _params = next;
    _curveStale = true;
    [self setNeedsDisplay:YES];
}

- (void)refreshEngaged {
    const bool now = [self askEngaged];
    if (now == _engaged) return;
    _engaged = now;
    [self relayout];
    [self setNeedsDisplay:YES];
}

- (void)setSampleRate:(double)rate {
    if (!(rate > 8000.0) || !(rate < 1e7)) rate = 44100.0;
    if (rate == _sampleRate) return;
    _sampleRate = rate;
    [self rebuildTable];
    [self setNeedsDisplay:YES];
}

// ---------------------------------------------------------------------------
// Size and screen changes
// ---------------------------------------------------------------------------

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self relayout];
    [self rebuildTable];
    [self setNeedsDisplay:YES];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self rebuildTable];
    [self setNeedsDisplay:YES];
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self setNeedsDisplay:YES];
}

- (void)viewWillMoveToWindow:(NSWindow *)window {
    // Leaving the window is the panel closing: the last move made in it goes
    // out now rather than being lost with the timer.
    if (window == nil) [self flushPush];
    [super viewWillMoveToWindow:window];
}

- (void)updateTrackingAreas {
    if (_tracking != nil) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingCursorUpdate | NSTrackingActiveInActiveApp |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

- (CGFloat)textWidth:(NSString *)s {
    return ceil([s sizeWithAttributes:@{ NSFontAttributeName : _font }].width);
}

- (void)relayout {
    const NSRect client = self.bounds;

    _strip = NSZeroRect;
    _foot  = NSZeroRect;
    _hasStrip = false;
    for (int b = 0; b <= kBandCount; b++) _cellX[b] = 0.0;
    for (int b = 0; b < kBtnCount; b++) _button[b] = NSZeroRect;

    for (int b = 0; b < kBandCount; b++) {
        _nameW[b] = [self textWidth:kSpec[b].name];
        _abbrW[b] = [self textWidth:kSpec[b].abbr];
    }

    NSRect rest = NSInsetRect(client, kPad, kPad);
    if (emptyRect(rest)) {
        _curve = client;
        return;
    }

    const CGFloat footH  = _rowH + 6.0;
    const CGFloat stripH = _rowH * (kRowCount + 1);   // a header row on top
    CGFloat height = rest.size.height;

    // Shed the chrome from the bottom up as the panel gets shorter.
    if (height >= kCurveMin + stripH + footH + kPad * 2) {
        _foot = NSMakeRect(rest.origin.x, NSMaxY(rest) - footH, rest.size.width, footH);
        rest.size.height = NSMinY(_foot) - kPad - rest.origin.y;
        height = rest.size.height;
    }
    if (height >= kCurveMin + stripH + kPad) {
        _strip = NSMakeRect(rest.origin.x, NSMaxY(rest) - stripH, rest.size.width, stripH);

        const CGFloat cellMin = [self textWidth:@"Q 0.00"] + 8.0;
        const CGFloat nameMin = [self textWidth:@"Shape"] + 6.0;

        const CGFloat stripW = _strip.size.width;
        const CGFloat labelW = clampd(floor(stripW / 9.0), nameMin, nameMin * 2.0);
        const CGFloat avail  = stripW - labelW;

        if (avail < cellMin * kBandCount) {
            _strip = NSZeroRect;
        } else {
            // Columns as wide as their titles want, cheapest first and all or
            // nothing - see layout() in paraeq_editor.cpp.
            CGFloat width[kBandCount], want[kBandCount];
            const CGFloat extra = avail - cellMin * kBandCount;

            for (int b = 0; b < kBandCount; b++) {
                width[b] = cellMin;
                want[b]  = _nameW[b] + 8.0 - cellMin;
                if (want[b] < 0.0) want[b] = 0.0;
            }

            int order[kBandCount];
            for (int b = 0; b < kBandCount; b++) order[b] = b;
            for (int i = 1; i < kBandCount; i++) {
                const int key = order[i];
                int j = i - 1;
                while (j >= 0 && want[order[j]] > want[key]) { order[j + 1] = order[j]; j--; }
                order[j + 1] = key;
            }

            CGFloat given = 0.0;
            for (int i = 0; i < kBandCount; i++) {
                const int b = order[i];
                if (want[b] > extra - given) break;
                width[b] += want[b];
                given    += want[b];
            }

            const CGFloat spare = floor((extra - given) / kBandCount);
            for (int b = 0; b < kBandCount; b++) width[b] += spare;

            _cellX[0] = _strip.origin.x + labelW;
            for (int b = 0; b < kBandCount; b++) _cellX[b + 1] = _cellX[b] + width[b];
            _cellX[kBandCount] = NSMaxX(_strip);

            _hasStrip = true;
            rest.size.height = NSMinY(_strip) - kPad - rest.origin.y;
        }
    }

    _curve = rest;

    if (!emptyRect(_foot)) {
        const CGFloat padX = 8.0;
        const CGFloat gap  = 4.0;
        CGFloat x = _foot.origin.x;

        for (int b = 0; b < kBtnCount; b++) {
            if (b == kBtnEngage && _engaged) continue;
            const CGFloat w = [self textWidth:kBtnText[b]] + padX * 2.0;
            if (x + w > NSMaxX(_foot)) break;
            _button[b] = NSMakeRect(x, _foot.origin.y, w, _foot.size.height);
            x += w + gap;
        }
    }
}

- (void)rebuildTable {
    const CGFloat w = _curve.size.width;
    if (w <= 1.0) {
        _hz.clear(); _trig.clear(); _db.clear(); _dbBand.clear();
        return;
    }

    const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    size_t n = (size_t)ceil(w * (scale > 1.0 ? scale : 1.0));
    if (n < 2) n = 2;

    _hz.resize(n);
    _trig.resize(n * (size_t)kCurveTrigStride);
    _db.resize(n);
    _dbBand.resize(n);

    const double ratio = kAxisHiHz / kAxisLoHz;
    for (size_t i = 0; i < n; i++) {
        _hz[i] = kAxisLoHz * pow(ratio, (double)i / (double)(n - 1));
    }
    curveTrig(_hz.data(), n, _sampleRate, _trig.data());
    _curveStale = true;
}

- (void)ensureCurve {
    if (!_curveStale || _hz.empty()) return;
    _curveStale = false;

    Config cfg;
    cfg.compute(_params, _sampleRate);

    const size_t n = _hz.size();
    curveDb(cfg, _trig.data(), n, _db.data());

    const int band = (_dragBand >= 0) ? _dragBand : _hover;
    if (band >= 0 && band != kOut) {
        curveDb(bandSection(cfg, band), _trig.data(), n, _dbBand.data());
        if (band == kHP && _params.hpSlope == 2) {
            _dbBand2.resize(n);
            curveDb(cfg.stage[kStageHighPass2], _trig.data(), n, _dbBand2.data());
            for (size_t i = 0; i < n; i++) _dbBand[i] += _dbBand2[i];
        }
    }
}

- (void)markCurveStale { _curveStale = true; }

// ---------------------------------------------------------------------------
// Axes
// ---------------------------------------------------------------------------

- (CGFloat)xOfHz:(double)hz {
    const CGFloat w = _curve.size.width;
    if (w <= 1.0) return _curve.origin.x;
    const double t = log(clampd(hz, 1.0, 1e6) / kAxisLoHz) / log(kAxisHiHz / kAxisLoHz);
    return _curve.origin.x + clampd(t, -0.2, 1.2) * w;
}

- (CGFloat)yOfDb:(double)db {
    const CGFloat h = _curve.size.height;
    if (h <= 1.0) return _curve.origin.y;
    const double t = (kAxisDb - db) / (2.0 * kAxisDb);
    return _curve.origin.y + clampd(t, -0.5, 1.5) * h;
}

- (CGFloat)handleX:(int)band {
    if (band == kOut) return NSMaxX(_curve) - kHandleRHot - 1.0;
    return clampd([self xOfHz:bandFreq(_params, band)],
                  NSMinX(_curve) + kHandleRHot, NSMaxX(_curve) - kHandleRHot - 1.0);
}

- (CGFloat)handleY:(int)band {
    if (band == kOut) return [self yOfDb:_params.outputGain];

    if (band == kHP) {
        // Rides the curve at its corner - see paraeq_editor.cpp.
        [self ensureCurve];
        const CGFloat w = _curve.size.width;
        if (!_db.empty() && w > 1.0) {
            const double t = ([self handleX:kHP] - NSMinX(_curve)) / w;
            const size_t i = (size_t)clampd(floor(t * (double)(_db.size() - 1) + 0.5),
                                            0.0, (double)(_db.size() - 1));
            return clampd([self yOfDb:_db[i]], NSMinY(_curve), NSMaxY(_curve) - 1.0);
        }
        return [self yOfDb:0.0];
    }

    return clampd([self yOfDb:bandGain(_params, band)],
                  NSMinY(_curve) + kHandleRHot, NSMaxY(_curve) - kHandleRHot - 1.0);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

- (Palette)palette {
    Palette p;
    p.back = resolved(NSColor.windowBackgroundColor);
    p.text = resolved(NSColor.textColor);
    p.sel  = resolved(NSColor.controlAccentColor);
    p.grid = blend(p.back, p.text, 0.18);
    p.zero = blend(p.back, p.text, 0.34);
    p.dim  = blend(p.back, p.text, 0.50);
    p.fill = blend(p.back, p.text, 0.10);
    return p;
}

- (void)drawText:(NSString *)s in:(NSRect)r align:(NSTextAlignment)align color:(NSColor *)c {
    NSMutableParagraphStyle * ps = [[NSMutableParagraphStyle alloc] init];
    ps.alignment = align;
    ps.lineBreakMode = NSLineBreakByClipping;
    NSDictionary * attrs = @{ NSFontAttributeName : _font,
                              NSForegroundColorAttributeName : c,
                              NSParagraphStyleAttributeName : ps };
    const CGFloat h = ceil(_font.ascender - _font.descender);
    const NSRect line = NSMakeRect(r.origin.x, floor(NSMidY(r) - h / 2.0), r.size.width, h);
    [s drawInRect:line withAttributes:attrs];
}

static void strokeLine(CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, NSColor * c) {
    // Half a point in, so a one-point line covers whole pixels on a 1x screen.
    if (x0 == x1) { x0 = x1 = floor(x0) + 0.5; }
    if (y0 == y1) { y0 = y1 = floor(y0) + 0.5; }
    NSBezierPath * path = [NSBezierPath bezierPath];
    path.lineWidth = 1.0;
    [path moveToPoint:NSMakePoint(x0, y0)];
    [path lineToPoint:NSMakePoint(x1, y1)];
    [c setStroke];
    [path stroke];
}

static void frameRect(NSRect r, NSColor * c) {
    [c setStroke];
    NSBezierPath * path = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)];
    path.lineWidth = 1.0;
    [path stroke];
}

- (void)drawRect:(NSRect)dirty {
    const Palette pal = [self palette];

    [pal.back setFill];
    NSRectFill(self.bounds);

    [self ensureCurve];
    [self drawCurve:pal];
    [self drawStrip:pal];
    [self drawFoot:pal];
}

- (NSBezierPath *)pathFor:(const std::vector<float> &)db {
    NSBezierPath * path = [NSBezierPath bezierPath];
    const size_t n = _hz.size();
    if (n < 2 || db.size() < n) return path;

    const CGFloat left = NSMinX(_curve);
    const CGFloat w    = _curve.size.width;
    const CGFloat lo   = NSMinY(_curve) - 4.0;
    const CGFloat hi   = NSMaxY(_curve) + 4.0;

    for (size_t i = 0; i < n; i++) {
        const NSPoint pt = NSMakePoint(left + w * (CGFloat)i / (CGFloat)(n - 1),
                                       clampd([self yOfDb:db[i]], lo, hi));
        if (i == 0) [path moveToPoint:pt];
        else        [path lineToPoint:pt];
    }
    path.lineJoinStyle = NSLineJoinStyleRound;
    return path;
}

- (void)drawCurve:(const Palette &)pal {
    const NSRect rc = _curve;
    if (emptyRect(rc)) return;

    const bool live = _engaged && !_params.bypass;

    for (int i = 0; i < kGridHzCount; i++) {
        const CGFloat x = [self xOfHz:kGridHz[i]];
        if (x < NSMinX(rc) || x >= NSMaxX(rc)) continue;
        strokeLine(x, NSMinY(rc), x, NSMaxY(rc), pal.grid);
    }
    for (int i = 0; i < kGridDbCount; i++) {
        const CGFloat y = [self yOfDb:kGridDb[i]];
        if (y < NSMinY(rc) || y >= NSMaxY(rc)) continue;
        strokeLine(NSMinX(rc), y, NSMaxX(rc), y, kGridDb[i] == 0.0 ? pal.zero : pal.grid);
    }

    const CGFloat labelPad = 2.0;
    for (int i = 0; i < kLabelHzCount; i++) {
        const CGFloat x = [self xOfHz:kLabelHz[i]];
        if (x < NSMinX(rc) || x >= NSMaxX(rc) - 18.0) continue;
        [self drawText:kLabelHzText[i]
                    in:NSMakeRect(x + labelPad, NSMaxY(rc) - _rowH, 40.0, _rowH)
                 align:NSTextAlignmentLeft color:pal.dim];
    }
    {
        const int step = (int)kAxisDb / 2;
        for (int s = -1; s <= 1; s += 2) {
            const CGFloat y = [self yOfDb:s * (double)step];
            [self drawText:[NSString stringWithFormat:@"%+d", s * step]
                        in:NSMakeRect(NSMinX(rc) + labelPad, y - _rowH / 2.0, 30.0, _rowH)
                     align:NSTextAlignmentLeft color:pal.dim];
        }
    }

    [NSGraphicsContext saveGraphicsState];
    NSRectClip(rc);

    const int band = (_dragBand >= 0) ? _dragBand : _hover;
    if (band >= 0 && band != kOut && !_dbBand.empty()) {
        NSBezierPath * path = [self pathFor:_dbBand];
        path.lineWidth = 1.0;
        [blend(pal.back, pal.sel, 0.55) setStroke];
        [path stroke];
    }

    if (_params.outputGain != 0.0f) {
        const CGFloat y = floor([self yOfDb:_params.outputGain]) + 0.5;
        NSBezierPath * path = [NSBezierPath bezierPath];
        const CGFloat dash[2] = { 2.0, 2.0 };
        [path setLineDash:dash count:2 phase:0.0];
        path.lineWidth = 1.0;
        [path moveToPoint:NSMakePoint(NSMinX(rc), y)];
        [path lineToPoint:NSMakePoint(NSMaxX(rc), y)];
        [pal.dim setStroke];
        [path stroke];
    }

    if (!_db.empty()) {
        NSBezierPath * path = [self pathFor:_db];
        path.lineWidth = 2.0;
        [(live ? pal.text : pal.dim) setStroke];
        [path stroke];
    }

    const bool marked = [self showSelection];
    for (int b = 0; b < kBandCount; b++) {
        if (b == kOut) continue;
        const CGFloat x = [self handleX:b];
        const CGFloat y = [self handleY:b];
        const bool sel = (b == _selected) && marked;
        const bool hot = (b == _hover) || (b == _dragBand);
        const CGFloat rad = (sel || hot) ? kHandleRHot : kHandleR;

        NSColor * edge = sel ? pal.sel : (live ? pal.text : pal.dim);
        NSBezierPath * dot = [NSBezierPath bezierPathWithOvalInRect:
                              NSMakeRect(x - rad, y - rad, rad * 2.0, rad * 2.0)];
        [((hot || sel) ? edge : pal.back) setFill];
        [dot fill];
        dot.lineWidth = sel ? 2.0 : 1.0;
        [edge setStroke];
        [dot stroke];
    }

    // The names beside the handles, placed so they do not land on each other.
    {
        NSRect claimed[kBandCount];
        int    claimedCount = 0;

        int order[kBandCount];
        int n = 0;
        if (_selected >= 0 && _selected != kOut) order[n++] = _selected;
        if (_hover >= 0 && _hover != kOut && _hover != _selected) order[n++] = _hover;
        for (int b = 0; b < kBandCount; b++) {
            if (b == kOut || b == _selected || b == _hover) continue;
            order[n++] = b;
        }

        for (int i = 0; i < n; i++) {
            const int b = order[i];
            const bool sel = (b == _selected) && marked;
            const CGFloat rad = (sel || b == _hover || b == _dragBand) ? kHandleRHot : kHandleR;
            const CGFloat x = [self handleX:b];
            const CGFloat y = [self handleY:b];

            NSString * text = nil;
            NSRect lr = NSZeroRect;
            bool onLeft = false;

            NSString * anyText = nil;
            NSRect anyRect = NSZeroRect;
            bool anyLeft = false;

            for (int attempt = 0; attempt < 4; attempt++) {
                const bool abbr  = (attempt >= 2);
                const bool below = (attempt % 2) != 0;

                NSString * candidate = abbr ? kSpec[b].abbr : kSpec[b].name;
                const CGFloat w   = abbr ? _abbrW[b] : _nameW[b];
                const CGFloat top = below ? y : y - _rowH;
                if (top < NSMinY(rc) || top + _rowH > NSMaxY(rc)) continue;

                const bool left = (x + rad + labelPad + w > NSMaxX(rc));
                const NSRect cand = left
                    ? NSMakeRect(x - rad - labelPad - w, top, w, _rowH)
                    : NSMakeRect(x + rad + labelPad,     top, w, _rowH);

                if (anyText == nil) { anyText = candidate; anyRect = cand; anyLeft = left; }

                bool clear = true;
                for (int c = 0; c < claimedCount && clear; c++) {
                    if (NSIntersectsRect(cand, claimed[c])) clear = false;
                }
                if (clear) { text = candidate; lr = cand; onLeft = left; break; }
            }

            if (text == nil) {
                if (i > 0 || anyText == nil) continue;
                text = anyText; lr = anyRect; onLeft = anyLeft;
            }

            claimed[claimedCount++] = lr;
            [self drawText:text in:lr
                     align:(onLeft ? NSTextAlignmentRight : NSTextAlignmentLeft)
                     color:(sel ? pal.sel : pal.dim)];
        }
    }

    [NSGraphicsContext restoreGraphicsState];

    frameRect(rc, pal.grid);
}

- (void)drawStrip:(const Palette &)pal {
    if (!_hasStrip || emptyRect(_strip)) return;

    const NSRect rc = _strip;
    const CGFloat pad = 3.0;
    const bool marked = [self showSelection];

    if (marked) {
        [pal.fill setFill];
        NSRectFill(NSMakeRect(_cellX[_selected], NSMinY(rc),
                              _cellX[_selected + 1] - _cellX[_selected], rc.size.height));
    }

    for (int b = 0; b < kBandCount; b++) {
        const NSRect r = NSInsetRect(NSMakeRect(_cellX[b], NSMinY(rc),
                                                _cellX[b + 1] - _cellX[b], _rowH), pad, 0.0);
        const bool full = (_nameW[b] <= r.size.width);
        [self drawText:(full ? kSpec[b].name : kSpec[b].abbr) in:r
                 align:NSTextAlignmentRight
                 color:((b == _selected && marked) ? pal.text : pal.dim)];
    }

    for (int row = 0; row < kRowCount; row++) {
        const CGFloat top = NSMinY(rc) + _rowH * (row + 1);
        [self drawText:kRowName[row]
                    in:NSInsetRect(NSMakeRect(NSMinX(rc), top, _cellX[0] - NSMinX(rc), _rowH),
                                   pad, 0.0)
                 align:NSTextAlignmentLeft color:pal.dim];

        for (int b = 0; b < kBandCount; b++) {
            NSString * text = @"-";

            switch (row) {
            case kRowFreq:
                if (kSpec[b].fMax > 0.0f) text = formatHz(bandFreq(_params, b));
                break;
            case kRowGain:
                if (kSpec[b].hasGain) {
                    text = [NSString stringWithFormat:@"%+.1f", bandGain(_params, b)];
                }
                break;
            default:
                if (b == kHP) {
                    const int s = clampi(_params.hpSlope, 0, 2);
                    text = (s == 0) ? @"off" : (s == 1 ? @"12dB" : @"24dB");
                } else if (kSpec[b].hasShelf) {
                    const bool bell = (b == kLF) ? _params.lfBell : _params.hfBell;
                    text = bell ? @"bell" : @"shelf";
                } else if (kSpec[b].hasQ) {
                    text = [NSString stringWithFormat:@"Q %.2f", bandQ(_params, b)];
                }
                break;
            }

            const NSRect r = NSInsetRect(NSMakeRect(_cellX[b], top,
                                                    _cellX[b + 1] - _cellX[b], _rowH), pad, 0.0);
            const bool dash = [text isEqualToString:@"-"];
            [self drawText:text in:r align:NSTextAlignmentRight
                     color:(dash ? pal.grid : (_engaged ? pal.text : pal.dim))];
        }
    }
}

- (void)drawFoot:(const Palette &)pal {
    if (emptyRect(_foot)) return;

    CGFloat rightmost = NSMinX(_foot);

    for (int b = 0; b < kBtnCount; b++) {
        const NSRect r = _button[b];
        if (emptyRect(r)) continue;

        const bool on  = (b == kBtnBypass && _params.bypass);
        const bool hot = (b == _footHot);

        NSBezierPath * box = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5)
                                                             xRadius:4.0 yRadius:4.0];
        if (on)       { [blend(pal.back, pal.sel, 0.35) setFill]; [box fill]; }
        else if (hot) { [pal.fill setFill]; [box fill]; }
        box.lineWidth = 1.0;
        [(on ? pal.sel : pal.grid) setStroke];
        [box stroke];

        [self drawText:kBtnText[b] in:r align:NSTextAlignmentCenter color:pal.text];
        if (NSMaxX(r) > rightmost) rightmost = NSMaxX(r);
    }

    if (!_engaged) {
        const NSRect r = NSMakeRect(rightmost + 8.0, NSMinY(_foot),
                                    NSMaxX(_foot) - rightmost - 8.0, _foot.size.height);
        if (r.size.width > 0.0) {
            [self drawText:@"not in the DSP chain" in:r align:NSTextAlignmentLeft color:pal.dim];
        }
    }
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

- (int)hitHandle:(NSPoint)pt {
    if (!NSPointInRect(pt, _curve)) return -1;

    int best = -1;
    CGFloat bestDist = kHandleReach * kHandleReach + 1.0;
    for (int b = 0; b < kBandCount; b++) {
        if (b == kOut) continue;
        const CGFloat dx = pt.x - [self handleX:b];
        const CGFloat dy = pt.y - [self handleY:b];
        const CGFloat d  = dx * dx + dy * dy;
        if (d < bestDist) { bestDist = d; best = b; }
    }
    if (best >= 0) return best;

    if (fabs(pt.y - [self yOfDb:_params.outputGain]) <= kTrimReach) return kOut;
    return -1;
}

- (int)hitStripCell:(NSPoint)pt row:(int *)row {
    if (!_hasStrip || !NSPointInRect(pt, _strip) || _rowH <= 0.0) return -1;

    int b = -1;
    for (int i = 0; i < kBandCount; i++) {
        if (pt.x >= _cellX[i] && pt.x < _cellX[i + 1]) { b = i; break; }
    }
    if (b < 0) return -1;

    const int r = (int)floor((pt.y - NSMinY(_strip)) / _rowH) - 1;   // header
    if (row != NULL) *row = clampi(r, -1, kRowCount - 1);
    return b;
}

- (int)hitFootButton:(NSPoint)pt {
    for (int b = 0; b < kBtnCount; b++) {
        if (!emptyRect(_button[b]) && NSPointInRect(pt, _button[b])) return b;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

- (void)select:(int)band {
    if (band == _selected) return;
    _selected = band;
    _curveStale = true;
    [self setNeedsDisplay:YES];
}

- (void)commit {
    _params.sanitize();
    _curveStale = true;
    [self setNeedsDisplay:YES];
    [self schedulePush];
}

- (void)schedulePush {
    _pushDirty = true;

    const NSTimeInterval since = NSProcessInfo.processInfo.systemUptime - _lastPush;
    if (since >= kPushInterval) {
        [self flushPush];
    } else if (!_pushScheduled) {
        _pushScheduled = true;
        // Common modes, so a push still goes out while a menu or a live resize
        // has the run loop.
        [self performSelector:@selector(pushTimerFired)
                   withObject:nil
                   afterDelay:kPushInterval - since
                      inModes:@[ NSRunLoopCommonModes ]];
    }
}

- (void)pushTimerFired {
    _pushScheduled = false;
    [self flushPush];
}

- (void)flushPush {
    if (_pushScheduled) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                                                 selector:@selector(pushTimerFired)
                                                   object:nil];
        _pushScheduled = false;
    }
    if (!_pushDirty) return;

    _pushDirty = false;
    _lastPush  = NSProcessInfo.processInfo.systemUptime;
    id<FooDspParaEQEditorHost> host = _host;
    [host editorParamsChanged:_params];
}

- (void)nudgeGain:(int)band by:(double)db {
    if (band < 0 || band >= kBandCount) return;
    if (band == kHP) {
        // Down is more cut - see nudgeGain() in paraeq_editor.cpp.
        _params.hpSlope = clampi(_params.hpSlope + (db > 0 ? -1 : 1), 0, 2);
        return;
    }
    if (!kSpec[band].hasGain) return;
    const double limit = (band == kOut) ? kOutputMaxDb : kGainMaxDb;
    setBandGain(_params, band, clampd(bandGain(_params, band) + db, -limit, limit));
}

- (void)nudgeFreq:(int)band by:(double)semitones {
    if (band < 0 || band >= kBandCount) return;
    if (!(kSpec[band].fMax > 0.0f)) return;
    const double hz = bandFreq(_params, band) * pow(2.0, semitones / 12.0);
    setBandFreq(_params, band, clampd(hz, kSpec[band].fMin, kSpec[band].fMax));
}

- (void)toggleShape:(int)band {
    if (band == kHP)      _params.hpSlope = (_params.hpSlope + 1) % 3;
    else if (band == kLF) _params.lfBell = !_params.lfBell;
    else if (band == kHF) _params.hfBell = !_params.hfBell;
}

- (void)nudgeQ:(int)band by:(int)steps {
    if (band < 0 || band >= kBandCount) return;
    if (band == kHP) {
        _params.hpSlope = clampi(_params.hpSlope - steps, 0, 2);
        return;
    }
    if (!kSpec[band].hasQ) {
        if (kSpec[band].hasShelf && steps != 0) [self toggleShape:band];
        return;
    }
    const double q = bandQ(_params, band) * pow(2.0, steps / 6.0);
    setBandQ(_params, band, clampd(q, kQMin, kQMax));
}

- (void)resetBand:(int)band {
    const Params d = Params::defaults();
    switch (band) {
    case kHP:  _params.hpFrequency = d.hpFrequency; _params.hpSlope = d.hpSlope; break;
    case kLF:  _params.lfGain = d.lfGain; _params.lfFrequency = d.lfFrequency;
               _params.lfBell = d.lfBell; break;
    case kLMF: _params.lmfGain = d.lmfGain; _params.lmfFrequency = d.lmfFrequency;
               _params.lmfQ = d.lmfQ; break;
    case kHMF: _params.hmfGain = d.hmfGain; _params.hmfFrequency = d.hmfFrequency;
               _params.hmfQ = d.hmfQ; break;
    case kHF:  _params.hfGain = d.hfGain; _params.hfFrequency = d.hfFrequency;
               _params.hfBell = d.hfBell; break;
    case kOut: _params.outputGain = d.outputGain; break;
    default: break;
    }
}

- (void)flatten {
    _params.lfGain = _params.lmfGain = 0.0f;
    _params.hmfGain = _params.hfGain = 0.0f;
    _params.outputGain = 0.0f;
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

- (NSPoint)localPoint:(NSEvent *)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)endDrag {
    if (!_dragging) return;
    _dragging = false;
    _dragBand = -1;
    _dragRow  = -1;
    _curveStale = true;
    [self setNeedsDisplay:YES];
    [self flushPush];
}

- (void)mouseDown:(NSEvent *)event {
    // Ctrl-click is the one-button mouse's right-click.
    if (event.modifierFlags & NSEventModifierFlagControl) {
        NSMenu * menu = [self menuForEvent:event];
        if (menu != nil) [NSMenu popUpContextMenu:menu withEvent:event forView:self];
        return;
    }

    [self.window makeFirstResponder:self];

    const NSPoint pt = [self localPoint:event];
    const bool doubleClick = (event.clickCount == 2);

    const int btn = [self hitFootButton:pt];
    if (btn >= 0) {
        id<FooDspParaEQEditorHost> host = _host;
        switch (btn) {
        case kBtnEngage:
            [self flushPush];
            if ([host respondsToSelector:@selector(editorEngage)]) [host editorEngage];
            [self refreshEngaged];
            return;
        case kBtnBypass:  _params.bypass = !_params.bypass; break;
        case kBtnFlatten: [self flatten]; break;
        default:          _params = Params::defaults(); break;
        }
        [self commit];
        [self flushPush];
        return;
    }

    int row = -1;
    const int cell = [self hitStripCell:pt row:&row];
    if (cell >= 0) {
        [self select:cell];
        if (row < 0) return;

        if (doubleClick) { [self resetBand:cell]; [self commit]; [self flushPush]; return; }

        if (row == kRowShape && (cell == kHP || kSpec[cell].hasShelf)) {
            [self toggleShape:cell];
            [self commit];
            [self flushPush];
            return;
        }

        _dragBand  = cell;
        _dragRow   = row;
        _dragFrom  = pt;
        _dragStart = _params;
        _dragging  = true;
        return;
    }

    if (!NSPointInRect(pt, _curve)) return;

    const int band = [self hitHandle:pt];
    if (band < 0) {
        // Empty plot: select the nearest band by frequency, move nothing.
        int best = -1;
        CGFloat bestDx = 1e9;
        for (int b = 0; b < kBandCount; b++) {
            if (b == kOut) continue;
            const CGFloat dx = fabs(pt.x - [self handleX:b]);
            if (dx < bestDx) { bestDx = dx; best = b; }
        }
        [self select:best];
        return;
    }

    [self select:band];

    if (doubleClick) {
        if (band == kHP) _params.hpSlope = (_params.hpSlope == 0) ? 1 : 0;
        else             setBandGain(_params, band, 0.0);
        [self commit];
        [self flushPush];
        return;
    }

    _dragBand  = band;
    _dragRow   = -1;
    _dragFrom  = pt;
    _dragStart = _params;
    _dragging  = true;
    [[NSCursor closedHandCursor] set];
}

- (void)mouseDragged:(NSEvent *)event {
    if (!_dragging || _dragBand < 0) return;

    const NSPoint pt = [self localPoint:event];

    // Option scales the whole displacement, as Ctrl does on Windows, so a drag
    // that turns fine part way does not jump.
    const double fine = (event.modifierFlags & NSEventModifierFlagOption) ? 0.25 : 1.0;
    const double dx = (pt.x - _dragFrom.x) * fine;
    const double dy = (_dragFrom.y - pt.y) * fine;   // up is positive
    const int b = _dragBand;

    if (_dragRow < 0) {
        if (kSpec[b].fMax > 0.0f) {
            const CGFloat w = _curve.size.width;
            if (w > 1.0) {
                const double octaves = dx / w * (log(kAxisHiHz / kAxisLoHz) / log(2.0));
                const double hz = bandFreq(_dragStart, b) * pow(2.0, octaves);
                setBandFreq(_params, b, clampd(hz, kSpec[b].fMin, kSpec[b].fMax));
            }
        }
        if (kSpec[b].hasGain) {
            const CGFloat h = _curve.size.height;
            if (h > 1.0) {
                const double limit = (b == kOut) ? kOutputMaxDb : kGainMaxDb;
                const double db = bandGain(_dragStart, b) + dy * (2.0 * kAxisDb) / h;
                setBandGain(_params, b, clampd(db, -limit, limit));
            }
        }
    } else {
        // A readout cell: vertical only, at the Windows editor's rates.
        switch (_dragRow) {
        case kRowFreq:
            if (kSpec[b].fMax > 0.0f) {
                const double hz = bandFreq(_dragStart, b) * pow(2.0, dy / 48.0);
                setBandFreq(_params, b, clampd(hz, kSpec[b].fMin, kSpec[b].fMax));
            }
            break;
        case kRowGain:
            if (kSpec[b].hasGain) {
                const double limit = (b == kOut) ? kOutputMaxDb : kGainMaxDb;
                setBandGain(_params, b,
                            clampd(bandGain(_dragStart, b) + dy * 0.25, -limit, limit));
            } else if (b == kHP) {
                _params.hpSlope = clampi(_dragStart.hpSlope - (int)(dy / 24.0), 0, 2);
            }
            break;
        default:
            if (kSpec[b].hasQ) {
                setBandQ(_params, b,
                         clampd(bandQ(_dragStart, b) * pow(2.0, dy / 48.0), kQMin, kQMax));
            }
            break;
        }
    }

    [self commit];
}

- (void)mouseUp:(NSEvent *)event {
    [self endDrag];
    [self updateCursorAt:[self localPoint:event]];
}

- (void)mouseMoved:(NSEvent *)event {
    if (_dragging) return;
    const NSPoint pt = [self localPoint:event];
    const int hover = [self hitHandle:pt];
    const int foot  = [self hitFootButton:pt];
    if (hover != _hover || foot != _footHot) {
        _hover   = hover;
        _footHot = foot;
        _curveStale = true;
        [self setNeedsDisplay:YES];
    }
    [self updateCursorAt:pt];
}

- (void)mouseExited:(NSEvent *)event {
    if (_hover >= 0 || _footHot >= 0) {
        _hover = -1;
        _footHot = -1;
        _curveStale = true;
        [self setNeedsDisplay:YES];
    }
}

- (void)cursorUpdate:(NSEvent *)event {
    [self updateCursorAt:[self localPoint:event]];
}

- (void)updateCursorAt:(NSPoint)pt {
    if (_dragging) return;
    const int band = [self hitHandle:pt];
    int row = -1;
    if (band == kOut || (band < 0 && [self hitStripCell:pt row:&row] >= 0 && row >= 0)) {
        [[NSCursor resizeUpDownCursor] set];
    } else if (band >= 0) {
        [[NSCursor openHandCursor] set];
    } else {
        [[NSCursor arrowCursor] set];
    }
}

- (void)scrollWheel:(NSEvent *)event {
    // Momentum would keep moving a band after the fingers have let go.
    if (event.momentumPhase != NSEventPhaseNone) return;

    // Shift turns a mouse wheel sideways on a Mac, so the Q gesture arrives
    // as horizontal scrolling. Inverted direction is undone so that up, on the
    // wheel or the trackpad, is up on the curve whichever way the system
    // scrolls documents.
    CGFloat d = event.scrollingDeltaY;
    if (d == 0.0) d = event.scrollingDeltaX;
    if (event.isDirectionInvertedFromDevice) d = -d;
    if (d == 0.0) return;

    const NSPoint pt = [self localPoint:event];
    int band = [self hitHandle:pt];
    if (band < 0) {
        int row = -1;
        band = [self hitStripCell:pt row:&row];
    }
    if (band < 0) band = _selected;
    if (band < 0) return;

    [self select:band];

    // A trackpad reports points; a notch is taken as a dozen of them. A wheel
    // reports lines, about one a notch. What does not add up to a whole notch
    // is carried, as WHEEL_DELTA remainders are on Windows.
    _wheelRest += event.hasPreciseScrollingDeltas ? d / 12.0 : d;
    const int n = (int)_wheelRest;
    _wheelRest -= n;
    if (n == 0) return;

    if (event.modifierFlags & NSEventModifierFlagShift) {
        [self nudgeQ:band by:n];
    } else {
        const double step = (event.modifierFlags & NSEventModifierFlagOption) ? 0.1 : 0.5;
        [self nudgeGain:band by:n * step];
    }
    [self commit];
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

- (NSMenuItem *)addItem:(NSString *)title tag:(NSInteger)tag on:(BOOL)on to:(NSMenu *)menu {
    NSMenuItem * item = [[NSMenuItem alloc] initWithTitle:title
                                                   action:@selector(menuPicked:)
                                            keyEquivalent:@""];
    item.target = self;
    item.tag = tag;
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:item];
    return item;
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    const NSPoint pt = [self localPoint:event];

    int band = [self hitHandle:pt];
    if (band < 0) {
        int row = -1;
        band = [self hitStripCell:pt row:&row];
    }
    if (band < 0) band = _selected;
    if (band < 0) band = kLMF;
    [self select:band];
    _menuBand = band;

    NSMenu * menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;

    NSMenuItem * header = [self addItem:[NSString stringWithFormat:@"%@  (%@)",
                                         kSpec[band].name, kSpec[band].abbr]
                                    tag:0 on:NO to:menu];
    header.enabled = NO;
    [menu addItem:[NSMenuItem separatorItem]];

    if (band == kHP) {
        const int s = clampi(_params.hpSlope, 0, 2);
        [self addItem:@"Off"       tag:kIdOff on:(s == 0) to:menu];
        [self addItem:@"12 dB/oct" tag:kId12  on:(s == 1) to:menu];
        [self addItem:@"24 dB/oct" tag:kId24  on:(s == 2) to:menu];
        [menu addItem:[NSMenuItem separatorItem]];
    } else if (kSpec[band].hasShelf) {
        const bool bell = (band == kLF) ? _params.lfBell : _params.hfBell;
        [self addItem:@"Shelf" tag:kIdShelf on:!bell to:menu];
        [self addItem:@"Bell"  tag:kIdBell  on:bell  to:menu];
        [menu addItem:[NSMenuItem separatorItem]];
    } else if (kSpec[band].hasQ) {
        NSMenu * q = [[NSMenu alloc] initWithTitle:@"Bandwidth (Q)"];
        q.autoenablesItems = NO;
        const double now = bandQ(_params, band);

        NSMenuItem * value = [self addItem:[NSString stringWithFormat:@"Q %.2f  (%.1f oct)",
                                            now, octavesForQ(now)]
                                       tag:0 on:NO to:q];
        value.enabled = NO;
        [q addItem:[NSMenuItem separatorItem]];

        NSMenuItem * narrow = [self addItem:@"Narrower" tag:kIdQNarrow on:NO to:q];
        narrow.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey];
        narrow.keyEquivalentModifierMask = NSEventModifierFlagOption;
        NSMenuItem * wide = [self addItem:@"Wider" tag:kIdQWide on:NO to:q];
        wide.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey];
        wide.keyEquivalentModifierMask = NSEventModifierFlagOption;
        [q addItem:[NSMenuItem separatorItem]];

        for (int i = 0; i < kQPresetCount; i++) {
            const bool on = fabs(now - kQPreset[i]) < 0.005;
            [self addItem:[NSString stringWithFormat:@"Q %.2f  (%.1f oct)",
                           kQPreset[i], octavesForQ(kQPreset[i])]
                      tag:kIdQFirst + i on:on to:q];
        }

        NSMenuItem * sub = [[NSMenuItem alloc] initWithTitle:@"Bandwidth (Q)"
                                                      action:nil keyEquivalent:@""];
        sub.submenu = q;
        [menu addItem:sub];
        [menu addItem:[NSMenuItem separatorItem]];
    }

    [self addItem:@"Reset this band" tag:kIdResetBand on:NO to:menu];
    [menu addItem:[NSMenuItem separatorItem]];
    [self addItem:@"Bypass" tag:kIdBypass on:_params.bypass to:menu];
    [self addItem:@"Flatten all gains" tag:kIdFlatten on:NO to:menu];
    [self addItem:@"Reset everything" tag:kIdResetAll on:NO to:menu];

    return menu;
}

- (void)menuPicked:(NSMenuItem *)item {
    const int band = _menuBand;
    if (band < 0) return;
    const NSInteger cmd = item.tag;

    if (cmd >= kIdQFirst && cmd < kIdQFirst + kQPresetCount) {
        setBandQ(_params, band, kQPreset[cmd - kIdQFirst]);
    } else {
        switch (cmd) {
        case kIdOff:   _params.hpSlope = 0; break;
        case kId12:    _params.hpSlope = 1; break;
        case kId24:    _params.hpSlope = 2; break;
        case kIdShelf: if (band == kLF) _params.lfBell = false; else _params.hfBell = false; break;
        case kIdBell:  if (band == kLF) _params.lfBell = true;  else _params.hfBell = true;  break;
        case kIdQNarrow:   [self nudgeQ:band by:1];  break;
        case kIdQWide:     [self nudgeQ:band by:-1]; break;
        case kIdResetBand: [self resetBand:band]; break;
        case kIdBypass:    _params.bypass = !_params.bypass; break;
        case kIdFlatten:   [self flatten]; break;
        case kIdResetAll:  _params = Params::defaults(); break;
        default: return;
        }
    }
    [self commit];
    [self flushPush];
}

// ---------------------------------------------------------------------------
// Keyboard - the table in paraeq_editor.cpp, with Option for Ctrl
// ---------------------------------------------------------------------------

- (void)keyDown:(NSEvent *)event {
    NSString * chars = event.charactersIgnoringModifiers;
    if (chars.length == 0) { [super keyDown:event]; return; }

    const unichar key = [chars characterAtIndex:0];
    const NSEventModifierFlags flags = event.modifierFlags;
    const bool shift = (flags & NSEventModifierFlagShift) != 0;
    // Command too, for when the menus leave Cmd-arrow alone.
    const bool alt = (flags & (NSEventModifierFlagOption | NSEventModifierFlagCommand)) != 0;
    const double coarse = shift ? 4.0 : 1.0;
    const int    qStep  = shift ? 4   : 1;

    const bool known =
        key == NSLeftArrowFunctionKey || key == NSRightArrowFunctionKey ||
        key == NSUpArrowFunctionKey   || key == NSDownArrowFunctionKey  ||
        key == NSPageUpFunctionKey    || key == NSPageDownFunctionKey   ||
        key == ' ' || key == NSHomeFunctionKey ||
        key == NSDeleteCharacter || key == NSBackspaceCharacter || key == NSDeleteFunctionKey;
    if (!known) { [super keyDown:event]; return; }

    if (_selected < 0 && !(alt && (key == NSLeftArrowFunctionKey || key == NSRightArrowFunctionKey))) {
        [self select:kLF];
    }

    switch (key) {
    case NSLeftArrowFunctionKey:
        if (alt) { [self select:(_selected <= 0 ? kBandCount - 1 : _selected - 1)]; return; }
        [self nudgeFreq:_selected by:-coarse];
        break;
    case NSRightArrowFunctionKey:
        if (alt) {
            [self select:(_selected < 0 || _selected >= kBandCount - 1 ? 0 : _selected + 1)];
            return;
        }
        [self nudgeFreq:_selected by:coarse];
        break;
    case NSUpArrowFunctionKey:
        if (alt) [self nudgeQ:_selected by:qStep];
        else     [self nudgeGain:_selected by:0.5 * coarse];
        break;
    case NSDownArrowFunctionKey:
        if (alt) [self nudgeQ:_selected by:-qStep];
        else     [self nudgeGain:_selected by:-0.5 * coarse];
        break;
    case NSPageUpFunctionKey:   [self nudgeQ:_selected by:qStep];  break;
    case NSPageDownFunctionKey: [self nudgeQ:_selected by:-qStep]; break;
    case ' ':                   [self toggleShape:_selected]; break;
    case NSHomeFunctionKey:
        if (alt) _params = Params::defaults();
        else     [self resetBand:_selected];
        break;
    default:    // the delete keys
        if (_selected == kHP) _params.hpSlope = 0;
        else                  setBandGain(_params, _selected, 0.0);
        break;
    }

    [self commit];
}

- (void)keyUp:(NSEvent *)event {
    // The last press of a held key gets out without waiting for the throttle.
    [self flushPush];
    [super keyUp:event];
}

@end
