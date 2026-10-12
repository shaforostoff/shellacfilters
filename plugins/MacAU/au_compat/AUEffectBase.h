/* ========================================
 *  au_compat - AUEffectBase.h
 *
 *  The Airwindows AU sources say `#include "AUEffectBase.h"` and then write
 *  `class X : public AUEffectBase`, `ComponentResult` and `COMPONENT_ENTRY`.
 *  Those spellings are Apple's from the CoreAudio SDK that shipped with Xcode
 *  3, which is not in this repository and has not shipped with Xcode in well
 *  over a decade. Apple's replacement for it is AudioUnitSDK, which is on
 *  GitHub under Apache 2.0, and it has all the same classes under different
 *  names: namespaced into `ausdk`, with `OSStatus` where the old headers said
 *  `ComponentResult` and a factory-function entry point where they had a
 *  Component Manager one.
 *
 *  This header is the whole of the difference. Put it ahead of AudioUnitSDK on
 *  the include path and the plug-in sources compile unmodified against the
 *  supported SDK - which is what scripts/build.sh does, and what makes a
 *  loadable .component possible here at all.
 *
 *  Not to be confused with ../au_shim/AUEffectBase.h, which answers to the same
 *  name for the same reason and is a different thing: a stand-in with no
 *  CoreAudio behind it, so that the verify tests can drive the wrappers on any
 *  machine. Whichever directory comes first on the include path decides which
 *  one a build gets. See ../au_shim/README.md.
 *
 *  MIT licensed, like the rest of the tree.
 * ======================================== */

#ifndef AU_COMPAT_AUEFFECTBASE_H
#define AU_COMPAT_AUEFFECTBASE_H

#include <AudioUnitSDK/AUEffectBase.h>
#include <AudioUnitSDK/ComponentBase.h>

//! CarbonCore's name for what is now just OSStatus. The two were always the
//! same 32 bit signed integer, so a `virtual ComponentResult Foo()` still
//! overrides the SDK's `virtual OSStatus Foo()`.
typedef OSStatus ComponentResult;

using ausdk::AUBase;
using ausdk::AUEffectBase;

//! The Component Manager entry point the old macro built is dead: a modern host
//! finds an Audio Unit through the AudioComponents array in its Info.plist,
//! which names a factory function rather than an entry point. AUSDK's macro
//! defines `<Class>Factory`, and that is the name the Info.plist and the .exp
//! file have to agree with - see AirwindowsAUToSignedAUProcess.txt two
//! directories up, which describes making the same change by hand.
#define COMPONENT_ENTRY(Class) AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Class)

#endif // AU_COMPAT_AUEFFECTBASE_H
