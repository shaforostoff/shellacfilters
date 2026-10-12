/* ========================================
 *  foo_dsp_dehum - preset (de)serialisation
 * ======================================== */

#include "stdafx.h"
#include "dehum_preset.h"

using dehum::Params;

namespace dehum_preset {

GUID guid() {
    // {2F1B6C84-9A57-4E30-8D2C-71B5A0E4C963}
    static const GUID g =
        { 0x2f1b6c84, 0x9a57, 0x4e30, { 0x8d, 0x2c, 0x71, 0xb5, 0xa0, 0xe4, 0xc9, 0x63 } };
    return g;
}

void make(const Params & params, dsp_preset & out) {
    Params p = params;
    p.sanitize();

    dsp_preset_builder builder;
    builder << (t_uint32)version;
    builder << p.sensitivity << p.bandwidth << p.searchTo << p.frequency
            << p.rumbleHz << p.dryWet;
    builder << (t_uint32)p.harmonics;
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
            if (ver == 1u || ver == 2u) {
                Params tmp = Params::defaults();
                t_uint32 harmonics = 0, delta = 0;
                parser >> tmp.sensitivity >> tmp.bandwidth >> tmp.searchTo
                       >> tmp.frequency >> tmp.rumbleHz >> tmp.dryWet;
                parser >> harmonics;
                if (ver >= 2u) parser >> delta;   // absent in version 1
                tmp.harmonics = (int)harmonics;
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

} // namespace dehum_preset
