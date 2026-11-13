/* ========================================
 *  foo_dsp_paraeq - room calibration window, macOS
 *
 *  See calibrate_window_mac.h, and calibrate_dialog.cpp, which this follows
 *  step for step: for as long as the window is open the equaliser's output
 *  tap is open and a worker thread keeps the last 20 to 30 s of it
 *  (foobar2000's buffer plus 8 s, see historyFor()); Start opens the chosen
 *  microphone on the same thread, and every two seconds of microphone signal
 *  is looked for in that history and, when found, added to the measurement.
 *
 *  The window polls the worker five times a second for a snapshot rather than
 *  being posted to, so the worker never waits on the UI and closing the
 *  window never races a message in flight.
 *
 *  What macOS adds is permission. foobar2000 declares the microphone in its
 *  Info.plist, so it may ask; until the user has said yes, a device delivers
 *  silence rather than an error, which would read as a microphone that hears
 *  nothing. So Start asks first and says plainly when the answer was no.
 * ======================================== */

#include "stdafx.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>

#include "calibrate_window_mac.h"
#include "coreaudio_capture.h"
#include "paraeq_calibrate.h"
#include "paraeq_core.h"

#include <math.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using paraeq_calib::Peak;
using paraeq_calib::Result;

namespace {

//! The microphone last used, so the phone's virtual device is preselected
//! next time. The Windows build's GUID: the setting means the same, and each
//! platform keeps its own configuration.
const GUID guid_cfg_device = { 0x7c3e9a51, 0x2d84, 0x4f6b, { 0x9b, 0x1e, 0x5a, 0x0c, 0x8d, 0x47, 0xe2, 0xf3 } };
cfg_string cfg_device(guid_cfg_device, "");

//! The delay is taken to be at least foobar2000's buffer less this, which
//! leaves room for a buffer that is not quite full and loses at most a block
//! or two after a seek, while it refills.
const double kSeekMarginSec = 1.0;

class Worker {
public:
    struct Snapshot {
        Result      result;
        double      historySec = 0.0;
        unsigned    outRate = 0;
        bool        capturing = false;
        double      micRate = 0.0;
        unsigned    micChannels = 0;
        std::string error;
        unsigned    serial = 0;     //!< bumps when the result changes
    };

    //! `bufferSec` is foobar2000's output buffer, which sets how much output
    //! is kept to search.
    explicit Worker(double bufferSec)
        : m_historySec(paraeq_calib::historyFor(bufferSec)),
          m_minLagSec(bufferSec - kSeekMarginSec)
    {
        paraeq_calib::tap::open();
        m_thread = std::thread([this] { run(); });
    }

    ~Worker()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
        paraeq_calib::tap::close();
    }

    void start(const std::string & id)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_deviceId = id;
        m_want = true;
        ++m_request;
        m_snap.error.clear();
        m_wake.notify_all();
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_want = false;
        ++m_request;
        m_wake.notify_all();
    }

    //! An error from the UI side - a refused permission - shown as the
    //! worker's own would be.
    void fail(const std::string & error)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_snap.error = error;
    }

    void snapshot(Snapshot & out)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        out = m_snap;
    }

