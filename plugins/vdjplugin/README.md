# VirtualDJ plug-ins — Declick and Dehum

Ports of the two restoration DSPs in this repository to VirtualDJ 8 / 2021 /
2023+, on Windows and macOS, built with CMake.

The DSP is not new. `declick_core.{h,cpp}` and `dehum_core.{h,cpp}` here are
byte-identical copies of the files `foo_dsp_declick` and `foo_dsp_dehum` build,
kept that way by `../foobar2000_dsp/scripts/sync_cores.ps1`; everything else in
this directory is glue. For what the algorithms do, how every default was
arrived at, and the measurements behind them, read
[`../foobar2000_dsp/README.md`](../foobar2000_dsp/README.md). This file is about
VirtualDJ.

---

## Does VirtualDJ let a plug-in read ahead?

Yes — and more directly than foobar2000 does. This mattered enough to decide the
shape of the whole port, so it is worth stating plainly.

Declick is a detect-and-interpolate repairer. To rebuild a damaged sample it
needs the audio on *both* sides of it, so the core holds `Config::latency`
samples — 880 at 44.1 kHz with the default Max repair and Model order. A host
that hands over n samples and wants n back can only get that by accepting a
delay. In a DJ program a delay is not a detail: it moves the deck later than
everything it is being mixed against.

VirtualDJ's SDK has two audio plug-in interfaces, and they answer this question
differently.

**`IVdjPluginDsp8`** is the ordinary sound effect:

```cpp
HRESULT OnProcessSamples(float *buffer, int nb);
```

Interleaved stereo floats, processed in place, n in and n out. There is no
latency to declare and nothing to compensate it with. Declick under this
interface plays about 20 ms late.

**`IVdjPluginBufferDsp8`** inverts the relationship. Rather than being handed
the audio in order, the plug-in is *asked* for it, and may fetch any part of the
decoded song it likes:

```cpp
short * OnGetSongBuffer(int pos, int nb);                  // VirtualDJ asks
HRESULT GetSongBuffer(int pos, int nb, short **buffer);    // the plug-in reads
```

So the lookahead stops being a delay and becomes an extra read. Asked for
`[pos, pos+nb)`, the buffer plug-ins here read `[pos, pos+nb+latency)` and hand
back audio aligned with what was asked for. **No delay at all** — and that is
not a claim about intent: `declick_vdj_verify` requires the output to be
bit-identical to the core run straight through the whole song, at block sizes of
64, 512, 1000, 4096 and 8192.

This is the same trick as the READ AHEAD note on `declick::Channel::prime()`,
which the foobar2000 component cannot use because its DSP is fed in order. Here
it falls out of the interface.

Dehum has no latency to begin with — its detector reads the signal but does not
sit in the path — so it gains nothing from readahead as such. What it gains from
random access is a **scout**: the opening of the record is analysed at eight
times playback speed while the deck plays, and the hum frequencies that turn up
are handed to `Channel::adopt()`. Unaided, Dehum's detector needs about 9 s to
confirm a line it can see and 43 s for one only the coherence route can reach.
Measured in `dehum_vdj_verify`, on a fixture where the unaided detector is 23 dB
short of the notch it eventually reaches, the scouted run is already there.

---

## The two plug-ins

| Plug-in | Interface | Latency | |
| --- | --- | --- | --- |
| **Declick** | `IVdjPluginBufferDsp8` | none | Detect-and-interpolate repair for shellac and vinyl transfers |
| **Dehum** | `IVdjPluginBufferDsp8` | none | Hum and drone cancellation, with the opening of the record scouted for lines |

Both use the buffer interface, and neither is built against `IVdjPluginDsp8`.
The live interface was implemented first and then dropped: it works, but under
it Declick plays 880 samples - about 20 ms - late, and on a deck that is being
mixed a delay is not a detail. What the live interface would have bought is the
ability to run on a microphone or on the master, where there is no song to read
ahead of; if you want that back, the wrapper is in the history of
`common/vdj_realtime_dsp.h` and it was working.

