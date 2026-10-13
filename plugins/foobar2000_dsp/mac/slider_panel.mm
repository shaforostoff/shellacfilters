/* ========================================
 *  slider_panel - the macOS configuration view shared by Declick and Dehum
 *
 *  Built in code rather than from a .xib: there is one layout, the table
 *  decides how many rows it has, and a nib would need a copy per component
 *  that differed only in the number of rows.
 * ======================================== */

#include "stdafx.h"

#import <Cocoa/Cocoa.h>

#include "slider_panel.h"

#include <math.h>
#include <stdio.h>
#include <vector>

// Objective-C has one flat namespace for class names across every bundle in
// the process, and this file is compiled into more than one component: two
// classes of the same name and whichever loaded second would silently lose.
// fb2k_component() defines the suffix as the component's own name.
#ifndef FOO_DSP_MAC_CLASS_SUFFIX
#error FOO_DSP_MAC_CLASS_SUFFIX must be defined - see cmake/fb2k_component.cmake
#endif
#define FOO_DSP_MAC_CAT2(a, b) a##b
#define FOO_DSP_MAC_CAT(a, b)  FOO_DSP_MAC_CAT2(a, b)
#define SliderPanelController  FOO_DSP_MAC_CAT(SliderPanelController_, FOO_DSP_MAC_CLASS_SUFFIX)

using slider_panel::row;

namespace {

// The same mappings as the Windows dialogs', so a slider position means the
// same value on either platform.

double pos_to_value(const row & r, int pos) {
    if (r.steps <= 0) return r.lo;
    if (r.offText != NULL) {
        if (pos <= 0) return 0.0;
        return r.lo + (r.hi - r.lo) * ((double)(pos - 1) / (double)(r.steps - 1));
    }
    return r.lo + (r.hi - r.lo) * ((double)pos / (double)r.steps);
}

int value_to_pos(const row & r, double v) {
    if (r.hi <= r.lo) return 0;
    if (r.offText != NULL && v <= 0.0) return 0;
    double t = (v - r.lo) / (r.hi - r.lo);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (r.offText != NULL) return 1 + (int)(t * (double)(r.steps - 1) + 0.5);
    return (int)(t * (double)r.steps + 0.5);
}

NSString * readout_text(const row & r, double v) {
    if (r.offText != NULL && v <= 0.0) return [NSString stringWithUTF8String:r.offText];
    char text[48];
    snprintf(text, sizeof(text), "%.*f%s", r.decimals, v, r.suffix);
    return [NSString stringWithUTF8String:text];
}

NSTextField * make_label(const char * text) {
    return [NSTextField labelWithString:[NSString stringWithUTF8String:text]];
}

// Wide enough for the longest readout either component produces ("500.0 Hz")
// with room to spare, so the sliders do not shift as the numbers change.
const CGFloat k_readout_width = 72.0;
const CGFloat k_slider_width  = 260.0;
const CGFloat k_margin        = 20.0;

// Tick marks only where there are few enough positions to tell apart, which is
// also where the Windows trackbars have them (TBS_AUTOTICKS).
const int k_max_ticks = 20;

} // anonymous namespace

@interface SliderPanelController : NSViewController
- (instancetype)initWithSpec:(const slider_panel::spec *)spec
                    callback:(dsp_preset_edit_callback_v2::ptr)callback;
@end

@implementation SliderPanelController {
    const slider_panel::spec *       _spec;
    dsp_preset_edit_callback_v2::ptr _callback;
    std::vector<double>              _values;
    bool                             _delta;

    NSMutableArray<NSSlider *> *     _sliders;
    NSMutableArray<NSTextField *> *  _readouts;
    NSButton *                       _deltaBox;
}

- (instancetype)initWithSpec:(const slider_panel::spec *)spec
                    callback:(dsp_preset_edit_callback_v2::ptr)callback {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _spec = spec;
        _callback = callback;
        _values.assign(spec->count, 0.0);
        _delta = false;

        dsp_preset_impl preset;
        _callback->get_preset(preset);
        _spec->read(preset, _values.data(), _delta);
    }
    return self;
}