private:
    void run()
    {
        // Created here rather than as a member: it holds several megabytes of
        // transform buffers, and only this thread touches it.
        std::unique_ptr<paraeq_calib::Analyzer> an(new paraeq_calib::Analyzer(m_historySec));
        an->setMinLag(m_minLagSec);
        coreaudio_capture::Capture cap;
        std::vector<paraeq_calib::tap::Segment> segs;
        std::vector<float> mic;
        unsigned seen = 0;
        unsigned outRate = 0;

        for (;;) {
            bool want;
            unsigned request;
            std::string id;
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait_for(lock, std::chrono::milliseconds(20),
                                [this, seen] { return m_quit || m_request != seen; });
                if (m_quit) break;
                want = m_want;
                request = m_request;
                id = m_deviceId;
            }

            std::string error;
            bool changed = false;
            if (request != seen) {
                seen = request;
                cap.close();
                if (want) {
                    an->resetMeasurement();
                    changed = true;
                    if (!cap.open(id, error)) cap.close();
                }
            }

            segs.clear();
            paraeq_calib::tap::drain(segs);
            for (size_t i = 0; i < segs.size(); ++i) {
                an->feedOutput(segs[i].samples.data(), segs[i].samples.size(), segs[i].rate);
                outRate = segs[i].rate;
            }

            if (cap.isOpen()) {
                mic.clear();
                if (!cap.read(mic)) {
                    error = "The microphone stopped delivering audio (unplugged?).";
                    cap.close();
                } else if (!mic.empty() && an->feedMic(mic.data(), mic.size(), cap.rate())) {
                    changed = true;
                }
            }

            std::lock_guard<std::mutex> lock(m_lock);
            if (changed) {
                m_snap.result = an->result();
                ++m_snap.serial;
            }
            m_snap.historySec  = an->historySec();
            m_snap.outRate     = outRate;
            m_snap.capturing   = cap.isOpen();
            m_snap.micRate     = cap.rate();
            m_snap.micChannels = cap.channels();
            if (!error.empty()) { m_snap.error = error; m_want = false; }
        }

        cap.close();
        an.reset();
    }

    const double            m_historySec;
    const double            m_minLagSec;
    std::thread             m_thread;
    std::mutex              m_lock;
    std::condition_variable m_wake;
    bool        m_quit = false;
    bool        m_want = false;
    unsigned    m_request = 0;
    std::string m_deviceId;
    Snapshot    m_snap;
};

//! foobar2000's configured output buffer, in seconds; 0 if it cannot say.
//! That is the most the DSP chain runs ahead of what is heard, not how far
//! ahead it is now - after a seek or at the start of playback it is less -
//! so it sizes the history and is shown, and the search still covers all of it.
double output_buffer_sec()
{
    try {
        const double b = output_manager::get()->getCoreConfig().m_buffer_length;
        return (b > 0.0 && b < 60.0) ? b : 0.0;
    } catch (...) {
        return 0.0;
    }
}

NSString * hz(double f)
{
    return f >= 1000.0 ? [NSString stringWithFormat:@"%.2f kHz", f / 1000.0]
                       : [NSString stringWithFormat:@"%.0f Hz", f];
}

NSString * ns(const std::string & s)
{
    NSString * r = [NSString stringWithUTF8String:s.c_str()];
    return r ? r : @"";
}

const double kPlotLoHz = paraeq_calib::kGridLoHz;
const double kPlotHiHz = paraeq_calib::kGridHiHz;
const double kPlotDb = 15.0;

const CGFloat kW = 660.0, kH = 560.0, kM = 16.0, kRow = 28.0, kGap = 8.0;
const CGFloat kStatusH = 76.0, kResultH = 52.0;

} // anonymous namespace

// ---------------------------------------------------------------------------
// The plot
// ---------------------------------------------------------------------------

@interface FooDspParaEQCalibratePlot : NSView
@property (nonatomic) const Result * result;    //!< owned by the window
@property (nonatomic) int selected;             //!< option, or -1
@end