One plug-in per module, two modules. That is forced by the SDK:
`DllGetClassObject` is handed a single interface IID and has one object to give
back, so a DLL cannot offer two differently named effects, and it is the IID -
not the file name and not the folder - that decides what kind of plug-in
VirtualDJ thinks it has. See [`common/vdj_entry.h`](common/vdj_entry.h).

---

## Building

The VirtualDJ SDK is fetched and checksummed on the first configure
([`cmake/vdj_download_sdk.cmake`](cmake/vdj_download_sdk.cmake)), so nothing
needs installing but CMake and a compiler. The archive is three headers and
7 kB — there is no library to link.

### Windows

```powershell
.\scripts\build.ps1                    # x64, verified, packaged
.\scripts\build.ps1 -Install           # ...and dropped where VirtualDJ finds it
.\scripts\build.ps1 -Arch x86,x64      # both, if you really run 32 bit VirtualDJ
```

Output lands in `..\dist\vdj\x64\` with PDBs in `symbols\` beside it. Release
builds carry symbols on purpose: a crash report from a booth is only useful with
them, and `/DEBUG` does not slow the generated code down.

Or by hand:

```powershell
cmake -S . -B build/x64 -A x64
cmake --build build/x64 --config Release
ctest --test-dir build/x64 -C Release
```

No Visual C++ redistributable is needed. `VDJ_STATIC_CRT` defaults to `ON`, and
the finished `Declick.dll` and `Dehum.dll` import nothing but `KERNEL32.dll`.

That is not a choice this port gets to make twice. The foobar2000 components can
drop the static CRT because foobar2000 ships `msvcp140.dll` and
`vcruntime140.dll` beside its exe, so on a normal install they are already in the
process; `virtualdj.exe` imports no VC runtime at all and the install directory
holds none, so it statically links its own. `VDJ_STATIC_CRT=OFF` takes the two
x64 plug-ins from 404 kB to 114 kB — 72 % of what ships is CRT — and every one of
those bytes has to stay, because a stock VirtualDJ install has nothing to resolve
`MSVCP140.dll` against.

Release builds also carry `/Gw`, `/Zc:inline` and `/GR-`, the same three switches
as the foobar2000 ports: `/OPT:REF` can then discard globals and inline functions
nothing reaches, and there is not one `dynamic_cast` or `typeid` here or in the
three SDK headers. They are worth far less here than there — about 1 kB per
plug-in, against 7.6 % of a foobar2000 component — because what they mostly trim
in that tree is the foobar2000 SDK, and this SDK is three headers. What they
remove is unreachable rather than slower, which is the only reason to take them.

### macOS

```bash
scripts/build.sh                       # universal, verified, packaged
scripts/build.sh --sign --install
```

One universal (x86_64 + arm64) bundle per plug-in, because VirtualDJ keeps Intel
plug-ins in `Plugins64` and Apple Silicon ones in `PluginsArm` and a universal
bundle serves either. `--sign` is ad-hoc signing, which is enough for a host
that insists a loadable bundle be signed at all and establishes nothing about
who built it; a bundle for anyone else's machine needs a real identity and
notarisation, the same argument
[`../AirwindowsVSTToSignedVSTProcess.txt`](../AirwindowsVSTToSignedVSTProcess.txt)
makes for the VSTs.

> The CMake project, the wrappers and both cores are platform-neutral and the
> mac paths are written from the SDK and Apple's layout rather than from a
> build — this port has so far only been compiled and run on Windows (x64 and
> x86, MSVC 19.44, against a VirtualDJ 8.5 install). Expect the macOS side to
> need a first pass.

---

## Installing

`scripts/install.ps1` and `scripts/install.sh` do this, and cope with both
locations without being told which VirtualDJ is installed. By hand:

| | Plug-in folder |
| --- | --- |
| Windows, 2023+ | `%LOCALAPPDATA%\VirtualDJ\Plugins64\SoundEffect` |
| Windows, 8–2021 | `%USERPROFILE%\Documents\VirtualDJ\Plugins64\SoundEffect` |
| Windows, 32 bit | ...`\Plugins\SoundEffect` |
| macOS, 2023+ | `~/Library/Application Support/VirtualDJ/Plugins64/SoundEffect` and `PluginsArm/SoundEffect` |
| macOS, 8–2021 | `~/Documents/VirtualDJ/Plugins64/SoundEffect` |

Both go in `SoundEffect`. A VirtualDJ 2025 install creates exactly these
category folders under `Plugins64`, and there is no separate one for a buffer
DSP:

```
AutoStart  OnlineSources  SoundEffect  VideoEffect  VideoTransition  Visualisations
```

which is what one would expect given that VirtualDJ identifies a plug-in by
asking it for an interface by IID rather than by where it sits. Confirmed
working against VirtualDJ 2025 build 8818: both appear under
**Settings > Extensions > Effects** and run on a deck.

Two things about the scan are worth knowing, because both look like failure.

**Nothing is loaded at scan time.** VirtualDJ does not call
`DllGetClassObject` while it walks the folder - only when an effect is first
switched on. So a plug-in appears in the list without its DLL ever having been
opened, and a load trace that is empty after a restart says nothing at all.

**The `.ini` is written per effect, in `SoundEffect`, not `Plugins64`.**
VirtualDJ saves slider positions to `<Name>.ini` next to the DLL once it has
registered the effect, so the presence of `Declick.ini` is the quick way to tell
that the scan found it. Renaming a plug-in orphans its settings file.

### One installer, for handing to somebody else

Both `package` scripts build and verify first, then wrap. That order is the
point of them: packaging an unverified DLL into something that looks
installable is the one mistake worth making impossible, so `--skip-build` /
`-SkipBuild` exists only for re-wrapping binaries you just built yourself. Both
take the version from `project()` in `CMakeLists.txt`, so the file name says
what the binaries say.

#### Windows

```powershell
.\scripts\package.ps1                  # build, verify, package
.\scripts\package.ps1 -SkipBuild       # package what is already in dist
```

`plugins\dist\vdj\ShellacFilters-VirtualDJ-<version>-x64-Setup.exe`
([`installer/vdj_plugins.nsi`](installer/vdj_plugins.nsi)): one file, ~200 kB,
no dependencies, runs on anything from Windows 2000 up. x64 only — VirtualDJ has
been 64 bit since 8.2 and a 32 bit plug-in cannot be loaded into it, so there
would be nothing for a 32 bit installer to install. Needs NSIS 3 to build;
`makensis.exe` is looked for on `PATH`, then under `HKLM\SOFTWARE\NSIS`, then in
the usual Program Files places, or pass `-MakeNsis`.

Three things about it are decisions rather than defaults.

**It does not elevate, and must not.** The plug-in folder is per-user. An
elevated installer resolves `%LOCALAPPDATA%` to the *administrator's* profile,
so the DLLs land somewhere the VirtualDJ the user is actually running never
scans and the effects simply never appear. Hence `RequestExecutionLevel user` —
and hence NSIS rather than an MSI, since a per-machine MSI cannot write a
per-user folder correctly and a per-user MSI is a fight with `ALLUSERS` for no
gain here.

**It refuses rather than lies when the DLL is locked.** VirtualDJ holds a module
open from the moment its effect is first switched on until it exits, so
reinstalling over a running VirtualDJ cannot overwrite the file. `SetOverwrite
try` turns that into the error flag instead of an abort, and the user gets a
Retry that names the actual cause. `/SD IDCANCEL` on that box matters more than
it looks: `MB_RETRYCANCEL` defaults to *Retry*, and a silent install answers
every box with its default, so without it `setup.exe /S` over a running
VirtualDJ would retry the same doomed copy for ever with no window to show for
it.

**It is DPI-aware, so it is crisp rather than upscaled.** `dpiAware` plus
`dpiAwareness PerMonitorV2,system` — per-monitor so dragging the window to a
different display re-lays it out, system awareness as the fallback for Windows 8
and 8.1, which have no per-monitor mode. Text scales with the DPI; bitmaps would
not, so the installer deliberately has none — no MUI header image, no welcome
bitmap. That is also why it looks plain.

Uninstalling is a Programs and Features entry (under HKCU, since the install is
per-user) pointing at
`%LOCALAPPDATA%\ShellacFilters\VirtualDJ\uninstall.exe` — kept out of the plug-in
folder, because VirtualDJ scans that directory and it should hold plug-ins and
nothing else. It removes the two DLLs by name, never by wildcard, and leaves
`Declick.ini` and `Dehum.ini` where they are: VirtualDJ writes them next to the
DLL and they hold the settings you tuned, which are worth more than a tidy
folder.

#### macOS

```bash
scripts/package.sh                     # build, verify, package
scripts/package.sh --skip-build        # package what is already in dist
```

`plugins/dist/vdj/mac/ShellacFilters-VirtualDJ-<version>.pkg`: one double-clickable
installer carrying both plug-ins, which asks for no administrator password
because VirtualDJ reads plug-ins per user. The payload is a staging copy in
`~/Library/Application Support/ShellacFilters/VirtualDJ/` and a postinstall script
fans it out into whichever VirtualDJ homes exist, both architecture folders in
each - the same decision `install.sh` makes, for the same reason: which folder
is the right one is not knowable when the package is built. An `uninstall.sh`
is left beside the staging copy.

Unsigned by default, which Gatekeeper will object to on any machine the file was
downloaded to; two Developer ID identities and then notarising is what fixes
that, the same argument
[`../AirwindowsVSTToSignedVSTProcess.txt`](../AirwindowsVSTToSignedVSTProcess.txt)
makes for the VSTs:

```bash
# once, so the app-specific password lives in the keychain and not in argv
xcrun notarytool store-credentials ShellacFilters \
      --apple-id you@example.com --team-id ABCDE12345 \
      --password abcd-efgh-ijkl-mnop

