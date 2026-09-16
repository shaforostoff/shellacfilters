# au_shim

Enough of Apple's Audio Unit base classes to compile `plugins/MacAU/Declick`,
`plugins/MacAU/Dehum` and `plugins/MacAU/ParaEQ` and drive them from a test.
MIT licensed, like the rest of the tree.

## Why it exists

`plugins/AirwindowsAUTemplate.txt` builds these plug-ins against the CoreAudio
`AUPublic` and `PublicUtility` sources that shipped with Xcode 3.2.6, which the
`.xcodeproj` reaches for at `$(SYSTEM_DEVELOPER_DIR)/Extras/CoreAudio/...`.
Those files are Apple's, they are not redistributable, they are not in this
repository, and they have not shipped with Xcode for well over a decade. So
`plugins/MacAU` is 545 folders of source that only builds on a machine that
still has that SDK — which is fine for the 542 that were written on one, and no
use at all for checking that the three written here are correct.

This is not the Xcode SDK, and it is not trying to be. It is the smallest thing
that lets `Declick.cpp`, `Dehum.cpp` and `ParaEQ.cpp` compile unmodified and be
driven the way a host drives them, so that `declick_au_verify`,
`dehum_au_verify` and `paraeq_au_verify` can compare their output against the
cores those files share with `foo_dsp_declick`/`foo_dsp_dehum`/`foo_dsp_paraeq`.

## Its sibling, and which one you get

There are two headers in this tree called `AUEffectBase.h`, because that is the
name the plug-in sources ask for and they are not edited for either of them.
Whichever directory comes first on the include path decides:

| | |
|---|---|
| `au_shim/AUEffectBase.h` | this file. No CoreAudio behind it, so the tests build and run anywhere. Nothing loads what it produces. |
| `au_compat/AUEffectBase.h` | 50 lines that map the same names onto Apple's current [AudioUnitSDK](https://github.com/apple/AudioUnitSDK), the supported descendant of the missing sources. This is what `scripts/build.sh` uses, and it produces a real `.component`. |

So the answer to "what about MacSignedAU" is: the useful half of it — the
`AudioComponents` array in the `Info.plist` and the factory-function entry point
that goes with it, both described in `../AirwindowsAUToSignedAUProcess.txt` — is
in these three sources already. The other half of MacSignedAU is a Header Search
Path pointing at a CoreAudio SDK on somebody else's desktop, which is the
problem rather than the answer.

## What is here

One header, `AUEffectBase.h`, holding:

| | |
|---|---|
| scalars and handles | `ComponentResult`, `OSStatus`, `Float32/64`, `AudioUnit`, the `AudioUnitScope`/`Element`/`ParameterID` family |
| structures | `AudioBufferList`, `AudioBuffer`, `AUChannelInfo`, `AudioUnitParameterInfo` |
| constants | the handful of `kAudioUnit*` values these three plug-ins name |
| `AUBase` | parameter storage, `Globals()`, `GetSampleRate()`, `FillInParameterName()`, `PropertyChanged()` |
| `AUEffectBase` | the virtuals the plug-ins override, each defaulting to the "not supported" answer |
| `CFArrayCreate`, `CFRetain` | the identity, for the parameter value strings and clump names ParaEQ publishes |
| `COMPONENT_ENTRY` | defines the `<Name>Entry` symbol, so a missing or misspelled one is a link error |

Plus four methods a real AU does not have, spelled `AUShim*` so they cannot be
mistaken for SDK surface: set the sample rate, and read back how many times the
plug-in called `PropertyChanged` and with what. That last pair is the point of
the whole file — it is what an AU says instead of the VST's
`audioMasterIOChanged`, and the tests assert on exactly when it happens, which
for ParaEQ is never.

## The part to be suspicious of

Unlike `plugins/WinVST/vst2_shim`, **this shim is not an ABI**. Nothing here is
loaded by anything; the tests link the plug-in's C++ classes directly. So the
numeric values below are inputs to a comparison and nothing more, and none of
them is asserted:

- The `kAudioUnit*` constants are written from the published headers but are
  never round-tripped through anything that would notice if one were wrong.
  (`kAudioUnitParameterFlag_HasCFNameString` was in fact wrong here — `1 << 20`,
  which is `HasClump`'s bit — and nothing noticed until ParaEQ needed `HasClump`
  and the two would have collided.)
- `AudioUnitParameterInfo` has the right *fields*, not necessarily the right
  layout, and nothing depends on its layout.
- `AudioBufferList` ends in a fixed `mBuffers[2]` rather than the real flexible
  `mBuffers[1]` that callers over-allocate. Nothing here goes past stereo —
  Declick and Dehum say 2 in 2 out and nothing else, ParaEQ adds mono — so two
  is the whole story.
- Parameter names are `const char *` and `CFSTR` is the identity, because
  nothing in this tree does anything with a parameter name but hand it straight
  back. That is also what lets the tests build on a machine with no
  CoreFoundation.

What that leaves unproven is everything between the compiler and a DAW: the
`AudioComponents` registration, the entry point, the `Info.plist`,
`GetPropertyInfo`/`GetProperty` beyond the fact that they forward. **That half
is `scripts/build.sh --validate`**, which builds the real universal
`.component` against AudioUnitSDK and hands each one to `auval` — Apple's own
host-side conformance suite, and as close to "a DAW loaded it" as a script gets.
It is macOS-only, which is why it is a shell script rather than another ctest.
Run both; they check disjoint things.

## Building

The two halves, separately.

The verify tests are ordinary targets in
`plugins/foobar2000_dsp/tests/CMakeLists.txt`. To run them from a Mac, where
that project does not configure, compile them directly — or just let
`scripts/build.sh` do it, which is the same three commands:

    cd plugins
    clang++ -std=c++11 -O2 -I MacAU/au_shim -I MacAU/Declick \
        foobar2000_dsp/tests/declick_au_verify.cpp \
        MacAU/Declick/Declick.cpp MacAU/Declick/declick_core.cpp \
        -o declick_au_verify && ./declick_au_verify

    clang++ -std=c++11 -O2 -I MacAU/au_shim -I MacAU/Dehum \
        foobar2000_dsp/tests/dehum_au_verify.cpp \
        MacAU/Dehum/Dehum.cpp MacAU/Dehum/dehum_core.cpp \
        -o dehum_au_verify && ./dehum_au_verify

    clang++ -std=c++11 -O2 -I MacAU/au_shim -I MacAU/ParaEQ \
        foobar2000_dsp/tests/paraeq_au_verify.cpp \
        MacAU/ParaEQ/ParaEQ.cpp MacAU/ParaEQ/paraeq_core.cpp \
        -o paraeq_au_verify && ./paraeq_au_verify

The components are `../scripts/build.sh`, which fetches AudioUnitSDK on the
first run and needs nothing installed but the Xcode command line tools:

    plugins/MacAU/scripts/build.sh --validate      build, verify, auval
    plugins/MacAU/scripts/package.sh --help        and then ship them