@implementation FooDspParaEQCalibratePlot

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty {
    const NSRect full = self.bounds;
    NSColor * bg = [NSColor textBackgroundColor];
    NSColor * fg = [NSColor textColor];
    NSColor * grid = [fg colorWithAlphaComponent:0.25];
    [bg setFill];
    NSRectFill(full);

    NSFont * font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    NSDictionary * text = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: fg };
    const CGFloat lineH = ceil(font.ascender - font.descender + font.leading);
    const CGFloat padL = 34.0, padB = lineH + 3.0, padT = 4.0, padR = 6.0;
    const NSRect pr = NSMakeRect(padL, padT, full.size.width - padL - padR, full.size.height - padT - padB);
    if (pr.size.width <= 10.0 || pr.size.height <= 10.0) return;

    auto xOf = [&](double f) {
        return NSMinX(pr) + pr.size.width * log(f / kPlotLoHz) / log(kPlotHiHz / kPlotLoHz);
    };
    auto yOf = [&](double db) {
        db = std::max(-kPlotDb, std::min(kPlotDb, db));
        return NSMinY(pr) + pr.size.height * (kPlotDb - db) / (2.0 * kPlotDb);
    };
    auto line = [&](CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1) {
        NSBezierPath * p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(x0, y0)];
        [p lineToPoint:NSMakePoint(x1, y1)];
        [p setLineWidth:1.0];
        [p stroke];
    };

    // The band's range, shaded.
    [[[NSColor systemBlueColor] colorWithAlphaComponent:0.10] setFill];
    NSRectFillUsingOperation(NSMakeRect(xOf(paraeq_calib::kSearchLoHz), NSMinY(pr),
                                        xOf(paraeq_calib::kSearchHiHz) - xOf(paraeq_calib::kSearchLoHz),
                                        pr.size.height), NSCompositingOperationSourceOver);

    [grid setStroke];
    const double ticks[] = { 100, 200, 300, 500, 1000, 2000, 3000 };
    for (double f : ticks) {
        const CGFloat x = floor(xOf(f)) + 0.5;
        line(x, NSMinY(pr), x, NSMaxY(pr));
        NSString * label = f >= 1000.0 ? [NSString stringWithFormat:@"%.0fk", f / 1000.0]
                                       : [NSString stringWithFormat:@"%.0f", f];
        const NSSize sz = [label sizeWithAttributes:text];
        [label drawAtPoint:NSMakePoint(x - sz.width / 2.0, NSMaxY(pr) + 1.0) withAttributes:text];
    }
    for (int db = -10; db <= 10; db += 5) {
        const CGFloat y = floor(yOf(db)) + 0.5;
        line(NSMinX(pr), y, NSMaxX(pr), y);
        NSString * label = [NSString stringWithFormat:db > 0 ? @"+%d" : @"%d", db];
        const NSSize sz = [label sizeWithAttributes:text];
        [label drawAtPoint:NSMakePoint(NSMinX(pr) - 3.0 - sz.width, y - sz.height / 2.0) withAttributes:text];
    }

    const Result * r = self.result;
    const size_t n = r ? r->frequency.size() : 0;
    if (n > 1) {
        [NSGraphicsContext saveGraphicsState];
        NSRectClip(pr);
        auto curve = [&](NSColor * c, CGFloat width, const double * extra, const std::vector<double> & v) {
            NSBezierPath * p = [NSBezierPath bezierPath];
            for (size_t i = 0; i < n; ++i) {
                const NSPoint pt = NSMakePoint(xOf(r->frequency[i]), yOf(v[i] + (extra ? extra[i] : 0.0)));
                if (i == 0) [p moveToPoint:pt]; else [p lineToPoint:pt];
            }
            [p setLineWidth:width];
            [p setLineJoinStyle:NSLineJoinStyleRound];
            [c setStroke];
            [p stroke];
        };

        curve([NSColor systemGrayColor], 1.0, nullptr, r->trendDb);
        if (r->ready() && self.selected >= 0 && self.selected < r->options) {
            // What the room would sound like with the selected option applied.
            const Peak & sel = r->option[self.selected];
            const double fs = 48000.0;
            const paraeq::Biquad b = paraeq::design(paraeq::kPeaking, sel.frequency, sel.q, sel.gainDb, fs);
            std::vector<double> bell(n);
            for (size_t i = 0; i < n; ++i) bell[i] = paraeq::magnitudeDb(b, r->frequency[i], fs);
            curve([NSColor systemGreenColor], 1.0, bell.data(), r->responseDb);

            // Every option marked, the selected one solid.
            NSColor * red = [NSColor systemRedColor];
            NSDictionary * redText = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: red };
            for (int k = 0; k < r->options; ++k) {
                const CGFloat x = floor(xOf(r->option[k].frequency)) + 0.5;
                NSBezierPath * p = [NSBezierPath bezierPath];
                [p moveToPoint:NSMakePoint(x, NSMinY(pr))];
                [p lineToPoint:NSMakePoint(x, NSMaxY(pr))];
                [p setLineWidth:1.0];
                if (k != self.selected) {
                    const CGFloat dash[] = { 2.0, 2.0 };
                    [p setLineDash:dash count:2 phase:0.0];
                }
                [red setStroke];
                [p stroke];
                [[NSString stringWithFormat:@"%d", k + 1]
                    drawAtPoint:NSMakePoint(x + 2.0, NSMaxY(pr) - lineH - 2.0) withAttributes:redText];
            }
        }
        curve([NSColor systemBlueColor], 2.0, nullptr, r->responseDb);
        [NSGraphicsContext restoreGraphicsState];
    } else {
        NSDictionary * dim = @{ NSFontAttributeName: font,
                                NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };
        NSString * s = @"The room's response appears here after a few seconds of matched music.";
        const NSSize sz = [s sizeWithAttributes:dim];
        [s drawAtPoint:NSMakePoint(NSMidX(pr) - sz.width / 2.0, NSMidY(pr) - sz.height / 2.0) withAttributes:dim];
    }

    // Legend.
    {
        NSString * s = @"blue: room   grey: trend   green: with the cut";
        const NSSize sz = [s sizeWithAttributes:text];
        [s drawAtPoint:NSMakePoint(NSMaxX(pr) - 4.0 - sz.width, NSMinY(pr) + 2.0) withAttributes:text];
    }

    [grid setStroke];
    NSBezierPath * frame = [NSBezierPath bezierPathWithRect:NSInsetRect(pr, 0.5, 0.5)];
    [frame setLineWidth:1.0];
    [frame stroke];
}

