/* ========================================
 *  foo_dsp_paraeq - configuration view, macOS
 *
 *  What foobar2000 for Mac shows for Preferences -> DSP -> configure: the curve
 *  editor in a view controller, which the host puts in a window of its own
 *  with its own buttons. The counterpart of config_dialog.cpp, and thinner
 *  still - there is no OK, Cancel or Defaults to wire up, because the host
 *  owns the first two and the editor's Reset is the third. Every change goes to
 *  the host as it is made, and the host keeps the preset it opened with for a
 *  cancel.
 *
 *  Below the editor, Calibrate opens the room measurement for the Reverb cut
 *  band (calibrate_window_mac.mm), whose Apply comes back here and goes the
 *  way an edit does. It closes with this view.
 * ======================================== */

#include "stdafx.h"

#import <Cocoa/Cocoa.h>

#include "calibrate_window_mac.h"
#include "paraeq_editor_mac.h"
#include "paraeq_preset.h"

using paraeq::Params;

namespace {

// What the window opens at. The editor lays itself out for whatever it is
// given, but this is the size at which every name in the readout strip is
// written out in full.
const CGFloat kWidth  = 560.0;
const CGFloat kHeight = 366.0;
const CGFloat kMargin = 16.0;
const CGFloat kButtonRow = 28.0;    //!< Calibrate, under the editor

} // anonymous namespace

@interface FooDspParaEQConfigController : NSViewController <FooDspParaEQEditorHost, FooDspParaEQCalibrateHost>
- (instancetype)initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback;
@end

@implementation FooDspParaEQConfigController {
    dsp_preset_edit_callback_v2::ptr _callback;
    Params                           _initial;
    FooDspParaEQEditorView *         _editor;
    FooDspParaEQCalibrateWindow *    _calibrate;
}

- (instancetype)initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _callback = callback;
        dsp_preset_impl preset;
        _callback->get_preset(preset);
        _initial = paraeq_preset::parse(preset);
    }
    return self;
}

- (void)loadView {
    // A plain container with a margin round the editor, sized by its frame:
    // the editor draws to its edges, and the host's window would otherwise
    // put the curve flush against its own.
    NSView * root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];

    NSRect editorFrame = NSInsetRect(root.bounds, kMargin, kMargin);
    editorFrame.origin.y += kButtonRow + 8.0;
    editorFrame.size.height -= kButtonRow + 8.0;
    _editor = [[FooDspParaEQEditorView alloc]
        initWithFrame:editorFrame
               params:_initial
                 host:self];
    _editor.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [_editor setSampleRate:paraeq_mac_sample_rate()];
    [root addSubview:_editor];

    NSButton * calibrate = [NSButton buttonWithTitle:@"Calibrate\u2026"
                                              target:self
                                              action:@selector(openCalibration:)];
    [calibrate sizeToFit];
    calibrate.frame = NSMakeRect(kMargin, kMargin, calibrate.frame.size.width + 12.0, kButtonRow);
    calibrate.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    calibrate.toolTip = @"Measure the room with a microphone and set the Reverb cut band to its main resonance";
    [root addSubview:calibrate];

    self.view = root;
    self.preferredContentSize = root.frame.size;
}

- (void)viewDidAppear {
    [super viewDidAppear];
    // So the arrow keys reach the curve without a click first.
    [self.view.window makeFirstResponder:_editor];
}

- (void)viewWillDisappear {
    [_calibrate shutdown];
    _calibrate = nil;
    [_editor flushPush];
    [super viewWillDisappear];
}

- (void)editorParamsChanged:(const Params &)params {
    dsp_preset_impl preset;
    paraeq_preset::make(params, preset);
    _callback->set_preset(preset);
}

- (void)openCalibration:(id)sender {
    if (!_calibrate) _calibrate = [[FooDspParaEQCalibrateWindow alloc] initWithHost:self];
    [_calibrate showOver:self.view.window];
}

//! The suggestion onto the Reverb cut band, everything else left where it
//! was, and the host told at once so the change is heard while the
//! measurement window is still open.
- (void)calibrationApplyFrequency:(double)frequency q:(double)q gainDb:(double)gainDb {
    Params p = [_editor currentParams];
    p.lmfFrequency = (float)frequency;
    p.lmfQ         = (float)q;
    p.lmfGain      = (float)gainDb;
    p.sanitize();
    [_editor takeParams:p];
    [self editorParamsChanged:p];
}

@end

service_ptr paraeq_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    FooDspParaEQConfigController * controller =
        [[FooDspParaEQConfigController alloc] initWithCallback:callback];
    return fb2k::wrapNSObject(controller);
}
