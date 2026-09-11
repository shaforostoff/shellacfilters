/* ========================================
 *  foo_dsp_paraeq - preset (de)serialisation
 * ======================================== */

#include "stdafx.h"
#include "paraeq_preset.h"

using paraeq::Params;

namespace paraeq_preset {

GUID guid() {
    // {9C1E9F62-33DC-4F55-9706-1061AC976D1D}
    static const GUID g =
        { 0x9c1e9f62, 0x33dc, 0x4f55, { 0x97, 0x06, 0x10, 0x61, 0xac, 0x97, 0x6d, 0x1d } };
    return g;
}

void make(const Params & params, dsp_preset & out) {
    Params p = params;
    p.sanitize();

    dsp_preset_builder builder;
    builder << (t_uint32)version;
    builder << p.hpFrequency;
    builder << (t_uint32)p.hpSlope;
    builder << p.lfGain << p.lfFrequency;
    builder << (t_uint32)(p.lfBell ? 1 : 0);
    builder << p.lmfGain << p.lmfFrequency << p.lmfQ;
    builder << p.hmfGain << p.hmfFrequency << p.hmfQ;
    builder << p.hfGain << p.hfFrequency;
    builder << (t_uint32)(p.hfBell ? 1 : 0);
    builder << p.outputGain;
    builder << (t_uint32)(p.bypass ? 1 : 0);
    builder.finish(guid(), out);
}

Params parse(const dsp_preset & in) {
    Params p = Params::defaults();
    if (in.get_owner() == guid()) {
        try {
            dsp_preset_parser parser(in);
            t_uint32 ver = 0;
            parser >> ver;
            if (ver == 1u) {
                // Into a scratch copy, so a stream that runs out half way
                // through leaves the defaults rather than a half-read setting.
                Params tmp = Params::defaults();
                t_uint32 slope = 0, lfBell = 0, hfBell = 0, bypass = 0;

                parser >> tmp.hpFrequency;
                parser >> slope;
                parser >> tmp.lfGain >> tmp.lfFrequency;
                parser >> lfBell;
                parser >> tmp.lmfGain >> tmp.lmfFrequency >> tmp.lmfQ;
                parser >> tmp.hmfGain >> tmp.hmfFrequency >> tmp.hmfQ;
                parser >> tmp.hfGain >> tmp.hfFrequency;
                parser >> hfBell;
                parser >> tmp.outputGain;
                parser >> bypass;

                tmp.hpSlope = (int)slope;
                tmp.lfBell  = (lfBell != 0);
                tmp.hfBell  = (hfBell != 0);
                tmp.bypass  = (bypass != 0);
                p = tmp;
            }
        } catch (...) {
            p = Params::defaults();
        }
    }
    p.sanitize();
    return p;
}

bool core_read(Params & out) {
    out = Params::defaults();

    dsp_config_manager::ptr api;
    if (!dsp_config_manager::tryGet(api)) return false;

    dsp_preset_impl preset;
    if (!api->core_query_dsp(guid(), preset)) return false;

    out = parse(preset);
    return true;
}

void core_write(const Params & params, bool insertIfAbsent) {
    dsp_config_manager::ptr api;
    if (!dsp_config_manager::tryGet(api)) return;

    dsp_preset_impl preset;
    make(params, preset);

    if (!insertIfAbsent) {
        dsp_preset_impl existing;
        if (!api->core_query_dsp(guid(), existing)) return;
    }

    // Replaces in place where the equaliser already sits in the chain, so the
    // user's ordering against the other components survives an edit. Only the
    // first insertion has to pick a position, and last is right for an
    // equaliser: the restoration components ahead of it are removing defects,
    // and there is no sense equalising a click that is about to be repaired.
    api->core_enable_dsp(preset, dsp_config_manager::default_insert_last);
}

void core_remove() {
    dsp_config_manager::ptr api;
    if (dsp_config_manager::tryGet(api)) api->core_disable_dsp(guid());
}

} // namespace paraeq_preset