@end

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

@implementation FooDspParaEQCalibrateWindow {
    __weak id<FooDspParaEQCalibrateHost> _host;
    std::unique_ptr<Worker> _worker;
    std::vector<coreaudio_capture::Device> _devices;
    Worker::Snapshot _snap;
    unsigned _drawnSerial;
    double   _bufferSec;
    BOOL     _running;
    BOOL     _asking;               //!< waiting on the permission prompt
    std::vector<std::string> _choices;   //!< what the options list holds
    NSTimer * _timer;

    NSPopUpButton * _device;
    NSButton *      _start;
    NSTextField *   _status;
    FooDspParaEQCalibratePlot * _plot;
    NSTextField *   _resultText;
    NSPopUpButton * _choice;
    NSButton *      _apply;
}

- (instancetype)initWithHost:(id<FooDspParaEQCalibrateHost>)host {
    NSWindow * w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, kW, kH)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:YES];
    if ((self = [super initWithWindow:w])) {
        _host = host;
        _drawnSerial = ~0u;
        w.title = @"Calibrate Reverb cut to the room";
        w.releasedWhenClosed = NO;
        w.contentMinSize = NSMakeSize(520.0, 420.0);
        w.delegate = self;
        [self buildContent];
    }
    return self;
}

- (NSTextField *)wrappingLabel {
    NSTextField * t = [NSTextField wrappingLabelWithString:@""];
    t.selectable = YES;
    return t;
}

