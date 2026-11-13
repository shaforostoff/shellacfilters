/* ========================================
 *  foo_dsp_paraeq - room calibration window, macOS
 *
 *  The Cocoa counterpart of calibrate_dialog.cpp, opened from the
 *  configuration view's Calibrate button: the same worker, the same texts and
 *  the same plot. A window of its own rather than a modal dialog, so the curve
 *  stays in view while the suggestion is applied; the configuration view
 *  closes it when it goes.
 *
 *  Objective-C++; include from .mm files only, after stdafx.h.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_CALIBRATE_WINDOW_MAC_H
#define FOO_DSP_PARAEQ_CALIBRATE_WINDOW_MAC_H

#import <Cocoa/Cocoa.h>

//! Where Apply goes.
@protocol FooDspParaEQCalibrateHost <NSObject>
- (void)calibrationApplyFrequency:(double)frequency q:(double)q gainDb:(double)gainDb;
@end

@interface FooDspParaEQCalibrateWindow : NSWindowController <NSWindowDelegate>

- (instancetype)initWithHost:(id<FooDspParaEQCalibrateHost>)host;

//! Shows it, over `parent` the first time. The output tap opens with it.
- (void)showOver:(NSWindow *)parent;

//! Stops the measurement, closes the tap and the window.
- (void)shutdown;

@end

#endif // FOO_DSP_PARAEQ_CALIBRATE_WINDOW_MAC_H
