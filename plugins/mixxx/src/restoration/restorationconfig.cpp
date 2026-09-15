#include "restoration/restorationconfig.h"

#include "util/assert.h"

namespace restoration {

const char* const kConfigGroup = "[Restoration]";

namespace {

ConfigKey key(const char* item) {
    return ConfigKey(kConfigGroup, item);
}

double readDouble(const UserSettingsPointer& pConfig, const char* item, double deflt) {
    return pConfig->getValue<double>(key(item), deflt);
}

} // namespace

Settings readSettings(const UserSettingsPointer& pConfig) {
    Settings s;  // starts at the cores' calibrated defaults, both filters off
    VERIFY_OR_DEBUG_ASSERT(pConfig) {
        return s;
    }

    s.declickEnabled = pConfig->getValue<bool>(key("declick_enabled"), s.declickEnabled);
    s.declick.sensitivity = static_cast<float>(
            readDouble(pConfig, "declick_sensitivity", s.declick.sensitivity));
    s.declick.extent = static_cast<float>(
            readDouble(pConfig, "declick_extent", s.declick.extent));
    s.declick.maxLengthMs = static_cast<float>(
            readDouble(pConfig, "declick_maxrepair_ms", s.declick.maxLengthMs));
    s.declick.depth = static_cast<float>(
            readDouble(pConfig, "declick_depth", s.declick.depth));
    s.declick.passes = pConfig->getValue<int>(key("declick_passes"), s.declick.passes);
    s.declick.order = pConfig->getValue<int>(key("declick_order"), s.declick.order);
    s.declick.dryWet = static_cast<float>(
            readDouble(pConfig, "declick_drywet", s.declick.dryWet));

    s.dehumEnabled = pConfig->getValue<bool>(key("dehum_enabled"), s.dehumEnabled);
    s.dehum.sensitivity = static_cast<float>(
            readDouble(pConfig, "dehum_sensitivity", s.dehum.sensitivity));
    s.dehum.bandwidth = static_cast<float>(
            readDouble(pConfig, "dehum_bandwidth", s.dehum.bandwidth));
    s.dehum.searchTo = static_cast<float>(
            readDouble(pConfig, "dehum_searchto", s.dehum.searchTo));
    s.dehum.harmonics = pConfig->getValue<int>(key("dehum_harmonics"), s.dehum.harmonics);
    // 0 means "search for it"; 0 means "off". Both are legal values rather than
    // missing ones, so they are read like any other number and sanitize() below
    // is what decides whether what came back is usable.
    s.dehum.frequency = static_cast<float>(
            readDouble(pConfig, "dehum_frequency", s.dehum.frequency));
    s.dehum.rumbleHz = static_cast<float>(
            readDouble(pConfig, "dehum_rumble", s.dehum.rumbleHz));
    s.dehum.dryWet = static_cast<float>(
            readDouble(pConfig, "dehum_drywet", s.dehum.dryWet));

    s.sanitize();
    return s;
}

void writeSettings(const UserSettingsPointer& pConfig, const Settings& settings) {
    VERIFY_OR_DEBUG_ASSERT(pConfig) {
        return;
    }
    Settings s = settings;
    s.sanitize();

    pConfig->setValue(key("declick_enabled"), s.declickEnabled);
    pConfig->setValue(key("declick_sensitivity"), static_cast<double>(s.declick.sensitivity));
    pConfig->setValue(key("declick_extent"), static_cast<double>(s.declick.extent));
    pConfig->setValue(key("declick_maxrepair_ms"), static_cast<double>(s.declick.maxLengthMs));
    pConfig->setValue(key("declick_depth"), static_cast<double>(s.declick.depth));
    pConfig->setValue(key("declick_passes"), s.declick.passes);
    pConfig->setValue(key("declick_order"), s.declick.order);
    pConfig->setValue(key("declick_drywet"), static_cast<double>(s.declick.dryWet));

    pConfig->setValue(key("dehum_enabled"), s.dehumEnabled);
    pConfig->setValue(key("dehum_sensitivity"), static_cast<double>(s.dehum.sensitivity));
    pConfig->setValue(key("dehum_bandwidth"), static_cast<double>(s.dehum.bandwidth));
    pConfig->setValue(key("dehum_searchto"), static_cast<double>(s.dehum.searchTo));
    pConfig->setValue(key("dehum_harmonics"), s.dehum.harmonics);
    pConfig->setValue(key("dehum_frequency"), static_cast<double>(s.dehum.frequency));
    pConfig->setValue(key("dehum_rumble"), static_cast<double>(s.dehum.rumbleHz));
    pConfig->setValue(key("dehum_drywet"), static_cast<double>(s.dehum.dryWet));
}

} // namespace restoration