- (void)buildContent {
    NSView * root = self.window.contentView;

    // Top row: the microphone and Start.
    const CGFloat top = kH - kM - kRow;
    NSTextField * label = [NSTextField labelWithString:@"Microphone:"];
    [label sizeToFit];
    label.frame = NSMakeRect(kM, top + (kRow - label.frame.size.height) / 2.0, label.frame.size.width, label.frame.size.height);
    label.autoresizingMask = NSViewMinYMargin;
    [root addSubview:label];

    _start = [NSButton buttonWithTitle:@"Start" target:self action:@selector(startStop:)];
    _start.frame = NSMakeRect(kW - kM - 90.0, top, 90.0, kRow);
    _start.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [root addSubview:_start];

    const CGFloat dx = NSMaxX(label.frame) + kGap;
    _device = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(dx, top, NSMinX(_start.frame) - kGap - dx, kRow)
                                         pullsDown:NO];
    _device.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:_device];

    // Status, under it.
    _status = [self wrappingLabel];
    _status.frame = NSMakeRect(kM, top - kGap - kStatusH, kW - 2 * kM, kStatusH);
    _status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:_status];

    // Bottom row: the options, Apply and Close.
    NSButton * close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(closeWindow:)];
    close.frame = NSMakeRect(kW - kM - 90.0, kM, 90.0, kRow);
    close.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    close.keyEquivalent = @"\033";
    [root addSubview:close];

    _apply = [NSButton buttonWithTitle:@"Apply to Reverb cut" target:self action:@selector(apply:)];
    _apply.frame = NSMakeRect(NSMinX(close.frame) - kGap - 170.0, kM, 170.0, kRow);
    _apply.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [root addSubview:_apply];

    _choice = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(kM, kM, NSMinX(_apply.frame) - kGap - kM, kRow)
                                         pullsDown:NO];
    _choice.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    _choice.target = self;
    _choice.action = @selector(choiceChanged:);
    [root addSubview:_choice];

    // The result above it, and the plot taking what is left.
    _resultText = [self wrappingLabel];
    _resultText.frame = NSMakeRect(kM, kM + kRow + kGap, kW - 2 * kM, kResultH);
    _resultText.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [root addSubview:_resultText];

    const CGFloat plotBottom = NSMaxY(_resultText.frame) + kGap;
    const CGFloat plotTop = NSMinY(_status.frame) - kGap;
    _plot = [[FooDspParaEQCalibratePlot alloc]
        initWithFrame:NSMakeRect(kM, plotBottom, kW - 2 * kM, plotTop - plotBottom)];
    _plot.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _plot.result = &_snap.result;
    _plot.selected = -1;
    [root addSubview:_plot];
}

- (void)showOver:(NSWindow *)parent {
    if (!_worker) {
        _bufferSec = output_buffer_sec();
        _worker.reset(new Worker(_bufferSec));
        [self fillDevices];
        [self setRunning:NO];
        [self updateText];
        __weak FooDspParaEQCalibrateWindow * weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *) {
            [weakSelf tick];
        }];
        if (parent) {
            const NSRect pf = parent.frame;
            NSRect f = self.window.frame;
            f.origin.x = NSMidX(pf) - f.size.width / 2.0;
            f.origin.y = NSMaxY(pf) - f.size.height - 40.0;
            [self.window setFrame:f display:NO];
        } else {
            [self.window center];
        }
    }
    [self.window makeKeyAndOrderFront:nil];
}

- (void)shutdown {
    [_timer invalidate];
    _timer = nil;
    _worker.reset();        // joins the thread and closes the tap
    _running = NO;
    if (self.window.visible) [self.window orderOut:nil];
}

- (void)windowWillClose:(NSNotification *)note {
    [self shutdown];
}

- (void)closeWindow:(id)sender {
    [self.window performClose:sender];
}

- (void)fillDevices {
    std::string defaultId;
    coreaudio_capture::listDevices(_devices, defaultId);
    [_device removeAllItems];
    const std::string remembered = cfg_device.get().c_str();
    int sel = -1, def = -1;
    for (size_t i = 0; i < _devices.size(); ++i) {
        // Titles are not unique keys for a popup; add then retitle.
        [_device addItemWithTitle:[NSString stringWithFormat:@"%zu", i]];
        [[_device lastItem] setTitle:ns(_devices[i].name)];
        if (_devices[i].id == remembered) sel = (int)i;
        if (_devices[i].id == defaultId) def = (int)i;
    }
    if (sel < 0) sel = def;
    if (sel < 0 && !_devices.empty()) sel = 0;
    if (sel >= 0) [_device selectItemAtIndex:sel];
    _start.enabled = !_devices.empty();
}

