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
 * ======================================== */

#include "stdafx.h"

#import <Cocoa/Cocoa.h>

#include "paraeq_editor_mac.h"
#include "paraeq_preset.h"

using paraeq::Params;

namespace {

// What the window opens at. The editor lays itself out for whatever it is
// given, but this is the size at which every name in the readout strip is
// written out in full.
const CGFloat kWidth  = 560.0;
const CGFloat kHeight = 330.0;
const CGFloat kMargin = 16.0;

} // anonymous namespace

@interface FooDspParaEQConfigController : NSViewController <FooDspParaEQEditorHost>
- (instancetype)initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback;
@end

@implementation FooDspParaEQConfigController {
    dsp_preset_edit_callback_v2::ptr _callback;
    Params                           _initial;
    FooDspParaEQEditorView *         _editor;
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

    _editor = [[FooDspParaEQEditorView alloc]
        initWithFrame:NSInsetRect(root.bounds, kMargin, kMargin)
               params:_initial
                 host:self];
    _editor.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [_editor setSampleRate:paraeq_mac_sample_rate()];
    [root addSubview:_editor];

    self.view = root;
    self.preferredContentSize = root.frame.size;
}

- (void)viewDidAppear {
    [super viewDidAppear];
    // So the arrow keys reach the curve without a click first.
    [self.view.window makeFirstResponder:_editor];
}

- (void)viewWillDisappear {
    [_editor flushPush];
    [super viewWillDisappear];
}

- (void)editorParamsChanged:(const Params &)params {
    dsp_preset_impl preset;
    paraeq_preset::make(params, preset);
    _callback->set_preset(preset);
}

@end

service_ptr paraeq_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    FooDspParaEQConfigController * controller =
        [[FooDspParaEQConfigController alloc] initWithCallback:callback];
    return fb2k::wrapNSObject(controller);
}
