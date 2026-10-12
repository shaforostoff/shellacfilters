/* ========================================
 *  foo_dsp_declick - preset (de)serialisation
 * ======================================== */

#include "stdafx.h"
#include "declick_preset.h"

using declick::Params;

namespace declick_preset {

GUID guid() {
    // {0800795F-CF0D-4355-BDF3-65E90B0A5A34}
    static const GUID g =
        { 0x0800795f, 0xcf0d, 0x4355, { 0xbd, 0xf3, 0x65, 0xe9, 0x0b, 0x0a, 0x5a, 0x34 } };
    return g;
}

void make(const Params & params, dsp_preset & out) {
    Params p = params;
    p.sanitize();

    dsp_preset_builder builder;
    builder << (t_uint32)version;
    builder << p.sensitivity << p.extent << p.maxLengthMs << p.depth << p.dryWet;
    builder << (t_uint32)p.passes << (t_uint32)p.order;
    builder << (t_uint32)(p.delta ? 1 : 0);
    builder.finish(guid(), out);
}

Params parse(const dsp_preset & in) {
    Params p = Params::defaults();
    if (in.get_owner() == guid()) {
        try {
            dsp_preset_parser parser(in);
            t_uint32 ver = 0;
            parser >> ver;
            if (ver >= 1u && ver <= 3u) {
                Params tmp = Params::defaults();
                t_uint32 passes = 0, order = 0, delta = 0;
                parser >> tmp.sensitivity >> tmp.extent >> tmp.maxLengthMs;
                if (ver >= 2u) parser >> tmp.depth;   // absent in version 1
                parser >> tmp.dryWet >> passes >> order;
                if (ver >= 3u) parser >> delta;       // absent before version 3
                tmp.passes = (int)passes;
                tmp.order = (int)order;
                tmp.delta = (delta != 0);
                p = tmp;
            }
        } catch (...) {
            p = Params::defaults();
        }
    }
    p.sanitize();
    return p;
}

} // namespace declick_preset