- (void)loadView {
    const slider_panel::spec & s = *_spec;

    _sliders  = [NSMutableArray arrayWithCapacity:s.count];
    _readouts = [NSMutableArray arrayWithCapacity:s.count];

    NSMutableArray<NSArray<NSView *> *> * cells = [NSMutableArray array];
    for (size_t i = 0; i < s.count; ++i) {
        const row & r = s.rows[i];

        NSTextField * label = make_label(r.label);

        NSSlider * slider = [NSSlider sliderWithValue:0 minValue:0 maxValue:r.steps
                                               target:self action:@selector(sliderMoved:)];
        slider.tag = (NSInteger)i;
        slider.continuous = YES;     // live, as the Windows trackbars are
        if (r.steps <= k_max_ticks) {
            slider.numberOfTickMarks = r.steps + 1;
            slider.allowsTickMarkValuesOnly = YES;
        }
        [slider.widthAnchor constraintEqualToConstant:k_slider_width].active = YES;

        NSTextField * readout = make_label("");
        readout.alignment = NSTextAlignmentRight;
        readout.font = [NSFont monospacedDigitSystemFontOfSize:[NSFont systemFontSize]
                                                        weight:NSFontWeightRegular];
        [readout.widthAnchor constraintEqualToConstant:k_readout_width].active = YES;

        // The hint comes up wherever on the row the pointer lands, as on
        // Windows, where all three controls are registered against it.
        NSString * help = [NSString stringWithUTF8String:r.help];
        label.toolTip = help;
        slider.toolTip = help;
        readout.toolTip = help;

        [_sliders addObject:slider];
        [_readouts addObject:readout];
        [cells addObject:@[ label, slider, readout ]];
    }

    _deltaBox = [NSButton checkboxWithTitle:[NSString stringWithUTF8String:s.deltaLabel]
                                     target:self action:@selector(deltaToggled:)];
    _deltaBox.toolTip = [NSString stringWithUTF8String:s.deltaHelp];
    // Under the sliders, as on Windows, and across the readout column too: the
    // label is longer than a slider.
    [cells addObject:@[ [NSGridCell emptyContentView], _deltaBox, [NSGridCell emptyContentView] ]];

    NSGridView * grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing = 8.0;
    grid.columnSpacing = 8.0;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [grid rowAtIndex:(NSInteger)s.count].topPadding = 4.0;
    [[grid rowAtIndex:(NSInteger)s.count] mergeCellsInRange:NSMakeRange(1, 2)];
    // A slider has no baseline worth aligning to; centre each row on it instead.
    for (size_t i = 0; i < s.count; ++i) {
        [grid rowAtIndex:(NSInteger)i].yPlacement = NSGridCellPlacementCenter;
        [grid rowAtIndex:(NSInteger)i].rowAlignment = NSGridRowAlignmentNone;
    }

    NSTextField * hint = [NSTextField wrappingLabelWithString:[NSString stringWithUTF8String:s.hint]];
    hint.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    hint.textColor = [NSColor secondaryLabelColor];
    hint.selectable = NO;

    NSButton * defaults = [NSButton buttonWithTitle:@"Defaults"
                                             target:self action:@selector(defaultsPressed:)];

    NSStackView * stack = [NSStackView stackViewWithViews:@[ grid, hint, defaults ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 14.0;
    stack.edgeInsets = NSEdgeInsetsMake(k_margin, k_margin, k_margin, k_margin);
    // The paragraph wraps to the width of the rows above it. Tying the two
    // widths together with a constraint is not enough: a wrapping label with no
    // preferred width asks for its whole text on one line, and wins, pulling
    // the rows out to match. So the rows are measured first and the paragraph
    // told how wide it may be.
    const CGFloat rowsWidth = grid.fittingSize.width;
    hint.preferredMaxLayoutWidth = rowsWidth;
    [hint.widthAnchor constraintEqualToConstant:rowsWidth].active = YES;

    self.view = stack;
    [self loadControls];
}

- (void)loadControls {
    for (size_t i = 0; i < _spec->count; ++i) {
        const row & r = _spec->rows[i];
        _sliders[i].integerValue = value_to_pos(r, _values[i]);
        _readouts[i].stringValue = readout_text(r, _values[i]);
    }
    _deltaBox.state = _delta ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)push {
    dsp_preset_impl preset;
    _spec->write(_values.data(), _delta, preset);
    _callback->set_preset(preset);
}

- (void)sliderMoved:(NSSlider *)sender {
    const size_t i = (size_t)sender.tag;
    if (i >= _spec->count) return;
    const row & r = _spec->rows[i];

    // A continuous NSSlider reports fractional positions; the steps are what
    // the Windows trackbar would have stopped on.
    const double v = pos_to_value(r, (int)lround(sender.doubleValue));
    if (_values[i] != v) {
        _values[i] = v;
        _readouts[i].stringValue = readout_text(r, v);
        [self push];
    }
}

- (void)deltaToggled:(NSButton *)sender {
    _delta = (sender.state == NSControlStateValueOn);
    [self push];
}

- (void)defaultsPressed:(id)sender {
    _spec->defaults(_values.data(), _delta);
    [self loadControls];
    [self push];
}

@end

service_ptr slider_panel::make(const spec & s, dsp_preset_edit_callback_v2::ptr callback) {
    SliderPanelController * controller =
        [[SliderPanelController alloc] initWithSpec:&s callback:callback];
    return fb2k::wrapNSObject(controller);
}
