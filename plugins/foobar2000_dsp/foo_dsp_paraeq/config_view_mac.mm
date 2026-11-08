/* ========================================
 *  foo_dsp_paraeq - configuration view, macOS
 *
 *  The Audio Unit's editor - the curve over the console strip - shown over a
 *  dsp_preset instead of an AudioUnit. The view is MacAU/ParaEQ/ParaEQEditor.mm,
 *  compiled in here unchanged; this file is the backend it reads from and
 *  writes to, and the view controller foobar2000 for Mac puts in its window.
 *
 *  Every change goes to the host as it is made, which is what the curve is for:
 *  dsp_v3::apply_preset takes it on the running chain without a click. There is
 *  no OK or Cancel here - the host's window has its own - and nothing to undo
 *  on Cancel, since the host keeps the preset it opened with.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_core.h"
#include "paraeq_preset.h"

#include "ParaEQControls.h"
#include "ParaEQEditor.h"

#define ParaEQPresetBackend    PARAEQ_EDITOR_NAME(ParaEQPresetBackend)
#define ParaEQViewController   PARAEQ_EDITOR_NAME(ParaEQViewController)

using paraeq::Params;

namespace {

// What the AU publishes as its parameter table (ParaEQ.cpp, kControls), from
// the same core constants, so a knob covers the same travel on both. Indexed by
// kParam_; the switched controls are here only for their defaults.
struct control {
    float             minValue, maxValue;
    ParaEQControlUnit unit;
    bool              logarithmic;
    const char *      name;
};

const control k_controls[kNumberOfParameters] = {
    { paraeq::kHpFreqMin,    paraeq::kHpFreqMax,   ParaEQControlUnitHertz,    true,  "Low cut frequency"   },
    { 0.0f,                  (float)(kNumberOfSlopes - 1), ParaEQControlUnitGeneric, false, "Low cut slope" },
    { -paraeq::kGainMaxDb,   paraeq::kGainMaxDb,   ParaEQControlUnitDecibels, false, "Bass gain"           },
    { paraeq::kLfFreqMin,    paraeq::kLfFreqMax,   ParaEQControlUnitHertz,    true,  "Bass frequency"      },
    { 0.0f,                  1.0f,                 ParaEQControlUnitGeneric,  false, "Bass shape"          },
    { -paraeq::kGainMaxDb,   paraeq::kGainMaxDb,   ParaEQControlUnitDecibels, false, "Reverb cut gain"     },
    { paraeq::kLmfFreqMin,   paraeq::kLmfFreqMax,  ParaEQControlUnitHertz,    true,  "Reverb cut frequency"},
    { paraeq::kQMin,         paraeq::kQMax,        ParaEQControlUnitGeneric,  true,  "Reverb cut Q"        },
    { -paraeq::kGainMaxDb,   paraeq::kGainMaxDb,   ParaEQControlUnitDecibels, false, "Brilliance gain"     },
    { paraeq::kHmfFreqMin,   paraeq::kHmfFreqMax,  ParaEQControlUnitHertz,    true,  "Brilliance frequency"},
    { paraeq::kQMin,         paraeq::kQMax,        ParaEQControlUnitGeneric,  true,  "Brilliance Q"        },
    { -paraeq::kGainMaxDb,   paraeq::kGainMaxDb,   ParaEQControlUnitDecibels, false, "Hiss cut gain"       },
    { paraeq::kHfFreqMin,    paraeq::kHfFreqMax,   ParaEQControlUnitHertz,    true,  "Hiss cut frequency"  },
    { 0.0f,                  1.0f,                 ParaEQControlUnitGeneric,  false, "Hiss cut shape"      },
    { -paraeq::kOutputMaxDb, paraeq::kOutputMaxDb, ParaEQControlUnitDecibels, false, "Output trim"         },
    { 0.0f,                  1.0f,                 ParaEQControlUnitGeneric,  false, "Bypass"              },
};

// A configuration window has no stream, so no rate to draw at. 44.1 kHz is
// what nearly every transfer this is for arrives at, and the rate only moves
// the curve in the top octave, where it bends towards Nyquist.
const double k_sample_rate = 44100.0;

} // anonymous namespace


@interface ParaEQPresetBackend : NSObject <ShellacFiltersParaEQBackend>
- (instancetype) initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback;
@end

@implementation ParaEQPresetBackend {
    dsp_preset_edit_callback_v2::ptr _callback;
    float _values[kNumberOfParameters];
    float _defaults[kNumberOfParameters];
}

- (instancetype) initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback {
    if ((self = [super init])) {
        _callback = callback;
        ParaEQControlsFromParams(Params::defaults(), _defaults);

        dsp_preset_impl preset;
        _callback->get_preset(preset);
        ParaEQControlsFromParams(paraeq_preset::parse(preset), _values);
    }
    return self;
}

- (float) valueForParameter:(int)parameter {
    if (parameter < 0 || parameter >= kNumberOfParameters) return 0;
    return _values[parameter];
}

- (void) setValue:(float)value forParameter:(int)parameter {
    if (parameter < 0 || parameter >= kNumberOfParameters) return;
    _values[parameter] = value;

    // Through sanitize() on the way out, and the result kept, so what the
    // knobs read back is what the preset says.
    const Params p = ParaEQParamsFromControls(_values);
    ParaEQControlsFromParams(p, _values);

    dsp_preset_impl preset;
    paraeq_preset::make(p, preset);
    _callback->set_preset(preset);
}

- (BOOL) getInfo:(ParaEQControlInfo *)outInfo forParameter:(int)parameter {
    if (parameter < 0 || parameter >= kNumberOfParameters) return NO;
    const control & c = k_controls[parameter];
    outInfo->minValue     = c.minValue;
    outInfo->maxValue     = c.maxValue;
    outInfo->defaultValue = _defaults[parameter];
    outInfo->unit         = c.unit;
    outInfo->logarithmic  = c.logarithmic ? YES : NO;
    return YES;
}

- (NSString *) nameForParameter:(int)parameter {
    if (parameter < 0 || parameter >= kNumberOfParameters) return nil;
    return @(k_controls[parameter].name);
}

- (double) sampleRate { return k_sample_rate; }

// foobar2000 has no bypass of its own for a DSP in the chain; the one there is
// is the Bypass switch, which the editor already reads as a control.
- (BOOL) isBypassedByHost { return NO; }

// Nothing records automation here, and a change goes to the host as it is
// made, so there is nothing to bracket.
- (void) beginGestureForParameter:(int)parameter {}
- (void) endGestureForParameter:(int)parameter {}

@end


@interface ParaEQViewController : NSViewController
- (instancetype) initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback;
@end

@implementation ParaEQViewController {
    dsp_preset_edit_callback_v2::ptr _callback;
}

- (instancetype) initWithCallback:(dsp_preset_edit_callback_v2::ptr)callback {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _callback = callback;
    }
    return self;
}

- (void) loadView {
    ParaEQPresetBackend * backend = [[ParaEQPresetBackend alloc] initWithCallback:_callback];

    // The view keeps the backend. It draws its own margin and background, and
    // is a fixed console strip that tells Auto Layout its size through
    // intrinsicContentSize - which is what foobar2000's dialog, a stack with
    // its Done button under the view, sizes it by.
    self.view = [[ShellacFiltersParaEQView alloc] initWithBackend:backend];
}

@end


service_ptr paraeq_config_view(dsp_preset_edit_callback_v2::ptr callback) {
    ParaEQViewController * controller = [[ParaEQViewController alloc] initWithCallback:callback];
    return fb2k::wrapNSObject(controller);
}
