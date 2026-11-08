/* ========================================
 *  foo_dsp_paraeq - the layout element, macOS
 *
 *  The counterpart of ui_element.cpp: the same curve editor living in the main
 *  window, editing the live DSP chain rather than a preset. foobar2000 for Mac
 *  builds its window from a text layout (View -> Layout -> Edit Layout), one
 *  element per line, and asks every ui_element_mac whether it answers to the
 *  name on the line. This one answers to `paraeq`:
 *
 *      splitter horizontal style=thin
 *       splitter vertical style=thin
 *        albumlist
 *        playlist
 *       paraeq
 *       playback-controls
 *
 *  The three design decisions are the Windows element's, for the same reasons
 *  - see the top of ui_element.cpp:
 *
 *  1. The element holds no settings. It reads and writes the chain through
 *     dsp_config_manager, so two copies show one equaliser and agree with the
 *     DSP's own configuration window.
 *  2. Copies stay level through the chain: a dsp_config_callback tells every
 *     live element whenever the chain changes, whoever changed it.
 *  3. An equaliser that is not in the chain is drawn anyway, dimmed, with a
 *     button that puts it there.
 *
 *  What there is no Mac counterpart of: the colour and font queries, since the
 *  editor takes the system's and follows the appearance by itself; layout
 *  editing mode, since the layout is edited as text; and the popup command and
 *  bump(), which ui_element_mac does not have.
 * ======================================== */

#include "stdafx.h"

#import <Cocoa/Cocoa.h>

#include "paraeq_editor_mac.h"
#include "paraeq_preset.h"

using paraeq::Params;

@class FooDspParaEQElementController;

namespace {

//! The same identity as the Windows element's: one element, two platforms.
GUID element_guid() {
    // {9833DE78-90A8-4700-8BCD-033E32FAB171}
    static const GUID g =
        { 0x9833de78, 0x90a8, 0x4700, { 0x8b, 0xcd, 0x03, 0x3e, 0x32, 0xfa, 0xb1, 0x71 } };
    return g;
}

//! Every live element. Weak, so an element the layout dropped leaves by
//! itself. Main thread only, which is where every caller lives.
NSHashTable<FooDspParaEQElementController *> * live_elements() {
    static NSHashTable * table = [NSHashTable weakObjectsHashTable];
    return table;
}

double g_sampleRate = 44100.0;

double rate_of(const metadb_handle_ptr & track) {
    if (track.is_valid()) {
        metadb_info_container::ptr info;
        if (track->get_info_ref(info) && info.is_valid()) {
            const t_int64 sr = info->info().info_get_int("samplerate");
            if (sr > 8000 && sr < 10000000) return (double)sr;
        }
    }
    return 44100.0;
}

} // anonymous namespace

double paraeq_mac_sample_rate() { return g_sampleRate; }

// ---------------------------------------------------------------------------
// The element instance
// ---------------------------------------------------------------------------

@interface FooDspParaEQElementController : NSViewController <FooDspParaEQEditorHost>
- (void)chainChanged:(const Params &)params engaged:(bool)engaged;
- (void)sampleRateChanged:(double)rate;
@end

@implementation FooDspParaEQElementController {
    FooDspParaEQEditorView * _editor;
    Params                   _params;
    bool                     _engaged;
}

- (instancetype)init {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _params = Params::defaults();
        _engaged = paraeq_preset::core_read(_params);
    }
    return self;
}

- (void)loadView {
    // The host sizes it; this is only what it is before the host does.
    _editor = [[FooDspParaEQEditorView alloc] initWithFrame:NSMakeRect(0, 0, 460, 300)
                                                     params:_params
                                                       host:self];
    [_editor setSampleRate:g_sampleRate];
    self.view = _editor;
    [live_elements() addObject:self];
}

- (void)dealloc {
    [live_elements() removeObject:self];
}

// -- FooDspParaEQEditorHost ---------------------------------------------------

- (void)editorParamsChanged:(const Params &)params {
    _params = params;
    // Not inserted: a move on a disengaged equaliser sets it up for later. The
    // footer button is the one thing that puts it in the chain, and says so.
    paraeq_preset::core_write(params, false);
}

- (BOOL)editorEngaged { return _engaged; }

- (void)editorEngage {
    paraeq_preset::core_write(_params, true);
    _engaged = true;
}

// -- told from outside --------------------------------------------------------

- (void)chainChanged:(const Params &)params engaged:(bool)engaged {
    _engaged = engaged;
    // A chain without the equaliser has no settings to offer, so the panel
    // keeps the ones it has: they are what the button puts back.
    if (engaged) {
        _params = params;
        [_editor takeParams:params];
    }
    [_editor refreshEngaged];
}

- (void)sampleRateChanged:(double)rate {
    [_editor setSampleRate:rate];
}

@end

namespace {

// ---------------------------------------------------------------------------
// The element
// ---------------------------------------------------------------------------

class paraeq_element_mac : public ui_element_mac {
public:
    service_ptr instantiate(service_ptr arg) override {
        // `arg` carries the attributes written after the name on the layout
        // line. There are none this element reads.
        FooDspParaEQElementController * controller =
            [[FooDspParaEQElementController alloc] init];
        return fb2k::wrapNSObject(controller);
    }

    //! The names a layout line may use for it. `paraeq` is the documented
    //! one; the rest are what someone guessing would type.
    bool match_name(const char * name) override {
        if (name == NULL) return false;
        static const char * const names[] = {
            "paraeq", "parametric-eq", "parametric_eq", "parametriceq",
            "Parametric EQ", "foo_dsp_paraeq"
        };
        for (const char * n : names) {
            if (pfc::stricmp_ascii(name, n) == 0) return true;
        }
        return false;
    }

    fb2k::stringRef get_name() override { return fb2k::makeString("Parametric EQ"); }

    GUID get_guid() override { return element_guid(); }
};

FB2K_SERVICE_FACTORY(paraeq_element_mac);

// ---------------------------------------------------------------------------
// Keeping every element level with the chain
// ---------------------------------------------------------------------------

class paraeq_chain_watch_mac : public dsp_config_callback {
public:
    void on_core_settings_change(const dsp_chain_config & chain) override {
        NSHashTable * table = live_elements();
        if (table.count == 0) return;

        // Read out of what arrived rather than asking dsp_config_manager
        // again - see ui_element.cpp.
        Params p = Params::defaults();
        bool engaged = false;
        for (t_size i = 0; i < chain.get_count(); ++i) {
            if (chain.get_item(i).get_owner() == paraeq_preset::guid()) {
                p = paraeq_preset::parse(chain.get_item(i));
                engaged = true;
                break;
            }
        }

        // A copy, because an element could change the chain under the loop.
        for (FooDspParaEQElementController * c in table.allObjects) {
            [c chainChanged:p engaged:engaged];
        }
    }
};

FB2K_SERVICE_FACTORY(paraeq_chain_watch_mac);

// ---------------------------------------------------------------------------
// Keeping the curve honest about the sample rate
// ---------------------------------------------------------------------------

class paraeq_rate_watch_mac : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_stop;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        set_rate(rate_of(track));
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        set_rate(44100.0);
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info &) override {}
    void on_playback_dynamic_info_track(const file_info &) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}

private:
    static void set_rate(double rate) {
        if (rate == g_sampleRate) return;
        g_sampleRate = rate;
        for (FooDspParaEQElementController * c in live_elements().allObjects) {
            [c sampleRateChanged:rate];
        }
    }
};

play_callback_static_factory_t<paraeq_rate_watch_mac> g_paraeq_rate_watch_mac_factory;

} // anonymous namespace