- (void)setRunning:(BOOL)on {
    _running = on;
    _start.title = on ? @"Stop" : @"Start";
    _device.enabled = !on && !_devices.empty();
}

- (void)tick {
    if (!_worker) return;
    _worker->snapshot(_snap);
    if (_running && !_snap.capturing && !_snap.error.empty()) [self setRunning:NO];
    [self updateText];
    if (_snap.serial != _drawnSerial) {
        _drawnSerial = _snap.serial;
        _plot.needsDisplay = YES;
    }
}

- (int)chosen {
    const Result & r = _snap.result;
    if (!r.ready()) return -1;
    int sel = (int)_choice.indexOfSelectedItem;
    if (sel < 0 || sel >= r.options) sel = 0;
    return sel;
}

- (void)updateText {
    const Worker::Snapshot & s = _snap;
    const Result & r = s.result;
    NSMutableString * status = [NSMutableString string];

    if (s.historySec < 0.5)
        [status appendString:@"No output from Parametric EQ yet - start playback with it in the active DSP chain."];
    else
        [status appendFormat:@"Output: %.0f s kept, %u Hz", s.historySec, s.outRate];
    if (_bufferSec > 0.0) [status appendFormat:@"%@foobar2000 buffer %.1f s.", s.historySec < 0.5 ? @" " : @"; ", _bufferSec];
    else if (s.historySec >= 0.5) [status appendString:@"."];

    if (_asking) {
        [status appendString:@"\nWaiting for permission to use the microphone..."];
    } else if (!s.error.empty()) {
        [status appendFormat:@"\n%@", ns(s.error)];
    } else if (s.capturing) {
        [status appendFormat:@" Microphone: %.0f Hz, %u ch", s.micRate, s.micChannels];
        if (r.blocks > 0) [status appendFormat:@", %.0f dBFS%@.", r.micDb, r.micClipped ? @" - CLIPPING, turn it down" : @""];
        else [status appendString:@"."];
        // First whether the microphone is lined up with the output at all;
        // nothing else means anything until it is.
        if (r.lockedLagSec >= 0.0)
            [status appendFormat:@"\nAligned: delay %.2f s, %d recent blocks agree.", r.lockedLagSec, r.agreeing];
        else if (r.matched > 0)
            [status appendString:@"\nAligning: a match, waiting for more to agree."];
        else if (r.blocks >= 8)
            [status appendString:@"\nNOT ALIGNED: the microphone does not hear this output. Is foobar2000 the one "
                                  "playing, through Parametric EQ, and loud enough at the microphone?"];
        else if (r.blocks > 0)
            [status appendString:@"\nAligning: listening for the output in the microphone..."];
        [status appendFormat:@"\nMeasured: %d of %d blocks (%.0f s)", r.matched, r.blocks,
                             r.matched * (double)paraeq_calib::kBlock / paraeq_calib::kRate];
        if (r.lagSec < 0.0 && r.blocks > 0 && r.lockedLagSec >= 0.0)
            [status appendString:@"; the last one was not - music too quiet, talking, or a pause?"];
        else [status appendString:@"."];
    } else if (r.blocks > 0) {
        [status appendFormat:@"\nStopped. Measured %d of %d blocks.", r.matched, r.blocks];
    }
    if (![_status.stringValue isEqualToString:status]) _status.stringValue = status;

    NSMutableString * result = [NSMutableString string];
    if (r.ready() && r.sure) {
        const Peak & p = r.option[0];
        [result appendFormat:@"Suggested Reverb cut: %@, Q %.1f, %.1f dB - the room's main bump, %.1f dB tall.",
                             hz(p.frequency), p.q, p.gainDb, p.heightDb];
        if (r.peaks > 1) {
            [result appendString:@"\nSmaller bumps:"];
            for (int i = 1; i < r.peaks; ++i)
                [result appendFormat:@"%@ %@ +%.1f dB", i > 1 ? @"," : @"", hz(r.peak[i].frequency), r.peak[i].heightDb];
        }
    } else if (r.ready()) {
        [result appendFormat:@"Not sure which bump is the room's main one: %d candidates, tallest first. "
                              "Try each by ear - pick one, Apply, listen.", r.options];
    } else if (r.matched >= paraeq_calib::kMinBlocks) {
        [result appendString:@"No bump between 200 Hz and 3 kHz stands out: this room needs no Reverb cut."];
    } else if (s.capturing) {
        const int need = (int)((paraeq_calib::kMinBlocks - r.matched) * (double)paraeq_calib::kBlock / paraeq_calib::kRate + 0.5);
        [result appendFormat:@"Keep walking: about %d s more of matched music before a suggestion.", need];
    }
    if (![_resultText.stringValue isEqualToString:result]) _resultText.stringValue = result;

    // The options list. Refilled only when it changes, keeping the user's
    // pick by position, so a choice is not lost every time a block lands.
    std::vector<std::string> items;
    if (r.ready())
        for (int i = 0; i < r.options; ++i)
            items.push_back([[NSString stringWithFormat:@"%d: %@, Q %.1f, %.1f dB", i + 1, hz(r.option[i].frequency),
                                                        r.option[i].q, r.option[i].gainDb] UTF8String]);
    if (items != _choices) {
        int sel = (int)_choice.indexOfSelectedItem;
        [_choice removeAllItems];
        for (size_t i = 0; i < items.size(); ++i) {
            [_choice addItemWithTitle:[NSString stringWithFormat:@"%zu", i]];
            [[_choice lastItem] setTitle:ns(items[i])];
        }
        if (sel < 0 || sel >= (int)items.size()) sel = 0;
        if (!items.empty()) [_choice selectItemAtIndex:sel];
        _choices.swap(items);
        _plot.needsDisplay = YES;
    }
    _choice.enabled = r.ready();
    _apply.enabled = r.ready();
    _plot.selected = [self chosen];
}

