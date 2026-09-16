# vst2_shim

A clean-room implementation of the VST 2.4 plug-in ABI, enough to build the
plug-ins in this folder into DLLs that real hosts load. MIT licensed, like the
rest of the tree.

It is used by the Linux ports as well. The folder is under `WinVST` because that
is the port that needed it first, but an ABI fixed in 1999 by hosts that ran on
both is not a Windows thing: the `AEffect` layout, the opcode numbers and the
calling convention are the same, and `vstplugmain.cpp` is the one file with a
platform branch in it — how a symbol is exported, and how the pre-2.4 `main`
alias is made without a `.def` file. See
`plugins/foobar2000_dsp/scripts/build_linuxvst.sh`.

## Why it exists

`plugins/AirwindowsWinVSTTemplate.txt` is blunt about it:

> the builds have been intentionally broken because Steinberg doesn't want you
> to develop VST2s, so you're on your own

The six files it says you need — `aeffeditor.h`, `audioeffect.{h,cpp}`,
`audioeffectx.{h,cpp}`, `vstplugmain.cpp` — are not redistributable and are not
here. Without them, `plugins/WinVST` is source code that cannot be built by
anybody who does not already have a copy of a discontinued SDK.

So this is written from the published description of the interface instead.
JUCE, Ardour and LMMS all arrived at the same place and did the same thing.

## What is here

| file | what it is |
|---|---|
| `vst2_abi.h` | the ABI: `AEffect`, the opcode enums, the flags. Mostly comments and `static_assert`s. |
| `audioeffectx.h` | `AudioEffect` and `AudioEffectX`, the classes a plug-in derives from. |
| `audioeffectx.cpp` | the opcode dispatcher and the five C thunks in `AEffect`. |
| `vstplugmain.cpp` | `VSTPluginMain`, the single exported symbol, and the `main` alias for hosts that predate the rename. |

No MIDI, no offline processing, no speaker arrangements, no parameter
properties. Those opcodes are answered "not supported", which is what the stock
Airwindows plug-ins answered anyway by not overriding them.

There is an editor, added for ParaEQ. `AEffEditor` in `audioeffectx.h` is the
SDK's class cut down to the four opcodes a host actually sends — `effEditGetRect`,
`effEditOpen`, `effEditClose`, `effEditIdle` — with no VSTGUI and no knowledge of
what is drawn inside; `ERect` in `vst2_abi.h` is the eight-byte struct they are
answered with, whose field order is `top, left, bottom, right` and not a Win32
`RECT`'s. A plug-in that calls `setEditor()` gets `effFlagsHasEditor` set for it
and those four routed. One that does not is byte for byte the plug-in it was
before, which is what Declick and Dehum assert.

## The part to be suspicious of

An ABI is not an interface you get to design. Every byte offset in `AEffect`
and every integer in the opcode enums was fixed in 1999 by hosts that are still
in use, and getting one wrong does not fail to compile — it loads, and then a
host reads a function pointer out of the middle of an integer field and jumps
to it.

Three things push back on that:

- **Every field offset is `static_assert`ed**, along with `sizeof(AEffect)` —
  144 bytes on 32 bit, 192 on 64 bit. Those two numbers are the one thing an
  outsider can check this against without reading it.
- **The opcode enums keep their deprecated entries.** `effGetVu` is unused and
  deleting it would silently move the ten opcodes after it. The values the
  plug-ins depend on are additionally asserted as literals.
- **`tests/vst_host_verify.cpp` loads a finished plug-in** and drives it through
  the C ABI and nothing else — `LoadLibrary`/`dlopen`, a symbol lookup, opcodes,
  the function pointers — comparing the audio that comes back against the same
  core driven directly, and checking that repeated open/close does not leak. It
  runs against both the `.dll` and the `.so`.

What none of that can prove is that 144 and 192 and `effGetChunk == 23` are
themselves right, because the shim and its test agree with each other by
construction. Only a real host settles that. What the assertions do is stop the
numbers moving once they are set.

## Building

    scripts\build_winvst.ps1      Declick32/64.dll, Dehum32/64.dll
    scripts/build_linuxvst.sh     Declick.so, Dehum.so

from `plugins/foobar2000_dsp`. The Windows one compiles with `cl.exe` directly
rather than through each plug-in's `VSTProject.vcxproj`, because those ask for
toolset v140 and Windows SDK 8.1 and expect the SDK at a path that is not in
this tree. The `.vcxproj`, `.sln` and `.def` files are left exactly as
Airwindows ships them, so the documented "drag the folder into VSTProject and
press build" route still works for anyone who does have the real SDK.

The Linux one drives `g++` over `plugins/LinuxVST/src/{Declick,Dehum}` for the
same reason: `plugins/LinuxVST/CMakeLists.txt` wants Steinberg's sources in
`LinuxVST/include/vstsdk`, and it is left alone so that it keeps working for
anyone who has them.
