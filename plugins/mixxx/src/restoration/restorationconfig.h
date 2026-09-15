/* ========================================
 *  Mixxx restoration - settings in mixxx.cfg
 *
 *  The bridge between restoration::Settings, which is plain C++ in the cores'
 *  own units, and Mixxx's ConfigObject. Kept apart from restorationsettings.h so
 *  that the pipeline and its test harness never pull in Qt, and so that the one
 *  place that decides what a key is called and what its default is can be read
 *  in one screen.
 *
 *  Everything lives under [Restoration]. Keys are named after the controls in
 *  ../../../foobar2000_dsp/README.md, and the values are in the cores' units -
 *  milliseconds, hertz, counts - not normalised 0..1. A DJ editing mixxx.cfg by
 *  hand should be able to read "dehum_rumble 67" and know what it means, and the
 *  numbers should be the same ones the measurements are quoted in.
 *
 *  WHEN A CHANGE TAKES EFFECT: the next time a track is loaded into a deck. The
 *  settings are read in CachingReaderWorker::loadTrack, where the pipeline is
 *  sized, and a deck that is already playing keeps the pipeline it was given -
 *  its decoded chunks were produced with those settings, and re-reading them all
 *  to change a parameter would stutter the deck. Load the track again to hear a
 *  change. The preferences page says so.
 * ======================================== */

#pragma once

#include "preferences/usersettings.h"
#include "restoration/restorationsettings.h"

namespace restoration {

//! The one config group, so a typo is a compile error rather than a setting
//! that silently reads its default for ever.
extern const char* const kConfigGroup;

//! Read the settings, substituting the cores' calibrated defaults for anything
//! not present. Never fails: a mixxx.cfg with nonsense in it comes back
//! sanitized, because Params::sanitize() is what decides what is legal.
Settings readSettings(const UserSettingsPointer& pConfig);

//! Write them all back.
void writeSettings(const UserSettingsPointer& pConfig, const Settings& settings);

} // namespace restoration
