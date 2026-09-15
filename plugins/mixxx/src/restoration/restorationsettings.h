/* ========================================
 *  Mixxx restoration - what the user set
 *
 *  Declick's and Dehum's parameters, in the cores' own units, with nothing Qt
 *  or Mixxx shaped about them. The preferences page reads and writes these
 *  through restorationconfig.h, which is where ConfigKey and QString live; this
 *  header is what the audio path uses, and it is deliberately something the test
 *  harness can construct without a Mixxx checkout.
 *
 *  The defaults are the cores' own - declick::Params::defaults() and
 *  dehum::Params::defaults() - because those are the calibrated ones, arrived at
 *  in ../../../foobar2000_dsp/README.md, and a port that opens on anything else
 *  is a port those measurements no longer describe. Both filters start OFF: this
 *  is a patch to a DJ program, and a restoration filter that switches itself on
 *  for everybody's whole library is not a default anyone asked for.
 * ======================================== */

#pragma once

#include "declick_core.h"
#include "dehum_core.h"

namespace restoration {

struct Settings {
    bool declickEnabled = false;
    bool dehumEnabled = false;

    declick::Params declick = declick::Params::defaults();
    dehum::Params   dehum   = dehum::Params::defaults();

    //! True if the audio path has anything to do at all. Both off is the case
    //! worth making free: the proxy then hands reads straight to the decoder and
    //! no restoration buffer is ever allocated.
    bool anyEnabled() const { return declickEnabled || dehumEnabled; }

    void sanitize() {
        declick.sanitize();
        dehum.sanitize();
    }

    bool operator==(const Settings& o) const {
        return declickEnabled == o.declickEnabled &&
                dehumEnabled == o.dehumEnabled &&
                declick == o.declick &&
                dehum == o.dehum;
    }
    bool operator!=(const Settings& o) const {
        return !(*this == o);
    }
};

} // namespace restoration