scripts/package.sh \
      --codesign "Developer ID Application: Some One (ABCDE12345)" \
      --sign     "Developer ID Installer: Some One (ABCDE12345)" \
      --notarize --notary-profile ShellacFilters
```

`--codesign` signs the bundles inside — with the hardened runtime and a secure
timestamp, which is what makes them notarisable — `--sign` signs the package,
and `--notarize` submits it, waits, staples the ticket and checks the result
with `spctl`. Stapling matters: without the ticket written into the package,
Gatekeeper has to ask Apple at open time, and a laptop in a booth with no
network refuses a package that is in fact notarised.

Credentials never come from an option, only from the keychain profile above or
from the environment — an option value is in the shell history and in `ps`
output. For a one-off without a stored profile:

```bash
NOTARY_PASSWORD=abcd-efgh-ijkl-mnop scripts/package.sh --codesign ... --sign ... \
      --notarize --apple-id you@example.com --team-id ABCDE12345
```

and on a shared build machine, an App Store Connect API key
(`NOTARY_KEY`, `NOTARY_KEY_ID`, `NOTARY_ISSUER`), which is not tied to
anyone's Apple ID and can be revoked on its own. All of it is checked before
the build starts rather than after it — `scripts/package.sh --help` lists the
rest.

The Windows installer is unsigned too, and SmartScreen will warn about it on any
machine it was downloaded to until enough people have run it. An Authenticode
certificate is the only real fix; there is no equivalent of ad-hoc signing here.

---

## Controls

Seven sliders each, mapped exactly as in the VST2 ports — a slider position has
to mean the same thing in every port or the measurements in the foobar2000
README stop describing any of them. `declick_vdj_verify` and `dehum_vdj_verify`
pin the default positions against `Params::defaults()`.

### Declick

| Slider | Range | Default | |
| --- | --- | --- | --- |
| Sensitivity | 0–1 → 6.0–2.5 σ trigger | 0.6 (3.9 σ) | Higher finds more, and eventually starts costing music |
| Extent | 0–1 | 0.5 | How far a detection spreads into its own tail |
| Max repair | 0.2–20 ms | 4.0 ms | Longest single repair. **Structural** — see below |
| Depth | 0–1 | 0 | 0 subtracts the calibrated fraction of each click, which adds the least error of its own; 1 replaces damaged samples outright |
| Passes | 1–3 | 2 | A second pass catches clicks the first uncovers; ~50 % more CPU |
| Model order | 8–64 in eights | 64 | The one control where CPU clearly buys quality. **Structural** |
| Dry/Wet | 0–1 | 1 | 0 is a bypass, and an exact one — see *16 bit*, below |

### Dehum

| Slider | Range | Default | |
| --- | --- | --- | --- |
| Sensitivity | 0–1 → prominence threshold | 0.5 (16 dB) | Above ~0.7, hum-free material starts activating lines |
| Bandwidth | 0.1–5 Hz | 1.0 Hz | Notch half width |
| Search to | 40–500 Hz | 100 Hz | Top of the automatic search. Higher finds sustained musical notes instead |
| Harmonics | 1–8 | 1 | Only raise for a genuine mains buzz; on rumbly transfers the harmonics took 84 % of everything removed |
| Frequency | off, then 10–500 Hz | off (automatic) | Pinning one turns the search off |
| Rumble | off, then 10–200 Hz | 67 Hz | High-pass corner. Wind back towards 40 where the low end is worth keeping |
| Dry/Wet | 0–1 | 1 | |

Moving a slider does not break the audio, with two exceptions. Dehum has none at
all: only the sample rate sizes anything in that core, so every control retunes
in place. Declick's **Max repair** and **Model order** resize the pipeline, so
they restart it — which here means a warm-up and a re-serve, not a gap. Neither
allocates: both
cores size their buffers from the sample rate alone, so after the first
configure at a given rate the audio thread never touches the heap.

---

## How the plug-ins work

[`common/vdj_buffer_dsp.h`](common/vdj_buffer_dsp.h) has the full argument. The
four things worth knowing from outside:

**`pos` is not monotonic.** It follows a deck, so scratching, seeking, cue
juggling and loops all arrive as jumps at audio rate, and both cores are
sequential. So finished output is kept in a 4-second ring addressed by song
position: scratching about inside it is free and, more importantly, *consistent* —
a sample heard twice sounds the same both times. Only a jump out of the cache,
or forward by more than 0.25 s, restarts the pipeline.

**A restart warms up.** It rewinds 0.25 s before the audio that was asked for and
runs the core over that first, so Declick's noise estimate and Dehum's analysis
window are settled by the time anyone hears anything. At the measured 180×
realtime for Declick, that is about 1.5 ms of work in the call that pays for it.
Wind Model order up to 256 and it is roughly ten times that — the figure to
revisit if you do.

**`pos` does not come from one consumer, and that one cost real debugging.**
Playback asks for its block at the play head; something else asks for a quite
different part of the record in between. In a VirtualDJ 2025 trace, Declick was
serving 512-frame blocks mid-record while also being handed 4096-frame requests
marching from the start of it — Dehum's own scout, upstream in the chain,
reading through it. A buffer plug-in's `GetSongBuffer` is not a file read; it is
whatever is upstream, which may be another plug-in doing real work somewhere
else in the song.

Every one of those alien reads misses the cache and restarts the pipeline, and
the playback read after it misses too and restarts it again. That is fine, and
cheap, *provided a restart is cheap*. It was not: the warm-up length was a
number this file's author invented (0.25 s) rather than one the core asked for,
and it was paid on every restart. Two restarts per audio buffer at 11,025 frames
each is 45× the necessary work, and it stuttered. So the warm-up now comes from
`Engine::warmupFrames()` — Declick's `madWindow`, 1323 frames, is what its
detector actually needs — and a restart within `kRestartCooldownSec` of the last
one gets none at all. `declick_vdj_verify::testSecondConsumer` replays that
interleaving and holds the ratio under 2× (it was 4.99×).

**Everything is on the audio thread.** No worker threads, no locks, and one
allocation per plug-in per sample rate. The Dehum scout is budgeted against
playback rather than run flat out for the same reason: eight times playback
speed is about 5 % of one core while it is running, and nothing afterwards.

---

## 16 bit

`GetSongBuffer` hands back `short*`, so VirtualDJ's decoded song cache is 16-bit
PCM and so is what a buffer plug-in gives back. That is the format shellac and
vinyl transfers arrive in anyway.

The conversion back is rounded, **not dithered**, and that is deliberate. Dither
is the right answer when a wider signal is being narrowed; this is not that. The
input was already 16 bit, so a sample Declick did not repair divides by 32768 and
multiplies back exactly, and rounding returns the original `short`. Dithering
would put half an LSB of noise across all of the audio to smooth the
quantisation of the fraction of a percent that actually changed. `Dry/Wet` at 0
is therefore an exactly bit-transparent bypass, which `declick_vdj_verify`
checks sample for sample.

---

## Verification

```
ctest --test-dir build/x64 -C Release
```

Three binaries, none of which needs VirtualDJ installed:

| | |
| --- | --- |
| `vdj_host_verify` | The ABI. Loads each finished module the way VirtualDJ does - one `GetProcAddress`, the IID, the vtable - and requires its audio to match the same wrapper linked statically, to the bit. Also that a module declines the interface it does not implement, since VirtualDJ would otherwise call it through the wrong vtable |
| `declick_vdj_verify` | The readahead claim — pipeline output bit-identical to the core straight through the song, at five block sizes; cache consistency under jumps; restart determinism; exact bypass; the slider mappings; and the second-consumer work bound described above |
| `dehum_vdj_verify` | The same pipeline checks for the zero-latency core; the scout finds the line at the right frequency inside its advertised budget; what it finds reaches `adopt()` and gets cancelled; the slider mappings |

Neither re-tests the DSP. `declick_verify`, `dehum_verify` and the rest in
[`../foobar2000_dsp/tests`](../foobar2000_dsp/tests) do that against independent
references, and these binaries compile the identical cores.

---

## Keeping the cores in sync

`vdj_declick/declick_core.{h,cpp}` and `vdj_dehum/dehum_core.{h,cpp}` are
**mirrors, not sources**. An edit made here is an edit that will be overwritten.
The canonical copies live in `../foobar2000_dsp/foo_dsp_declick` and
`../foobar2000_dsp/foo_dsp_dehum`; change those and run

```powershell
..\foobar2000_dsp\scripts\sync_cores.ps1
```

or `sync_cores.sh` from macOS. `sync_cores.ps1 -Check` reports drift and exits
non-zero, which is what CI wants.

---

## Layout

```
cmake/
  vdj_download_sdk.cmake   fetch + checksum the SDK
  vdj_plugin.cmake         add_vdj_plugin(): a loadable module, .dll or .bundle