- (void)beginCapture {
    const NSInteger sel = _device.indexOfSelectedItem;
    if (!_worker || sel < 0 || sel >= (NSInteger)_devices.size()) return;
    cfg_device = _devices[(size_t)sel].id.c_str();
    _worker->start(_devices[(size_t)sel].id);
    [self setRunning:YES];
}

- (void)startStop:(id)sender {
    if (!_worker || _asking) return;
    if (_running) {
        _worker->stop();
        [self setRunning:NO];
        return;
    }
    // Asking needs the host to have said why in its Info.plist - without it
    // macOS ends the process rather than ask - so only then; a host without
    // it is tried as is, and a microphone that stays silent says so below.
    NSString * why = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"];
    const AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (st == AVAuthorizationStatusDenied || st == AVAuthorizationStatusRestricted) {
        _worker->fail("foobar2000 is not allowed to use the microphone. Allow it in System Settings, "
                      "Privacy & Security, Microphone, then restart foobar2000.");
        [self tick];
        return;
    }
    if (st == AVAuthorizationStatusNotDetermined && why != nil) {
        _asking = YES;
        _start.enabled = NO;
        [self updateText];
        __weak FooDspParaEQCalibrateWindow * weakSelf = self;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf permissionAnswered:granted];
            });
        }];
        return;
    }
    [self beginCapture];
}

- (void)permissionAnswered:(BOOL)granted {
    _asking = NO;
    _start.enabled = !_devices.empty();
    if (!_worker) return;
    if (granted) {
        [self beginCapture];
    } else {
        _worker->fail("foobar2000 was not allowed to use the microphone. Allow it in System Settings, "
                      "Privacy & Security, Microphone, then restart foobar2000.");
        [self tick];
    }
}

- (void)apply:(id)sender {
    const int sel = [self chosen];
    if (sel < 0) return;
    const Peak & p = _snap.result.option[sel];
    [_host calibrationApplyFrequency:p.frequency q:p.q gainDb:p.gainDb];
}

- (void)choiceChanged:(id)sender {
    _plot.selected = [self chosen];
    _plot.needsDisplay = YES;
}

@end