common/
  vdj_engine.h             what the wrapper drives; conversions, FIFO, SongSource
  vdj_buffer_dsp.h         IVdjPluginBufferDsp8 wrapper: the cache and the readahead
  vdj_entry.h              DllGetClassObject
  vdj_trace.h              opt-in load trace, for when a host lists nothing
  vdjplugin.def            so the 32 bit export is not decorated
vdj_declick/
  declick_core.{h,cpp}     MIRROR - see above
  declick_engine.h         sliders -> declick::Params -> Channel
  declick_plugin.cpp       Declick.dll
vdj_dehum/
  dehum_core.{h,cpp}       MIRROR - see above
  dehum_engine.h           sliders -> dehum::Params -> Channel, and the scout
  dehum_vdj_scout.h        reads the opening of the record faster than it plays
  dehum_plugin.cpp         Dehum.dll
installer/
  vdj_plugins.nsi          the Windows installer: per-user, no elevation, DPI-aware
scripts/
  get_sdk.{ps1,sh}         fetch the SDK by hand, if you want to
  build.{ps1,sh}           configure, build, verify, package into ../dist/vdj
  install.{ps1,sh}         copy into VirtualDJ's plug-in folder
  package.ps1              both plug-ins as one Windows setup.exe (NSIS)
  package.sh               both plug-ins as one macOS .pkg installer
tests/
  vdj_test_support.h       a stub host, a stub song, signal generators
  declick_vdj_verify.cpp
  dehum_vdj_verify.cpp
  vdj_host_verify.cpp      loads the built modules through the C ABI
```

MIT licensed, like the rest of the tree. The VirtualDJ SDK headers are Atomix
Productions' and are downloaded rather than vendored.


Install path: C:\Users\USER\AppData\Local\VirtualDJ\Plugins64\SoundEffect
