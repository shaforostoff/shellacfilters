# foobar2000 DSP components

Four DSPs for restoring 78s and vinyl transfers:

| Component | What it is |
| --- | --- |
| **foo_dsp_decrackle** | A port of the Airwindows **DeCrackle** plug-in, verified **bit-identical** to the VST source. Best on stereo material. |
| **foo_dsp_declick** | An autoregressive detect-and-interpolate declicker. **Use this one for mono shellac** — see [Which one to use](#which-one-to-use). |
| **foo_dsp_dehum** | Finds continuous narrowband tones by itself and cancels them. For hum, which is a different defect from clicks and needs a different instrument. Zero latency. |
| **foo_dsp_paraeq** | A console channel equaliser with a curve editor, for the tone of the transfer once the defects are out of it. Also a Default UI element, so it can live in the main window. Zero latency. |

All four:

* Target **foobar2000 1.5 and later**, 32-bit and 64-bit.
* Build with **CMake**, so any Visual Studio from 2017 15.7 onward works.
* Depend on nothing beyond the foobar2000 SDK, which is downloaded
  automatically. No WTL, no ATL, no vcpkg, no 7-Zip.
* Link the CRT statically — nothing to install alongside them.

---

## Which one to use

They are not interchangeable, and the difference is structural rather than a
matter of taste.

**DeCrackle** decides a click is present by looking at `|L * R * 64|` — a
measure of correlated energy that a one-channel surface tick fails to produce.
On a **mono** transfer `L == R`, so that term collapses to `64x²`, which sits
2.6–7.3× above `|x|` on typical 78 rpm material. The detector is then either
silent or firing on 60% of samples, with nothing useful in between.

It also repairs by crossfading to a lowpassed copy of the damaged audio, so
during a click it outputs a smoothed version of the click rather than a
reconstruction of the music underneath.

**Declick** detects clicks as spikes in an autoregressive prediction residual —
which works the same whether the source is mono or stereo — and repairs them by
least-squares interpolation, reconstructing what the waveform should have been.

Rough guide: **stereo vinyl → either; mono shellac → Declick.**

**Dehum is not an alternative to either of them.** Clicks are impulsive and
broadband, hum is continuous and narrowband, and nothing that detects one will
find the other. Run Dehum alongside whichever declicker suits the material.

**The Parametric EQ is not a restoration tool at all.** The other three remove
things that are not in the performance. It changes the balance of what is, which
is the job that is left once they have finished, and it belongs after them in
the chain — there is no sense equalising a click that is about to be repaired.
It is also the only one of the four with a reason to be on screen while the
record plays, which is why it is a UI element as well as a dialog.

---

## Building a release package

```bash
powershell -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

That configures and builds x86 and x64 in Release, runs the test suites, and
writes to `..\dist\` (one level up, because it is shared with the VST2 builds
below, and a `.dll` for a DAW is not a foobar2000 component):

| File | What it is |
| --- | --- |
| `foo_dsp_decrackle-1.0.0.fb2k-component` | Installable component, both architectures in one file |
| `foo_dsp_declick-1.1.2.fb2k-component` | Same, for the declicker |
| `foo_dsp_dehum-1.1.1.fb2k-component` | Same, for the dehummer |
| `*-symbols.zip` | PDBs — keep these, they are what makes a foobar2000 crash report readable |

Install by dragging the `.fb2k-component` onto foobar2000, or via
**File → Preferences → Components → Install…**

Inside the archive:

```
foo_dsp_decrackle.dll        32-bit, loaded by foobar2000 1.5 – 2.x (x86)
x64/foo_dsp_decrackle.dll    64-bit, loaded by foobar2000 2.x (x64)
```

foobar2000 ignores architecture folders it does not understand, so a single
file installs correctly on every version.

### Options

```powershell
# Build only 64-bit
.\scripts\build_release.ps1 -Arch x64

# Build only one component
.\scripts\build_release.ps1 -Component foo_dsp_declick

# Windows 7 compatible build, verified before packaging (see the note below)
.\scripts\build_release.ps1 -Win7

# Assume AVX2 - only if the playback machine definitely has it
.\scripts\build_release.ps1 -InstructionSet AVX2

.\scripts\build_release.ps1 -Clean -SkipTests
```

### Targeting Windows 7

```powershell
.\scripts\build_release.ps1 -Win7
```

`-Win7` picks the newest installed MSVC toolset that still supports Windows 7
(v142, otherwise v141), and runs `scripts\check_win7.ps1` over the built DLLs
before packaging, so a component that cannot load on Windows 7 is never
produced.

The defaults already do most of the work: the `SSE2` baseline runs on anything
Windows 7 runs on, `_WIN32_WINNT=0x0601` keeps newer APIs out of reach at
compile time, and the static CRT means the Visual C++ redistributable — which
no longer installs on Windows 7 from version 14.40 onward — is not needed.

What `-Win7` adds is the toolset. From Visual Studio 2022 17.10 onward, v143
dropped Windows 7 as a supported target. v142 ships with Visual Studio 2019, or
as the *MSVC v142 – VS 2019 C++ build tools* individual component in the Visual
Studio 2022 installer; v141 ships with Visual Studio 2017. If neither is
installed, `-Win7` warns and builds with v143 anyway — the check still runs, but
the result is not something Microsoft supports.

The Windows SDK version is a red herring here. `cmake` reporting

```
-- Selecting Windows SDK version 10.0.26100.0 to target Windows 10.0.19045.
```

says which headers and import libraries are used, not which Windows versions
the binary runs on — that is decided by `_WIN32_WINNT`, the toolset and the CRT.
`-WindowsSdk 10.0.17763.0` pins an older one if you want it, but note that the
foobar2000 SDK needs `ERROR_NO_SUCH_DEVICE`, which SDK 10.0.17763.0 does not
define.

#### Checking a binary on its own

```powershell
.\scripts\check_win7.ps1 ..\dist\foo_dsp_declick-1.1.2.fb2k-component
```

Takes DLLs, directories or `.fb2k-component` archives, and reports anything
that would stop the image from loading on Windows 7:

- a minimum OS or subsystem version above 6.1 in the PE header,
- imports from `api-ms-win-*` / `ext-ms-*` API sets, `combase.dll` or
  `shcore.dll`, none of which exist on Windows 7,
- imports of `vcruntime140.dll`, `msvcp140.dll` or `ucrtbase.dll`, i.e. a
  dynamic CRT,
- individual Windows 8/8.1/10 exports, such as the `WaitOnAddress` family a
  modern STL likes to reach for.

It is a load-time check. Functions resolved at runtime through `GetProcAddress`
are invisible to it, and so is the instruction set the code was compiled for —
an `AVX2` build loads fine on a CPU without AVX2 and then crashes. Declick's
AVX2 kernels are not that: they are compiled into the SSE2 build and run only
where CPUID says the CPU and the OS support them.

### macOS

```bash
scripts/build_release_macos.sh
```

Declick, Dehum and the Parametric EQ have macOS builds; DeCrackle does not.
This builds them universal (Apple Silicon and Intel), runs the test suites
that apply there, signs each bundle ad hoc, and writes
`foo_dsp_<name>-<version>-mac.fb2k-component` plus a `-symbols.zip` with the
`.dSYM` to `../dist/`. Needs CMake and the Xcode command line tools; the SDK
is fetched the same way as on Windows.

To ship one archive per component that installs on both platforms, build the
Windows ones first, copy them into `../dist/`, and merge:

```bash
scripts/build_release_macos.sh --merge ../dist
```

That adds `mac/foo_dsp_<name>.component` next to the DLLs in each Windows
archive and writes it back under the Windows name. `--help` lists the rest
(`--arch`, `--sign`, `--skip-tests`, `--clean`, and naming components).

Declick and Dehum have the same settings on macOS as on Windows, in a
view foobar2000 for Mac shows in its own window. The Parametric EQ has no
configuration view there yet, nor a UI element, so it runs with its
defaults - which is flat.

---

## Building by hand

```bash
cmake -S . -B build/x64 -A x64
cmake --build build/x64 --config Release
```

```bash
cmake -S . -B build/x86 -A Win32
cmake --build build/x86 --config Release
```

```bash
cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build/mac
```

The first configure downloads and unpacks
`https://www.foobar2000.org/downloads/SDK-2025-03-07.7z` into
`external/foobar2000_sdk` (checksum verified). CMake's bundled libarchive
reads `.7z`, so no external tool is needed.

To fetch the SDK on its own:

```bash
powershell -ExecutionPolicy Bypass -File scripts\get_sdk.ps1
```

or straight from CMake:

```bash
cmake -DFB2K_SDK_DEST=external/foobar2000_sdk -P cmake/fb2k_download_sdk.cmake
```

### CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `FB2K_SDK_DIR` | `external/foobar2000_sdk` | Where the SDK lives |
| `FB2K_SDK_AUTO_DOWNLOAD` | `ON` | Fetch it if missing |
| `FOO_DSP_STATIC_CRT` | `ON` | `/MT` instead of `/MD` |
| `FOO_DSP_LTO` | `ON` | `/GL` + `/LTCG` in Release |
| `FOO_DSP_ARCH` | `SSE2` | `SSE2`, `AVX` or `AVX2` — see [Vectorization](#vectorization); leaving it at `SSE2` costs nothing |
| `FOO_DSP_WIN32_WINNT` | `0x0601` | Minimum Windows version |
| `FOO_DSP_BUILD_TESTS` | `ON` | Build the verification harnesses |

Not options, but worth knowing about: Release builds add `/Gw` and `/Zc:inline`,
so `/OPT:REF` can discard globals and inline functions nothing reaches, and
`/GR-`, because there is not one `dynamic_cast` or `typeid` in these components
or anywhere in the SDK — exception handling does not use RTTI, catch matching
carries its own type descriptors. Together they take about **7.6 %** off each
component and nothing off the DSP path: what they remove is unreachable, not
slower.

The static CRT is the other 60 %. `FOO_DSP_STATIC_CRT=OFF` takes the four x64
components from 1,034 kB to 335 kB, and foobar2000 ships `msvcp140.dll` and
`vcruntime140.dll` beside `foobar2000.exe`, so on a normal install they are
already in the process. It stays `ON` by default because the components are
also meant to drop into an install that is older or stranger than that one.

---

## Declick parameters

Add *Declick (AR interpolation)* to the chain and press **Configure selected**.

| | |
| --- | --- |
| **Sensitivity** | The one to reach for. Raise it until the crackle goes, then back off as soon as the music starts to dull. Maps onto the trigger threshold in robust sigmas: 0 → 6.0σ, 1 → 2.5σ. Default **0.9** (2.85σ). Runs that barely cross the trigger are only partly repaired — half of what the model says is wrong at the trigger, rising to all of it 3.5σ above — so the default reaches small clicks without taking much from clean records; on clean records 0.7–0.8 takes less still. See [Dense crackle](#dense-crackle). |
| **Extent** | How far a detection spreads into its own tail before the repair stops. Raise it if repairs leave a residual tick behind them — but it lowers the floor for every repair, so it spreads into good audio everywhere; a strong click whose error rings through zero is already repaired whole without it (see the dip rule under [Dense crackle](#dense-crackle)). 0.3 is gentler on clean records. |
| **Max repair** | Longest single repair. Anything longer is treated as music and left alone. 4 ms suits 78s. |
| **Passes** | A second pass catches clicks the first one uncovers; the model is refitted in between. Small but real gain, roughly 50–60% more CPU. |
| **Model order** | 8–256 in steps of 2, default **128**. Higher orders model the music better, so they change clean records least, and remove isolated clicks better; but they also predict some short clicks well enough to miss them on heavily crackled transfers. 128 is the balance. Any even value is as good as a power of two — CPU and latency grow smoothly with it: 256 costs about 2.1× what 128 does, 64 about 0.6×. The VST2, AU and VirtualDJ ports run 16–256 in eights, with the same default of 128. See [Choosing settings](#choosing-settings) and [Does more CPU help?](#does-more-cpu-help). |
| **Repair depth** | How much of each click to subtract. 1, the default, replaces the damaged samples outright; 0 subtracts the 45% fraction that adds the least whole-file error, which caps every repair at −5.2 dB. See [Repair depth](#repair-depth) and [Dense crackle](#dense-crackle). |
| **Dry/Wet** | 0 bypasses. |
| **Output only what is removed** | Delta monitoring, for testing: outputs input minus output, so you hear what the repair takes out. It should be crackle and clicks and nothing else; music heard in it is collateral damage, and the cue to lower Sensitivity. Saved with the preset, so untick it when done. foobar2000 only. |

Measured over 20 s excerpts of four 78 rpm transfers, at the repair depth of 0
and order 32 that were the defaults then. The unprocessed originals sit at
**71** impulsive events/s.

| Sensitivity | samples repaired | events/s | collateral damage | HF change | precision |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 0.7 % | 39 | −60.8 dB | −0.3 dB | 0.91 |
| 0.30 | 1.5 % | 26 | −50.2 dB | −0.5 dB | 0.85 |
| **0.60** (then the default) | 3.4 % | 21 | −46.5 dB | −0.8 dB | 0.72 |
| 0.70 | 4.6 % | 20 | −45.4 dB | −1.0 dB | 0.66 |
| 0.85 | 7.6 % | 19 | −43.2 dB | −1.3 dB | 0.55 |
| 1.00 | 14.0 % | 18 | −41.8 dB | −1.7 dB | 0.43 |

Note how flat the right-hand end is: quadrupling the number of samples touched
(3.4 % → 14.0 %) only takes 21 events/s down to 18. Past about 0.7, sensitivity
mostly buys collateral damage. **Repair depth is the more effective lever**.

Throughput: ~205× realtime at the defaults on a CPU with AVX2 and FMA, ~110×
without; ~365× and ~215× at order 64. Measured end to end over 60 s of
44.1 kHz mono shellac including file I/O — about half a percent of one core on
a modern desktop. See [Where the time goes](#where-the-time-goes-and-what-was-done-about-it).
Those figures predate the [body stage](#the-body-of-dense-crackle-the-body-stage).
Where its key is shut it costs little: ~4% on that clip, measured interleaved.
Where crackle is dense enough to open it, it adds ~20% to the AVX2 time and
~12% to the SSE2 time. On Tanturi's *Cuatro compases* the key is open almost
throughout: the stage changes 98% of the song's 512-sample blocks, against 78%
before the key was [held for 2 s](#the-body-of-dense-crackle-the-body-stage).
There that is 184× → 154× and 97× → 87× realtime, best of seven runs
interleaved. Before the stage's speed-ups it was 185× → 132× and 99× → 83×.
The [fine stage](#the-top-band-of-dense-crackle-the-fine-stage) runs under the
same key. Shut, it costs nothing measurable; open on *Cuatro*, it takes the
same build from 153× to 106× with AVX2 and from 85× to 66× with SSE2. Those
are other runs on a different day, so set them against each other, not
against the figures above. The Mac table below is of the body stage alone;
the one after it has the fine stage on.
The [crackle floor stage](#the-floor-under-dense-crackle-the-crackle-floor-stage-on),
on by default since 2026-10-19, costs ~5% where its key is shut (that clip
178× → 170× with AVX2, *Ahora no me conoces* 191× → 181×) and ~20% where it
is open (*El rodeo* 113× → 94×), best of seven interleaved. None of the
tables below include it.

On the Macs, with the body stage included, the three builds taking turns:

| | M1, NEON | M1, baseline | 2014 MacBook Pro, AVX2 | MacBook Pro, SSE2 |
| --- | --- | --- | --- | --- |
| *Serenata*, 60 s stereo, before soft repair (`5c43b45b9`) | 90× | 32× | 52× | 26× |
| … with soft repair, before the body stage (`072857cd0`) | 85× | 32× | 50× | 25× |
| … with the body stage | **83×** | 31× | **48×** | 24× |
| *Cuatro compases*, 154 s mono 48 kHz, before the body stage | 136× | 54× | 80× | 42× |
| … with the body stage as first committed | 100× | 47× | 54× | 34× |
| … after its [speed-ups](#the-body-of-dense-crackle-the-body-stage) | **116×** | 50× | **67×** | 37× |

That is order 128, best of seven on the M1 and best of nine to eleven on
the MacBook Pro. The M1's runs agreed to its timer's 10 ms. The MacBook Pro
was not always idle, and its medians are up to 25 % lower. With the key open
the stage now adds 17 % to NEON and 7 % to the M1's baseline, and 19 % and
15 % on the Haswell. It added 37 %, 14 %, 47 % and 24 % as first committed.
These Mac figures predate the key's 2 s release, which costs *Cuatro* about
2 % more on the Zen 4 because the key is open on more of it.
With it shut, on *Serenata*, it costs 0–5 % on the M1 at orders 32–256. On
the Haswell a separate fifteen-round run put that at 12 %, 9 % and 6 % at
orders 32, 64 and 128. That is a fixed cost per sample, so it weighs more
the cheaper the core is.

With the fine stage on as well, over the same two clips, before and after
[what came off it](#what-the-fine-stage-costs-and-what-came-off-it):

| | M1, NEON | M1, baseline | 2014 MacBook Pro, AVX2 | MacBook Pro, SSE2 |
| --- | --- | --- | --- | --- |
| *Serenata*, 60 s stereo, as the stage was first committed (`8e3b00387`) | 60.9× | 22.6× | 38.5× | 20.9× |
| … now | **62.1×** | 22.8× | **40.4×** | 22.0× |
| *Cuatro compases*, 154 s mono 48 kHz, as first committed | 79.2× | 35.2× | 47.6× | 29.1× |
| … now | **80.4×** | 35.7× | **51.4×** | 30.5× |

Order 128, best of seven on the M1 and of nine on the MacBook Pro, the two
builds taking turns. These are measured in process, over `push`/`pull` in
1024-frame blocks, so unlike the figures above they leave out the file I/O —
set them against each other rather than against the table before them. The
gain is 7.5 % on the Haswell with AVX2, 4.7 % with SSE2 and 1.8 % on the M1;
at order 64 it is 9.4 % and 4.3 %, because what came off is a fixed cost per
sample and the core it sits beside is cheaper there. The Haswell gains most
because what it was losing was branch mispredictions, which an M1 is far
better at hiding. **Every output is byte-identical** to what the same build
produced before, on both machines and with either kernel set: what changed is
how the work is laid out, not the arithmetic.
Latency ~140 ms (6192 samples at 44.1 kHz: ~70 ms of it the body stage, 35 ms
the [crackle floor stage](#the-floor-under-dense-crackle-the-crackle-floor-stage-on)
and 11.6 ms the [flank repair](#band-limited-transfers-the-flank-repair)'s hold),
reported to foobar2000 so visualisations stay in sync. Note
that `config().latency` is a buffering delay, not a shift: that many samples
must go in before the first block comes out, but the emitted stream is aligned
with the input. The component reads ahead rather than holding the output back —
it feeds every chunk in and emits whatever the pipeline has finished — so what
those 140 ms describe is how far ahead of the listener the decoder is running,
which is exactly what `get_latency()` exists to tell the core. After a track
change or a seek the [warm start](#the-first-seconds-from-the-read-ahead-a-warm-start-foobar2000)
holds up to seven seconds more, and reports those too.

**Nothing is carried from one track to the next.** Every record has its own
surface, and the noise floor the model measures is what every threshold in the
core is relative to, so the pipeline is emptied and reset at each track
boundary: the window stops holding the previous transfer, and so does `m_scale`.
The boundary is noticed from `get_cur_file()` rather than by asking for a track
change mark, because asking force-flushes every DSP placed ahead of this one and
the SDK calls that out as a way to break gapless playback. Reading ahead is what
makes the drain necessary — at the moment the last chunk of a track arrives, its
final `latency` samples are still inside the pipeline, and without draining they
would be emitted into the opening of the next track. The drain is capped at the
samples the pipeline actually owes, so the zeros it is fed to push that tail out
never reach the stream and no silence is spliced into a gapless playlist.

Memory is **750 kB per channel** at 44.1 kHz and 1.5 MB at 192 kHz, fixed at
`configure()` and independent of the parameters — see
[Real-time safety](#real-time-safety) for why it is sized that way, and why
that figure is *lower* than what the same code used to reach at run time.

### Choosing settings

The defaults are tuned on crackly 78 rpm transfers. Measured on whole tracks
(removed signal relative to the music; large clicks left are events above 8σ
in the output):

| Order | Malvon master, removed | clean D'Arienzo, removed | crackly D'Arienzo MP3, large clicks left | *Hotel Victoria* MP3, large clicks left | CPU vs 128 |
| --- | --- | --- | --- | --- | --- |
| 64 | −37.5 dB | −44.5 dB | 1.78/s | 0.52/s | ~0.6× |
| **128** | −38.6 dB | −47.3 dB | 2.08/s | 0.48/s | 1× |
| 192 | −40.7 dB | −47.2 dB | 2.50/s | 0.37/s | ~1.6× |
| 256 | −41.4 dB | −48.5 dB | 2.78/s | 0.42/s | ~2.2× |

* **Crackly 78s** — the defaults.
* **Clean or lightly crackled records, remasters** — Model order 256 and
  Sensitivity 0.7–0.8: the least change to the music. Remasters that clip
  can make *Output only what is removed* pulse with the rhythm; that is the
  clipping and crackle on the loud beats being taken out, not the attacks
  (they measure unchanged, and none was heard missing).
* **Heavily crackled transfers, MP3s especially** — keep order at 128, or try
  64, which catches more of the short clicks.

### Real-time safety

An audio callback must not allocate: `malloc` may take a lock, and a lock held
by a lower-priority thread is a dropout. `declick_rt_verify` replaces global
`operator new` and requires the count to be **zero** across the processing path,
every parameter change, `reset()`, `prime()` and `drain()`, in both access
patterns — one sample at a time as the VST does it, and whole-chunk-then-drain
as foobar2000 does.

Getting there took three fixes, none of which was visible until counted:

* **The output FIFO** was a `std::vector` that grew by `push_back` to 65536
  samples and was then compacted with `erase()`. About a second and a half into
  *every stream* the audio thread took a **720 kB reallocation**. It is now a
  fixed-capacity ring, sized for `maxBlock + 2 * kBlock`.
* **The per-click interpolation scratch** was `assign()`ed to a length that
  varies with the run being repaired, so it grew whenever a longer click turned
  up than any seen so far — a long tail of reallocations that never quite
  stopped. It is now reserved for the worst case the detector can produce.
* **The wrapper rebuilt the pipeline on every control move.** Both fixes above
  are in the core, and the core passed; `dsp_declick` then built *fresh*
  `Channel` objects on every parameter change and configured those — **38
  allocations and 4.3 MB**, one block of it 1.8 MB, inside `on_chunk()`. The
  constructor made it worse by configuring itself at 44.1 kHz defaults first,
  so each Channel sized an envelope its caller was about to replace. It now
  reuses the channels it has: `retune()` where the pipeline keeps its shape,
  `configure()` on those same objects where it does not. Neither allocates.
  Nothing caught this because every case in the test drove a `Channel`
  directly, which is the shape the test now also covers.

The buffers are sized from `Config`'s `buf*` envelope, which is derived from the
**sample rate alone** — `bufOrder` is `kMaxOrder` and `bufMaxRun` is
`Params::sanitize()`'s 20 ms ceiling. So a parameter change that resizes the
pipeline still resets it, but `configure()` reassigns every vector to the length
it already has, and neither `assign()` nor `resize()` reallocates below the
capacity it is holding. Only a sample rate change touches the heap.

Sizing for the envelope rather than the current settings sounds like it should
cost memory, and it does not:

| | after `configure()` | peak during playback |
| --- | --- | --- |
| before | 100 kB | **858 kB** |
| now | **750 kB** | 750 kB |

The old code simply reached its footprint later, on the audio thread, one
reallocation at a time. Two things keep the new figure down: the ring is 139 kB
where the grown FIFO reached 720 kB, and the Wiener buffers — 455 kB of it, and
the largest single item — were **not reserved at all** while the Wiener path
was off, as it always was (it has since been removed).

The refactor is bit-exact: 8 parameter/sample-rate combinations × 3 block
patterns × Wiener off and on, hashed before and after, identical.

---

## Ground truth

A **clean master transfer of the same performance** was compared to a crackly copy.

The two cannot be compared sample for sample — they are different pressings.
(For the pair used here the local waveform correlation is 0.32, though the
speed difference is a clean 328 ppm with only 8 samples of residual, which
confirms it is the same 1943 recording.) So instead:

1. Harvest real click waveforms from the crackly copy — 40 000 of them, median
   6 samples long.
2. Inject them into the clean master **at known positions**.

That gives exact ground truth: the true clean signal and the exact damaged
samples are both known, so detection and reconstruction can be scored directly
instead of by proxy.

* **Detection is not the bottleneck.** 81 % of injected clicks are found, and
  those hold **98 % of the injected energy**. Clicks above −26 dBFS: 100 %.
  Stronger than that — see [The threshold is a selector](#the-threshold-is-a-selector)
  — *perfect* detection is actively **worse** than the real detector.
* **Reconstruction is.** AR interpolation manages ~30 dB SNR over a 3-sample
  hole, 22 dB over 6, and only 8 dB over 16 — while the click itself sits about
  18 dB below the music. Past roughly 6 samples, replacing the samples was
  *worse than leaving the click alone*.

### The threshold is a selector

Replacing the detector with the true mask — perfect detection, identical repair —
makes the result **worse**:

| | repair dB | harm dB | net dB | events/s |
| --- | --- | --- | --- | --- |
| the real detector, default | 1.79 | −42.4 | +0.60 | 28 |
| perfect detection, all 3600 clicks | 0.62 | −∞ | +0.62 | 37 |
| perfect detection, top 50 % by size | 1.59 | −∞ | +1.59 | 42 |
| **perfect detection, top 25 % by size** | **2.07** | −∞ | **+2.07** | 52 |
| perfect detection, top 5 % by size | 1.59 | −∞ | +1.59 | 66 |

Repairing *fewer* clicks is three times better than repairing all of them, and
the reason is arithmetic. The median injected click is only **−20.8 dB**
relative to the local signal, while reconstruction SNR is 11.9 dB — so for a
typical click the interpolation error is about **9 dB larger than the click it
replaces**. Only the loudest fifth are worth touching at all.

So the sensitivity threshold is not merely *finding* clicks. It is **selecting
which repairs are worth making**, and it is doing that job well: the real
detector at its default reaches +0.60 dB while leaving only 28 events/s, where
the selective oracle buys +1.47 dB more fidelity at the cost of nearly doubling
the residual crackle. Both columns matter to a listener, so those are not
directly comparable — but it does mean there is at most ~1.5 dB available to any
better *ranking* of clicks, and none at all to merely finding more of them.

This is the single most useful thing the ground truth has produced, because it
bounds a whole family of proposals at once. Wavelet or STFT sub-band detection,
kurtosis or skew thresholds on sub-band energy, Bayesian inference, Markov
random fields, HMM burst models — every one of these is a way of deciding
*which samples are damaged*, so every one of them is capped by the table above.
Two further notes on that family:

* The AR prediction residual is already a **signal-adaptive whitening
  transform**, which is what makes small broadband impulses stand out against
  coloured music. A wavelet high-band is a *fixed* filter bank, so it is not
  obviously a better place to look.
* The hysteresis in `interpolate()` is already a two-state sticky chain — high
  threshold to enter, low threshold to stay, which is what **Extent** tunes. An
  HMM would be the principled version of something that is present in crude
  form, not a missing capability.
* The premise that dense crackle turns the signal into "a sequence of missing
  data gaps" does not hold at real densities. Genuine 78 rpm transfers measure
  1–2 % contaminated; injecting 1800 clicks/s only reached **5.2 %**. At 95 %
  intact this is not an inpainting problem.

### Repair depth

That last point produced the one real algorithmic change to come out of this.
A click **adds** to the music, it does not erase it, so the damaged samples
still carry the signal underneath; replacing them outright throws that away.
So the repair is **subtractive**: what gets removed is the discrepancy
`d = x − v` between the sample and the model's estimate, not the sample.

| gap length | full replacement | partial subtraction |
| --- | --- | --- |
| 1–3 samples | +7.7 dB | +8.0 dB |
| 4–6 | +3.6 dB | +5.0 dB |
| 7–10 | **−2.1 dB** | +1.6 dB |
| 11–15 | **−6.4 dB** | +0.7 dB |
| 24+ | **−10.0 dB** | ~0 dB |

With perfect detection, full replacement scored **−6.9 dB** — actively harmful.
Subtracting a fixed **0.45** of the discrepancy is what depth 0 does. The **Repair
depth** control scales from there towards outright replacement:

| depth | click reduction | collateral harm | whole-file error | events/s left |
| --- | --- | --- | --- | --- |
| **0.00** (then the default) | 1.79 dB | −42.4 dB | **+0.60 dB** | 28 |
| 0.25 | 2.00 dB | −40.1 dB | +0.07 dB | 21 |
| 0.50 | 1.98 dB | −38.2 dB | −0.70 dB | 17 |
| 1.00 (full replacement) | 1.29 dB | −35.4 dB | −2.52 dB | 15 |

Two things worth noting. Full replacement is worse at *removing clicks* than a
partial subtraction is (1.29 dB against 2.00 dB) — its own interpolation error
partly undoes the repair. And the clean master itself measures 27 impulsive
events per second; the default lands at 28, essentially back at that natural
level, while higher depths drive it *below* the master, which means they are
removing real musical transients. Hence what was, until the dense-crackle
measurements below, the conservative default. It is now 1: whole-file error
turned out to be the wrong yardstick for what a listener hears — see
[Dense crackle](#dense-crackle). Lower it if you
would rather leave more crackle in exchange for less added error.

#### What did not work: per-sample Wiener weighting

The subtraction fraction started life as a curve indexed by gap length (0.75
up to 6 samples, 0.25 out to 23, nothing beyond), fitted by hand to the
per-length optima above. Replacing that with something derived rather than
tabulated looked compelling: the least-squares interpolation solves `G u = −b`,
so under the model's Gaussian innovation the posterior covariance of the repair
is `σ²·G⁻¹`, and the solve has already produced the Cholesky factor of `G`.
One extra banded recursion (Takahashi's, `O(n·band²)`) yields `diag(G⁻¹)` —
the per-sample uncertainty — and the MMSE fraction to subtract is then the
Wiener gain `1 − P/d²`. That should taper the correction towards the middle of
a long run, where the estimate is worst, which a per-run constant cannot
express at all.

It was implemented, verified exact against a dense inverse (1.6e-15), and
measured. **It does not pay on this material.** Against the flat fraction at
the same cap it was worth +0.05 dB of whole-file error and made the residual
event rate *worse* by 4–7 events/s. The reason is visible in the formula: the
click is nearly always far larger than the estimate's own uncertainty, so
`d² ≫ P`, the gain saturates at 1, and only the cap ever binds. Inflating `P`
by 4× does make it gate marginal detections — collateral harm improves by up
to 2.2 dB — but that trade is strictly worse than simply lowering sensitivity,
which reaches the same fidelity with 13 fewer events/s left behind.

What *did* pay was the thing the experiment surfaced along the way: the
length-indexed curve was the problem, not the lack of a variance term. A flat
fraction beats it on every axis of both datasets, mostly because the curve gave
up entirely past 28 samples and was too timid between 7 and 23.

| | click reduction | collateral harm | whole-file error | events/s |
| --- | --- | --- | --- | --- |
| old length-indexed curve | 1.37 dB | −41.5 dB | +0.07 dB | 31 |
| **flat 0.45** (depth 0) | **1.79 dB** | **−42.4 dB** | **+0.60 dB** | **28** |

The machinery stayed in the tree, off, for a while, and has since been removed;
the git history has it, should material with different click statistics —
louder clicks on stereo vinyl, say — want it tried again.

#### What did not work: robust (M-estimation) AR fitting

The standard advice for AR declicking is to fit the model iteratively,
downweighting the equations where the prediction error is largest, so the fit
can "see through" the crackle instead of being dragged towards it. The fit here
does something much cruder — a single amplitude clip at 6× mean `|x|` — so this
looked like an obvious gap. It is not, and the reason is worth recording.

The ceiling was measured before implementing anything: hold detection perfect,
keep the interpolation identical, and change **only** where the coefficients
come from. Fitting on the *true clean master* is what no robust estimator can
beat.

| gap | shipped | no clip at all | IRLS Huber | IRLS reject | **oracle (clean)** |
| --- | --- | --- | --- | --- | --- |
| 1–3 | 30.6 | 30.6 | 30.6 | 30.5 | 31.3 |
| 4–6 | 26.5 | 26.5 | 26.2 | 26.1 | 26.7 |
| 7–10 | 19.5 | 19.5 | 19.2 | 19.1 | 19.9 |
| 16–23 | 9.9 | 9.9 | 9.8 | 9.7 | 10.2 |
| **all** | **11.9** | 11.9 | 11.7 | 11.7 | **12.2** |

A perfect model is worth **+0.28 dB**. Every IRLS variant tried — Huber and
Tukey weights, hard rejection, 3 iterations, cut-offs from 1σ to 4σ — came out
*behind* the shipped fit, by 0.16 to 0.33 dB.

Since the objection to that is "your ground truth is not crackly enough", the
density was raised until it was, using 31 920 real click waveforms harvested
from a heavily crackled 1938 D'Arienzo transfer:

| clicks/s | contamination | shipped | IRLS Huber | oracle | headroom |
| --- | --- | --- | --- | --- | --- |
| 60 | 0.7 % | 16.87 | 16.51 | 16.76 | −0.11 dB |
| 180 | 2.1 % | 19.36 | 18.52 | 19.60 | +0.24 dB |
| 600 | 4.7 % | 18.82 | 18.66 | 18.65 | −0.17 dB |
| 1800 | 5.2 % | 17.78 | 17.60 | 18.08 | +0.30 dB |

The headroom never clears +0.30 dB, at times it is negative (noise around
zero), and IRLS is behind at **every** density. Three reasons, all measurable:

* **The clicks are tiny, not large.** Median peak −39 dBFS on the crackly
  transfer; the injected set has median amplitude −50.8 dBFS. The shipped clip
  threshold sits at −8.5 dBFS, so **0.0 %** of damaged samples ever reach it —
  the existing "protection" is inert, which is also why *no clip at all*
  scores identically.
* **Contamination is too sparse to bias a sum over a thousand lag products.**
  Even at 5 % it barely moves the autocorrelation.
* **IRLS actively hurts because music is unpredictable too.** A high residual
  means "surprising", and piano attacks and bandoneón accents are surprising.
  Downweighting them biases the model towards the smooth part of the spectrum —
  precisely the wrong direction for reconstructing transients.

The published advice is sound; it is aimed at a different regime. Vinyl ticks
and scratches are *loud* outliers, tens of dB above the noise floor and
sometimes clipping, and there M-estimation earns its keep. Shellac crackle is
the opposite: dense but minuscule. It corrupts the samples it hits badly and
the model estimate hardly at all.

The practical conclusion is that **the AR coefficients are not the bottleneck**
and never were. Reconstruction is limited by how much information the
surrounding samples carry about a hole, not by the accuracy of the model
describing them. That is also why more model order and more context bought so
little.

The defaults then were conservative. On the same four transfers (originals at 71 events/s):

| | events/s | collateral damage | HF change |
| --- | --- | --- | --- |
| then the default (s 0.60, depth 0) | 21 | −46.5 dB | −0.8 dB |
| s 0.60, depth 0.50 | 16 | −42.4 dB | −1.2 dB |
| s 0.80, depth 0.50 | 13 | −39.8 dB | −1.7 dB |
| s 0.80, depth 1.00 | 12 | −37.0 dB | −2.0 dB |

The ground-truth numbers above say that trade is net-negative in fidelity terms;
whether it is net-positive to your ears is a listening question, not a measurement one.

### Does more CPU help?

**Yes — model order is the one lever that clearly pays.** An earlier revision of
this section said the opposite; it was wrong, and the correction is instructive.
The reconstruction figures it quoted were right, but judging them per gap length
hid what they do end to end.

Reconstruction SNR (dB) by gap length, detection held perfect and the
interpolator identical, changing only the coefficients:

| model | 1–3 | 4–6 | 7–10 | 11–15 | 16–23 | 24–40 | all |
| --- | --- | --- | --- | --- | --- | --- | --- |
| order 32, ctx ±96 | 30.6 | 26.5 | 19.5 | 13.0 | 9.9 | 6.5 | **11.9** |
| order 32, ctx ±528 | 30.6 | 26.5 | 19.5 | 13.0 | 9.9 | 6.5 | **11.9** |
| order 128, ctx ±528 | 31.2 | 27.2 | 21.8 | 16.3 | 12.7 | 9.9 | **15.0** |
| order 256, ctx ±768 | 30.3 | 27.7 | 23.5 | 19.2 | 15.7 | 14.1 | **18.3** |

Context alone is worth **+0.00 dB** — the identical first two rows are not a
copy-paste error. Order is worth up to **+6.4 dB**.

And it survives end to end. With the *real* detector's own verdict and only the
model changed:

| | repair dB | harm dB | net dB | events/s |
| --- | --- | --- | --- | --- |
| order 32, w 0.45 | 1.79 | −42.4 | +0.60 | 28 |
| order 64 (then shipped), w 0.45 | 2.47 | −41.2 | +0.76 | 28 |
| order 128, w 0.45 | 2.87 | −42.1 | +1.31 | 29 |
| **order 256, w 0.60** | **4.34** | **−42.8** | **+2.55** | **25** |

Order 256 is better on **all four axes at once**, and note that its best
subtraction fraction rises from 0.45 to 0.60 — exactly what the break-even
argument predicts, because a better estimate makes more of each click worth
removing.

Two things that do *not* need changing: the fit window and the context. The
window the component already has (`2*(maxRun + 3*order) + 512`) beats a fixed
4096 at every order, by 0.25–0.38 dB — music is not stationary, so a tighter
window tracks the local spectrum better. Raising **Model order** is sufficient
on its own.

#### The cost

Measured end to end over 60 s of 44.1 kHz mono shellac including file I/O, at
the defaults apart from the order (sensitivity 0.8, depth 1, two passes,
re-repair):

| order | x64, AVX2 | x64, SSE2 | x86, AVX2 | x86, SSE2 | before the per-click fix, x64 SSE2 | 2014 MacBook Pro, stereo, AVX2 / SSE2 | M1, stereo, NEON / baseline |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 32 | 264× | 195× | 218× | 178× | 175× | 57× / 36× | 171× / 85× |
| 64 | 206× | 130× | 159× | 117× | 101× | 41× / 24× | 109× / 46× |
| **128** (default) | **120×** | 67× | 87× | 60× | 44× | **24×** / 13× | **54×** / 20× |
| 256 | 52× | 27× | 36× | 24× | 15× | 9.3× / 4.8× | 20× / 7.2× |

The kernels are picked when the module loads, so the same binary runs the SSE2
columns on a CPU without AVX2 and FMA. See
[Where the time goes](#where-the-time-goes-and-what-was-done-about-it).

The same, at the defaults of the time — sensitivity 0.6, depth 0, no
re-repair — and before and after the optimisation below:

| order | x64 | x86 | before optimisation | 2014 MacBook Air, stereo (estimated) |
| --- | --- | --- | --- | --- |
| **32** (then the default) | **181×** | 156× | 84× | ~25× |
| 64 | 110× | 99× | 39× | ~15× |
| 128 | **50×** | 45× | 16× | ~7× |
| 256 | 18× | 15× | 5× | ~2.5× |

The MacBook Pro column of the first table is measured, on a 2.4 GHz Haswell
i5-4258U (MacBookPro11,1): best of seven runs, on a different 60 s of shellac
from the other columns, with the machine otherwise in light use. Mono, it
runs 120×, 87×, 48× and 19× with AVX2 at orders 32 to 256, and 77×, 52×, 25×
and 9.5× without — about 2.5× slower than the Zen 4 above, allowing for the
different clip. Its gain from AVX2, 1.9× at the defaults, is no smaller than
the Zen 4's, although its FMA takes 5 cycles against 4. The 2014 MacBook Air
has the same generation of Haswell, an i5-4260U, at 1.4 GHz base and 2.7 GHz
turbo against 2.4 and 2.9, so expect it somewhat below that column.

The M1 column is measured too, on an M1 Mac mini: the arm64 half of the
universal Release build, best of seven runs, on a third 60 s of shellac —
Canaro's *Serenata* (1934), stereo. Runs agreed to within 3 %. The baseline
figures are with `--no-neon`. Like the rest of the table it is the build
before the round [below](#where-the-time-goes-and-what-was-done-about-it),
which took the M1 to 90× at the defaults. NEON gains 2.6× at the defaults,
more than AVX2 does on either x64 machine. Order costs it a little more than
it costs the Zen 4: 256 takes 2.7× as long as 128 there against 2.3×, and
2.8× against 2.5× with the baseline kernels.

The last column of the second table is an estimate, not a measurement: the
i5-4260U taken as roughly 3–4× slower per core than the machine above, and
stereo doubling the work. After the optimisation below, **order 128 is viable
on that machine** — it was not before.

`kMaxOrder` is 256 and **the default is 128**. It used to be 64, chosen as the
point where paying for the one lever that clearly pays was still cheap, with
128 left as the user's call. It became the default once the target was set as
the best result real-time playback can afford: the slowest machine this is
played on is a 2014 MacBook Air, and the MacBook Pro of the same Haswell
generation runs 128 at 24× realtime stereo. See [Dense crackle](#dense-crackle).
Offline nothing has a deadline at all, and 256 is there for it.

#### Where the time goes, and what was done about it

At the defaults of the time — sensitivity 0.6, depth 0, no re-repair —
profiling by the difference between a clean file and a crackly one put the
per-click interpolation at only **2 % of the run time at order 32 and 7 % at
order 256**, with almost everything else in the model fit and the two
prediction-residual passes. So an earlier note here, suggesting the per-click
residual be shared across clicks and patched per gap, was aimed at the wrong
5 % and was dropped.

The [dense-crackle](#dense-crackle) defaults changed that. Sensitivity 0.8
repairs about 4 % of samples, and re-repair solves every first-pass run twice,
so at order 128 the repairs grew to **44 %** of the run time — measured against
the same 60 s at sensitivity 0 with re-repair off, which repairs 0.08 %. The
per-click fix below brings that back down to **16 %**.

What did pay, for **2.2× at order 32 rising to 3.6× at order 256**:

* **The Hann window was recomputed every block**, at one `cos()` per sample of
  the fit window — about nine `cos()` calls per output sample at order 256. The
  fit always spans the whole sliding window, so the taper is identical every
  time; it is now built once in `configure()`. Bit-exact, since it is the same
  values from the same calls.
* **Every hot loop was a serial accumulator.** `for (k) s += a[k]*x[i-k]` is a
  chain of dependent additions, so it ran at one multiply-add per add latency —
  about four cycles — no matter what the machine could sustain. All six of them
  (signal autocorrelation, forward and backward residual, interpolation
  residual, its right-hand side, coefficient autocorrelation) now go through
  one `dot()` kernel with four independent accumulators and SSE2. Breaking the
  dependency chain is the larger half of the win; the vector width is the
  smaller.
  The forward and interpolation residuals walk the window backwards, so
  `fitModel()` also keeps `m_a` reversed in `m_arev`, which turns them into
  plain inner products.

Summation order therefore differs from a plain serial loop. The measured
consequence, over 60 s of a real transfer: **99.9965 % of output samples
bit-identical**, worst deviation **−156.5 dBFS** (an eighth of a 24-bit LSB),
rms deviation **193 dB below the signal**. Every ground-truth and
reference-transfer figure in this README is unchanged to the last digit printed.

Later, once the dense-crackle defaults had made the repairs expensive:

* **The per-click residual filtered rows the solve never reads.**
  `repairRun()` computed the prediction error over the whole repair window,
  `m + 5·order` rows, but only the `m + order` rows that touch the run enter
  the normal equations — every other row is a constant in the least-squares
  objective. At order 128 that was some 80k multiply-adds per run thrown away,
  twice over under re-repair. It now filters only the rows the right-hand side
  reads; the coefficient autocorrelation, which depends on the model alone, is
  built once per fit instead of once per run; and the second pass no longer
  filters the margin below the block, which only the first pass's noise
  estimate reads. **1.5× at the defaults** (44× → 67× realtime), 1.3× at
  order 64 and 1.8× at order 256, and bit-exact: the same products of the same
  inputs, checked byte for byte on twelve renders across order, depth and
  re-repair mode.
* **AVX2 and FMA, picked at run time.** Every hot loop but the fit is a filter,
  `out[i] = sum_k h[k] * x[i+k]`, and the fit's autocorrelation becomes one too
  once the window is followed by `order` zeros: the window filtered by itself.
  The AVX2 filter multiplies one broadcast tap into four consecutive samples
  at a time, with eight such accumulators side by side. Each multiply-add then
  needs about one load instead of `dot()`'s two, and eight chains cover the
  FMA latency. **1.8× at the defaults on x64** (67× → 120× realtime), 1.6× at
  order 64 and 1.9× at order 256. x86 gets 1.45×: it has only eight vector
  registers, so it runs four chains rather than eight. A 2014 Haswell MacBook
  Pro gains as much, 1.9× at the defaults (25× → 48× realtime, mono).
  CPUID and XGETBV pick the kernels once, when the module loads. Without AVX2
  and FMA the SSE2 code runs unchanged, bit for bit. The builds stay at
  `/arch:SSE2` (see the note under `check_win7.ps1`). That has a cost: without
  `/arch:AVX2`, MSVC encodes the intrinsics as AVX but plain `double`
  arithmetic as legacy SSE, and Haswell charges about 70 cycles for each
  switch between the two. So the kernels do every floating-point operation
  with intrinsics, down to the masked tail loads and the stores, and end with
  `vzeroupper`. GCC and clang get the same code through
  `__attribute__((target("avx2,fma")))`, which covers the Linux VST and the
  Intel half of the Mac builds; Apple silicon has its own path.
  The AVX2 output is **not bit-identical** to SSE2. FMA rounds once where a
  multiply and an add round twice, and the sums are split differently. Over
  60 s of shellac, **99.9986 %** of output samples are identical, the worst
  deviation is **−156.5 dBFS**, and the rms deviation is **191 dB below the
  signal**. Across the click probes and the dense-crackle benches, the worst
  is −144.5 dBFS, at order 256 with re-repair 2. Not one detection decision
  changed. `declick_verify` holds the two kernels within 1e-9 of each other
  at every order, and `declick_cli --no-avx2` runs the baseline for
  comparisons.
* **NEON on AArch64.** The same filter, two doubles per register instead of
  four. There is nothing to detect: NEON with double lanes and FMA is part of
  AArch64, so every Apple silicon Mac and every ARM64 Linux machine runs it.
  The baseline there was scalar `dot()`, one multiply-add per instruction.
  AArch64 has 32 vector registers, so the kernel keeps sixteen accumulators
  side by side, as many as an Apple core's four FMA pipes at four cycles'
  latency need. The taps come in two at a time and are used from their lanes
  by `fmla`-by-element, so one load of `h` feeds thirty-two multiply-adds,
  each with its one load of `x`, and nothing is spilled. **2.4× at the
  defaults on an M1** (36× → 88× realtime), and the gain grows with the order,
  as there is more filtering to do:

  | order | M1 Mac mini, NEON | M1, scalar baseline | gain |
  | --- | --- | --- | --- |
  | 32 | 280× | 159× | 1.8× |
  | 64 | 178× | 87× | 2.0× |
  | **128** (default) | **88×** | 36× | 2.4× |
  | 256 | 33× | 12× | 2.6× |

  Mono, best of seven runs, on the same 60 s of shellac as the 2014 MacBook Pro
  column above, not the clip of the x64 columns. The scalar baseline was the
  weakest of all the machines here: about half the Zen 4's SSE2 at the
  defaults, allowing for the different clip.
  Like AVX2, NEON differs from the baseline in the last bits only: at the
  defaults 99.992 % of samples are identical, the worst deviation is
  −144.5 dBFS and the rms deviation 185 dB below the signal, and as many
  samples are repaired at orders 64, 128 and 256. `declick_verify` holds
  the two within 1e-9 on the M1, and `declick_cli --no-neon` runs the baseline.

Later still, a round that took the defaults from 134× to 217× realtime on
x64 with AVX2 (74× → 116× without), 1.5× at order 64 and 1.85× at order 256.
Profiled by cycle counter first: the fit was 45 % of the run time, detection
36 % and the repairs 19 %, and three things inside those stood out.

| order | x64, AVX2, before | after | x64, SSE2, before | after |
| --- | --- | --- | --- | --- |
| 32 | 320× | 500× | 245× | 334× |
| 64 | 245× | 367× | 151× | 216× |
| **128** (default) | 134× | **217×** | 74× | 116× |
| 256 | 55× | 102× | 29× | 52× |

Best of seven (five for SSE2), the same 60 s clip and machine as the first
table; the "before" columns are a fresh run of the previous build, which
reads a little above that table on the day.

The same round on the 2014 MacBook Pro and the M1, stereo, best of seven on
the *Serenata* clip of the M1 column above, the two builds run alternately:

| order | MacBook Pro, AVX2, before | after | M1, NEON, before | after |
| --- | --- | --- | --- | --- |
| 32 | 64× | 81× | 171× | 261× |
| 64 | 45× | 66× | 109× | 167× |
| **128** (default) | 25.5× | **42×** | 54× | **90×** |
| 256 | 10.5× | 20× | 20× | 37× |

1.64× at the defaults on Haswell and 1.67× on the M1, against the Zen 4's
1.6×; at order 256, 1.9× on both. The MacBook Pro was not idle (load average
around 4), so its absolute figures are good to perhaps 10 %; the ratios less
so affected, since the two builds took turns.

* **The noise estimate's median** was `nth_element` over the whole 30 ms ring,
  1323 values every block: 14 % of the run time. The median moves little from
  block to block, so one branch-free pass now keeps only the values within
  ±12.5 % of the last one and counts those below, and `nth_element` sorts out
  that tenth. If the median has left the band, a second pass keeps the side it
  went to. An order statistic is the same however it is found, so this is
  **bit-exact**.
* **Levinson-Durbin** was 17 %: its inner product walks the autocorrelation
  backwards, as a serial chain of dependent additions. With the
  autocorrelation reversed it goes through `dot()`, and the symmetric
  coefficient update runs two from each end at a time in SSE2 (that half is
  bit-exact).
* **Every pass after the first refits the same window**, which differs from
  the last fit's only where that pass repaired it — a few dozen samples of
  some 1600. `r'[d] = r[d] + Σ δ[p]·(w'[p+d] + w[p−d])` over the changed
  samples is two short filters per stretch of changes instead of the whole
  autocorrelation again.
* **The per-click right-hand side was built the long way.** It is `−AᵀA`
  applied to the window with the run zeroed, and was computed exactly so: the
  prediction error of the `m + order` rows touching the run, multiplied back
  through the model — about 17 000 multiply-adds for a 4-sample click at
  order 128. But `AᵀA` is the Toeplitz matrix of the coefficients'
  autocorrelation, which the solve already builds once per fit, so each
  sample's right-hand side is a sum of `aa[|t − c|]·x[t]` over the known
  samples within `order` of it: `2·order − m + 1` terms, an eighth of the work
  for a typical click. The repairs went from 19 % of the run time to 11 %. The
  coefficient autocorrelation now goes through the wide kernel as well, and
  the scan for each repair's local range is vectorised (bit-exact: a maximum
  is a maximum).
* **The AVX2 filter takes its taps in groups of four**, a stride of four apart,
  so each unaligned load of `x` feeds up to four FMAs: 11 loads per 32
  multiply-adds instead of 36. Worth only 2–3 % on the Zen 4, which has three
  load ports; it is aimed at Haswell, which has two and pays double for a load
  that straddles a cache line. There it is worth **5–11 %** end to end at
  orders 64 to 256, growing with the order (best of fifteen, mono, against the
  same build taking one tap at a time; 10–13 % by the median), so it stays.
  The two differ in 0.004 % of output samples, by at most −156 dBFS.

Apart from the median, the local-range scan and the Levinson update, these
change the sums' order, so the output moves in the last bits — as the AVX2
kernels did. On the 60 s benchmark clip 99.9997 % of samples are identical,
the worst deviation is −162.6 dBFS and the rms deviation 205 dB below the
signal; on the four reference transfers (Cachirulo and La tablada, hum and
clean), the worst is −144.5 dBFS with either kernel, and every one repairs
exactly as many samples as before.

What is left is the first pass's autocorrelation and the two prediction
residuals, both close to the FMA units' peak on the Zen 4, and Levinson, now
bound by the latency of its recursion rather than by its arithmetic.

One idea that is now *less* attractive than it looked: replacing the fit
autocorrelation with an FFT. Direct is `O(n × order)` against `O(n log n)`, but
at n = 2400 and order 256 the two land within about 10 % of each other once the
direct loop is vectorised — and the FFT would mean hand-rolling one, since the
component links nothing but COMCTL32, shared, KERNEL32 and USER32. Not worth
it.

#### What the data no longer supports

* **Long-term (pitch) prediction** was implemented and measured: **+1.53 dB**
  overall, +2.46 dB at 24–40 samples, and it never hurt. But a plain dense
  order-128 model gives **+3.07 dB at half the cost**, and beats LTP in every
  gap bucket. The reason is that an AR model of order ≥ T already represents
  periodicity — the median pitch lag selected was 225 samples, so an order-256
  model spans a whole period densely and does LTP's job more thoroughly. LTP is
  the *cheap* approximation, which is why it belongs in speech codecs, where a
  256-tap filter is unaffordable. Here it is not.
* **Sinusoidal modelling** for tonal passages. Still unimplemented, and the
  case for it is weaker than it looks: runs longer than 20 samples are 6.4 % of
  runs but **46.4 %** of the damage energy, so the long tail does matter — but
  order 256 already buys **+7.55 dB** there for far less engineering.

### Dense crackle

Everything above was measured against clicks this detector had itself
harvested, and scored by whole-file error. Both choices turned out to hide the
same thing: **dense, low-level crackle**, and how much of each click a repair
leaves behind.

**What it looks like.** Shellac crackle is a carpet of small impulses at
250–300 per second — often one per cycle of the music — plus some top-octave
hiss. The old defaults touched ~2 % of samples and left most of it in place.

**Ground truth.** Real crackle of that kind, gated around its impulsive peaks,
was injected into three 78-era excerpts — the Malvon master and two shellac
transfers — so the crackle is real but its positions are known. Modern
recordings were tried as the clean base and rejected: a peak-limited modern
master is *spikier* in the AR residual than the crackle is. Scored in the
whitened domain, by event size, alongside click probes of three shapes at
−66…−24 dBFS on a tone and on the Malvon master.

| | dense crackle removed, all / largest | harm | clicks in music, imp / tick / thump | change to clean music |
| --- | --- | --- | --- | --- |
| old defaults (s 0.6, depth 0, order 64) | −2.5 / −3.2 dB | −18.4 | −4.5 / −4.6 / −3.0 dB | −43.0 dB |
| s 0.8, depth 1, order 128, re-repair | −6.1 / −8.9 dB | −13.9 | −9.7 / −9.8 / −4.3 dB | −34.9 dB |
| the same at order 64 | −6.3 / −9.6 dB | −13.4 | −10.0 / −9.2 / −4.2 dB | −33.5 dB |
| the same at s 0.7 | −5.6 / −8.7 dB | −15.0 | −11.0 / −10.8 / −4.8 dB | −35.6 dB |
| plus dip 1 / 0.5, on every run | −6.8 / −10.4 dB | −13.7 | −8.4 / −8.1 / −3.8 dB | −31.8 dB |
| dip only on runs peaking at 6σ | −6.4 / −9.8 dB | −13.9 | −9.5 / −8.0 / −3.7 dB | −34.6 dB |
| the same at order 64 | −6.6 / −10.3 dB | −13.4 | −9.9 / −9.1 / −1.6 dB | −32.3 dB |
| **current defaults** (s 0.9, weak runs half repaired) | **−6.2 / −9.5 dB** | −15.5 | −11.2 / −9.5 / −3.9 dB | −36.2 dB |
| current, order 64 | −6.5 / −10.1 dB | −15.0 | −11.5 / −9.5 / −1.9 dB | −33.2 dB |

Lower is better everywhere; the last column is how much each changes music
with no clicks injected at all, and that excerpt has a little crackle of its
own.

What changed, and why:

* **Repair depth 1.** The 45 % subtraction was the right answer to the
  question it was asked — least whole-file error — but whole-file error weighs
  a smooth interpolation error the same as an impulsive residual, and the ear
  does not. It also capped every repair at −5.2 dB however good the estimate:
  a −24 dBFS click on a pure tone came out 5 dB quieter.
* **Re-repair** (`Config::rerepair`, 1). The second pass already refitted the
  model on the window the first had cleaned, but never revisited what the
  first had repaired with its crackle-contaminated model. Now it restores
  those runs from the dry signal and repairs them again. On the largest
  crackle events that is −6.8 → −8.9 dB for ~1 dB of harm and 9 % more CPU
  (25 % until the per-click residual was narrowed — see
  [Where the time goes](#where-the-time-goes-and-what-was-done-about-it));
  mode 2, a further refit after the last pass, adds 0.6 dB for a further 27 %.
* **Sensitivity 0.8, order 128.** By the numbers 0.7 is marginally ahead — it
  gives up 0.5 dB of dense crackle removal for ~1 dB less harm — but 0.8 won
  a listening comparison on three full shellac transfers, with no music
  audible in what it removes, and the ear is the measure the numbers are
  standing in for. 128 over 64 is worth ~1 dB on the probes and ~1.5 dB less
  harm.
* **Hysteresis that steps over a dip** (`Config::dip` 1, `dipFar` 0.5). Found
  on MP3 transfers with a cleaner copy of the same record to compare against
  (Canaro, *Hotel Victoria*): the clicks left behind had all been detected,
  but repaired over too short a run. A click's prediction error rings, and on
  material that is empty above 16 kHz it swings through zero inside the
  click; where it dipped under the floor the run ended there, and the repair
  leaned on damaged samples just past its edge — leaving the click, sometimes
  rebuilding it larger. A run may now step over one sample below the floor if
  the detection function is back above it, halfway to the trigger, on the far
  side; if that makes a run too long to repair, the plain run is repaired
  instead. Clicks left above 8σ on that transfer: 1.1/s → 0.48/s (its clean copy
  measures 0.2/s). On the benchmark it costs 1.1 dB of whole-file SNR, and
  some of the probe removal above; no CPU to speak of. Accepting any return
  above the floor went further, to 0.3/s, but audibly took more of the
  attacks on a clean D'Arienzo transfer (−30.9 dB of change against −35.6,
  and −46.4 with no dip at all); the halfway rule was heard to take no more
  attack than none.
* **…but only for runs that trigger hard** (`Config::dipPeak` 6σ). Most of
  the runs the dip stretched had barely crossed the trigger — noise alone
  crosses 3.2σ about 100 times a second — and on clean material they carried
  most of what the dip removed: two thirds of it on the Malvon master. With
  the dip kept to runs peaking at 6σ or more, the Malvon master goes back
  from −33.6 to −36.4 dB removed (−36.8 with no dip) and the clean D'Arienzo
  from −35.6 to −45.5 (−46.4), while a crackly D'Arienzo MP3 keeps 85 % of
  what the dip gained on clicks above 8σ, and *Hotel Victoria* most of it
  (1.12/s with no dip, 0.48/s on every run, 0.60/s at 6σ on this excerpt).
  On the benchmark harm, SNR and clean-music change return almost to where
  they were without the dip. Preferred by ear over 5σ and over no limit.
* **Weak runs repaired by half, and Sensitivity 0.9** (`Config::softMin`
  0.5, `softSpan` 3.5). Even after the dip limit, runs peaking under 6σ
  carried most of what the Malvon master lost — 210 a second under 4σ,
  at the rate noise alone crosses the trigger. No feature of a single run
  told those apart from small real clicks: not the run's length, nor how
  many samples crossed, nor whether the forward and backward errors agreed,
  nor a local noise estimate (it matched the 30 ms one around every run).
  What does pay is to stop betting everything on them: a run at the trigger
  now has half its discrepancy taken out, rising to all of it 3.5σ higher,
  and the trigger drops to 2.85σ so the small clicks are still reached.
  Malvon master −36.4 → −38.6 dB removed, clean D'Arienzo −45.5 → −47.3,
  while the crackly D'Arienzo MP3 keeps 2.08/s clicks above 8σ against
  1.93. Lowering Sensitivity to 0.7 instead gets −37.2 dB for 2.28/s — the
  soft repair buys two to three times as much per click left. On the
  benchmark the small crackle comes out as well as before (−3.9 dB against
  −3.8), harm drops 1.6 dB and the click probes improve but for the thump
  on a tone (−5.8 → −5.5 dB); the largest crackle gives up 0.3 dB. About
  5 % more CPU. Preferred by ear over 0.6 / 3, which leaves the MP3 exactly as
  many clicks as before (1.93/s) but takes 0.9 dB more from the Malvon
  master.
* **Repairs solved against full repairs.** As first built, a run's
  estimate was solved against the window as the soft repair left it, with
  half of every weak neighbour's click still in it. The refit after the
  first pass has the clicks out and whitens harder, and through it those
  half clicks pushed a re-repair past the local range to the clamp, which
  was then the next run's context. In a cluster of weak runs on Tanturi's
  *Voz de tango* after Dehum (14.37 s) the estimates went 0.066, 0.10,
  0.18, 0.71, 1.0 against limits of 0.053–0.066, and the gaps between the
  runs then read up to 440σ and were repaired in full at the clamp: a loud
  6 ms burst. Smaller ones turned up on *La tablada* (.m4a, 103.6 s) and
  on *Voz de tango* without Dehum (59.2 s). Each repair now writes a second
  copy of the window in full, `Channel::m_full`, and solves against it;
  what the detection and the refits read, and what is output, still has
  the half repair. All three bursts are gone. Hard repair (`--soft-span 0`)
  is unchanged bit for bit. Removed, against the first build: Malvon
  master −38.6 → −38.4 dB, clean D'Arienzo −48.1 → −48.1, *La tablada*
  .flac −33.4 → −33.2, *Cachirulo* .flac −31.0 → −31.1. The other cure,
  keeping only full repairs in the window and taking the half at the
  output, cures it as well but takes 0.3–0.7 dB more from the clean
  records: the half clicks the detection and the refits see are part of
  what Sensitivity 0.9 was tuned with. No measurable CPU.

Tried and dropped:

* **Shorter runs for weak detections** — a higher floor for runs that peak
  under 5–6σ. Kind to the clean D'Arienzo, but it cost the crackly MP3 more
  clicks than it saved on the Malvon master; the half repair above does
  both better. Requiring the forward and backward errors to agree inside a
  weak run, the textbook signature of an additive click, traded worse too:
  on real records it separated clicks from noise only weakly.

* **Sparse deconvolution** (L1 on the residual, FISTA) — the textbook
  decrackler. Same crackle removal as the existing detector at 4.5 dB more
  harm.
* **Uncapped Wiener weighting.** With the cap lifted the model's own variance
  gates in the wrong places: −3.7 dB of crackle at α = 1, less than depth 1.
* **A long-history fit** — the autocorrelation of already-repaired output,
  exponentially forgotten over 4–16k samples. Catastrophic: whole-file SNR fell
  from 22.8 to 4–11 dB. A model of the last 0.4 s describes the previous notes,
  so detection flags the new ones. The fit has to stay local.
* **A coarse (order 4–16) model for large events.** Big-event removal rose to
  −12 dB on dense crackle — and fell from −15 to −1 dB on a loud click on a
  pure tone, which a low order cannot carry across a gap.
* **More Extent** for the clicks the dip rule catches. The floor drops for
  every run, so they spread into the noise everywhere: Extent 0.8 left 0.72/s
  on *Hotel Victoria* at the cost dip 1 / 0.5 pays for 0.48/s.
* **Merging runs that touch**, or that a later pass finds next to an earlier
  repair. The first is nearly as good as the dip rule but treats the clean
  samples between two real clicks as damage, at 1.3 dB of SNR; the second
  found nothing, because the damage it was meant for is inside the run.
* **Mid/side processing** of stereo transfers of mono records. The side
  channel holds crackle and almost no music, but declicking it separately
  did nothing for the mid, which is what is heard.
* **More Extent** for thumps. A thump's tail is a decaying resonance, which is
  exactly what an AR model predicts well, so the residual detector cannot see
  it whatever the hysteresis. Thumps need a mechanism of their own.
* **The body of dense crackle.** On the held first note of Tanturi's *Cuatro
  compases* (1942) the transfer "squirts": by ear, splicing bands of the
  decrackled version into our output put it in 1–2 kHz. There each crackle
  is a smooth bump, 0.3–0.6 ms wide and up to ±0.1, around a sharp core of
  a few samples. We find and repair the core; the bump stays. Two thirds of
  what the decrackled version takes from that band lies within 0.25 ms of a
  run we already repair, but only 8 % of it is in what we take. Four ways at
  the bump, all in Python against the decrackled version, the Malvon master
  and the clean D'Arienzo:
  * *Residual detection*, raw, band-limited to 0.3–3 kHz or integrated over
    0.5–2 ms. On the ten largest bumps it scores no higher than the
    decrackled output's own 99.9th percentile on the same note: an order-128
    model predicts something that smooth.
  * *A time-frequency suppressor* that cuts a bin back to the louder of the
    medians before and after it. At a fifth of the decrackled version's removal it
    took −26 to −28 dB from the clean records. The bumps sit only 12 dB
    under the note in their band.
  * *A pitch predictor*, one period either side. The chord is only 0.86
    periodic and beats, and only the largest bump stood out.
  * *Repairing the run and a Gaussian-derivative bump jointly*, keeping the
    bump when it cuts the residual by 25 noise variances or more. Bump and
    music cannot be told apart for the same reason: the master went from
    −38.6 to −21.8 dB and the D'Arienzo from −47.3 to −20.4, on bumps
    bigger than the music around them. Capping the bump at the click's size
    accepts nothing, the real ones included: at 0.79 s the bump peaks at
    0.14 around a core discrepancy of 0.0013.
  Widening every run by 3–22 samples, the obvious shortcut, took the master
  to between −33 and −17 dB and the MP3 from 2.1 to up to 15.7 clicks/s left.
  Like thumps, the bump needs a model that sees further than a few
  milliseconds. One that does is described next.

### The body of dense crackle: the body stage

**Why every per-click approach failed.** Our output's 1–2 kHz band during
the *Cuatro* note is no louder within 0.5 ms of a repaired run than 3 ms or
more from one (−0.6 to −1.3 dB). Clean records are *louder* near runs, where
those runs sit on attacks. A time-domain version of the median suppressor,
with a 0.3 ms band envelope against the louder of the medians before and
after, puts the decrackled version's 15 largest events only 1.0–3.5× above
their neighbourhood. The Malvon master and the clean D'Arienzo reach 2.9× at
p99.9. At ~750 bodies a second the neighbourhood *is* bodies. They are a
carpet about 12 dB under the note in its band, and the cores say where the
carpet is, not when each body happens.

Two other leads also closed:

* The body is not a fixed response to the core. A ±1.5 ms least-squares FIR
  from the original's >4 kHz band explains 1–2.5% of the decrackled 1–2 kHz
  removal.
* A sinusoidal model of the held note over 46–186 ms fits it to only −10…−16
  dB.

**What does work is to treat the carpet as noise.** Across 0.1 s frames of
60 of the 170 sample-aligned decrackled pairs, the decrackled version's
0.5–3 kHz removal tracks the crackle-to-music ratio. Within a song it
correlates 0.73 with what our own repair takes from that band. It correlates
only 0.15 with the rate of strong runs. Overall it takes 3–5 dB more than we
do at every level.

**The stage** (`Config::body`). An STFT runs on the repaired signal: N
2048, about 46 ms at any rate, with hop N/4. In 1–3 kHz it cuts each bin
by its non-tonal share, P²/(H²+P²), and never by more than 12 dB. The band
started at 500 Hz; why it moved is under [The band starts at 1 kHz](#the-band-starts-at-1-khz).

* **P** is the median over ~370 Hz of the same frame.
* **H** is the louder of the medians over the frames before and the frames
  after, so an onset or a release counts as tonal.
* **The key** sets how much of that cut is applied. It counts runs per second
  that peak at 6σ or more, over 0.1 s, and takes the *minimum* of that over
  the last 0.5 s. The surface's crackle is steady, and the bursts that
  attacks fire come and go. The key opens from 20/s and is fully open at
  60/s.
* **The release.** Once open, the key is held for 2 s
  (`Config::bodyRelease`): the stage applies the strongest key of the last
  2 s. Over 0.1 s the count is a handful of runs, so its minimum dips below
  20/s under crackle that has not changed. On *Cuatro compases* the key
  flickered about 59 times a minute and was 41% open on average, and the
  squirt came back in patches, heard at 1:05–1:08. Held for 2 s it is 86%
  open and changes state about 7 times a minute; on *Que solo estoy* it went
  from 68% to 98%. A record whose crackle never opens the key is untouched by
  this. `declick_cli --body-release 0` gives the stage without it, bit for bit.
* **What it outputs.** It computes only what it removes, and subtracts it.
  So where the key is shut, the output is the repaired audio to the bit, a
  constant delay later.

| | Cuatro note: shared / left | Malvon master | clean D'Arienzo | *El Marne* | *El irresistible* |
|---|---|---|---|---|---|
| without the stage | 8% / 0.98 | −38.6 dB | −48.1 dB | −29.9 dB | 4.88/s |
| with it (C++, causal key), key held 2 s | **45% / 1.04** | −38.6, bit-identical | −48.1, bit-identical | −29.9, bit-identical | 4.80/s |
| … key not held (`--body-release 0`) | 40% / 0.99 | −38.6, bit-identical | −48.1, bit-identical | −29.9, bit-identical | 4.83/s |
| Python prototype, centred key | 45% / 1.03 | −38.6 | −48.1 | −29.9 | 4.83/s |
| prototype, key without the 0.5 s minimum | 33% / 0.92 | −31.1 | −45.7 | −23.1 | 4.75/s |

How the columns are scored:

* **Shared** is how much of the decrackled version's 1–2 kHz removal ours
  reproduces. **Left** is the 1–2 kHz distance to the decrackled output,
  relative to the original's.
* Removal from the clean records is over 30–90 s of each. *El irresistible*
  is scored in clicks per second left above 8σ in the whitened output (this
  counter reads higher than the one in [Dense crackle](#dense-crackle)).
* The previous best "shared", the joint bump fit, was 28%, at −21.8 dB on
  the master.

Run on whole songs before the release, the C++ stage took −21.7, −19.8 and −32.3 dB of the
0.5–3 kHz band from *Cuatro compases*, *Que solo estoy* and *Aleli*. The
prototype took −22.2, −20.3 and −32.0 dB, and the removals correlate 0.81,
0.90 and 0.70.

**Chosen by ear.** The test was the prototype at floors of −12 and −6 dB on
those three songs. At −6 dB the squirt could still be heard by careful
listening. At −12 dB it was gone, and the stage's removed files held no music
to the ear. That held even on *Que solo estoy*, where the key is open 91% of
the time and the stage takes 8 dB more of the band than the decrackled
version does.

**Latency.** A sample's removal is final once the frame two hops after the
last one covering it has arrived, 1.5 N − 1 samples later. The stage delays
every sample by that, rounded up to whole 512-sample blocks: 3072 samples at
44.1 kHz. Whole blocks because `latency` has to be exact in both directions
of the streaming contract, and a stage fed a block at a time can only keep
both with a delay of whole blocks. Hosts that read ahead do not hear it:
foobar2000, and VirtualDJ's buffer plug-in. A VST or AU declares it, and the
host compensates for it.

**Cost.** Done naïvely, with full complex FFTs at hop N/8, the stage cost
1.5× the whole AR core. It is now down to the figures given under
[Declick parameters](#declick-parameters). These are the changes:

* **Hop N/4.** The removal it gives correlates 0.995 with hop N/8's.
* **Packed transforms.** The real frames go through N/2-point complex
  transforms, and only the band's bins are unpacked.
* **Faster butterflies.** Each FFT stage has its own contiguous twiddles,
  and the first two stages need no multiplies.
* **Nothing while shut.** The stage does no transforms at all while the key
  is shut.
* **A sliding median.** The non-tonal estimate is a median of 17 bins for
  each of the band's 118, and it ran `nth_element` on each. The window moves
  one bin a step, so it is now kept sorted with one value displaced, as in
  Dehum's detector. That took the median from 56 % of the open stage's time
  on the M1 and 41 % on the Haswell to a fifth of what it was.
* **No bit-reversal pass.** Swapping where `rev[i] > i` mispredicts on half
  the points. The swaps were first made a list of pairs. Now the callers
  write each frame, and the inverse's band, straight into bit-reversed
  positions as they fill them, so there is no permuting pass at all.
* **No divide per sample.** Finding the next hop was a 64-bit modulo of the
  sample count, on every sample whether the key was open or not: cheap on the
  M1, a third of the shut stage's cost on the Haswell. It is a countdown now.

Those three are bit-exact. On *Cuatro compases*, the stage's cost over the core
went from +37 % to +20 % with NEON on the M1 (+14 % → +8 % baseline), and from
+47 % to +34 % with AVX2 on the Haswell (+23 % → +16 % SSE2). With the key shut,
*Serenata* at order 32 on the Haswell went from +10.7 % to +8.9 %.

* **Two stages a pass.** After that the FFTs were most of the open stage,
  and on the Haswell they ran at about 6 cycles a butterfly, three times
  what its store port allows: they were bound by passes over the arrays,
  not by arithmetic. The first two stages now go together, without
  multiplies, and the rest in pairs: each group of four points goes through
  both stages while it is in registers. The twiddles all come from the table
  and every butterfly is the radix-2 one, so this is bit-exact too. A true
  radix-4 would derive half the second stage's twiddles as −i times the
  others and save a quarter of the multiplies, but it would round differently,
  and multiplies are not what the FFT is short of.
* **An AVX2 pass.** The paired pass is a kernel like the filters, picked
  when the module loads. With AVX2 it does four groups at a time, using
  multiplies and adds in the scalar order and no FMA, so it gives the same
  bits as the scalar pass. NEON uses the scalar pass, which the compiler
  vectorises two wide.

With those two, *Cuatro* costs +17 % over the core on the M1 (+7 %
baseline) and **+19 %** on the Haswell with AVX2 (+15 % SSE2). Output is
byte-identical to the body stage as first committed, at every order, sample
rate and kernel tried, with the key forced open.

The 2 s release keeps the key open on more of *Cuatro*, and the stage costs
more there for it. On the Zen 4, alternating the two in two runs of seven and
nine rounds, the stage cost +18–19 % over the core with AVX2 without the
release and +20–22 % with it, and +10 % and +12 % with SSE2.

#### The band starts at 1 kHz

The stage was chosen on records with no clean copy. Three Demare shellacs
(*Torrente*, *Telón*, *Pa' mí es igual*), aligned to vinyl transfers of the
same takes, gave it one. On every song where the body key opens, what the
stage took at 0.5–1 kHz was music, and no measure of crackle depended on it:

| | 0.5–1 kHz, dB re the vinyl: from 500 Hz → from 1 kHz (the shellac) | music taken at 0.5–1 kHz |
|---|---|---|
| *Torrente* | −16.0 → −17.4 (−17.5) | 4.8 → 0.3 % |
| *A las siete en el café* (Calo) | −17.8 → −20.7 (−20.9) | 4.4 → 0.2 % |
| *Tristezas* (Calo) | −17.3 → −18.1 (−18.2) | 2.4 → 0.1 % |

* From 1 kHz, the band returns to the untouched shellac's distance from the
  vinyl. Every other band, the quiet passages and the click counts are the
  same to within 0.1 dB on all seven songs with a reference.
* On *Torrente*, which has a second, independent vinyl transfer, three copies
  measure each one's noise without trusting any as clean. The stage's
  0.5–1 kHz cut lost 2.9 dB of signal-to-noise there; from 1 kHz, 0.4 dB.
* Why it opened there: *Torrente*'s clean first half has 55–84 strong runs a
  second, so the key is open over it, and the cut's law then takes from the
  music below 1 kHz.
* 750 Hz gave back only half of it.
* The squirt is heard in 1–2 kHz, and the records it was chosen on lose
  their 0.5–1 kHz removal too.
* The band is 93 bins at 44.1 kHz instead of 116, so the open stage is a
  little cheaper.

### The top band of dense crackle: the fine stage

**What was left.** Four of Tanturi's sample-aligned decrackled pairs were the
test: *Recuerdo* (1941), *Voz de tango* (1942), *Viejo ciego* (1946) and
*Tu olvido* (1946, 96 kHz). The decrackled version's removal is impulsive in
every band, with 75–97 % of its energy in 5 % of the samples, where Gaussian
noise would have 28 %. Above 6 kHz our output was as loud as the decrackled
version's whole content of that band, and on *Recuerdo* 3 dB louder. There
the music is weak and the crackle is not. On *Viejo ciego* and *Tu olvido*
each crackle leaves a burst of ringing 1–2 ms long, 5–10 times the floor
between bursts. The detector repairs the largest; the decrackled version takes
nearly all of them.

**By ear.** On *Tu olvido*, of our output with one band of the decrackled
version swapped in, only the band above 4 kHz was heard as clean.

**The stage** (`Config::fine`). An STFT of the repaired audio, with frames of
~2.9 ms (128 samples at 44.1 and 48 kHz, 256 at 96 kHz), hop N/4 and a
square-root Hann either side. Each bin from 4.5 kHz to 20 kHz is clipped to
twice its background, and never cut by more than 18 dB.

* **The background** is the louder of two 20th percentiles of the bin's
  magnitude: over the 16 frames before and over the 16 after, ~12 ms each.
  A median fails, because the bursts come every ~4 ms and the median is made
  of them. The louder side is used so that an onset or a release is not cut.
* **The voice guard.** A voice's top band, and a reed's, also comes in bursts:
  one per glottal pulse or reed closure, every 4–8 ms. The clip cannot tell
  those from crackle. So below 8 kHz the cut backs off as far as the 4.5–8 kHz
  envelope repeats with a period of 2.5–12 ms. That is measured as its
  normalised autocorrelation over ±24 ms at the best lag. At 0.1 the back-off
  begins, and at 0.3 there is no cut below 8 kHz at all. Above 8 kHz a voice on
  shellac has little, and the clip stays as it is.
* **It runs alongside the body stage**, on the same repaired audio, and adds
  to what that stage removes. The two bands do not meet. Run after the body
  stage instead, the result differs by 30–34 dB less than what it removes.
* **The key.** It is gated by the body stage's key. Where that is shut it does
  no transforms and subtracts nothing, so the Malvon master, *Olga* and
  *El Marne* come out bit-identical, as do the `.flac` of *La tablada* and
  everything else whose key never opens.
* **Latency.** Its delay is a frame and the larger of 16 hops and the guard's
  24 ms: 1183 samples at 44.1 kHz. That fits inside the body stage's 3072, so
  it adds none.
* **Off.** `declick_cli --fine 0` gives the previous output to the bit, and
  the stage is off whenever the body stage is. `--fine-mid 0` turns off only
  the voice guard.

Over whole songs, the distance to the decrackled version in 4.5–8 / 8–16 kHz
(lower is closer):

| | key open | without | with | the stage takes |
|---|---|---|---|---|
| *Recuerdo* | 99 % | −3.9 / +2.2 dB | −4.1 / +0.8 dB | −46.7 dB |
| *Voz de tango* | 52 % | −3.5 / +0.1 dB | −3.5 / −0.2 dB | −49.9 dB |
| *Viejo ciego* | 25 % | −5.7 / 0.0 dB | −5.7 / −0.2 dB | −65.1 dB |
| *Tu olvido* | 99 % | −8.1 / −0.8 dB | −8.5 / −3.4 dB | −43.5 dB |

The last column is against the whole song.

**Chosen by ear**, in three rounds on *Tu olvido*:

1. **The clip's strength.** The Python prototype was heard at twice and at
   1.5 times the background, with no guard. Twice, the gentler, was heard as
   good on a 15 s clip.
2. **The guard.** Built in and heard over the whole song, that version split
   the vocals. In the most strongly voiced seconds the decrackled version
   leaves 4.5–8 kHz almost alone, −0.6 and −1.7 dB against the original. The
   unguarded stage took enough there to push the energy it cut below the
   decrackled version's from −14 to −8 dB of that band.
3. **The setting.** Three ways of sparing the voice were heard, and all kept
   it whole:
   * starting the stage at 8 kHz;
   * backing off from a periodicity of 0.3 to 0.6;
   * the same from 0.2 to 0.4.

   Starting at 8 kHz left crackle the back-offs took. Asked for the least
   destructive back-off that still takes it, the default is softer still,
   0.1 to 0.3. A fourth way was measured but not heard: a pitch-aware
   background, keeping a bin as loud as it was one or two periods away. It
   traded no better than backing off.

On *Tu olvido*, with the stage, our version was heard as much better than the
decrackled one.

Over the whole of *Tu olvido* in 4.5–8 kHz, against the decrackled version:

| | taken below its level | left above it |
|---|---|---|
| no stage | −14.4 dB | −5.7 dB |
| unguarded | −9.1 dB | −9.1 dB |
| guard, 0.2 to 0.4 | −12.0 dB | −6.7 dB |
| guard, 0.1 to 0.3 (the default) | −13.4 dB | −6.1 dB |

The C++ stage matches the prototype to 103–106 dB below what it removes.

**What it takes elsewhere.** Of the dehum reference transfers, *Cachirulo*'s
`.m4a` has the key open 99 % of the time. The stage takes −52 dB of it, and
−25 dB of its band above 4.5 kHz. *La tablada*'s `.m4a` and *Cachirulo*'s
`.flac` change by −76 and −64 dB, and *La tablada*'s `.flac` not at all.
Unkeyed, the stage without its guard would take 16 dB of the Malvon master's
6–12 kHz band, which is why it has to stay behind the key.

**Not reached.** *Viejo ciego*'s key is open only 25 % of the time. Its rate
of strong runs, 39 a second at 6σ, is that of *El Marne* (38). So a key of its
own that opens there but stays shut on *El Marne* was not found. The 1–4 kHz
bumps on *Viejo ciego* and *Tu olvido* are still left. The core does trigger
on them, but its runs cover only their sharpest samples. An AR model fitted on
the crackly audio treats the rest of each bump as music. Fitted on the
decrackled version instead, the same detector finds 93–97 % of them at the
false-alarm rate where it now finds 62–79 %.

**Cost.** With the key shut it costs nothing measurable. On *Cuatro
compases*, where the key is open throughout, it adds 44 % with AVX2 and 28 %
with SSE2 on the Zen 4, best of seven interleaved. On *Tu olvido* at 96 kHz
it adds 24 % with AVX2. Of that, the voice guard is about a sixth.

* **What was done about it.** As first written it made *Cuatro* five times
  slower. The changes:
  * The key is read once per repair block, not per hop.
  * Each window of 16 frames is ranked once, not twice.
  * Each bin's window is kept sorted, one value out and one in a hop. The
    positions are found by counting rather than searching.
  * The guard's autocorrelation uses the core's vector inner product.
  * The band stops at 20 kHz, which at 96 kHz leaves a third of the bins.
* **Output.** The first three leave it to the bit what it was. The cap changes
  it only above 20 kHz; `--fine-hi 100000` gives back the uncapped output to
  the bit. The packed transforms added later do move the last bits, by one or
  two ULP on a crackly file and not at all on a clean one: see below.

#### What the fine stage costs, and what came off it

Even after those changes the stage was a third of the whole declicker. On
*Cuatro compases* with AVX2 it took **475 of the 1475 cycles a sample** the
2014 MacBook Pro spends (counted off its invariant timer, so at its nominal
2.4 GHz). That is not because any one piece of it is heavy: it is that its
frames are 128 points at a hop of N/4, so it runs **sixteen times as often as
the body stage**, whose frames are 2048. Everything it does, it does every
32 samples.

Where those 475 cycles went, per input sample:

| | cycles | |
| --- | --- | --- |
| the sorted windows, one value out and one in for each of 42 bins | 148 | 31 % |
| the cut, the packing and the inverse transform | 129 | 27 % |
| the voice guard | 100 | 21 % |
| the windowed load and the forward transform | 81 | 17 % |
| the magnitudes | 18 | 4 % |

Nearly all of the first row was **the move, not the ranking**: of its 148
cycles, 97 were the two loops that shift the window by one place. Their trip
count is one or two and depends on the data, so each of their guards
mispredicted about half the time — about 74 cycles a window for three or four
moves of a double. They are now one predicated pass over the window, which
costs a few more moves and no branches and vectorises (`sortedMove`).

That left the row **latency-bound rather than busy**: each window's ranks
reach its own move through a vector-to-register-to-vector round trip, and
nothing in that window can start until they do, so a window cost 71 cycles
against the dozen its uops take. The ranks for a tile of sixteen windows are
now counted first and the moves made after, so each window's latency is
covered by the next window's counting (`sortedRowAvx2`). Together: **148
cycles to 76**.

Three smaller things, in order of what they were worth:

* **The odd stage out of the FFT had no kernel.** `fftRadix2` takes its stages
  two at a time, and at 128 points there are five, so one of them ran alone in
  scalar code — twice a hop, forward and inverse, and the only scalar stage
  left. It is now `fftOne`, with an AVX2 kernel that is `fftPairAvx2`'s
  butterfly: multiply and add, never FMA, so it is the scalar one to the bit.
  Worth 1.5 %.
* **The body stage's sliding median** found the value leaving by scanning for
  an exact match and then shifted with the same two branchy loops. It now
  shares `sortedReplace`, counting instead of searching. The median is the
  same to the bit — the window holds its own copies of these magnitudes, so
  counting what is below a value finds the first place equal to it, and equal
  doubles are indistinguishable. End to end it is inside the noise on both
  Macs (±0.6 %), but a mispredicting scan is gone from the body stage too.
* **The voice guard read its window with a remainder per frame.** Its ring is
  74 long, not a power of two, so each of the 73 reads was a 64-bit hardware
  divide. One remainder finds the window's start and the rest step. Worth
  0.3 %, which is less than the instruction count suggests: the loop is bound
  by the scattered reads, not the divides.

And two that were tried, measured and dropped:

* **Lane masks from a table** instead of index comparisons in the move, to
  keep it off the one vector port its blend also wants: 1.6 % *slower* on the
  Haswell, where it just moved the pressure onto the load ports, and 0.5 %
  faster on the M1 — not enough for a 2 kB table and a second code path.
* **Counting the ranks through `movemask` and a popcount table** rather than
  adding in the vector registers: 34 cycles a sample against 27. Shortening
  the reduction at the end did not pay for a GPR load per step, and once the
  row was tiled the reduction was not on anybody's critical path any more.

**And the transforms, which did change the output.** The forward and inverse
transforms were about 100 of the remaining cycles, and both were full
128-point complex transforms of real data. They are now packed as 64-point
complex ones, the way the body stage already transforms its 2048 real samples
as 1024 complex ones: even samples into the real part and odd into the
imaginary through the bit-reversal table, `fftRadix2` at N/2, and only the
band's bins unpacked, `X[k] = E + e^(-2 pi i k/N) O` with `E` and `O` the
transforms of the even and odd samples. On the way back the band and its
mirror are packed into N/2 points the same way. It needs a twiddle table of
N at the fine stage's own size, built in `configure()` beside the stage
twiddles, with one place more than the body stage's: the fine band may reach
the Nyquist bin, which folds onto `Z[0]`. It also takes the transform from
five stages to four, so the odd stage out — the one `fftOne` above was written
for — is gone from this stage, and with it half the butterflies.

Best of five interleaved runs on each machine, whole files through
`Channel::push`/`pull` in 1024-frame blocks:

| | M1, NEON | Haswell, AVX2 | Haswell, SSE2 |
| --- | --- | --- | --- |
| *Cuatro compases*, dense crackle, 48 kHz | **−5.7 %** | −2.5 % | −4.2 % |
| *Tu olvido*, 96 kHz | **−4.1 %** | −3.0 % | |
| *Malvón*, clean, 44.1 kHz | −0.1 % | | |

The M1 repeats to 0.03 % and is the number to believe; the Haswell throttles
about 30 % over a few minutes, so its two builds were run alternately. The
AVX2 column is smaller than the SSE2 one because `fftPairAvx2` had already
made the butterflies cheap there, and there are simply fewer of them to save.
*Malvón* is the control: it is clean enough that the body key never opens, the
stage returns before it transforms anything, and that line is noise.

**What it costs in the last bits.** The packing is the same transform by a
different route — a different summation order, and a twiddle multiply per bin
where there was none — so unlike everything above it is not bit-exact. Over
whole files, against the same build without it:

| | identical samples | worst deviation |
| --- | --- | --- |
| *Malvón*, clean | **100 %** | none, byte for byte |
| *Tu olvido* | 71.0 % | 1.1e−16, −319.1 dBFS |
| *Cuatro compases* | 51.2 % | 2.6e−16, −311.8 dBFS |

So about half the samples of a crackly file move, and they move by one or two
ULP of a double — two-billionths of a 24-bit LSB, and 155 dB below the worst
deviation the `dot()` kernel [already
introduces](#where-the-time-goes-and-what-was-done-about-it). A clean file
does not move at all, in any bit: where the key is shut this stage does
nothing, and it still does nothing. Every ground-truth and reference-transfer
figure in this README is unchanged to the last digit printed.

### Band-limited transfers: the flank repair

**What was left.** Four of D'Arienzo's 1940s sides come as two transfers: a
`.flac` flat to 20 kHz, and an `.m4a` (ALAC, lossless) whose spectrum falls
40 dB by 11.7–14.4 kHz. Declick was heard as good on the `.flac` and bad on the
`.m4a`. They are different copies of the record, so the crackle differs and the
music does not. Warped onto the `.m4a` to a fraction of a sample and EQ-matched
to it every 2 s, the `.flac` copy became a clean reference for it, independent
of any decrackled version. The warp is a Viterbi path through 40 ms
cross-correlations that follows the 1.32 Hz wow of either disc; 94–97 % of the
songs match at ρ ≥ 0.75. Against that reference:

* **The `.m4a`'s excess above 3 kHz is crackle.** Events at more than 16 times
  their local floor carry 19–49 % of its energy there, where a Gaussian of the
  same spectrum puts 0–3 %.
* **The detector finds it.** 99–100 % of the larger events' energy at
  4.5–10 kHz lies on a run. The repairs leave 36–55 % of it.
* **The half repair is not the limit.** Repairing every weak run in full
  changed that by a point, and took 2.6 dB more from *Malvón*.
* **The click has one shape**, the same in all four songs: a 3–4 sample spike
  that rings for ±10–15 samples, an impulse through the transfer's low-pass. A
  run ends inside the ringing. With one sample of slack, its interpolation is
  pinned to flanks that are still click, and rebuilds part of it.

**The stage** (`Config::flank`). Once a block is repaired, each of its runs is
widened by 0.18 ms (8 samples at 44.1 kHz) either side, into the samples
between it and its neighbours but never into one, and interpolated again
against every repair in full, at its own gain. Only the difference above
3 kHz is kept, through a 127-tap Kaiser-windowed high-pass. Below that the
audio is exactly what the repair made of it.

* **Band-limited transfers only**, as first built. Every fourth block's
  spectrum goes into a ten-second average. From three seconds on, its −40 dB
  edge — the highest frequency within 40 dB of its 2–5 kHz level — sets the
  pad: full at 13 kHz and below, none at 15 kHz and above. A full-band record
  got none. Since [the micro
  stage](#micro-clicks-closer-than-the-fine-frame-the-micro-stage) every
  transfer gets the full pad where the body key is open (`--flank-wide 1`);
  the edge still sets it in the fade after the key shuts.
* **The key, and a fade after it.** It acts where the body stage's key is
  open, and once the key shuts it fades out over 12 s (`--flank-hold`). The
  key counts runs that peak at 6σ, and a dense carpet inflates σ until few
  reach it: on *Amarras* it shuts over 2:04–2:08, just where the `.m4a` stands
  furthest above the clean copy, and with the key alone nothing was repaired
  there. A record whose key never opens is untouched by the fade.
  * Tried instead: a softer repair wherever the key is shut, on band-limited
    transfers (`flankShut`, since removed). At half strength it did as much for *Amarras*
    as the fade, but it also took −20 to −27 dB of 3–10 kHz from nearly every
    band-limited 1950s remaster in the sample below, whose key never opens.
  * Tried too: only the strong runs where the key is shut
    (`flankShutGain`, since removed). That gained nothing; the gain there comes from the
    weak runs, the carpet.
  * And a pad for full-band transfers under the key (`--flank-wide`). On *No
    está*, *Ruiseñor* and *Cornetín* what it took beyond the stage
    correlated as much with what the decrackled version keeps as with what it
    removes, so it was left off. Measured later against vinyl copies rather
    than a decrackled version, it helped, and it is now on: see [the micro
    stage](#micro-clicks-closer-than-the-fine-frame-the-micro-stage).
* **Off below 32 kHz**, where a low sample rate cannot be told from a
  band-limited transfer.
* **Each run on its own.** Merged into one span where their pads met, a
  cluster on dense crackle became one long interpolation that lost the music's
  3–4.5 kHz: *Si supiera* went 1.8 dB further from its reference there.
* **Latency.** The next block's flanks reach back into this one, so each block
  waits one more block before the body stage: 512 samples, 11.6 ms. The body
  and fine stages see the same samples under the same key as before, a block
  later.
* **Off.** `declick_cli --flank 0` gives the previous output to the bit, at the
  previous latency.

Against the reference, dB of (output − reference) against the reference's
level per band (lower is closer), and the share of the `.m4a`'s crackle events
left at 4.5–10 kHz:

| | | 3–4.5 kHz | 4.5–7 kHz | 7–10 kHz | events left, larger / smaller |
|---|---|---|---|---|---|
| *La madrugada* | without | −8.9 | +0.2 | +6.3 | 36 / 75 % |
| | **with** | **−9.1** | **−1.1** | **+4.9** | **22 / 50 %** |
| | decrackled | −8.8 | −1.1 | +3.6 | 21 / 48 % |
| *Amarras* | without | −5.8 | +1.9 | +7.8 | 55 / 86 % |
| | **with** | **−6.4** | **+0.7** | **+6.6** | **36 / 61 %** |
| | decrackled | −6.1 | 0.0 | +4.5 | 21 / 48 % |
| *Si supiera* | without | −11.4 | −1.5 | +5.2 | 45 / 86 % |
| | **with** | −11.2 | **−2.1** | **+4.6** | **33 / 74 %** |

Below 3 kHz nothing moves by more than 0.1 dB. With the key alone, *Amarras*
came to 40 / 69 %; the fade brings 2:05–2:08 0.6–1.3 dB closer to the clean
copy. *Yo quiero cantar un tango*'s key never opens, and that song barely
changes. **Heard** on *Amarras* and *La madrugada*, in its Python prototype:
cleaner, closer to the reference, and no voice in what it takes. Then the
fade: right on *Amarras* 1:58–2:18, and no music in what it takes from two
remasters it reaches further on (*Homero*, *Carpeta*).

**Records it must not touch.** Bit-identical to the build before it: the
*Malvón* master, *No está*, *Va a cantar un ruiseñor*, *Cornetín*, *El Marne*,
the Troilo dehum pairs, the `.flac` copies above, and *Olga* — band-limited,
but its key never opens. So are 78 of 98 further songs: a random 80 of
1950–79 and the 18 references of the hard-case study. The other 20 are
band-limited transfers whose key opens at some point. In four of them it acts
over 72–98 % of the song, taking −18 to −23 dB of the 3–10 kHz band: Donato's
*Por una mujer*, de Angelis's *Jorge Duval* and *Que nadie se entere (2)*,
D'Arienzo's *La payanca*. *Homero* and *Carpeta* lose −26 and −27 dB over
about half the song, and were heard to lose no music. The other 14 lose −28 to
−48 dB over 5–50 %. `scripts/declick_regress.py` passes, with no clamp count
changed. *El irresistible* and *Hotel Victoria* lose 0.3 dB more. Its one new
event is 3 samples at 1.36 times the local range on *Si supiera*, where the
build before reached 1.24.

**Cost.** +2.0–2.1 % on the full-band clips (*Pa que bailen los muchachos*,
*Malvón*): the spectrum and the block's wait. +24.4 % on *La madrugada*, where
every run is interpolated a second time.

### Crackle the body key misses: the carpet key

**What was left.** The body key counts runs that peak at 6σ, and on a crackly
transfer that count sits right at its 20–60 a second ramp. Dense crackle
inflates σ, so fewer of its clicks reach 6σ just where it is densest, and the
key flickers or stays shut:

* *Yo quiero cantar un tango* (`.m4a`) had it open 3 % of the song;
* *Si supiera* 36 %;
* *Viejo ciego* 34 %;
* *Margarita Gauthier* 27 %.

Where it is shut the fine stage and the flank repair do nothing either. Against
the clean copy, the crackle there was not denser from one second to the next:

* On *Amarras* 1:48–2:21 the detector repaired 1,200–1,500 runs a second
  throughout, about 300 of them at 4σ or more.
* The worst seconds were where the music dropped by 8 dB.
* Every per-second measure tried tracked the music rather than the crackle:
  run rates at lower peaks, the share repaired, σ against its own floor, the
  high band's kurtosis.

What tells a crackly transfer from a clean one is the record. Its surface keeps
a carpet of runs going through the quiet passages; on a clean record runs come
and go with the music.

**The key** (`Config::carpet`).

* **What it counts.** Every repair block, the runs that peak at 4σ or more,
  where the body key counts its own.
* **Its value.** Their rate per 44.1k samples over each second, and the 20th
  percentile of the last 20 seconds of those rates.
* **The ramp.** It opens from 110 runs a second and is full at 150.
* **Its first seconds.** It may open after 3 s. While its 20 s fill, both
  thresholds stand up to half as high again: a loud opening of a few seconds is
  little evidence. Before 3 s it now has an early value, from 0.25 s: see
  [the first seconds](#the-first-seconds-an-early-carpet-key).

| | its value, 5th / 50th percentile |
|---|---|
| the four D'Arienzo `.m4a` | 101–278 / 145–341 |
| ten Tanturi and Calo `.m4a` the user named as not perfect | 99–274 / 127–350 |
| clean band-limited 1958–75 remasters, *Olga* | at most 77 |
| *El Marne* | at most 108; 120 on its first seconds |
| the *Malvón* master | at most 66 |

**It opens the fine stage and the flank repair, not the body stage.**

* The fine stage cuts by the larger of the two keys.
* The flank repair's strength is at least the carpet key.
* Opening the body stage too moved six of the seven songs below 1.1–3.1 dB
  further from their clean copy at 0.5–1 kHz: that is music.

Against clean copies of seven songs, dB of (output − reference) per band
(lower is closer), and the `.m4a`'s crackle events left at 4.5–10 kHz:

* the four D'Arienzo `.m4a` against their `.flac` copies, as above;
* three Calo `.m4a` against vinyl transfers, warped onto them after a speed
  search (the vinyl runs 1.6 and 2.4 % slow on two of them).

| | | 4.5–7 kHz | 7–10 kHz | events left, larger / smaller |
|---|---|---|---|---|
| *Yo quiero* | without | +4.6 | +11.8 | 56 / 86 % |
| | **with** | **+3.8** | **+10.5** | **37 / 65 %** |
| *Amarras* | without | +0.7 | +6.6 | 36 / 61 % |
| | **with** | **+0.4** | **+5.9** | **31 / 54 %** |
| *Si supiera* | without | −2.1 | +4.6 | 33 / 74 % |
| | **with** | **−2.2** | **+4.2** | **31 / 69 %** |
| *Margarita Gauthier* | without | −4.6 | +1.5 | 42 / 78 % |
| | **with** | **−4.7** | **+0.9** | **40 / 74 %** |
| *A las siete en el café* | without | −4.1 | +4.2 | 26 / 72 % |
| | **with** | −4.1 | **+3.9** | **25 / 70 %** |
| *Tristezas de la calle Corrientes* | without | −4.8 | +0.9 | 39 / 76 % |
| | **with** | **−4.9** | **+0.6** | **38 / 75 %** |

Below 3 kHz the output is the same as without it on all seven. *La madrugada*'s
key was already open, and it hardly changes. **Heard** as helping on *Homero*
and *Si supiera*.

**Records it must not touch.**

* **Bit-identical to the build before it:**
  * *Malvón*, *Olga*, *El Marne*, *La tablada*;
  * the band-limited 1958–75 remasters of the sample below;
  * the full-band songs of the sample whose body key never opens;
  * 81 of 108 songs in all (the 98-song sample, the six regression songs and
    the four D'Arienzo `.flac` copies).
* **The others are crackly transfers:**
  * the `.flac` copies, *No está*, *Ruiseñor*, *Cornetín*;
  * the 1950s remasters the body key already opens on.
  * The most changed are *Bien milonga* (−30.8 dB of 3–10 kHz over 79 % of the
    song), *Homero* (−29.9 dB over 51 %) and *Mano a mano* (−27.0 dB over
    28 %).
  * One 1970 remaster, *Senda florida (2)*, changes over 6 % of the song by
    −52 dB.
* **`scripts/declick_regress.py` passes** on the reference set and on the
  fourteen songs above:
  * no new out-of-range event, no clamp count changed;
  * *Yo quiero* takes 1.3 dB more, *Amarras* and *Viejo ciego* 0.2–0.3 dB,
    *Hotel Victoria* 0.1–0.2 dB.
* **Off:** `declick_cli --carpet 0` gives the previous output to the bit.
* **Where the count stays under the ramp** on a crackly record, the
  [Poisson key](#crackle-that-comes-at-random-the-poisson-key) opens it.

**Cost.**

* **Where it stays shut,** nothing measurable (+0.2 to +1.4 %, the noise of
  interleaved best-of-7 runs): a counter per block, twenty numbers sorted once a
  second.
* **Where it opens,** the stages it opens: +44 % on *Yo quiero*, +20 % on
  *Viejo ciego*, as on a record whose body key is open.

### Micro-clicks closer than the fine frame: the micro stage

**What was left.** Five Calo `.m4a` were named as not clean enough: *A las
siete en el café*, *Margarita Gauthier*, *Tristezas de la calle Corrientes*,
*Gime el viento* and *Trasnochando*. Each also exists as a vinyl transfer, a
different copy of the record, so its crackle is independent. Each copy was
warped onto its `.m4a` with a speed search (the vinyl runs 0.04–2.4 % slow),
then the Viterbi path that follows wow, and EQ-matched every 2 s. 86–96 % of
each song lines up, and below 3 kHz the vinyl is a waveform reference. Against
it:

* **The clicks are found.** A run lies within 0.25 ms of 75–89 % of the crackle
  cores above 4 kHz.
* **Their spacing beats the fine stage.** At 8 times their floor they come
  350–505 a second, a median 1.5–1.75 ms apart, and 67–80 % of the gaps are
  shorter than the fine stage's 2.9 ms frame. Its background, a 20th
  percentile over 12 ms, is then made of clicks.
* **So quiet passages are where it fell behind.** Where the vinyl has no voice
  in its top band, the output kept 53–86 crackle events a second above 16
  times their floor. The decrackled version kept 15–26. Worst at song ends and
  pauses: 107–121 a second against 2–3 at 2:57–2:58 of *A las siete*.
* **The surface sets the trigger.** On these `.m4a` the core's σ is 2.0–3.4
  times what it is on the vinyl, in quiet and loud passages alike, and 7–11 %
  of samples are in a run (the vinyl: 3.1–3.7 %).

**The stage** (`Config::micro`). The fine stage's clip runs again, on the fine
stage's output, at a quarter of its frame: 32 samples at 44.1 and 48 kHz (64
at 96 kHz), ~0.73 ms, at hop N/2.

* **The background** is the louder of the 20th percentiles over 6 ms before
  and 6 ms after (`--micro-ms`).
* **Shared with the fine stage:** the band (4.5–20 kHz), the clip at twice the
  background, the −18 dB floor and the key.
* **The voice guard is the fine stage's.** Below 8 kHz the cut backs off as
  far as the fine stage's guard did for its frame nearest this one's centre,
  and where the fine stage did not cut that frame this one is not cut.
* **Where it sits.** It reads the repaired audio less the fine stage's own
  removal, which the stage now keeps in a ring of its own, once that removal is
  final: 1183 samples on at 44.1 kHz. Its own is final 17 hops and a frame
  later, 1486 in all, inside the body stage's 3072. It adds no latency.
* **It needs the fine stage**, and is off when that is.

**And the wide pad.** The [flank repair](#band-limited-transfers-the-flank-repair)'s
pad now goes to full-band transfers as well where the body key is open
(`Config::flankWide` 0 → 1). It had been left off on what it took from three
full-band songs, judged against their decrackled versions. Against the vinyl,
on the three of these five whose body key opens:

* 3–4.5 kHz came 0.1–0.3 dB closer, and 4.5–7 kHz 0.2–0.6 dB;
* the quietest third's 3–4.5 kHz crackle left went from 56–64 % to 42–52 %;
* it took 1.7–3.1 % of the music at 3–4.5 kHz, where the decrackled version
  takes 1.3–2.9 %.

Against the vinyl, dB of (output − vinyl) re the vinyl per band (lower is
closer); the share of the `.m4a`'s excess left in the quietest third of each
song at 3–4.5 / 4.5–7 / 7–10 kHz; events above 16 times their floor left a
second; and the voice taken: the projection of the removal on the vinyl at
4.5–7 kHz, in the frames where the vinyl's top band repeats with a voice's
period, as a share of its amplitude.

| | | 3–4.5 | 4.5–7 | 7–10 kHz | quiet left | events/s | voice taken |
|---|---|---|---|---|---|---|---|
| *A las siete* | before | −9.9 | −4.1 | +3.9 | 56 / 41 / 33 % | 102 | 2.5 % |
| | **now** | **−10.1** | **−4.7** | **+2.2** | **42 / 29 / 20 %** | **51** | 6.8 % |
| | decrackled | −9.5 | −4.4 | +2.7 | 42 / 26 / 19 % | 66 | 6.4 % |
| *Margarita* | before | −9.9 | −4.7 | +0.9 | 75 / 63 / 56 % | 69 | 1.1 % |
| | **now** | **−10.0** | **−4.9** | **−0.3** | **70 / 56 / 41 %** | **53** | 1.9 % |
| | decrackled | −9.7 | −5.2 | −0.2 | 56 / 42 / 37 % | 46 | 4.0 % |
| *Tristezas* | before | −10.2 | −4.9 | +0.6 | 64 / 52 / 46 % | 91 | 2.1 % |
| | **now** | **−10.3** | **−5.2** | **−0.3** | **52 / 42 / 34 %** | **62** | 4.5 % |
| | decrackled | −10.0 | −5.1 | +0.2 | 46 / 34 / 32 % | 66 | 4.2 % |
| *Gime el viento* | before | −11.6 | −6.2 | −2.1 | 80 / 70 / 63 % | 74 | 0.7 % |
| | **now** | −11.6 | **−6.3** | **−2.4** | **78 / 66 / 57 %** | **69** | 1.0 % |
| | decrackled | −11.3 | −6.3 | −2.7 | 64 / 50 / 43 % | 65 | 2.2 % |
| *Trasnochando* | before | −8.2 | −3.8 | +0.8 | 62 / 49 / 41 % | 102 | 1.4 % |
| | **now** | **−8.5** | **−4.5** | **−0.7** | **47 / 37 / 26 %** | **56** | 4.2 % |
| | decrackled | −8.2 | −4.2 | −0.1 | 46 / 32 / 23 % | 64 | 4.0 % |

Below 3 kHz the output is the same as before to 0.1 dB. The C++ stage's
removal correlates 0.989–0.997 with its Python prototype's, at the same level
to 0.1 dB. *Gime el viento* barely moves: its carpet key reads 86–145 runs a
second, inside its 110–150 ramp, so the stages run at about a third there.

**Chosen by ear**, on those five, from three versions:

* the micro stage alone;
* the micro stage and the wide pad;
* both, with a voice guard that reads the period from the 0.2–3.5 kHz band,
  where voice stands well above crackle, instead of from the top band. There,
  on the two songs measured, the fine stage's guard reads 0.25–0.26 on frames
  with no voice in them, and backs off on 94–96 % of them.

The third was liked most. It took 1.25–2 times the decrackled version's
voice, and by design it would follow the wrong source in a duet or under a
loud bandoneón. The second was chosen as the safer.

**Records it must not touch.**

* **Bit-identical to the build before it:**
  * the *Malvón* master, *Olga*, *El Marne*, *La tablada* (both copies);
  * 52 of 99 songs in all: the 98-song sample and the *Malvón* master.
* **The other 47** are the sample songs whose keys open at some point.
  * The most changed: the *Malvón* and *Cachirulo* `.m4a`, −12.6 and −12.8 dB
    of 4.5–10 kHz beyond the build before, mostly the pad.
  * *Por una mujer* and *Jorge Duval*, band-limited 1950s transfers (edge
    ~10.5 kHz), lose −13.5 and −16.6 dB of it to the micro stage.
  * Out-of-range events are the same on all 99.
* **`scripts/declick_regress.py` passes,** on the reference set and on five
  more (*A las siete*, *Trasnochando*, *Gime el viento*, *Tu olvido*, *Malvón*
  `.m4a`):
  * no clamp count changed;
  * one new event, 2 samples at 1.3 times the local range on *Voz de tango*
    after Dehum, under the script's limit;
  * 0.4–0.5 dB more taken from *Cachirulo* `.m4a` and *Voz de tango*, 0.25–0.54
    dB from the five.
* **Off:** `declick_cli --micro 0 --flank-wide 0` gives the build before it to
  the bit.

**Cost**, the two builds taking turns, best of seven, on the Zen 4:

* **Where the keys stay shut,** nothing measurable: +1.0 % on the *Malvón*
  master, +2.3 % on *Pa que bailen*.
* **Where they open,** +24 to +38 % with AVX2 and +22 to +24 % with SSE2: *Cuatro
  compases* 103× → 76×, *A las siete* 117× → 84×, *Tristezas* 112× → 87×,
  *Tu olvido* (96 kHz) 40× → 33×.
  * The micro stage is +13 to +17 % of that.
  * The wide pad is +13 to +23 % where it is new: the flank repair, running
    on full-band transfers too.
* **Memory:** about 40 kB more a channel at 44.1 kHz: the fine stage's own
  removal ring, and the micro stage's frames.

### Clicks that outlast their run: the mid stage

**What was left.** Four Pugliese `.m4a` were named as not clean enough: *El
rodeo*, *El día de tu ausencia*, *El abrojito* and *Nada más que un corazón*.
Their vinyl copies were warped onto them as for the Calo songs. Those copies
are low-passed (−40 dB by 6.8–8.5 kHz) and some are processed: measured
against further copies, only *Nada más*'s and a second copy of *El abrojito*
are references above 1.5 kHz. So what follows is mostly counted on envelopes,
which needs no waveform match: impulses are 0.5 ms frames of a band over 8
times their ±20 ms median, and a click is one where the vinyl's frame is not
an event too.

* **The gap band.** Over the quietest third of each song, impulses a second at
  2–3 / 3–4.5 kHz:

| | `.m4a` | vinyl | before | decrackled |
|---|---|---|---|---|
| *El rodeo* | 35 / 43 | 5 / 6 | 29 / 31 | 11 / 7 |
| *El abrojito* | 26 / 34 | 9 / 8 | 20 / 16 | 10 / 10 |
| *Nada más* | 10 / 21 | 6 / 9 | 8 / 13 | 6 / 6 |
| *El día de tu ausencia* | 9 / 11 | 4 / 3 | 7 / 6 | 6 / 4 |

* **They are found, not repaired.** 96–98 % of these clicks have a repaired
  run within 0.5 ms. The stages before removed a median 15–44 % of their
  2–4.5 kHz energy; the decrackled version 65–96 %.
* **The click outlasts its run.** The run is the onset, a median 5 samples.
  Around it the band stays over 1.8 times its level 3 ms away for ±0.6 ms, and
  above it for ±0.9 ms. The AR model predicts that ringing, so it is never
  flagged, and the repair keeps it.

**What did not work.**

* **Interpolating wider** (a 0.7 ms flank pad kept from 2 kHz): 35–74 % of
  the clicks' energy, but 1.5–11 % of the music at 2–3 kHz.
* **The fine stage's clip moved down with no guard:** 21–59 % of the record's
  own 2–4.5 kHz events.
* **A broadband test.** A click should also show at 7–11 kHz, but in a crackly
  `.m4a` so do the record's attacks (AUC 0.52–0.66).

**What tells them apart** in the `.m4a` alone is how the band's envelope
repeats. By the fine stage's guard statistic, taken on this band, it reads
0.21–0.24 at the clicks and 0.81–0.83 at the record's own events: AUC 0.92 on
the four. A voice's or an instrument's pulses repeat; clicks do not.

**The stage** (`Config::mid`). The fine stage's frames also take the bins from
2 kHz (`--mid-lo`) up to the fine band, 6–13 at 44.1 kHz.

* **The clip is the fine stage's:** twice the louder of the 20th percentiles
  either side, −18 dB floor, under its key.
* **Only in frames that hold, or neighbour, a run repaired at 4σ or more**
  (`--mid-peak`). The runs are flagged as they are repaired, and the flags
  travel with the audio into the body stage's ring, one per hop.
* **Its own guard:** the cut backs off as this band's envelope repeats, from
  0.3 (none) to 0.5 (all), over ±24 ms at lags of 2.5–12 ms.
* **It yields to the body stage** in their shared band, 2–3 kHz, by the body
  key. The two read the same audio, and their cuts would add on one click.
* **No latency added.** Its frames, background and guard are the fine
  stage's.

Against the vinyl, the stage alone; impulses at 2–3 / 3–4.5 kHz over the
quietest third, a second; the median share of the clicks' and the record's
events' 2–4.5 kHz energy removed; music taken at 2–3 / 3–4.5 kHz, the
projection of the removal on the vinyl as a share of its amplitude:

| | | impulses | at clicks | at the record's events | taken |
|---|---|---|---|---|---|
| *El rodeo* | before | 29 / 31 | 15 % | 1 % | 0.2 / 0.2 % |
| | **now** | **27 / 28** | **29 %** | 1 % | 0.3 / 0.2 % |
| | decrackled | 11 / 7 | 94 % | 47 % | 0.8 / 1.1 % |
| *El día de tu ausencia* | before | 7 / 6 | 31 % | 4 % | 0.9 / 0.4 % |
| | **now** | **5 / 4** | **54 %** | 5 % | 1.6 / 0.6 % |
| | decrackled | 6 / 4 | 65 % | 2 % | 0.4 / 0.0 % |
| *El abrojito* | before | 20 / 16 | 44 % | 2 % | 0.6 / 0.7 % |
| | **now** | **13 / 10** | **71 %** | 2 % | 1.0 / 1.1 % |
| | decrackled | 10 / 10 | 96 % | 17 % | 2.3 / 0.8 % |
| *Nada más* | before | 8 / 13 | 40 % | 1 % | 0.7 / 0.4 % |
| | **now** | **6 / 9** | **61 %** | 1 % | 1.2 / 0.7 % |
| | decrackled | 6 / 6 | 95 % | 10 % | 1.1 / 0.9 % |

Nothing below 2 kHz or above 4.5 kHz moves. The C++ stage's removal correlates
0.976–0.989 with its Python prototype's on the four, 0.5–0.8 dB less. Where
the body key is open it yields, and takes 2.8–3.5 dB less than the prototype
did after the body stage.

**Chosen by ear** from three versions:

* this stage with the guard from 0.4 to 0.6;
* the same, with the guard from 0.3 to 0.5;
* the first, with a lower carpet ramp of its own (80–110), which opens it on
  *El rodeo*. There the carpet key is open 46 % of the time (mean 0.19), and
  with that ramp the stage took 56 % of the clicks' energy.

The second was chosen. *El rodeo* keeps its shut stretches, 40–90 s and from
120 s on: no key built on this band told it from clean records.

### Clicks the interpolation gets wrong: the shape fit

`Config::shape`, on. `declick_cli --shape 0` turns it off.

**What was left.** Biagi's *Sentimiento gaucho* (TangoTunes shellac `.m4a`)
was aligned with a much cleaner vinyl copy. Its clicks are one shape: a
doublet, down then up 0.15 ms later, about 0.3 ms in all. On the long runs
(13–20 samples) that the big clicks make, the AR interpolation gets the second
lobe wrong, and 25–75 % of it is left. At 1.686 s the interpolation puts +0.033
where the clean copy has −0.012. Handing a click's split-off tail the strong
run's gain changes nothing; the estimate is the problem, not the gain.

**The stage.** Before the first pass looks for runs, each block's strongest
clicks are fitted as a sum of three fixed shapes, ±0.75 ms:

* a 1–8 kHz band-limited impulse, its derivative, and a measured click: the
  mean of *Sentimiento gaucho*'s strong clicks less the vinyl copy, at
  44.1 kHz (`kShClick`), taken to the rate by windowed-sinc interpolation;
* found as peaks of the matched filter on the model's residual, 3σ for two
  shapes (the three-shape threshold holds the same false-alarm rate);
* only where the input's 3–8 kHz power over 1 ms is 3.5 times its median over
  ±15 ms, the matched filter worked out within 0.25 ms of where that holds
  (`shapeSlackMs`), and its peaks taken only where it was worked out either
  side;
* and only where the fit stands out of the music over ±5 ms: its peak is 0.4
  (`shapePeak`) or more of the input's peak, or 4 (`shapeHigh`) or more of the
  input's 3–8 kHz RMS, the click's own span left out.

Only the fitted shapes are subtracted, from the window and from `m_in`, the
input the repairs take their clicks from. The music under a click stays,
where a repair replaces it. `m_dry` stays the input, for the mix and the delta.
The model is refitted on what is left, and the passes run as before.

**Why each guard.**

* **The peak.** Unguarded, the fit shaved loud transients on the clean *Olga*
  (−45.7 → −40.1 dB): fits that peak at 0.2–0.33 of a full-scale input. A
  click stands out of the music around it; a loud attack the shapes half fit
  does not. `shapeFitq` (the share of the residual the shapes explain) stopped
  that only at 0.3, where it stopped most of the fits on *Sentimiento gaucho*
  too; it is 0, off.
* **The high band.** On its own the peak guard turned away nine fits in ten at
  2:24–2:34 of *Sentimiento gaucho*: the music is twice as loud there and the
  clicks are not. Against the music's own 3–8 kHz, the same clicks stand out
  as much as anywhere in the song, and *Olga*'s attacks do not.
* **The gate's band.** It was 2–8 kHz, a fourth-order high-pass at 2 kHz.
  Where the orchestra is loud, its 1–2 kHz leaked into the gate's median and
  held the gate shut on a third of the clicks a prototype fitted (64 % passed
  against the prototype's 93 %); from 3 kHz, 92 % pass, at the same cost.
* **The slack.** The gate's band filter is causal, so its power comes a few
  samples after the click. Gated start by start, a click's true peak could
  fall just outside, and the edge of the open stretch passed for a peak, fitted
  off the click at half its size.

**What it did.** On *Sentimiento gaucho*, against the vinyl copy, whole song
and 2:24–2:34 (where it was heard to leave lots of crackle with only the peak
guard):

| | quiet left 2–3 / 3–4.5 / 4.5–7 kHz | taken 1–2 / 2–3 / 3–4.5 / 4.5–7 kHz | 2:24–2:34 left 2–3 / 3–4.5 / 4.5–7, events |
|---|---|---|---|
| before | 50 / 47 / 52 % | 0.8 / 2.0 / 2.3 / 3.3 % | 86 / 77 / 71 %, 2651 |
| peak guard only | 52 / 48 / 54 % | 0.5 / 1.6 / 1.9 / 2.7 % | 86 / 76 / 71 %, 2631 |
| now | 52 / 47 / 52 % | 0.5 / 1.6 / 1.9 / 2.7 % | 86 / 75 / 69 %, 2411 |
| prototype, heard as better | | | 85 / 73 / 68 %, 2383 |

It fits 51 clicks a second there. On the Demare and Calo songs with vinyl
references it is within two points of crackle left, and takes less music on
most (1–2 kHz on *Tristezas de la calle Corrientes* 2.6 → 1.3 %).

**The measured click.** The third shape was a doublet 0.15 ms apart, and the
prototype heard as better still led in quiet passages: by 6 / 5 points of
crackle left at 0.5–1 / 1–2 kHz, 2–4 points at 2–7 kHz. Its shape was the
measured click, whose slow tail after the second lobe carries most of a
click's 0.5–2 kHz; a band-limited impulse cannot follow it, and taking the
shapes' low edge from 1 kHz to 500 or 300 Hz changed nothing. With the
measured click in the doublet's place, quiet left at 0.5–1 / 1–2 / 2–3 /
3–4.5 / 4.5–7 kHz goes 98 / 78 / 37 / 36 / 46 → 91 / 72 / 35 / 35 / 46 %
(prototype 92 / 73 / 34 / 33 / 44), music taken equal or lower, 58 fits a
second instead of 51. On the Demare and Calo check set, whose clicks were not
measured, crackle left is equal or lower (1–2 kHz on the three Calo songs
3–5 points) and music taken equal or lower: the shape carries over. The rest
of the gap is the high-band guard: at 3 it is within half a point of the
prototype, but takes 0.15 dB more on Malvon, so it stays at 4.

**What it does not touch.** *Olga* −45.74 → −45.77 dB and Malvon −35.07 dB
unchanged, whole song; a handful of fits on each. On the Troilo .flac controls
removal moves by 0.02 dB at most; on the Cachirulo .m4a it takes 0.6 dB more.

**Cost.** 11 % more CPU on *Olga*, 17 % on *Sentimiento gaucho* (60 s, x64,
interleaved best of 11). The gate is worked out first, and a block where it
never opens stops there. A median of the gate is only sorted where a start
could clear it. The shapes go through the model and the matched filter through
the SIMD kernels. What is left is mostly the band filter, every sample, and on
a crackly record the fits.

### The first seconds: an early carpet key

**What was heard.** The first three seconds of *La vida me engaño* were
noisier than the rest. They were, as on any crackly record:

* **The body key** waits for a whole rate window: shut for the first second,
  then 0.15 → 0.70 by 2 s there.
* **The carpet key** may not open before 3 s, and its first rate needs a whole
  second of input.
* *La vida me engaño* has ~400 runs a second at 4σ from its first block; the
  key's ramp starts at 110. Until 3 s only the repair and a partly open body
  stage ran. The output kept 25 and 13 impulses a second at 7–10 kHz in its
  first two seconds, 0–2 after.
* `--carpet-min 1 --carpet-warm 0` mends the second second (13 → 1) but not the
  first.

**The change** (`Config::carpetFirst`, `--carpet-first`). From 0.25 s until
the key may open as before, its value is the rate of 4σ runs over everything
seen so far, against the thresholds at their warmest, half as high again.

* *La vida me engaño*, seconds 0 / 1: 7–10 kHz 25 / 13 → 4 / 0; 4.5–7 kHz
  14 / 19 → 12 / 14.
* *El abrojito*: 7–10 kHz 10 / 11 → 5 / 1; 4.5–7 kHz 22 / 22 → 17 / 10.
* It opens the stages the carpet key opens, the mid stage among them.

**Heard** on its own, on the first 20 s of seven songs.

**Records they must not touch,** the two changes together:

* **Bit-identical to the build before them:**
  * the *Malvón* master, *Olga*, *La tablada* (both copies);
  * 30 of 101 songs in all: the 98-song sample, the *Malvón* master, *Olga* and
    *El Marne*.
* **29 more change only in their first 3 s**, by −46 to −116 dB: records
  that start with crackle before the music. *El Marne* is one, by −90 dB.
* **The other 42** are those whose keys open. The most changed is *La payanca*
  (1954), −30.1 dB of 2–4.5 kHz beyond the build before, then *Cachirulo*
  `.m4a`, −32.2 dB. With the prototype, the worst single seconds on the
  sample were all clicks in near-silence, at run-ins and run-outs. Outside the
  sample, *Tu olvido* and *La vida me engaño* change most, −28.5 and −29.5 dB.
* **Out-of-range events** are the same on all 109 songs (with the four
  Pugliese, *Tu olvido*, *Llévame carretero*, *La vida me engaño* and *Cuatro
  compases*).
* **`scripts/declick_regress.py` passes** on the reference set and on six more
  (the four Pugliese, *La vida me engaño*, *Tu olvido*):
  * no new out-of-range event, no clamp count changed;
  * 0.08–0.4 dB more taken from the six, 0.05 dB from *Cachirulo* `.m4a`;
    *La tablada*, *Malvón*, *Olga* and *El Marne* as before.
* **Off:** `declick_cli --mid 0 --carpet-first 0` gives the build before them
  to the bit, at 22.05, 44.1, 48 and 96 kHz.

**Cost**, the builds taking turns, best of seven, on the Zen 4:

* **Where the keys stay shut,** +1 to +2 % (*Pa que bailen*): a flag per
  sample and per hop.
* **Where they open,** +4 to +5 %: *El abrojito* 106× → 101× with AVX2, 68× →
  65× with SSE2. The stage is 8 more bins of a transform the fine stage
  already makes, and a second guard.
* **Memory:** about 15 kB more a channel at 44.1 kHz.

### Crackle that comes at random: the Poisson key

**What was left.** On Pugliese's *El rodeo* (`.m4a`, 1943) the carpet key was
open on 46 % of the song, mean 0.19, and shut at 40–90 s and from 120 s on:
its count of 4σ runs reads ~104 at its 20 s median, under the 110–150 ramp.
Where it is shut, the fine, micro, mid and flank stages do nothing.

* **No threshold on the count works.** At 3.2–5σ alike, *El rodeo* sits
  among *Cornetín*, *Cachirulo* `.flac` and four 1950s sample songs, and
  *El Marne* reaches 108 after its opening.
* **How the runs arrive does.** A surface's crackle is a Poisson process; a
  clean record's runs bunch on the music's attacks. The Fano factor (variance
  over mean) of the 4σ runs' counts in ~11.6 ms bins, median over the song:

| | Fano factor |
|---|---|
| the four Pugliese `.m4a` | 1.01–1.09 |
| other crackly `.m4a` (Calo, D'Arienzo, Tanturi) | 1.0–1.1 |
| *La tablada* `.flac` | 1.33 |
| *Cachirulo* `.flac` | 1.57 |
| the *Malvón* master | 1.61 |
| *El Marne* | 7.6 |
| the songs whose count is near *El rodeo*'s | 1.5–6 |

**The key** (`Config::carpetFano`, on).

* **The bins** are the repair blocks, two of them at 88.2 and 96 kHz: a sum
  and a sum of squares per block.
* **Each second's Fano factor** goes into a ring beside the carpet key's
  rates, and its median over the window is the key's.
* **The ramp:** ramp(carpet value, 80 → 110) × ramp(median Fano, 1.4 → 1.2),
  with the carpet key's warm-up. The carpet key is the larger of this and its
  own ramp.
* **Its first seconds.** From 0.5 s (`fanoFirst`) until the key may open as
  before, it reads the counts so far, as the early carpet key does from
  0.25 s. Not from 0.25 s: over a quarter second the factor is rough, and
  *La tablada*'s `.flac` read 1.28 there, at its needle drop.
* **Off:** `declick_cli --carpet-fano 0` gives the build before it to the bit.

**On *El rodeo*:**

* **The key:** mean 0.19 → 0.72, open on 99 % of the song. Weakest at 90–130 s
  (0.1–0.6), where its runs bunch more.
* **Impulses over 8× their ±20 ms median, quiet third,** 2–3 / 3–4.5 /
  4.5–7 kHz: 27 / 28 / 15 → 22 / 22 / 11. Its vinyl copy has 5 / 6 / 4; the
  decrackled version 11 / 7 / 3.
* **Heard** as the base of every floor-stage audition (below), and asked for
  on by default.

**Records it must not touch** (the 101-song check set):

* **Bit-identical to the build before it:** 99 of 101, among them the
  *Malvón* master and `.m4a`, *Olga*, *El Marne*, *La tablada* (both copies)
  and *Cachirulo* `.m4a`.
* **The two that change:**
  * *Bien milonga* (1951, crackly; the carpet key already opened on 82 % of
    it): −53 dB, mean key 0.49 → 0.68.
  * *Cachirulo* `.flac`: −78 dB, on 4 % of its seconds.
* **Outside the set:**
  * *Llévame carretero* −41 dB, key 0.38 → 0.69;
  * *Nada más que un corazón* −45 dB, 0.50 → 0.92: its shut second half is
    open now;
  * *El abrojito* −47 dB and *El día de tu ausencia* −56 dB, their keys
    already open.

**Cost:** two additions and a multiply per repair block, and a second sort of
twenty numbers a second. On the Zen 4, best of 15 interleaved runs:

* ***Pa que bailen*, keys shut:** 168× → 162–166×. The same code from two
  builds spans that, so it is layout, not work.
* **Where it opens the stages,** what they cost wherever they open:
  * *El rodeo*: 135× → 104×, about *El abrojito*'s 94×;
  * *El abrojito*, already open: 94× → 92×.

### The floor under dense crackle: the crackle floor stage (on)

**What was heard.** After the key above, *El rodeo* and *El abrojito* still
kept "the majority of crackle": a fine sizzle, and a duller frying. The user
heard the decrackled version keep too much as well.

* **It is not separate clicks.** Over 8× their surroundings *El abrojito*'s
  output had no more impulses than its vinyl copy in any loudness third.
* **It is a floor.** At 600–900 runs a second the crackle merges into one.
  The fine, micro and mid clips cut only what stands above it.
  * *El abrojito*'s quiet passages were still 5 dB over its vinyl copy at
    4.5–7 kHz.
  * *El rodeo*'s were 12–13 dB over its copy at 2–7 kHz, and Declick took
    about 1 dB of that.
* **The body stage**, opened by the carpet key, took 6–9 % of the music at
  1–3 kHz against the copies. It stays on the body key.

**The stage** (`Config::crackleFloor`, `--crackle-floor 1`; named `denoise`
in b525d0a7f): a spectral denoiser on the finished output, under the key the
fine stage follows. It is named for what it lowers, the floor that dense
crackle forms. It is not a general denoiser: it leaves the hiss above 4.5 kHz
and everything under 1 kHz, and it rests where the key is shut. On by default
since its mid-song openings were fixed (below); it has no control of its own in
the plug-ins, and `--crackle-floor 0` turns it off in `declick_cli`.

* **Frames:** an STFT of ~23 ms (1024 at 44.1 kHz), hop N/4, Hann either side.
* **The band:** from `crackleFloorLo` (1 kHz), ramped in over the half octave
  below, to `crackleFloorHi` (4.5 kHz), faded out over the half octave below it.
  The top is left as it was; see below.
* **The noise floor:** per bin, the 10th percentile of its power, smoothed
  over 5 frames, every fourth frame over the last 6 s.
  * A record that fades in would hold that quantile under the floor for
    seconds. The TangoTunes transfers fade in over ~0.5 s from 30 dB under
    their crackle. So the window leaves out the first second, and before 2 s
    the first half of what it has.
  * It then cuts from the first half second: −12 to −32 dB of 1–10 kHz there
    on the four Pugliese, against a song median of −31 to −33 dB.
* **The gain:** Wiener's, on the power over 7 bins × 5 frames (~300 Hz ×
  29 ms) against 1.2 × the floor, never below −10 dB.
  * It rises at once and falls back over 60 ms, then is averaged over 3 bins.
  * A stretch of pure noise gets one steady cut rather than bins that
    twinkle: the cut flickers half as much as a per-bin gain does.

**How it was chosen.**

* **Folder 5:** strengths from −4 to −15 dB, with a per-bin gain. Only the
  gentlest was liked; the stronger ones "remove the crackle better, but
  instead I hear a typical digital denoiser leftover".
* **Folder 6:** the smoothed gain above, at −10 dB ("A").
* **Folder 7:** A's removal kept at −3 to −20 dB rather than removed. "−10
  or even less loud" was liked better: the sound should breathe.
* **The stage as first committed** was A, the full removal, over the whole
  band. Heard as taking crackle, but leaving high-frequency noise that feels
  unnatural on music played by people. So it was turned off.
* **Folder 8:** why the top sounds unnatural, and four remedies. The top
  left untouched was liked best, and that is now the default.
* **Folder 10:** the fix for a key that opens mid-song (below). Heard, and
  the stage was then turned on by default.

**The top band: left as it was.** The user: "usually it doesn't bother me,
but when the Denoise stage is applied, the leftover noise feels unnatural".

* **Nothing near 8 kHz is spared.** In quiet passages the stage took more
  from the top than from the middle (*El abrojito* −2 / −3.4 dB at 6.3–10 /
  10–16 kHz, −0.4 dB at 2–4 kHz).
* **It made the hiss uneven.** It stretched the 6–10 kHz band's level swings
  1.45 times on *El abrojito* and 1.27 times on *El rodeo*.
  * Most of that is a 2–8 Hz wobble, 1.44 → 2.14 dB rms on *El abrojito*
    (its vinyl copy 1.22).
  * It comes in patches of ~300 Hz, the gain's 7-bin box.
* **On steady noise the law alone does it.** With no clicks or music,
  Wiener's gain against 1.2 × the 10th-percentile floor puts the hiss at
  1.7–3.4 × that floor, where the cut runs from −10 to −4 dB. So the cut
  follows the hiss's own slow ups and downs and doubles the wobble (0.34 →
  0.63 dB at 8 kHz).
* **On the records it follows the crackle, not the music.** Above ~5 kHz the
  music lies ~15 dB under the crackle, so the stage cannot see it.
  * The vinyl copy stood in for the music, and the `.m4a` minus the copy for
    the crackle.
  * Against those, the 6–10 kHz cut opened 0.51 dB per dB of the crackle's
    level on *El abrojito* (0.30 on *El rodeo*), and −0.01 to −0.03 per dB of
    the music.
  * It let the crackle's swells through and pushed the bed between them down.
* **Anything that follows the top band's own level does this:**

| the top band (*El abrojito* / *El rodeo*) | dB opened per dB of crackle | swings stretched | music taken, 4.5–7 kHz |
|---|---|---|---|
| A | 0.51 / 0.30 | 1.45 / 1.27 | 2.2 / 1.9 % |
| a flat −6 dB up to 6× the floor, open by 24× | 0.39 / 0.19 | 1.38 / 1.23 | 2.1 / 1.7 % |
| the same on a 9-frame median, opening from 16× | 0.17 / 0.10 | 1.19 / 1.14 | 3.7 / 2.8 % |
| one smooth shelf (below) | 0.17 / 0.08 | 1.17 / 1.08 | 2.1 / 2.0 % |
| a steady −4 dB | 0 | 1.00 | 39 / 29 % |
| left as it was | 0 | 1.00 | 1.6 / 1.5 % |

  A steady cut is a treble cut. So the top is left as it was:
  `crackleFloorHi = 4500`; `0` cuts to Nyquist as before.

**The top shelf (`crackleFloorShelf`, since removed).** Heard in folder 8, and
not chosen.

* Above `crackleFloorHi`, every bin gets one gain, so nothing moves in patches.
* The gain is −4 dB (`crackleFloorShelfGain`) while the 4–11 kHz band as a whole
  sits near its floor: its power summed, over its floors summed.
* It opens to 0 dB from 5 to 40 times that (`crackleFloorShelfX0`, `X1`),
  smoothstep in the log.
* The ratio is a median over 9 frames, so a crackle shorter than ~25 ms
  cannot open it. The 9 frames end two hops after the cut frame, so it opens
  up to 12 ms late; the prototype's were centred on it.
* It rises at once and falls back over 150 ms (`crackleFloorShelfRelease`).
* It takes 1.5–2.4 dB of the top's crackle, with the music taken as A's.
  But it still opens a little with the crackle, as the table shows.
* With it, the floor is followed up to 11 kHz, and the stage holds and costs
  about what the full band did.

**Against the prototype heard.**

* The prototype looked ±3 s around a frame for the floor; the stage looks
  behind only.
* Over the whole song that takes 0.7 dB more than A did on *El rodeo* and
  *El abrojito*, removals correlating 0.90 and 0.78.
* Against the same prototype looking behind only: within 0.06–0.15 dB,
  correlating 0.96–1.00.

| | *El rodeo*'s quiet floor over its vinyl, 2–3 kHz | *El abrojito*'s music taken, 1–2 / 2–3 / 3–4.5 kHz |
|---|---|---|
| without it | 12.2 dB | 0.4 / 1.1 / 1.2 % |
| with it (A) | 9.6 dB | 1.1 / 1.9 / 1.7 % |
| the decrackled version | 11.1 dB | 0.3 / 2.3 / 0.8 % |

**What it holds.**

* **Buffers** only while it is on.
  * It needs ~0.4 MB a channel at 44.1 kHz, mostly the floor's history and
    its sorted copy, for the ~100 bins up to just past 4.5 kHz.
  * It needs ~1 MB with the shelf, and ~2.2 MB with no upper edge.
  * Turning it, its edge or its shelf on or off is a structural change,
    through `configure()`.
* **Latency:** `bodyDelay(crackleFloorN)` more, 1536 samples at 44.1 kHz.
* **Off,** the output is the build before it to the bit; so is it with
  `crackleFloorHi = 0`.
* **Where its key is shut,** it cuts nothing, and the output is its input
  to the bit.
* **The floor's history under a shut key.** It used to wait there, and the
  five frames started again when the key opened.
  * So where the key opened again mid-song, the first-second rule threw out
    the little the history held, and the window was a few frames of the
    music. *Y_ahora*'s key opens at 82 s, on loud music: for ~3 s the stage
    took up to 0.8–1.5 dB of 1–3 kHz, following the music's spectrum (r 0.80).
  * Now, after the stream's first two seconds, the history goes on under a
    shut key. Of every 16 hops the first five are transformed and their
    power taken once, standing for the four values the open key would have
    taken. The window is still the last 6 s, whatever the key did.
  * The first-second rule leaves out only what was taken in the first two
    seconds. Those seconds are as before: the history waits there while the
    key is shut, so a record's start is cut as it was.
  * **A full window alone took more** where the old history had kept a lower
    floor from the key's last opening: *x_carpeta* at 54 s −91 → −44 dB re
    its 1–4.5 kHz band, *s12*, *s33*, *s39*, *s64*. A floor taken under the
    open key is the crackle's; one taken while it was shut may be the
    music's.
  * **So after the key opens again,** until the window has been renewed under
    it (6 s of open key), the floor is no higher than it stood when the key
    last shut. Not in the first two seconds.
* **The 109-song check set with the stage on** (the 101 songs, the four
  Pugliese and four voice / crackle songs):
  * With the stage off, all 109 are the build before to the bit.
  * With it on, 72 are the build before to the bit. Among them are *El rodeo*,
    *El abrojito*, the other two Pugliese, *El Marne*, *Malvón* (both), *Olga*
    and both *La tablada*. Over the whole song, none of the 37 that change
    loses more than before. *Bien milonga* and *A tu recuerdo* lose the same
    to 0.01 dB, and the others less.
  * Removal at 1–4.5 kHz in the 3 s after a mid-song opening, dB re the band,
    before → now:

| | at | before | now |
|---|---|---|---|
| *Y_ahora* | 83 s | −12.9 | −76.6 |
| s54 | 78 s | −13.2 | −29.8 |
| s78 | 70 s | −16.4 | −47.7 |
| s32 | 62 s | −17.0 | −41.7 |
| s51 | 10 s | −17.3 | −42.7 |
| s13 | 220 s | −19.9 | −47.4 |
| s72 | 69 s | −22.3 | −37.4 |
| *Cadenas* | 31 s | −23.5 | −32.1 |
| s21 | 31 s | −25.4 | −52.1 |

  * The most any second takes over before: +3.7 dB (s76 at 127 s, at
    −45 dB re the band).
* **CPU when on** (Zen 4, realtime multiples, best of 7 interleaved):

| | off | on (top untouched) | + shelf | no upper edge |
|---|---|---|---|---|
| *Pa que bailen*, 60 s (keys shut) | 184× | 171× | 170× | 170× |
| *El rodeo* | 113× | 89× | 74× | 58× |
| *El abrojito* | 96× | 81× | 67× | 54× |

  Most of the cost is the floor's history: a sorted window of 259 values per
  followed bin, every fourth hop. Its going on under a shut key costs 1–3 %
  (*Pa que bailen* 172 → 170×, *Y_ahora* 187 → 181×, measured beside the
  build before; *El rodeo*, whose key stays open, 94× both).

### The first seconds from the read-ahead: a warm start (foobar2000)

**What was heard.** With the crackle floor stage on, the first second of
*Jueves* (D'Arienzo, 1955) and *Ahora no me conoces* (Pugliese, Moran, 1952)
lost music to it: −35 and −66 dBFS rms in what the stage took, raised 12 dB.
Two things start cold:

* **The carpet key's early value.** On a loud opening it opened for about half
  a second on clean records (*Jueves*, *Ahora no me conoces*, *El Marne*).
* **The floor's history.** It held a few frames, so the floor was the music's
  own.

foobar2000 and VirtualDJ can read a track ahead, so there is no reason to
start cold.

**The change** (`Channel::beginWarm()` / `endWarm()`, `Config::warmSeconds()`,
`declick_cli --warm-start`). The host runs the coming seconds through the core
with its output held back, then the track from its start:

* **The carpet key**, with its Poisson ramp, is what it is after
  `warmSeconds()`, and stands still until the track is past that point. From
  there it is the cold stream's to the bit.
* **The crackle floor** of the warm pass is a ceiling for that long. The
  stage's own history starts as before, so a quiet opening keeps its lower
  floor.
* **The warm length** is 7 s with the stage on, 4 s with it off.

**What was tried and dropped:**

* **The history seeded and frozen.** It took 6–9 dB more from *El abrojito*'s
  quiet opening bars than the stage takes at that level elsewhere in the song:
  the floor came from the loud seconds after them.
* **The flank repair's spectrum kept.** It brought the pad in from the first
  sample. On clean band-limited records the repair then acted on the body
  key's opening at the needle drop: 3–25 dB more at 4.5–10 kHz in the first
  five seconds of 15 songs.
* **A 4 s warm pass with the stage on.** It took 3–17 dB more than the 7 s one
  in ~30 opening seconds of the set.

**Records they must not touch** (the 109-song check set, first 7 s of each):

* **Without the calls**, all 109 are the build before to the bit, stage off and
  on.
* **No second takes 3 dB more** than the cold start, at 1–4.5 or 4.5–10 kHz.
  With the stage on, 40 seconds at 1–4.5 kHz (28 songs) and 10 at 4.5–10 kHz
  (10 songs) take that much less.
* **The floor stage alone** takes more in 13 seconds of four crackly records
  whose key is now open from the first sample: *El rodeo*, *Llévame
  carretero*, *Homero*, and one second of *Estás conmigo* at about −60 dBFS.
  It takes less in 83 seconds of 43 songs. Where the cold start took more than
  −30 dB re the band (63 seconds), the median is now −32.5 dB.
* **Bit-identical outright:** 39 of 109 with the stage on, the *Malvón* master
  among them. Past the warm length, the rest are the cold stream's to the bit
  with the stage off. With it on they are within −55 dB, because the history takes
  every fourth frame, counted from a different start.
* **Against the vinyl copies** of *El rodeo* and *El abrojito*, the music taken
  in the first seconds is at most what it was.

**What it costs.** The warm pass is the pipeline run over the warm length,
before the track's first sample. With the stage off it runs only the repair.
Per channel, Zen 4:

| | AVX2 | SSE2 | 2014 MacBook Air (estimate) |
|---|---|---|---|
| stage on, 7 s, clean record | 32–34 ms | 61–64 ms | ~0.45 s |
| stage on, 7 s, *El rodeo* / *El abrojito* | 60–64 ms | 96–100 ms | ~0.7 s |
| stage off, 4 s | 17–18 ms | 33–34 ms | ~0.23 s |

Decoding 7 s of MP3, AAC or FLAC takes 3–7 ms.

**In foobar2000** (`warm_hold.h`). After a track change, a seek (`flush()`)
or a new format, the component holds the incoming chunks instead of pushing
them; foobar2000 keeps decoding while a DSP returns nothing. Once
`warmSeconds()` are in, or the track ends first, it runs the warm pass over
them, a thread per channel, and then hands them to the core as the stream,
twice what comes in per chunk, so the stall is the warm pass alone and the
extra delay drains away over the next seven seconds. `get_latency()` counts
what is held. The hold is a ring of `warmSeconds()` per channel, made with
the worker threads at a format change (2.5 MB stereo at 44.1 kHz, 10.8 MB at
192 kHz); nothing after that allocates, and the output is the core's warm
start to the bit, however the chunks fall and on whichever thread a channel
warmed (`declick_rt_verify`). Advanced Preferences → Playback → Declick turns
it off, for machines where the stall is felt, and can log each stall to the
console.

The stall at a track start, stereo, 44.1 kHz, chunks of 4096, Zen 4, best of
seven (median in brackets where the two threads differ):

| | AVX2, 1 thread | AVX2, 2 threads | SSE2, 1 thread | SSE2, 2 threads |
|---|---|---|---|---|
| clean stereo record | 60 ms | 34 ms (43) | 117 ms | 65 ms (110) |
| *El rodeo*, crackly, mono in both channels | 115 ms | 64 ms (93) | 190 ms | 99 ms (176) |

On the 2014 MacBook Air that is roughly 0.25–0.8 s for a clean record and
0.45–1.3 s for a crackly one, before 7 s of decoding (another 25–50 ms). Two
threads halve it only when they land on two cores: the median is often close
to one thread's, which looks like the worker sharing a core with the playback
thread. Holding the seven seconds costs nothing measurable; handing them back
costs twice the usual CPU per chunk for those seven seconds.

**Gapless track changes.** foobar2000's output buffer (Preferences → Playback
→ Output → Buffer length, 1000 ms by default) is full when the next track's
first chunk arrives, and the stall is played out of it. A stall shorter than
the buffer is not heard; on the Air a clean record warms inside 1000 ms,
and a crackly one may not, which is a dropout at the track change. A 2000 ms
buffer covers every estimate above; turning the warm start off is the other
way. A seek is a fresh start and has nothing in the buffer to hide the stall
behind: it waits for it.

VirtualDJ can read the coming seconds with GetSongBuffer, but not inside the
audio callback's time: there the pass runs on a worker thread, and every
restart after it in that record (a cue, a seek, a loop) starts with what it
learned, through `Channel::saveWarm()` and `adoptWarm()`. The first play of a
record starts cold and is swapped onto a pair the worker has caught up with
it, a second or so in (two to three on the Air). See the VirtualDJ README's
[warm start section](../vdjplugin/README.md#warm-start-declick). The VST and
AU cannot read ahead and stay as they are.

---

## Dehum parameters

Add *Dehum (line detection)* to the chain and press **Configure selected**.

| | |
| --- | --- |
| **Sensitivity** | How prominent a line has to be. 0 → 22 dB, 1 → 10 dB; the default 0.5 is 16 dB. Raise it if hum survives, lower it if music is being touched. |
| **Bandwidth** | Half width of each notch, 0.1–5 Hz. Wider catches a drifting line at the cost of more music around it. |
| **Search to** | Top of the range searched automatically, 40–500 Hz, default **100 Hz**. The bottom is fixed at 16 Hz. Detection weakens over the top 20 Hz of whatever you set — prominence is measured against a baseline 20 Hz either side, and that window runs out of room at the edge of the band — so treat the usable ceiling as roughly 20 Hz under the setting, and reach for **Frequency** for a line above it. |
| **Harmonics** | Multiples of each detected line to cancel as well, 1–8, default **1**. Locked to exact multiples of the fundamental, not tracked separately. Raise it for a genuine mains buzz; leave it alone otherwise, because a notch removes the coherent part at its frequency whether or not that part is hum — see below. |
| **Frequency** | 0 = detect automatically. Anything else pins the fundamental there and turns the detector off. **This is the control to use if you already know the frequency**, because it acts immediately where detection needs a few seconds. |
| **Rumble** | 4th-order Butterworth high-pass, default **67 Hz**, 0 = off. Broadband low-frequency noise is a *different defect* from hum — see below — and this is the control for it. 40 Hz was the cautious end: about 4 dB out of the 32–45 Hz band, nothing above 90 Hz touched, safe on material that has real bass — but it leaves rumble audible on the transfers that have it. **67 Hz goes after it properly.** It takes the bottom octave of a double bass with it, so wind it back towards 40 where the low end is worth keeping. |
| **Dry/Wet** | 0 bypasses, bit-exactly. |
| **Output only what is removed** | Delta monitoring, for testing: outputs what the notches take out, which should be the hum and nothing else. Rumble is deliberately left out of it — see [Delta monitoring works for the notch, not for Rumble](#delta-monitoring-works-for-the-notch-not-for-rumble). Saved with the preset, so untick it when done. foobar2000 only. |

Latency is **zero**: the detector reads the signal but does not sit in the path,
so the component processes in place and needs no FIFO. Throughput is **320×
realtime** on x64 and 310× on x86, measured end to end over 177 s of 44.1 kHz
mono including file I/O. On the Macs, over the same 177 s (*La tablada*):

| | M1 | 2014 MacBook Pro |
| --- | --- | --- |
| mono, defaults | 656× | 233× |
| **stereo, defaults** | **328×** | **112×** |
| stereo, base probe kernel (`--no-avx`) | — | 101× |
| stereo, 8 harmonics, search to 500 Hz | 138× | 61× |

The M1 is best of seven, its runs within 1 % of each other bar one outlier. The
MacBook Pro, a Haswell i5-4258U, is best of fifteen on a machine that was not
idle, and its medians run 5–10 % lower. The AVX probe kernel is x86 only, so
the M1 runs the base one. AVX is worth 11 % on the Haswell. The heaviest
setting, all eight harmonics over the widest search, costs 1.8–2.4× the
defaults. Memory is **410 kB per channel** at 44.1 kHz, 387 kB at
96 kHz and 196 kB at 192 kHz, fixed at `configure()` and independent of the
parameters.

### The material this was calibrated on

Two 78 rpm transfers with audible hum, each paired with a **transfer of the same
performance from a different disc** that has none — the same ground-truth trick
the declicker uses. Measuring the pairs first turned out to matter, because it
showed that the two files do not have the same defect:

| ⅓-octave band | cachirulo m4a | its control | la tablada m4a | its control |
| --- | --- | --- | --- | --- |
| 32–45 Hz | **−41.0** | −59.0 | **−39.6** | −67.3 |
| 45–63 Hz | **−36.9** | −58.4 | −48.9 | −64.2 |
| 63–90 Hz | −32.0 | −42.5 | −43.4 | −56.0 |
| 125–250 Hz | −29.1 | −30.7 | −28.1 | −28.7 |

*La tablada* has a genuine hum: a single line at **41.3 Hz**, 38 dB above the
clean transfer at that bin and stable to ±0.02 Hz across the whole side.

*Cachirulo* is mostly a **different defect**. Its excess is smooth across
33–62 Hz — turntable rumble, not hum — and that is what dominates the sound.
There is also a faint coherent line at **38.94 Hz** (confirmed by a heterodyne
coherence probe: 0.51 against a local background of 0.14–0.27, and 0.16 at the
same frequency in its clean control), but it is worth almost nothing next to the
rumble:

| cachirulo, 32–45 Hz | level | gain |
| --- | --- | --- |
| original | −39.5 dB | |
| cancelling the 38.94 Hz line | −41.2 dB | 1.7 dB |
| **Rumble at 60 Hz** | **−52.4 dB** | **12.9 dB** |
| both | −53.4 dB | 13.9 dB |
| its clean control | −57.4 dB | |

Prominence cannot find that line and cannot be tuned into finding it. Its
amplitude sits at the level of the pedestal it stands on, so it is not prominent
at any baseline geometry — measured at four, including guard-banded ones, its
duty cycle above threshold is 0.0% in every case. That is what the
[coherence detector](#the-coherence-detector) exists for. **But note the size of
the prize: for this transfer, Rumble is the control that matters, not either
detector.**

Note also that neither line is at 50 or 60 Hz, and the two differ from each
other. A speed-corrected transfer moves whatever was on the disc along with the
music, so **a fixed-frequency notch is the wrong instrument here** and the
detector is not a convenience.

### Why detection is a duty cycle test, not a threshold

The obvious design — flag whatever stands out from the local spectrum — does not
work, and the reason is worth recording because it looks like it should.

Prominence measured against a ±20 Hz median baseline, over the whole of each
side:

| | at the hum line | loudest momentary peak anywhere in 16–150 Hz |
| --- | --- | --- |
| la tablada (hum) | median **19.0** dB, 5th pct 14.2 | 28.2 |
| la tablada (control) | median 0.1 dB | **21.7** |
| cachirulo (control) | — | **22.3** |

**The distributions overlap.** Catching the line continuously needs a threshold
around 14 dB; never firing on a hum-free control needs one above 22. There is no
value that does both, and an early revision that used one duly detected a
sustained bandoneón E4 at 329 Hz as hum — *in both transfers of the same piece* —
and cancelled it.

What separates them is not level but persistence. At a 16 dB threshold:

| | best duty cycle any frequency reaches | highest evidence score |
| --- | --- | --- |
| la tablada (hum) | **87.0%** at 41.05 Hz | **48** (saturated) |
| la tablada (control) | 10.2% | 16 |
| cachirulo (rumble) | 2.1% | 9 |
| cachirulo (control) | 3.3% | 13 |

So a candidate scores +1 per hop it is prominent, plus a quarter for each dB
past the threshold, and −2 per hop it is not. Anything below a two-thirds duty
cycle drifts to zero and stays there; the real line saturates. Cancellation
starts at 24, which sits in the gap with a factor of 1.5 on either side. At
1 up and 1 down instead, a 50% duty cycle is a random walk that reaches any
threshold eventually.

Two consequences worth knowing:

* **It takes about 5 seconds of a track to engage** — 1.5 s to fill the analysis
  window, then ~19 hops of evidence — and considerably longer for a line only the
  coherence route can reach. That is the price of the test. On a file it is paid
  off-thread before playback reaches it; see *Scouting the file before the
  detector gets there*. Everywhere else it is what **Frequency** is for.
* Once engaged, a line is held on much weaker evidence than it took to establish
  (6 dB lower, and a miss costs 0.25 instead of 2). Without that the line was
  acquired and dropped **ten times** over one side and removal was intermittent.
  Hum does not come and go; a gap in the evidence means the music got loud.

### The coherence detector

There is a second way in, for lines prominence cannot see. A magnitude spectrum
cannot tell a coherent tone at level X from noise at level X — only phase can —
so each candidate also gets two heterodyne integrators at the same frequency and
very different bandwidths, **0.15 Hz** and **3 Hz**. A continuous tone drives
both to the same complex amplitude; noise and separate note attacks arrive with
independent phases, so the narrow one averages them away. The ratio
`|w_narrow| / |w_wide|` is then a tonality measure that does not care how loud
the surroundings are. Its two extremes are analytic and are pinned in the tests:

| | pure tone | white noise |
| --- | --- | --- |
| measured | **0.993** | **0.222** |
| analytic | 1 | `sqrt(0.15/3)` = 0.224 |

Three things about it were not obvious, and each was found by being wrong first.

**The narrow bandwidth has to be much narrower than the notch.** A first attempt
used the notch's own 1 Hz, whose time constant is 0.16 s — shorter than a musical
note, so it tracked each note individually and scored a recurring note **0.92**,
indistinguishable from a real hum. At 0.15 Hz the time constant is 1.1 s and the
same note scores 0.41.

**The ratio has to be accumulated over tens of seconds.** Read over one hop it
returns 0.9 or better for everything on real transfers, hum-free ones included,
because turntable rumble is a slowly wandering narrowband process that is
perfectly tone-like when you only look at it for a fifth of a second. It stops
looking like one over twenty, and a hum does not. The sums therefore decay with a
20 s time constant rather than being reset per hop.

**It only works below about 80 Hz**, and that ceiling is not a tuning choice:

| | max coherence below 80 Hz | max 80–150 Hz |
| --- | --- | --- |
| la tablada (hum) | **0.571** | 0.394 |
| cachirulo (hum) | **0.475** | 0.621 ← *a bass note* |
| la tablada (control) | none | 0.378 |
| cachirulo (control) | none | 0.323 |

Below 80 Hz the separation is total: both hum transfers score high and neither
control has a single surviving coherent probe. Above it the order reverses — a
sustained B♮ at 123.5 Hz out-scores both real hums — because **a held musical
note is a coherent tone** and nothing computed from the signal can say otherwise.
Prominence still searches the whole range; only this second route is capped.

The measurement needs the frequency to about 0.15 Hz, far finer than a spectrum
can nominate, so this only became possible once the frequency tracker existed:
the nominee is parked on a probe, the tracker locks it, and the ratio is read at
the locked frequency. Candidates for it are the loudest local maxima of the low
band whether or not they are prominent — prominence decides nothing here, it only
suggests where to look.

**What it is worth, honestly.** It does what it was built to do: on the rumbly
reference it finds the 38.9 Hz line that prominence cannot, and it confirms
nothing at all on either hum-free control. But the line is so far under the
rumble that removing it moves that bin by **0.4 dB**. It will matter on material
where a buried line is stronger; it does not rescue this one, and Rumble is what
does.

### Why the window is 1.5 seconds

A coherent line's FFT peak grows with the window length while noise and music
grow with its square root, so prominence separates them 3 dB better per
doubling. Measured on the reference transfers:

| analysis window | the 41.3 Hz line | loudest music peak |
| --- | --- | --- |
| 0.37 s | 24.7 dB | 19–22 dB |
| 0.74 s | 26.8 dB | 19–22 dB |
| **1.49 s** | **28.2 dB** | 19–22 dB |

The line gains 3.5 dB, the music gains nothing. The same length gives the
0.67 Hz bins the notch has to be placed on. Hopping more often does *not* make
the detector decide sooner — successive frames overlap more, so they are that
much more correlated and the evidence counter has to be raised in step. That was
measured at a sixteenth of the window and bought nothing but FFTs.

### The notch, and why the tracker matters more than anything else

Each line is removed by heterodyning it to DC, lowpassing to recover its complex
amplitude, and subtracting it back:

```
z = x * exp(-i*theta)
w += lam * (z - w)                 lam = 2*pi*bandwidth/rate
y = x - 2*Re{w * exp(i*theta)}
```

`theta` is a deterministic phase ramp, so nothing here adapts on the signal:
it is a linear time-invariant notch that cannot ring or go unstable, and away
from the line the response is `|d|/sqrt(d² + bandwidth²)` — a partial 5 Hz away
loses **0.24 dB** at the default 1 Hz. Its depth is not infinite: heterodyning
also puts an image at −2f₀, and what the one-pole passes of that lands back on
f₀, so the floor is `bandwidth/(2·f0)`, i.e. −40 dB at 50 Hz. Measured at 0.3, 1
and 3 Hz the depths are −50.5, −40.0 and −30.5 dB, matching that to the decimal.

Because the notch is narrow, **placing it is the whole problem**. The detector's
own estimate is good to a few tenths of a hertz, and a few tenths is enough to
ruin it. So the frequency is refined from the rotation of `w`: an error `d`
makes it rotate at `d` Hz, so reading its phase advance measures `d` directly.

| starting error | without tracking | with tracking |
| --- | --- | --- |
| 0.2 Hz | −14.7 dB | — |
| 0.5 Hz | −7.2 dB | locks to 0.009 Hz, **−91.7 dB** |
| 1.2 Hz | −3.1 dB | locks to 0.003 Hz, **−38.4 dB** |

Two details that are not optional:

* **The interval has to be long.** A quarter second turns a 0.1 Hz error into
  0.157 radians. Done per block instead it is 0.004 radians, which is noise —
  and the first prototype duly random-walked the notch 2 Hz off the line and
  left the hum untouched.
* **It has to stop when there is nothing to read.** In a run-out groove the
  weight collapses to noise and the reading becomes a random walk again, so the
  weight is compared against its own recent peak and the tracker freezes below
  30% of it.

### What it does to the reference material

Median-over-time spectrum around the line, at the defaults:

| Hz | original | processed | pinned at 41.3 |
| --- | --- | --- | --- |
| 40.71 | −46.1 | −52.8 | −54.7 |
| 41.05 | −41.4 | −52.8 | −57.3 |
| **41.38** | **−39.6** | **−52.9** | **−57.1** |
| 41.72 | −42.9 | −51.9 | −53.3 |
| 42.06 | −47.4 | −52.1 | −52.9 |
| 43.40 | −55.1 | −56.3 | −56.4 |

The line is gone: what is left at 41.38 sits at the level of its neighbours.
It cannot go lower than that, because the rumble pedestal underneath it is at
about −52 — which is also why a naive "reduction at the line" figure tops out
around 13 dB no matter how good the cancellation is. Pinning the frequency does
slightly better than detecting it (the notch is exactly on the line from the
first sample) and dips below the pedestal.

On the **controls** — the hum-free transfers of the same two performances — the
detector confirms **no lines at all**, and with `Rumble` at 0 the output is
bit-identical to the input. That is the result that matters most: a restoration
tool that fires on clean material is worse than none. (At the default `Rumble` of
67 Hz the output is of course not bit-identical — the high-pass runs regardless
of what the detector decides.)

On **cachirulo**, whose defect is rumble rather than a prominent line, the
detector finds nothing and `Rumble` does the work:

| band | input | with Rumble 60 Hz | its clean control |
| --- | --- | --- | --- |
| 20–32 Hz | −52.1 | **−72.1** | −58.3 |
| 32–45 Hz | −39.5 | **−52.4** | −57.4 |
| 45–63 Hz | −36.3 | **−41.3** | −56.7 |
| 63–90 Hz | −33.2 | −33.9 | −43.7 |
| 90 Hz and up | unchanged | unchanged | |

### Harmonics you do not have cost music

**Harmonics** defaults to 1, and it used to default to 4. That was set while an
earlier design gated each harmonic by its own tonality measure; the gate was
removed and the default was not revisited, which is exactly the kind of thing a
measurement catches and reading the code does not.

Multiples of a low fundamental land in the musical register, and a notch there
removes the coherent part at its frequency whether or not that part is hum. On
the rumbly reference with two detected lines and 4 harmonics, six notches fell
between 80 and 200 Hz and took **84% of everything removed** with them — roughly
a tenth of the energy in that band — to gain 0.8 dB at the line. Dropping to one
harmonic changed the same file's removal profile completely:

| share of removed energy | 36–50 Hz | 50–80 Hz | 80–200 Hz |
| --- | --- | --- | --- |
| la tablada, 1 harmonic | **99.0%** | 0.2% | 0.5% |
| cachirulo, 4 harmonics | 4.2% | 7.5% | **84.1%** |
| cachirulo, 1 harmonic | 30.0% | 55.8% | 13.0% |

A real mains buzz does have harmonics and they are worth removing. An
off-frequency disc drone generally does not.

### Delta monitoring works for the notch, not for Rumble

`dehum_cli --delta` writes what was removed instead of what was kept, which is
the quickest way to hear whether a repair is taking music with it. That reading
is sound for the **notch** and misleading for **Rumble**, and the difference is
structural rather than a matter of degree.

What a notch removes really is confined to the line. Measured on the reference
transfer, the share of the removed energy by band:

| 36–42 Hz | 42–60 Hz | 60–200 Hz | 200 Hz–2 kHz | above 2 kHz |
| --- | --- | --- | --- | --- |
| **79.1%** | 11.1% | 7.1% | 0.32% | **0.00%** |

Rumble's delta, by contrast, contains the whole spectrum, and that is unavoidable
for any minimum-phase high-pass. What gets removed is `1 - H`, and at high
frequency `H → 1` does *not* make `1 - H` fall at the filter's own slope: for a
4th-order Butterworth the numerator's leading term is `a₃s³` against `s⁴` in the
denominator, so

```
|1 - H| ≈ a₃ · fc / f          a₃ = 2.6131
```

— **6 dB per octave whatever the order**. At a 60 Hz corner that is −16 dB at
1 kHz and −32 dB at 6 kHz, so the delta is full of music and sounds alarming.

It is phase, not amplitude. The output's magnitude response measured against its
input over a whole side:

| | 30 Hz | 60 Hz | 125 Hz | 250 Hz | 1 kHz | 6 kHz | 10 kHz |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Rumble at 60 Hz | −23.5 dB | −3.3 dB | −0.01 dB | **0.00** | **0.00** | **0.00** | **0.00** |

Nothing above 125 Hz is touched. **Judge Rumble by the result, not by its
delta**; judge the notch either way.

### Scouting the file before the detector gets there

Acquisition time is a property of the material, not of how the caller feeds the
core. The line is confirmed at a fixed point in the stream, so handing over
longer blocks does not bring it forward — measured against a synthetic 40 dB
line it lands at 2.97 s whether the caller pushes 4 s or 20 s. What moves it is
prominence, and therefore which route finds it. On the 78 rpm transfers, at
44.1 kHz, with the defaults:

| | prominence | confirmed at |
| --- | --- | --- |
| a line the prominence route can see | 19.6 dB | ~9 s |
| a line down in the rumble | 7.8 dB | ~43 s (coherence route) |
| a second line on the same transfer | | ~67 s |

Forty-three seconds is a long time to play a record with the hum still in it,
and the coherence case is not an edge case — it is the transfer this component
exists for. But a file player is not obliged to wait. The whole file is on disk,
so `dehum_scout.cpp` opens a **second decoder on a worker thread** the moment a
track starts, reads the first **60 seconds** through a scratch `dehum::Channel`,
and hands whatever it finds to `Channel::adopt()` on the playback thread. In
practice that lands within a couple of seconds of the track starting — the scan
costs about 1.5 s of one core — so removal is engaged from roughly the first
second of audio rather than from the 43rd.

A few things about it are deliberate:

* **The scratch channel runs at the file's own sample rate**, which saves
  resampling: a hum sits at the same frequency in Hz whatever rate you measure it
  from. `adopt()` converts its search range into Hz for exactly this reason, so a
  report from a 78 kHz transfer lands correctly in a channel running at 48.
* **The scout downmixes to mono.** Hum is common mode, so one summed channel
  finds it for a fraction of the work of running a detector per channel — and all
  live channels adopt the same lines.
* **Adopted lines arrive at the activation threshold, not saturated.** The
  detector keeps running: it tracks them, drops them again if the evidence is not
  really there, and can still find lines the scout missed. A line adopted on
  somebody else's word is given up in about the time it took to believe it,
  instead of the half minute a saturated score would buy.
* **Nothing is ever carried between tracks.** Each record has its own hum, so the
  channels are reset on every track change and a fresh scout is started. This is
  also a fix in its own right: before, consecutive tracks in a playlist inherited
  the previous record's lines.
* **Local files only.** A stream has no opening to read ahead of, and fetching a
  remote file a second time would not arrive before the detector got there
  anyway.
* **Not while Frequency is pinned.** A frequency the user set by hand outranks
  anything found by searching, and `adopt()` refuses lines in that case regardless.

When it finds something it says so once, in the console:

```
Dehum: scouted C:\...\la tablada.flac - 41.284 Hz (prominence), 78.102 Hz (coherence)
```

Only the foobar2000 component does this. The VST has no file to read — the host
gives it a stream and nothing else — so there the acquisition time stands, which
is what **Frequency** is for.

### What Dehum will not do

* **Nothing happens for the first few seconds of a track** *unless* the scout
  found the line first — which it can only do for local files, in the foobar2000
  component, with **Frequency** on automatic. Everywhere else the acquisition
  time above applies, and **Frequency** is the answer when the hum is known.
* **A hum whose fundamental is weak but whose harmonics are strong** — a buzz
  rather than a hum — will not be found by searching for the fundamental. Pin it
  with **Frequency** and raise **Harmonics**.
* **Detection is per channel.** On a stereo transfer where the hum is common to
  both, each channel finds it independently and they may engage a hop or two
  apart. Nothing has been measured about whether that is audible.
* **Coherence detection stops at 80 Hz**, because above it a sustained musical
  note is a coherent tone and scores higher than either real hum. A buried line
  above 80 Hz has to be pinned with **Frequency**.
* **Only tested on mono 78 rpm transfers**, like the declicker.

---

## DeCrackle parameters

Reachable through **Preferences → Playback → DSP Manager**; add *DeCrackle
(Airwindows)* to the active chain and press **Configure selected**.

| | |
| --- | --- |
| **Filter** | How dark the audio that replaces a click is, from bass-only to full range. Tune it to hide the transitions — full bass is not always the best setting. |
| **Window** | Width of the detection window, from very narrow to very wide. Also what the latency scales with. |
| **Thresld** | Lower catches more. Be careful about it triggering on actual music; that sounds bad. |
| **Surface** | 0 is off. Higher settings apply increasing treble filtering aimed at general surface noise in quiet passages. Not a plain lowpass — it responds to micro-crackle rather than to underlying high frequencies. |
| **Dry/Wet** | At exactly `0.000` this becomes **delta monitoring**: you hear only what is being removed. If music comes through, Thresld is too low. |

Chris Johnson's own description is in `Airwindopedia.txt` under *DeCrackle*.

Moving a slider takes effect immediately on the playing audio without
restarting the DSP (via `dsp_v3::apply_preset`), so there is no gap or click
while adjusting.

**This DSP is not zero latency.** It reports its group delay to foobar2000, so
visualisations stay in sync. At the default Window setting it is about 1.1 ms
at 44.1 kHz, growing to roughly 9 ms with Window at maximum.

---

## Parametric EQ parameters

Reachable the same way — **Preferences → Playback → DSP Manager**, add
*Parametric EQ*, press **Configure selected** — and also as a panel in the main
window; see [Putting the equaliser in the
layout](#putting-the-equaliser-in-the-layout).

Five bands in a **fixed layout**, not a bank of identical ones. The bank is the
more general instrument and the worse tool for this job. Restoring disc
transfers is repetitive work: the same four moves serve most of a box of
records, and what an operator wants is to set frequency and Q once, before the
session, and then reach for four gain knobs that always mean the same four
things. On 1926–1949 shellac those four are

| Band | Where it lives | What it is for |
| --- | --- | --- |
| **Bass** | 60–125 Hz | Weight the transfer lost at the bottom. Shelf, switchable to a bell. |
| **Reverb cut** | around 1 kHz | The boxy room the horn or the hall adds. Peaking, Q 0.5–8. |
| **Brilliance** | 4–6 kHz | The brilliance sitting under the surface noise. Peaking, Q 0.5–8. |
| **Hiss cut** | around 8 kHz | The surface noise itself. Shelf, switchable to a bell. |

plus a **low cut** for rumble and turntable roar under all of it (16–350 Hz,
off / 12 / 24 dB per octave, Butterworth at both slopes) and an **output** trim
for whatever the bands did. Every range is chosen to put its target near the
middle of the control's travel, which is what makes a knob usable rather than
merely capable.

The names are the ones the [tango transfer
literature](https://elespejero.wordpress.com/2014/12/17/eq-4-tj-getting-the-best-sound-out-of-your-tango-records/)
uses rather than the console's, because they say what the knob is for: an
operator reaching for the hiss should not have to know that a strip would have
called that band HF. The shorthand — *HP, LF, LMF, HMF, HF, Out* — is what a
column falls back to when it has not the room for the long name, and both appear
together at the top of a band's context menu, so neither name is a secret from
anyone who knows only the other.

The ranges are wider than a console strip's — ±20 dB a band, Q to 8 — again
because a transfer can need it. 20 dB of 8 kHz cut is a plausible setting on a
worn shellac and an implausible one on a microphone, and a horn honk or a
turntable ring is a single narrow feature that a Q of 3 cannot take out without
costing the music either side of it.

The filters themselves are the biquads from Robert Bristow-Johnson's Audio EQ
Cookbook, whose formulas are public domain. Nothing there is novel; what is
worth reading is in [Why a control move cannot
click](#why-a-control-move-cannot-click) below, and at more length in the header
of `foo_dsp_paraeq/paraeq_core.h`.

**This DSP is zero latency** and its `heapBytes()` is 0. Nothing in it is sized
by the parameters or by the sample rate, so every control move retunes the
running filters in place — no reallocation, no reset, and no gap in the audio
while a handle is being dragged.

### The editor

The whole interface is the curve, painted rather than assembled out of
trackbars. Everything is reachable three ways.

The VST2 build has the same editor, not a second one: `paraeq_editor.{h,cpp}` is
plain Win32 with no SDK in it and both hosts compile that one file. So everything
described here is true of `ParaEQ64.dll` as well, bar the "Add to DSP chain"
button, which exists for the UI element and has nothing to mean in a plug-in
window. See [The ParaEQ editor](#the-paraeq-editor).

**Mouse**

| | |
| --- | --- |
| Drag a handle | Frequency across, gain up and down, one to one with the axes. |
| Wheel | Gain, 0.5 dB a notch. On the low cut, its slope, down for more. |
| Shift + wheel | Q, or the shelf/bell switch on a band that has no Q. |
| Right-click | Shelf or bell, the low-cut slope, Q as a list of widths, reset this band, flatten, bypass. |
| Double-click a handle | That band's gain to zero. |
| Drag a readout cell | The value in it, vertically. This is how to set a number you can name rather than one you can see. |
| Hold ctrl | Finer: a quarter of a drag, a fifth of a wheel notch. |

**Keyboard** — the panel takes focus, so all of this works with the pointer
somewhere else entirely. The mark on the selected band follows that focus: tab
away and it goes, because the arrows have gone with it and a panel that went on
showing a band as being edited would be promising something it could no longer
do. Which band it was is remembered for when the focus comes back.

| | |
| --- | --- |
| ← → | The selected band's frequency, a semitone a press. |
| shift + ← → | The same, four semitones a press. |
| ctrl + ← → | Select the previous or next band, wrapping. |
| ↑ ↓ | Its gain, 0.5 dB a press. On the low cut, its slope — **down** for more of it. |
| shift + ↑ ↓ | The same, 2 dB a press. |
| ctrl + ↑ ↓ | Its Q, a sixth of an octave a press. On the low cut, its slope again, down for more; on a shelf, the shelf/bell switch. **shift** takes four steps at a time. |
| page up / down | The same, on a keyboard that has them. |
| space | Shelf to bell, or the next low-cut slope. |
| home | Reset the selected band. **ctrl + home** resets everything. |
| delete / backspace | The selected band's gain to zero. |

The arrows move the band rather than the selection because that is what a hand
reaches for: gain is up and down, and frequency is the other axis of the same
handle. Choosing which of the five to work on is the rarer act, so it takes the
modifier — which puts it on ctrl with an arrow key, and a host can bind that to
something of its own and swallow it before it arrives. That is the right one to
risk: if it goes missing the mouse still selects, and every command that needs a
band is in the context menu, whereas a frequency that never arrived would leave
the keyboard unable to reach a control at all.

The low cut is the one band where down means more of the control, because the
control is a slope and more of it takes the curve down. Up for more would read
backwards on a plot, and a plot is the only place this band is ever seen. That
holds on the arrows, the wheel, the Q keys, and the readout cell, so the
gesture means the same thing wherever it is made.

**Q**, the width of a peaking band, is on five of those routes, because it is
the control with no handle of its own — the two axes of the curve are already
the frequency and the gain — and so the one thing here somebody has to be able
to find rather than be told about:

| | |
| --- | --- |
| shift + wheel | Over the curve. Q moves geometrically, 24 notches from 0.5 to 8. |
| ctrl + ↑ ↓ | The same, on the selected band. |
| page up / down | The same again. Q has two chords because the page keys read better - a third control rather than a modified second one - and a laptop keyboard often has no page keys at all. |
| Drag the **Q** readout cell | Vertically, for a value you can name. |
| Right-click → **Bandwidth (Q)** | The current value, the keys that move it, and a list of widths to pick from. |

That menu writes every Q as its bandwidth in octaves as well — *Q 2.00 (0.7
oct)* — because Q is the number the control is set in and octaves are the thing
it does. The two shelves have no Q: they run at the fixed width a console shelf
has, and what their third control does is switch them to bells. The low cut has
its slope there instead.

**Readouts.** Every value is also written out under the curve, in a grid of one
column per band, so what a drag did is legible without moving the pointer off
it. The selected band's column is marked, and the cells drag.

The columns are not all one width. The values in them are short and much of a
muchness; the titles over them are not, and six columns each wide enough for
*Brilliance* is most of a narrow strip spent on the two names that need it. So
each column starts at the width a value needs and what is left over goes to the
titles that want more of it, cheapest first and all or nothing — sharing a
shortage out in proportion would leave every column a few pixels short of its
name and so write none of them, which spends the space and prints the shorthand
anyway. The upshot is that the whole strip is still written out at 360 px wide
with the default font, a width at which six equal columns had to print the
shorthand; below that the longest names go first and the rest stay.

The panel sheds its chrome from the bottom up as it gets shorter — the footer
buttons first, then the readouts — because the curve is the part that still says
something at forty pixels tall, and everything in the footer has a key and a
context-menu item as well. Below roughly 200 × 90 the layout editor will not
shrink it any further.

### Putting the equaliser in the layout

It is a **Default UI element**, so it can live in the main window beside the
playlist rather than behind a modal dialog:

1. **View → Layout → Enable layout editing mode**
2. Right-click the panel you want to split or replace, and pick **Add new UI
   element** or **Replace UI element**
3. Choose **Parametric EQ**, under the **DSP** group
4. **View → Layout → Enable layout editing mode** again to turn editing off

The element also asks the host for a menu command of its own
(`KFlagHavePopupCommand`), which opens it as a floating window, and implements
`bump()` so that such a command brings an existing panel forward rather than
opening a second window onto the same equaliser. Whether the command actually
appears is the host's decision: the SDK generates them for some element groups
and not others, and the DSP group is a recent addition. The layout route above
is the one that is certain.

While layout editing mode is on, the editor ignores mouse and keyboard input, so
that rearranging the panel does not also drag the curve.

**The element holds no settings of its own.** It reads and writes the live DSP
chain through `dsp_config_manager`, which has three consequences worth knowing:

* Two copies of the element in one layout show one equaliser, and so do the
  element and the Preferences dialog. They stay level through
  `dsp_config_callback`, which is the same route any other component's changes
  would arrive by.
* A saved layout comes back pointed at whatever the chain holds then, rather
  than at a stale copy of what it held when the layout was saved.
* If the equaliser is **not** in the DSP chain, the element still draws it —
  dimmed, with the settings the chain last held, and a footer button that puts
  it back. An element that showed nothing until the DSP was enabled somewhere
  else would be blank exactly when a user is looking for the thing to press.
  Moving a control on a disengaged equaliser sets it up for later; only that
  button inserts anything into the chain.

The panel is drawn entirely with GDI, asking the host for two colours and a
font, so it follows **dark mode** without any code that knows dark mode exists.
That is also why it is owner-drawn rather than a dialog full of trackbars:
standard controls do not follow foobar2000's dark mode without the SDK's
`DarkMode` helpers, and those live in `helpers/` and `libPPUI/`, which this
project deliberately does not build — see [Layout](#layout).

### Why a control move cannot click

Two decisions in `paraeq_core.h` are worth the reading.

**Every control reaches the audio as a move of the same five numbers per
stage.** Gain, frequency and Q obviously do; so do the two shelf/bell switches
and the high-pass slope, because a stage that is off is a *unity biquad* rather
than a stage that is skipped. One glide mechanism therefore covers every
control, including the discrete ones, and the count of biquads actually run
never changes.

That glide is on the coefficients, not on the controls, and it runs **per
sample**. Per sub-block was the first attempt and it is audible: a jump in `b0`
puts a step of `b0·x` straight into the output, so 32-sample blocks leave a
staircase about 35 dB below the signal on a fast move. Stepping every sample
took the worst second difference of the output from 2.2e-2 down to 4.6e-4 — the
test tone's own curvature — and costs nothing in the settled state, which is the
state an equaliser is in for all but 300 ms after a knob stops moving.

It is also safe for a *reason* rather than by measurement. A biquad is stable
exactly when `(a1, a2)` lies inside `|a2| < 1, |a1| < 1 + a2`, which is a
triangle and therefore convex. A straight line between two stable settings
cannot leave a convex region, so no intermediate coefficient set can ring or
blow up, whatever the two endpoints are — shelf to bell, off to 24 dB/oct, 16 Hz
to 16 kHz. `paraeq_verify` pins that across 49 pairs of the most distant
settings the controls allow, 1001 interpolants each.

**Bypass glides the whole equaliser to flat.** It does not blend a dry path in,
which is what Declick and Dehum do and what would be wrong here: their wet mix
is meaningful because the difference signal *is* the repair, whereas summing a
dry path across an EQ combs. Retargeting every stage to unity and the trim to
0 dB reaches the same place silently, and the signal is a real equaliser curve
at every instant of the transition.

### What it costs to run

There is no command-line tool for the equaliser, so this is measured in
process. The harness puts 177 s of 44.1 kHz stereo (*La tablada*), as floats
the way foobar2000 hands them over, through `Channel::process` in place,
1024 frames at a time. Best of fifteen on the M1 and of twenty-five on the
2014 MacBook Pro:

| | M1 | MacBook Pro | M1, ns/sample | MacBook Pro, ns/sample |
| --- | --- | --- | --- | --- |
| flat (the defaults) | 17 000× | 6 400× | 0.65 | 1.8 |
| two shelves in use | 2 400× | 1 200× | 4.7 | 9.3 |
| all six stages in use | 2 200× | 930× | 5.2 | 12.2 |
| **always gliding** | **920×** | **400×** | 12.3 | 28.2 |

"Always gliding" retargets every block, so the glide never settles and every
stage runs its per-sample coefficient step. That is a knob being dragged
without pause, and it is the worst case. 128-frame blocks measure the same to
within the noise. Even the worst case is 0.25 % of a Haswell core for stereo.
The settled cases are where an equaliser spends its life, and there the
skipped stages show: a flat equaliser is almost free, and four bands and the
high-pass cost at most a third more than two shelves.

### Drawing the curve cheaply

`magnitudeDb(cfg, hz)` is the honest way to ask what the curve does at one
frequency and the wrong way to ask it several hundred times, which is what
drawing costs: four trig calls per stage is twenty-four per point, and a
logarithm per stage on top of them.

Both are per-point constants in disguise. `cos(w)` and `cos(2w)` depend on the
frequency and the sample rate and on nothing a knob can move, so they hold still
across a whole drag; and the six stage magnitudes are *multiplied*, so their six
logarithms are one logarithm of the product. So `curveTrig()` builds a table
once — on a resize, not on a mouse move — and `curveDb()` after that is
arithmetic.

Measured over 600 points, x64 release: **110 µs** the direct way, **20 µs** this
way, and 8 µs to build the table on the resize that needs it. Neither figure is
alarming on a fast machine; the point is that a redraw is a fifth of the work on
a slow one, and that it stays a fifth when a second curve is overlaid for the
band being dragged. `paraeq_verify` checks the fast path against the slow one at
every point of a sweep that runs past Nyquist at both ends, for the cascade and
for each section on its own.

The rest of the drawing budget goes the same way:

* the curve is recomputed only when the settings, the size or the sample rate
  change, not on every paint;
* the whole panel is composed in a back buffer that is kept between paints
  rather than allocated per frame;
* the footer button rectangles are measured in `layout()` rather than on every
  mouse move;
* and a control move is pushed into the DSP chain at most every 25 ms, with one
  more push when the drag ends. That last one matters because every push
  rewrites the chain configuration and wakes every `dsp_config_callback` in the
  process — and 40 pushes a second is already faster than the 20 ms the audio
  takes to glide there.

---

## Performance

`decrackle_verify` reports this, best of five passes over 30 s of 44.1 kHz
stereo, Release, on a modern desktop:

| Build | | scalar | SSE2 | speedup |
| --- | --- | --- | --- | --- |
| x64 | defaults | 57.2 ms | 46.3 ms | 1.24× |
| x64 | Surface off | 45.9 ms | 34.5 ms | 1.33× |
| x86 | defaults | 90.9 ms | 80.2 ms | 1.13× |
| x86 | Surface off | 52.1 ms | 41.4 ms | 1.26× |

That is ~650× realtime for the default x64 build — 0.15 % of one core.
Memory is about 64 kB per stereo pair.

### Vectorization

The inner loop has three kinds of dependency, and only one of them is
exploitable:

* **Across samples** — fully recursive (six-pole IIRs, the `iirClick` attack
  and release ramps). Nothing to vectorize.
* **Across the six poles** — each pole feeds the next within the same sample.
  Also serial.
* **Across L and R** — completely independent apart from two cross-terms.
  **This is the 2-wide SSE2 opportunity**, and it is what `runStereoSSE2`
  exploits: L in the low lane, R in the high lane. The delay lines are already
  stored as interleaved `{l, r}` pairs, so each read is a single 16-byte load.

Because this is *lane parallelism* and not reassociation — same operations,
same order, same associativity, no FMA contraction — packed IEEE-754 doubles
round exactly like scalar ones. The harness asserts
`worst deviation vector vs. scalar path: 0.000e+00`, verified under
`/arch:SSE2`, `/arch:AVX` and `/arch:AVX2`.

Two things stay scalar because they genuinely are serial: the rectified
control band (one value derived from `L*R`, then six serial poles) and
`sin()`, of which there are two per sample when Surface is engaged. MSVC does
not ship a packed `sin`, and a polynomial approximation would forfeit
bit-exactness for a couple of percent — not a trade worth making. That is why
the Surface-off column shows the larger speedup.

**AVX and AVX2 buy nothing here.** A 256-bit register holds four doubles but
there are only two lanes of real parallelism, and the sample-to-sample
recursion rules out processing two samples at once. Measured, AVX2 lands
within noise of SSE2. FMA would change results (single rounding instead of
two) and break the bit-exactness guarantee for no measurable gain. `SSE2` is
therefore the default and there is no reason to change it.

### Other differences from a straight transcription of the VST

* **Coefficients are computed once per parameter change**, not once per buffer.
  The VST does ten `pow()` calls per `processReplacing` call.
* **`pow(x, 3.0)` becomes `x*x*x`** in the click detector, twice per sample.
* **`pow(2, expon + 62)` and `frexpf` in the dither** become bit manipulation
  on the float's exponent field. Exact, no libm call.
* **The Surface block is skipped entirely when Surface is 0**, which is where
  the two `sin()` calls per sample live. The VST computes them regardless and
  then discards the result.
* **The L and R delay lines are interleaved**, halving the number of cache
  lines the inner loop touches — they are always read at identical indices.
* **Dead code removed**: `prevSampleL/R` and `prevSurfaceL/R` are computed by
  the VST but never used.
* **Flush-to-zero is enabled** for the duration of each chunk (FTZ only; DAZ
  lives in a bit that early SSE2 parts treat as reserved).

None of these change the output — see below.

`decrackle_core.cpp` keeps a plain scalar implementation alongside the SSE2
one. It is the portable fallback, it is what the single-channel path uses, and
the test harness runs both against the Airwindows source so neither can drift.

---

## Verification

```bash
ctest --test-dir build/x64 -C Release --output-on-failure
```

**`decrackle_verify`** compares the port against
`tests/decrackle_reference.h`, a verbatim copy of the Airwindows VST source
used as an oracle. Across 12 parameter/sample-rate combinations × 5 signal
types, fed in 1024-sample chunks so buffer boundaries are exercised, the worst
deviation is **0.000e+00** — bit-identical, on both x86 and x64, for both the
SSE2 and the scalar path. It also:

* feeds NaN, ±infinity, `1e30` and denormals in and checks the output stays
  finite and that clean audio afterwards comes back clean;
* asserts the SSE2 and scalar paths agree to the bit;
* sweeps every parameter across 21 steps × 14 sample rates from 1 kHz to
  20 MHz, checking buffer indices stay in bounds and nothing diverges;
* checks the single-channel path against a duplicated-stereo run;
* reports throughput.

**`declick_verify`** checks the declicker's linear algebra against dense brute
force — this is the part where a subtle error yields plausible but wrong audio
rather than an obvious failure. `levinson()` against a Gaussian-elimination
solve of the Yule–Walker system (worst deviation **5.6e-16**),
and `solveBandedToeplitz()` against dense elimination (**3.8e-15**), over
random positive-definite systems across the whole order and run-length range.
It also covers
streaming properties end to end: silence in, silence out; clean audio passes
through untouched; injected clicks come out 26 dB smaller; NaN, ±infinity and
denormals produce nothing non-finite and the stream recovers afterwards;
dry/wet 0 is a bit-exact bypass; and the latency contract holds in both
directions (no output before `latency` samples are fed, output immediately
after).

**`declick_rt_verify`** counts heap allocations on the processing path and
requires zero — see [Real-time safety](#real-time-safety) for what it caught.
It also records the one documented exception (a push larger than
`Config::maxBlock`) as a positive assertion rather than leaving it implicit, and
prints the per-channel footprint so a regression in that shows up in the log.
It also mirrors the route `dsp_declick` itself takes through a control move,
because for a long time that was where the allocation actually was — driving a
`Channel` directly, as every other case here does, walked straight past it.

**`paraeq_verify`** checks the equaliser three ways. Each cookbook section
against the formula it comes from — a +6 dB bell is +6 dB at its centre and
0 dB two decades away, a +6 dB low shelf is +3 dB at its corner, the high-pass
is −3.01 dB at its corner and −12.30 or −24.10 dB an octave below at its
two slopes. Then what a `Channel` actually does against what `magnitudeDb()`
promises, measured by running sines through it at nine frequencies rather than
by evaluating the same formula twice — which is what makes the drawn curve a
check on the audio instead of a restatement of it. Then the glide: that 49 pairs
of the most distant settings the controls allow stay inside the stability
triangle at all 1001 interpolants, that a move settles within 350 ms, and that
no step in the output is larger than the test tone's own curvature — the check
that caught the glide stepping per sub-block. It also pins the fast drawing path
against the slow one, and that a NaN fed in does not stay in the filter.

**`declick_vst_verify`** holds the WinVST port to the same maths — see
[Sharing a core with the other plug-in formats](#sharing-a-core-with-the-other-plug-in-formats).
The central check drives `declick::Channel` directly with the same `Config` and
the same per-sample push/pull; the plug-in's `processDoubleReplacing` output is
**bit-identical**, worst deviation **0.000e+00**, on both x86 and x64. If that
ever stops being true, one of the two wrappers has grown DSP of its own. It also
covers the parts a VST has to get right on its own: that block patterns from
`1/1/1/1` to `1024/64/4096/1` all yield the *same* stream, which is what proves
the pre-roll arithmetic (get it wrong and the core zero-fills mid-stream); that
the declared latency is the real one in both directions; that a Sensitivity move
retunes without renegotiating latency or leaving a gap while a Model order move
does the opposite; dry/wet 0 as a bit-exact bypass; `resume()` starting from
silence rather than the previous take; hostile input; the slider-to-core
mappings; and the preset chunk, including pinning out-of-range stored values.
Built against `plugins/WinVST/vst2_shim`, so it needs no SDK — the same shim the
shipped DLL links, with the caveat noted above about what that does not
establish. What it cannot see at all is the ABI, because the plug-in is linked
in here rather than loaded; that is `vst_host_verify`'s job.

**`dehum_verify`** checks the dehummer against independent references rather
than against itself. The tonality ratio is pinned at both of its analytic
extremes — 1 for a pure tone, `sqrt(narrow/wide)` for noise — which catches a
wrong coefficient, a swapped pair or an accumulator measuring the wrong thing;
measured 0.993 and 0.222 against 1 and 0.224. The hand-rolled FFT is pinned by requiring tones placed on
known bin centres to be detected there to within 0.15 Hz — a transposed
butterfly, a wrong twiddle sign or a bad real-input unpack all move the peak. The
notch depth is checked against its *analytic* floor `bandwidth/(2·f0)` rather
than an arbitrary threshold, so it fails if the lowpass coefficient is wrong in
either direction; measured −50.5, −40.0 and −30.5 dB at 0.3, 1 and 3 Hz against
predictions of −50.5, −40.0 and −30.5. The frequency tracker is driven from four
starting offsets up to 1.2 Hz and required to converge within 0.1 Hz. The rumble
filter is compared with the closed-form Butterworth response at five
frequencies, to within 1.5 dB. It also covers: detection and removal end to end
on synthetic hum, with the removed signal required to be **100% the line
itself**; clean material with recurring notes, where nothing may be detected and
the output must come back bit-identical; dry/wet 0 as a bit-exact bypass; the
same output for block sizes from 1 to 65536; NaN, ±infinity, `1e30` and denormals
in with finite audio out and full recovery afterwards; six sample rates from
8 kHz to 192 kHz; and that every parameter move can be retuned live while a
sample rate change cannot.

**`dehum_rt_verify`** counts heap allocations on the processing path and requires
zero — a sharper question here than for the declicker, because a whole
FFT-based detector runs on the audio thread. It covers 37 s of steady
processing, a 65536-sample block, 4096 single-sample calls, 21 live parameter
changes, `flush()`, `reset()` and manual mode, and prints the footprint before
and after so a regression shows up in the log. All zero, 410 kB either side.

**`dehum_vst_verify`** holds the WinVST dehummer to the same maths — see
[Sharing a core with the other plug-in formats](#sharing-a-core-with-the-other-plug-in-formats).
Its central check drives `dehum::Channel` with the same `Config` and requires the
plug-in's `processDoubleReplacing` output to be **bit-identical**, worst deviation
**0.000e+00**. It also covers what this wrapper has to get right on its own: that
block sizes from 1 to 4096 — including 1025, one past the scratch buffer it
chunks through — all give the *same* stream; that the declared latency is zero
and that **nothing** renegotiates it, for any parameter or across a sample rate
change; that all seven sliders retune live without a gap; that `resume()` keeps
what the detector learned rather than re-acquiring; that the float path is the
double path plus dither to within a float ULP; hostile input; dry/wet 0 as a
bit-exact bypass; the slider mappings against `Params::defaults()`, including the
off positions on Freq and Rumble; and the preset chunk.

**`paraeq_vst_verify`** does the same for the WinVST equaliser. Its central
check drives `paraeq::Channel` with the same `Config` and requires
`processDoubleReplacing` to be **bit-identical**, worst deviation **0.000e+00** —
and it repeats that across a control move, which is the case a wrapper can get
wrong without anything else noticing: `configure()` where `retune()` belongs
zeroes the filter states and snaps the coefficients, and the stream that comes
out is a different one. It asks the plug-in for its `Params` rather than assuming
`Params::defaults()`, because a logarithmic slider takes a default through a
`powf()` of a `logf()` and 5 kHz comes back as 5000.0005 — one float ULP, held to
account with a tolerance in the mapping checks where it belongs. It also covers:
block sizes from 1 to 4096 including 1025, one past the scratch; bypass settling
to a bit-exact passthrough; that a control move puts no step in the audio, by
requiring no sample in the glide to jump further than the settled signal does;
that a +20 dB bell is +20 dB and a 24 dB/oct high-pass is 92.9 dB down on the
rumble; that all sixteen sliders move live without renegotiating latency; a
corner above Nyquist; hostile input; `resume()`; the slider mappings against
`Params::defaults()`; and the preset chunk, including the NaN that
`pinParameter()` lets through and `sanitize()` stops. It also covers the editor
in both directions: that a drag reaches the parameters and tells the host about
exactly the sliders that moved, that `idle()` pulls a host-side change into the
curve, and that a quiet `idle()` writes no automation — the feedback loop this
join could have had.

**`declick_au_verify`**, **`dehum_au_verify`** and **`paraeq_au_verify`** do the
same job for the MacAU ports — see [Sharing a core with the other plug-in
formats](#sharing-a-core-with-the-other-plug-in-formats). Declick and Dehum
render into 32-bit float with the house dither on the way out, so there is no
undithered path to compare and the requirement is the core driven directly to
within **two ULP** rather than to the bit: measured **1.36 ULP** worst for the
declicker and **1.41 ULP** for the dehummer. ParaEQ has no dither — it asks the
core for floats directly, `process()` being instantiated for them — so that test
requires **0 of 30000 samples to differ**, and gets it, at every block size,
in place, in mono, and at five sample rates. What that still catches is any
difference that is not rounding, and the gross ones cannot hide in a ULP — a
wrong pre-roll makes the core zero-fill mid-stream. Around it, the things an AU
has to get right that a VST does not:

* `GetLatency()` and `GetTailTime()` are **seconds**, not samples, and both have
  to come back out as `cfg.latency` at the rate in force — 6192 samples at the
  shipping default, 5904 after dropping to order 32, 11008 at 96 kHz;
* `PropertyChanged(kAudioUnitProperty_Latency)` is what an AU says instead of
  `audioMasterIOChanged`, so it must fire **exactly once** for Max repair or
  Model order and **never** for the other five, for Dehum's seven, or across a
  sample rate change;
* `Reset()` is where `resume()` went, with the same split: Declick starts the
  next take from silence, Dehum keeps what the detector learned (hum still
  **44.5 dB** down within 2 s of it);
* `kAudioUnitRenderAction_OutputIsSilence` has to be **cleared**, or a host that
  trusts it clips the tail of the pipeline off every gap;
* a host may hand the same buffer in and out, so both are run in place as well.

Plus what the host is told about the controls: for Declick and Dehum, seven
parameters, named, in the VST's order, all 0..1, readable and writable, with the
advertised default equal to the one the constructor actually set — the classic
template slip — and a bad index or a non-global scope refused. For ParaEQ,
sixteen, each in the core's own units with the core's own constant for its range,
each in one of six clumps, value strings on the one indexed control and on
nothing else, and the whole set round-tripping to `paraeq::Params::defaults()`.
ParaEQ also publishes an editor, so its test checks the property that says so —
`kAudioUnitProperty_CocoaUI` sized the way a host reads it, read-only, refused
outside the global scope, and routed through to the view's own factory. The view
itself is AppKit and is not in that build; `auval` is what loads a real one.

All three are built against `plugins/MacAU/au_shim` and need no SDK. Read that
folder's [README](../MacAU/au_shim/README.md) first: unlike `vst2_shim` it is
**not an ABI** and nothing loads it, so what these tests establish is the DSP,
the latency arithmetic and the parameter table, and nothing about the bundle.
The other half is `plugins/MacAU/scripts/build.sh --validate`, which builds the
real universal components against Apple's AudioUnitSDK and hands each one to
**`auval`** — Apple's host-side conformance suite. All three pass. That is the
AU counterpart of `vst_host_verify`, and it is a shell script rather than a
ctest because it only runs on macOS.

**`preset_roundtrip`** saves each component's parameters to a `dsp_preset` and
reads them back, checking every field individually with values chosen so that a
field read out of position cannot pass by accident. It also covers the
version-1 declick layout (written before **Repair depth** existed, still
loadable, depth falls back to its default), a preset owned by a different GUID,
and a truncated payload. This test exists because `depth` was once written by
`make()` and skipped by `parse()`, which silently shifted every field after it
— saved chains came back with **Dry/Wet at 0**, i.e. bypassed. A dialog that
looks right and a DSP that runs will not catch that; only a save/load cycle
will. Like the smoke test it needs `shared.dll` and skips itself when no
matching-architecture foobar2000 is installed; the logic under test is
architecture-independent, so one architecture is enough.

**`component_smoke`** loads the built DLL exactly as foobar2000 does — through
`foobar2000_get_interface()` and the service factory list — then registers the
DSP, instantiates it from a preset and pushes audio through it: format changes
mid-stream (stereo → mono → 5.1 → 192 kHz), `flush()`, live preset changes, and
truncated/empty stored presets. It also opens the configuration dialog for
real, moves a slider, and checks that the live update fires and that Cancel
restores the original preset. It skips itself if no matching-architecture
foobar2000 is installed (it needs `shared.dll`); set `FOOBAR2000_DIR` to point
at one.

To eyeball the dialog without installing anything:

```bash
build\x64\tests\Release\component_smoke.exe ^
  build\x64\foo_dsp_decrackle\Release\foo_dsp_decrackle.dll ^
  --screenshot dialog.bmp
```

### Before committing a core change

The tests above check linear algebra, allocations and that the ports agree.
None of them looks at what the core does to a record. So a change to
`declick_core` or `dehum_core` also gets a whole-song A/B against the commit
before it, with `scripts/declick_regress.py`:

```bash
"C:\Program Files\Python314\python.exe" scripts\declick_regress.py
"C:\Program Files\Python314\python.exe" scripts\declick_regress.py --base fe53b2314 --cand f9557b738
```

With no arguments it compares HEAD against the working tree. It builds
`declick_cli` and `dehum_cli` for both from `git archive`, runs ten reference
songs whole, through declick alone and through dehum then declick, and prints
for each: removal in dB, clamped estimates, and every out-of-range event, a
stretch where the output exceeds 1.25 times the input's 10 ms peak. It exits 1
if the candidate has an event longer than 3 samples that the base does not, or
more clamped estimates than 20 (or half the base's count, if that is more) over
the base. The songs live outside the repository; their paths are at the top of
the script, and `--file` runs it on others. About 25 s on the dev box once the
builds are cached.

Why whole songs and the chain: the half repair of 59c398f48 passed every test
and every 60 s excerpt, moved whole-song removal by about 0.1 dB, and put a
6 ms burst at the clamp into *Voz de tango* at 14.37 s, but only with Dehum
ahead of it. That burst is chaotic. The same audio started 100 samples later,
or with white noise 70 dB down added, does not produce it. Nor did any of about
2,000 synthetic signals tried for a ctest, which is why there is none.

`declick_cli` prints `clamped N first, M again` after the repaired count: the
estimates `repairRun()` had to pull back to 1.5 times the local range, first
repairs and re-repairs, which means solves that went wrong. At f9557b738 the
clean records (Malvon, *Olga*, the Troilo .flac sides) have none, the crackly
ones a handful, and *Hotel Victoria* the most, 31+2. fe53b2314, the core before
the fix, had 169+11 on *Voz de tango* after Dehum and 893+58 on *El
irresistible* after Dehum, in that song's near-silent tail. The output there
never left the 1.25x range, so the counter catches runaways the scan does not.
Quantised digital silence, such as a 16-bit fade-out tail, clamps by the
thousand on any build without anything being heard. So read the counter
against the base, never as an absolute.

Abrupt level steps are a known gap, not a regression. Synthetic signals that
drop by 40 dB or more take the output 6 to 17 times past the scan's limit,
before f9557b738 and after it. The worst case is a pair of clicks whose run is
longer than `maxRun`. That run is refused and the clicks stay in, while the
clean samples either side of it, flagged only because the clicks ring through
the detector, are solved against them. The clamp's ±3·order window holds the
same raw clicks, so it lets those estimates through. A hard step does it on its
own too, the quiet side's repairs carrying on the loud side's tone, and so does
the body stage, whose 46 ms frames smear across the step. Real records never
get there. Across 400 TangoTunes transfers run whole, the deepest drop out of a
loud passage was about 26 dB, where the shellac floor stops it. The worst event
peaked at 1.56 times the input's 10 ms peak, a few samples on a near-clipping
transient, which is what an ordinary event looks like. Where the clamp binds on
a real song, a ±order window gives the same limit, and on *Hotel Victoria* that
window would bind four times as often. So the window stays as it is.

None of this replaces listening. And the "clicks above 8 sigma" score is not a
pass or fail: it swings 2x between near-identical outputs.

### Deliberate behavioural differences

Two places where the port does not follow the VST, both to avoid producing
garbage:

1. **Filter coefficients are clamped to ≤ 1.0.** `filterOut` and `filterRef`
   are divided by `sampleRate / 44100`, so below roughly 22.7 kHz they exceed 1
   and the recursion diverges to infinity. The VST has the same flaw; it just
   never gets handed a 22 kHz file. At 44.1 kHz and above the unclamped values
   never exceed 0.52, so the clamp cannot alter normal playback.
2. **Input is sanitised.** Samples that are NaN, infinite or above `1e30` are
   replaced with silence before they can lodge themselves in the recursive
   state. Real audio is untouched. Delay-line indices are additionally clamped
   into range, which only ever matters at absurd sample-rate/Window
   combinations.

The dither generator is also seeded with fixed constants instead of the VST's
`rand()`, so the same file renders to the same bits every time — which matters
for a player that can also be used as a converter.

---

## How multichannel is handled

DeCrackle's click detector correlates left against right, so channels are fed
to it in their natural stereo pairs (front L/R, side L/R, back L/R, and so on),
read out of the chunk's channel map. Channels with no partner — centre, LFE,
back centre — go through a degenerate single-channel path where the cross
detector collapses to `x²`. A chunk with an unrecognised channel map falls back
to pairing in interleave order.

---

## Layout

```
CMakeLists.txt                    top level: options, toolchain flags
cmake/
  fb2k_download_sdk.cmake         SDK download + unpack, also runnable with -P
  fb2k_sdk.cmake                  builds pfc + SDK + component client
  fb2k_find_runtime.cmake         locates an installed foobar2000 for the smoke test
scripts/
  build_release.ps1               the release build + packaging entry point
  build_winvst.ps1                the WinVST plug-ins, into ../dist/winvst
  build_linuxvst.sh               the LinuxVST plug-ins, into ../dist/linuxvst
  check_win7.ps1                  reads a built DLL's PE headers and imports for Windows 7 compatibility
  get_sdk.ps1                     wrapper around fb2k_download_sdk.cmake
  sync_cores.ps1                  mirrors the cores and the VST wrapper out to the other plug-in formats
  sync_cores.sh                   the same list, for machines with no PowerShell
foo_dsp_decrackle/
  decrackle_core.{h,cpp}          the DSP (scalar + SSE2); no foobar2000 or Win32 dependency
  dsp_decrackle.cpp               the foobar2000 DSP service
  decrackle_preset.{h,cpp}        preset serialisation
  config_dialog.cpp               plain Win32 configuration dialog
  component.cpp                   DECLARE_COMPONENT_VERSION
  foo_dsp_decrackle.rc            dialog template + version resource
foo_dsp_declick/                  same layout, declick_core.{h,cpp} etc.
foo_dsp_dehum/                    same layout, dehum_core.{h,cpp} etc., plus
  dehum_scout.{h,cpp}             reads the opening of the playing file on a
                                  worker thread so the detector does not have to
                                  find the line the slow way
foo_dsp_paraeq/                   same layout, paraeq_core.{h,cpp} etc., plus
  paraeq_editor.{h,cpp}           the owner-drawn curve editor, used unchanged by
                                  the modal dialog, by the UI element and - via
                                  sync_cores - by the WinVST plug-in
  ui_element.cpp                  the Default UI element, so the editor can live
                                  in the main window; edits the DSP chain directly
tools/
  decrackle_cli.cpp               offline WAV in / WAV out, for sweeps
  declick_cli.cpp                 ditto for the declicker
  dehum_cli.cpp                   ditto for the dehummer; also reports the lines
                                  it found and where the tracker put them
  wav_io.h                        minimal WAV reader/writer
tests/
  decrackle_reference.h           verbatim Airwindows VST source, used as an oracle
  decrackle_verify.cpp            correctness, robustness, throughput
  declick_verify.cpp              AR linear algebra vs. dense brute force
  declick_rt_verify.cpp           counts audio-thread allocations, requires zero
  declick_vst_verify.cpp          the WinVST port vs. the core it shares
  dehum_verify.cpp                FFT, notch, tracker and detector vs. references
  dehum_rt_verify.cpp             ditto for the dehummer's audio thread
  dehum_vst_verify.cpp            the WinVST dehummer vs. the core it shares
  paraeq_vst_verify.cpp           the WinVST equaliser vs. the core it shares
  declick_au_verify.cpp           the MacAU declicker vs. the core it shares
  dehum_au_verify.cpp             the MacAU dehummer vs. the core it shares
  paraeq_au_verify.cpp            the MacAU equaliser vs. the core it shares -
                                  bit-exact, there being no dither in between
  paraeq_verify.cpp               the cookbook sections vs. their formulas, the
                                  glide vs. the stability triangle, and the fast
                                  drawing path vs. the slow one it replaces
  vst_host_verify.cpp             loads a finished VST2 plug-in - .dll or .so -
                                  over the C ABI alone
  preset_roundtrip.cpp            parameters survive save/load, every component
  component_smoke.cpp             loads a DLL through the real SDK plumbing
external/                         the downloaded SDK (git-ignored)
../dist/                          release artefacts, shared with the VST2 builds
                                  (git-ignored)
```

The VST2 shim the two `*_vst_verify` tests and every shipped VST2 build link
against lives with the plug-ins it serves, not here:

```
../WinVST/vst2_shim/
  vst2_abi.h                      AEffect, the opcode enums, and the static_asserts
                                  that pin every offset
  audioeffectx.h                  AudioEffect / AudioEffectX
  audioeffectx.cpp                the opcode dispatcher and the C thunks
  vstplugmain.cpp                 VSTPluginMain, the one exported symbol, plus the
                                  pre-2.4 `main` alias
```

It is under `WinVST/` because that is the port that needed it first; the Linux
build compiles the same four files. The ABI was fixed in 1999 by hosts that ran
on both platforms, so only `vstplugmain.cpp` has a platform branch in it — how
a symbol is exported, and how the `main` alias is made without a `.def` file to
make it in.

The stand-in the three `*_au_verify` tests build against lives with its plug-ins
too, and exists for the same reason — Apple's CoreAudio sources are not
redistributable either. Beside it is the other half of the arrangement, which
maps the same names onto Apple's current SDK so that a real `.component` can be
built from the same unmodified sources:

```
../MacAU/au_shim/
  AUEffectBase.h                  AUBase / AUEffectBase, the CoreAudio types the
                                  three plug-ins name, and COMPONENT_ENTRY
  README.md                       what it is not: an ABI, or a real host
../MacAU/au_compat/
  AUEffectBase.h                  the same names, forwarded to Apple's
                                  AudioUnitSDK. 50 lines, and what the shipped
                                  components are built against
../MacAU/scripts/
  get_sdk.sh                      fetches AudioUnitSDK (git-ignored, pinned)
  build.sh                        the three .component bundles, universal
  package.sh                      sign, notarise and wrap them in a .pkg
```

`decrackle_core.{h,cpp}` deliberately knows nothing about foobar2000, VST or
Win32, which is what lets the test harness compare it against the original
source directly.

---

## Sharing a core with the other plug-in formats

Three of the four cores have further consumers — builds of the same algorithms
for hosts on three platforms, in two plug-in formats:

| | |
| --- | --- |
| `plugins/WinVST/Declick`, `plugins/WinVST/Dehum`, `plugins/WinVST/ParaEQ` | VST2, `.dll`, 32 and 64 bit |
| `plugins/LinuxVST/src/Declick`, `plugins/LinuxVST/src/Dehum` | VST2, `.so` |
| `plugins/MacVST/Declick`, `plugins/MacVST/Dehum` | VST2, `.vst` bundle |
| `plugins/MacAU/Declick`, `plugins/MacAU/Dehum`, `plugins/MacAU/ParaEQ` | Audio Unit, `.component` |

ParaEQ is the newest and so far the narrowest: VST2 on Windows and an Audio
Unit on macOS, with no Linux, MacVST or vdjplugin port of it yet.

They all compile `declick_core.{h,cpp}`, `dehum_core.{h,cpp}` or
`paraeq_core.{h,cpp}` — not reimplementations of them, and not translations.
There is exactly one copy of each piece of maths in this repository that anything
is allowed to diverge from, and it is the one under `foo_dsp_declick/`,
`foo_dsp_dehum/` or `foo_dsp_paraeq/`.

Each plug-in folder holds a **byte-identical copy** rather than reaching across
the tree for it. That is not laziness: an Airwindows WinVST folder has to stand
on its own, because the build is "drag the plug-in's files into VSTProject and
press build" (`plugins/AirwindowsWinVSTTemplate.txt`) and the folder that gets
committed is the folder that was dragged; `plugins/LinuxVST` keeps a folder per
plug-in and globs it; and `plugins/AirwindowsMacVSTTemplate.txt` opens with
"option-drag the 'VSTMaster' folder to create a copy of it". A `..\..\` include
path would break the moment anyone followed any of those instructions.

The **wrapper** is mirrored the same way and for the same reason. `Declick.h`,
`Declick.cpp` and `DeclickProc.cpp` are one file each, not one per platform:
the parameter mappings, the latency reporting and the dither are things the
three VST2 ports must not disagree about, and the only copy of them lives in
`plugins/WinVST/Declick`. There is nothing to be canonical in `foo_dsp_*`,
which is a foobar2000 component and has no VST wrapper at all.

`plugins/MacAU` takes the cores and not the wrapper. An Audio Unit is a
different interface, so its `.cpp` is a wrapper in its own right rather than a
copy of anybody's — see [The Mac ports](#the-mac-ports).

One **editor** is mirrored as well, which nothing else here does: cores are
shared between formats and wrappers between ports, and `paraeq_editor.{h,cpp}`
is the only *interface* two hosts have in common. It is canonical in
`foo_dsp_paraeq` like the core and goes to `plugins/WinVST/ParaEQ` and nowhere
else — MacAU is deliberately not on that list, an Audio Unit having no Win32
window to put it in. See [The ParaEQ editor](#the-paraeq-editor).

`plugins/MacAU/ParaEQ` has an editor too, and it is a **second** one rather than
a mirror of that one, because there is nothing structural for the two to share:
`paraeq_editor.{h,cpp}` is `windows.h` from the first line to the last, and
`ParaEQView.mm` is AppKit. What they do share is the only thing that matters,
which is `paraeq_core.h` — the same `curveTrig()` and `curveDb()` draw both
curves, so the two editors cannot come to disagree about what the filters do
even though no line of drawing code is common to them. See [The ParaEQ Audio
Unit editor](#the-paraeq-audio-unit-editor).

So the copies are mechanical and checked:

```powershell
.\scripts\sync_cores.ps1          # push each canonical copy out to its mirrors
.\scripts\sync_cores.ps1 -Check   # compare only, non-zero exit on any drift
```

```sh
scripts/sync_cores.sh              # the same two things, without PowerShell
scripts/sync_cores.sh --check
```

The two scripts hold the same mirror list and have the same exit codes, so
either can front the other's gate — which is the point, because the Linux and
Mac ports are edited on machines that cannot run the `.ps1`. `build_release.ps1` runs
`-Check` before it configures anything and `build_linuxvst.sh` runs `--check`
before it compiles anything, so a build whose maths no longer matches the
others' cannot be packaged. Adding a format is a directory in an existing
entry's destination list, in both scripts.

**Edit the canonical copy, never a mirror** — `foo_dsp_declick/` for a core,
`WinVST/Declick/` for the wrapper. A mirror edit is not merged, it is
overwritten.

### What the Declick VST wrapper adds, and why none of it is in the core

| | |
| --- | --- |
| **Pre-roll** | A VST must return n samples for every n it is given, and the core holds `config().latency` samples of lookahead. `Channel::prime()` feeds it that many zeros up front, after which `available() >= n` holds for *any* block size, so the wrapper is a plain one-in-one-out loop with no FIFO of its own and no risk of the core zero-filling mid-stream. Those zeros are the reported delay. |
| **`setInitialDelay` / `getGetTailSize`** | foobar2000 is told the latency through `get_latency()` and flushes with `on_endofplayback()`. A VST needs the equivalent two, and `ioChanged()` when Max repair or Model order changes it. |
| **`Channel::retune()`** | A DAW moves sliders while audio runs, and `configure()` resets. `retune()` swaps in a config that needs the same buffers — everything except Max repair and Model order — with no discontinuity. Written for the VST, but foobar2000 has the same problem for a different reason: `on_chunk` is the audio thread, so it takes the same route. |
| **Dither** | Airwindows house style, on the 32-bit float path only. |

### The Dehum VST wrapper is shorter, and mostly by subtraction

Nearly everything on the list above is a consequence of Declick's lookahead.
Dehum has none — the detector reads the signal but does not sit in the path — so
its wrapper needs none of it:

| | |
| --- | --- |
| **No pre-roll, no FIFO** | The core works in place and returns what it was given, so the wrapper is a copy, a `process()` call and a dither. |
| **No latency to declare** | `setInitialDelay(0)` once in the constructor and nothing after it. `dehum_vst_verify` asserts `ioChanged()` is **never** called, for any parameter and across a sample rate change — the exact opposite of what it asserts for Declick. |
| **No tail** | Nothing to keep pulling for at the end of an offline bounce, so no `getGetTailSize()`. |
| **Every parameter retunes live** | Only the sample rate sizes anything in this core, so all seven sliders move without a rebuild, a reset or a gap. Declick can only manage five of its seven. |
| **A scratch buffer** | The one thing it adds. The DSP is done in double while the float path dithers on the way out, so the doubles need somewhere to live: a fixed 1024-sample member, chunked, because the audio thread must not allocate. Splitting a buffer across chunks is undetectable to the core, which is what the block size checks establish — including a deliberate 1025, one past the scratch. |
| **`resume()` flushes rather than resets** | The analysis window is stale across a transport jump and the integrators hold a discontinuity, but the hum on the far side is the same hum. Forgetting the lines would cost several seconds of it every time the user hit play; the test measures the hum still 34.9 dB down within 2 s of `resume()`. |

The one thing that moved *into* the core is the flush-to-zero guard, which used
to be a local class in `dsp_declick.cpp`. FTZ changes results in the last bits,
so it is part of the numerical contract rather than an optimisation, and two
ports that disagree about it are not comparable. It is now
`declick::scoped_flush_denormals` and both wrappers hold one.

### The ParaEQ VST wrapper is the Dehum one, plus an editor

The same shape underneath — no pre-roll, no FIFO, no latency, no tail, a
1024-sample double scratch and a dither — because the equaliser asks the host for
even less than the dehummer does. What is different is above the DSP rather than
in it:

| | |
| --- | --- |
| **Sixteen parameters, and a curve to move them with** | `kParamA`–`kParamP`, against Declick's and Dehum's seven, because a fixed layout has five bands and each of them has two or three controls. Sixteen sliders in a host's generic list is not an interface, so this is the one plug-in here that sets `effFlagsHasEditor` — see [The ParaEQ editor](#the-paraeq-editor) below. The sliders do not go away: they are what a VST parameter *is* and what a host automates, and the editor moves them rather than replacing them. |
| **Logarithmic frequency and Q** | A linear 1.5–16 kHz control spends four fifths of its travel in the top octave and a half, and the useful settings on a transfer are clustered at the bottom of every one of these ranges. So the frequency and Q sliders are `lo * (hi/lo)^x`, and the midpoint of the travel is the geometric mean of the range rather than its arithmetic one. Gains stay linear because dB already is, which also puts unity exactly at the centre detent. |
| **No default constants** | Dehum writes its default slider positions out as named constants and has a test pinning them against `Params::defaults()`. ParaEQ runs the mapping backwards instead — the constructor calls `setControlsFromParams(paraeq::Params::defaults())` — so this file holds no second opinion about what a default is and there is nothing to pin. |
| **A snap on a rate change, a glide on everything else** | Nothing here is sized by the parameters, so `retune()` cannot fail and there is no rebuild branch — not even the one Dehum needs for a sample rate change. A rate change takes `configure()` instead, because the filter states describe samples at a rate that is no longer the rate, and gliding across that would be gliding over a discontinuity. |
| **Bypass is a real bypass** | It retargets every stage to unity and the trim to 0 dB rather than blending a dry path in, and `checkSettled()` lands exactly on the target rather than asymptotically near it. So once the glide has arrived the cascade is six unity biquads and the output is the input **to the bit**, which `paraeq_vst_verify` asserts as an identity rather than a tolerance. |

The sliders, in order. The names are the restoration ones from [Parametric EQ
parameters](#parametric-eq-parameters) rather than a console's HP/LF/LMF/HMF/HF,
so that an automation lane says what the knob is *for*; `kVstMaxParamStrLen` is
eight characters, which is why two of them are cut short. The editor has the room
and writes them out in full.

| | Control | Band | Range | Slider |
| --- | --- | --- | --- | --- |
| A | `Low cut` | Low cut | 16–350 Hz | logarithmic |
| B | `Slope` | | off / 12 / 24 dB per octave | three equal buckets |
| C | `Bass` | Bass | ±20 dB | linear |
| D | `Bass Hz` | | 30–450 Hz | logarithmic |
| E | `Bass Shp` | | shelf / bell | two equal buckets |
| F | `Revrb` | Reverb cut | ±20 dB | linear |
| G | `Revrb Hz` | | 200–3000 Hz | logarithmic |
| H | `Revrb Q` | | 0.5–8 | logarithmic |
| I | `Brill` | Brilliance | ±20 dB | linear |
| J | `Brill Hz` | | 600–8000 Hz | logarithmic |
| K | `Brill Q` | | 0.5–8 | logarithmic |
| L | `Hiss` | Hiss cut | ±20 dB | linear |
| M | `Hiss Hz` | | 1500–16000 Hz | logarithmic |
| N | `Hiss Shp` | | shelf / bell | two equal buckets |
| O | `Output` | Output | ±20 dB | linear |
| P | `Bypass` | | off / on | two equal buckets |

A discrete control's inverse returns the **centre** of its bucket rather than the
lower edge, so a stored preset cannot come back as the neighbouring setting.

`paramsFromControls()` is public here, unlike its two siblings'.
`paraeq_vst_verify` needs the exact `Params` the plug-in is running in order to
build the same `Config` and compare to the bit, and a second copy of the mapping
living in the test is the thing that would drift. It is not part of the VST2
interface; a host never sees it.

One property is worth knowing before reading that test's block size checks.
`Channel::process()` tests every `kSettleCheck` = 32 samples whether a glide has
arrived, and counts those 32 from the start of each call — so a block size that
is not a multiple of 32 puts the test on a different grid, and `checkSettled()`
snaps exactly onto the target when it fires. A **settled** equaliser is therefore
bit-identical at every block size, and a **gliding** one is not: at block 1025
the streams differ by 1.7e-06 for the length of the glide and by 1.3e-14 — the
double rounding floor — a second later. That is a residual coefficient move
bounded by the core's `kSettleTolerance` of 1e-6, around 120 dB down, lasting a
fraction of a second. It is in the core, so `foo_dsp_paraeq` has it too, a
foobar2000 chunk being no more a fixed size than a VST buffer is, and neither
wrapper can do anything about it from outside.

### The ParaEQ editor

It is the same editor the foobar2000 component draws — a curve with five handles
on it, the readout strip under it, and Bypass / Flatten / Reset along the bottom.
Not a port of it and not a second one: `paraeq_editor.{h,cpp}` is one file that
both hosts compile, mirrored into `plugins/WinVST/ParaEQ` by `sync_cores`
alongside the core. Two owner-drawn editors of that size would have diverged
inside a month, and the one thing worse than no curve in the VST would be a
different curve.

Making that possible took three small changes to the editor, all of them in the
direction of it knowing less:

| | |
| --- | --- |
| **No `stdafx.h`** | It includes `windows.h` and `windowsx.h` and names what it uses. In the foobar2000 build it is still reached after the SDK's own header, so the WinSock2 ordering that `stdafx.h` documents is untouched. |
| **Colours by enum, not by GUID** | It used to ask for `ui_color_background` and friends. A VST2 host has no theme to ask about — there is nothing in the ABI that corresponds — so the editor now names the three colours it wants as `paraeq_editor::Color` and `ui_element.cpp` maps those onto the SDK's GUIDs. Everything else it draws is blended from those three, so it is correct in light and dark without knowing which it is in. The VST answers "no" to all three and gets `GetSysColor()`. |
| **`__ImageBase`, not `core_api::get_my_instance()`** | `RegisterClassExW` and `CreateWindowExW` want the module handle. The linker's `__ImageBase` sits at the start of the mapped image, so its address *is* the `HINSTANCE`, resolved at link time with nothing to call and nothing to fail. |

What is left on the VST side is `ParaEQEditor`, which is a join and nothing else:
`AEffEditor` on one face, `paraeq_editor::Host` on the other, and in the middle
the one thing neither can know — that a VST parameter is a float in 0..1 and a
`paraeq::Params` is not.

| | |
| --- | --- |
| **640 × 400, fixed** | The one number here that is a taste. Resizing a VST2 window needs `audioMasterSizeWindow` and a host that honours it, and the hosts that do not leave the plug-in drawing outside a window they still think is the old size. For comparison the foobar2000 modal dialog gives the same editor about 486 × 384, and the UI element gets whatever it is dragged to. |
| **A drag becomes `setParameterAutomated()`** | Not `setParameter()`. A host that is recording has to be told a knob moved, and `audioMasterAutomate` is the only thing in VST2 that says so. Only the sliders that actually changed are sent, which is what keeps a drag on one handle out of the other fifteen lanes. |
| **`effEditIdle` is how the editor finds out** | VST2 has no callback in the plug-in's direction saying a parameter moved: the host calls `setParameter()` and nothing tells the editor. So `idle()` compares the sixteen sliders against the last agreed set and pushes a change in when it finds one. **A host that never sends `effEditIdle` shows an editor that does not follow automation.** There is no second mechanism in the ABI to fall back on, and building one out of a timer would fight the editor's own. |
| **Comparison is on sliders, not on `Params`** | The round trip through the mapping is not exact to the last bit, so comparing `Params` would make every push look like a fresh change and bounce it back. Comparing the sixteen floats is exact, and that is what `m_synced` holds. |
| **The window is per-opening, the settings are not** | `open()` creates the editor and `close()` destroys it, because a host opens and closes the window every time the user shows and hides it. Nothing is lost with it: the settings live in the sixteen parameters, which is where a VST's state belongs. |

The footer's fourth button, "Add to DSP chain", never appears: it exists for the
foobar2000 UI element, which can be on screen while the equaliser is not in the
chain. A VST in a host's window is in the chain by construction, so
`editorEngaged()` takes the default and says so.

`paraeq_vst_verify` drives both directions — a push arrives in the parameters and
in exactly three automation lanes, an `idle()` pulls a host-side move into the
editor, and a quiet `idle()` writes nothing — and `vst_host_verify` does the
window lifecycle over the ABI, where a host would break it: size before there is
a window, open, close, open again, close again, and a second `effEditClose` in a
row for the hosts that send one.

### The ParaEQ Audio Unit editor

Sixteen controls with their real units and six clumps makes a good generic view,
and a generic view is still a column of sliders that says what each one *is* and
nothing about what they do together. So `plugins/MacAU/ParaEQ` publishes
`kAudioUnitProperty_CocoaUI` and draws its own: a response curve over a console
strip, read left to right as **low cut, bass, reverb cut, brilliance, hiss cut,
output**.

Those are the restoration names, the same five the foobar2000 editor uses and
from the same place — the transfer literature calls them that rather than HP,
LF, LMF, HMF and HF. The shorthand stays on the parameters, which is what a
host's own generic view draws and what a saved preset stores. The four gain
knobs are drawn larger than the other twelve controls, which is the point of a
fixed layout rather than decoration: those four are what an operator reaches for
once the session has started, and everything else gets set once for a box of
records.

It is the editor EmbraceNG — the tango DJ player this core is also embedded in
— carries for it, moved from AUv3 to AUv2. There a view holds an `AUParameterTree` and
asks an `AUParameter` for a value, a range and a default; here it holds the
opaque `AudioUnit` and asks `AudioUnitGetParameter`,
`kAudioUnitProperty_ParameterInfo` and an `AUEventListener` for the same three
things. Below those four helpers at the top of `ParaEQView.mm` the two files are
the same editor. What the move changed:

| | |
| --- | --- |
| **`AUParameterSet`, not `AudioUnitSetParameter`** | The second one sets the value and tells nobody, so a host's generic view would go on showing the old one and its automation would not record the move. Naming the view's own listener as the sender is what keeps the change from arriving back as news — the same job the AUv3 original's originator token does. |
| **A drag is bracketed by gestures** | `kAudioUnitEvent_Begin`/`EndParameterChangeGesture` on mouse down and up, so a host records one automation move and one undo step rather than two hundred. The wheel and the arrow keys send bare value changes: a gesture claims a move has a beginning and an end, and those two have neither. |
| **The listener callback may touch AppKit** | It arrives on the run loop the view was built on, which is the main one. The AUv3 observer could not: it is called on whatever thread set the value, the render thread included, and has to hop. |
| **A preset load is a property change** | `AUBase::RestoreState` writes the parameters into the scope directly and then says so once, with `kAudioUnitProperty_PresentPreset`. Without listening for that the editor would still be showing the old setting. |
| **Defaults come from the unit** | An `AUParameter` has no default value, so EmbraceNG carries a side protocol for the unit to answer "what does double-click restore?" with. An AUv2 parameter carries its own, and that protocol is gone here. |
| **Nothing about a control is written down twice** | Whether a knob is logarithmic is the unit's own `DisplayLogarithmic` flag and whether it is bipolar is whether its range crosses zero, both read from the table the host's generic view reads. An editor knob and a host slider cannot come to disagree about what a control's travel means. |
| **One more section** | `OUTPUT` is two columns where the AUv3 original had one, because this unit has a **Bypass** control of its own — the core glides to it rather than switching — and a switch needs the width to say so. |
| **No `NSLocalizedString`** | It reads the *main* bundle, which in a plug-in is the host application, so every string would be looked up in Logic's tables and come back as whatever Logic has under that key. The strings are literals. |

The view class is in the component's own binary rather than in a nested
`.bundle` under `Resources/`, which is the other way this is often done: one
thing to build, one thing to sign, one thing to notarise. The property answers
with `-[NSBundle bundleForClass:]` rather than the identifier out of the
`Info.plist`, so there is no copy of that string to drift.

`paraeq_core.h`'s drawing path is what makes the curve cheap enough to redraw on
a drag: `curveTrig()` builds a table of the per-point constants once per resize
and `curveDb()` walks the whole cascade over it, which is about a fifth of the
work of asking `magnitudeDb()` per point. Both editors use it; see [Drawing the
curve cheaply](#drawing-the-curve-cheaply).

Flatten and Restore Defaults are on a contextual menu rather than on buttons.
An AU editor is a rectangle inside somebody else's window with no toolbar of its
own, and two buttons' worth of chrome across the bottom would cost more room
than the gestures are worth. Flatten is the VST editor's: every gain and the
trim to 0 dB, leaving frequencies and Q alone, because the setup is worth
keeping and the curve is not.

### What version a host shows

`AEffect::version` and `effGetVendorVersion` are both **byte packed**, most
significant byte first: `0x00010001` is 1.0.1. A host that prints a version
splits it back into bytes — Audacity does — which is why the stock Airwindows
answer of `1000` appears there as **3.232**. `0x000003E8` is 3, and then 232.
All 518 stock plug-ins in `WinVST` answer 1000 and are left alone: it is a house
pattern rather than a version, exactly as `0x00010000` was on the Audio Unit
side until these three shipped.

The three that do ship keep the number in `kVersion`, beside `kUniqueId` in the
wrapper's header, and use it twice:

| | |
| --- | --- |
| `setVersion(kVersion)` in the constructor | Fills `AEffect::version`, which a host may read straight out of the struct without asking for anything. |
| `getVendorVersion()` returns `kVersion` | Answers `effGetVendorVersion`, which is what a host asks when it would rather ask. |

Both of them, because hosts differ on which one they want and some consult one
and fall back to the other — Audacity asks first and falls back to the field —
so a plug-in that answers two different numbers is one that displays as two
different versions in two different hosts. `vst_host_verify` checks each against
`kVersion` and prints them the way a host would, so a mismatch reads as
`0x00010001 (1.0.1)` beside whatever it should have been.

The numbers are the Audio Units': **Declick and Dehum at 1.1.2, ParaEQ at
1.0.2**, matching `k<Name>Version` and the `version` integer in each bundle's
`Info.plist`, so that one plug-in is one version wherever it is loaded. Moving a
version means moving all of them — nothing here is a single place a version
lives across three plug-in formats, only checks that the copies agree.

### The Mac ports

**`plugins/MacVST` is the same wrapper**, mirrored from `WinVST` by
`sync_cores` exactly as `LinuxVST` is — the wrapper section above covers all
three. That is also how the rest of the tree does it: the 518 stock plug-ins
keep byte-identical VST sources in the two folders and differ only in the
project files, which is the whole of what a MacVST port *is*. So there is
nothing to describe here that the sections above have not, and nothing to test
separately: `declick_vst_verify` and `dehum_vst_verify` are already compiling
that code, or the mirror check fails first.

**`plugins/MacAU` is a different wrapper**, because an Audio Unit is a different
interface. For Declick and Dehum it is the same seven sliders, same mappings,
same defaults, same core, same dither — the differences are all in how the host
is talked to:

| | |
| --- | --- |
| **`GetLatency()`, `GetTailTime()`** | Seconds, not samples, so both are `(1.0/GetSampleRate())*cfg.latency` and Airwindows' house `*0.0` for Dehum. There is no `setInitialDelay()`: the host asks. |
| **`PropertyChanged(kAudioUnitProperty_Latency)`** | The AU's `ioChanged()`. Declick calls it when Max repair or Model order resizes the pipeline; Dehum never calls it at all. |
| **`Reset()`** | Where `resume()` went. Declick starts the next take from silence, Dehum flushes and keeps the lines — the same split as the VST, at a different entry point. |
| **`Initialize()`** | The one thing an AU makes *easier*. It is told its sample rate before it renders, which a VST is not, so the reallocation a rate change forces happens here instead of on the first render call. |
| **`kAudioUnitRenderAction_OutputIsSilence`** | Cleared by both. Declick holds `cfg.latency` samples of pipeline and Dehum's notches are integrators that ring on into a gap, so silence in is not silence out for either, and a host that trusts the flag would clip that off. |
| **No `getChunk`/`setChunk`** | An AU's parameters *are* its state; the host serialises them. So there is no preset chunk to get wrong, and no equivalent of the VST tests' chunk round trip. |
| **No `getParameterDisplay`** | The cost of keeping every slider generic 0..1 like the other 542 AUs in the tree — and of keeping the two formats interchangeable, since `paramsFromControls()` is then the VST's unchanged. Max repair reads `0.2` in an AU host rather than `4.0 ms`. The mapping tables in [Declick parameters](#declick-parameters) and [Dehum parameters](#dehum-parameters) are what to read it against. |

**`plugins/MacAU/ParaEQ` departs from that last row deliberately**, because it is
the one plug-in here with no VST twin to stay interchangeable with. Nothing
constrains its parameters to 0..1, so they are not: each of the sixteen carries
its real unit — `kAudioUnitParameterUnit_Hertz` for the five frequencies,
`_Decibels` for the five gains and the trim, `_Indexed` with value strings for
the high-pass slope, `_Boolean` for the two bell switches and bypass — with
`DisplayLogarithmic` on everything that is a ratio, and the six bands as parameter
clumps so a generic view draws headings instead of sixteen undifferentiated
sliders. The payoff is not only that a host shows `100 Hz` rather than `0.43`:
the number the host stores **is** the number `paraeq::Params` holds, so a setting
written down in an AU session is the same setting in `foo_dsp_paraeq`, with no
mapping table in between to get wrong.

It is also the only one of the three with an **editor**. Sixteen grouped sliders
is a readable generic view and not an interface, where Declick's four and
Dehum's six are a form that reads perfectly well, so ParaEQ is the one that
answers `kAudioUnitProperty_CocoaUI` — see [The ParaEQ Audio Unit
editor](#the-paraeq-audio-unit-editor).

Its wrapper is also much the shortest of the three, and by subtraction. The core
is zero latency and allocates nothing — `heapBytes()` is 0, `retune()` cannot
fail — so there is no `GetLatency()`, no `GetTailTime()`, no `PropertyChanged`,
no pre-roll and no rebuild path. What is left is gathering sixteen controls,
noticing when one moved, and handing the host's own `Float32` buffer to the core
in place.

Three things to know about building them.

The `.xcodeproj` files are the Airwindows templates, cloned from the DeCrackle
folders next to them and left alone otherwise, so the documented route still
works for anyone who has the toolchain they expect. One setting is added to each:
`CLANG_CXX_LANGUAGE_STANDARD = "c++11"`. The cores use `= delete` and default
member initialisers, and the templates target the gcc 4.2 that came with
Xcode 3.2.6, which has neither. A newer Xcode honours the setting; Xcode 3
ignores it, and cannot build these two plug-ins.

The Audio Unit projects also reach for Apple's CoreAudio `AUPublic` and
`PublicUtility` sources under `$(SYSTEM_DEVELOPER_DIR)`, where no Xcode has put
them for over a decade. Those are not redistributable and are not here, which is
the same hole `plugins/WinVST/vst2_shim` fills on the Windows side.
`plugins/MacAU/au_shim` fills enough of it to compile the wrappers and drive
them from a test — it is **not** an ABI and nothing loads it.

The third thing is what actually produces a loadable plug-in, and it is not the
`.xcodeproj`. Apple's replacement for those missing sources is
[AudioUnitSDK](https://github.com/apple/AudioUnitSDK): the same base classes,
maintained in the open under Apache 2.0, namespaced into `ausdk` and with a
factory-function entry point in place of the Component Manager one.
`plugins/MacAU/au_compat/AUEffectBase.h` is 50 lines reconciling those spellings,
so putting it ahead of the SDK on the include path compiles the three wrappers
**unmodified** — and `scripts/build.sh` then links universal `.component` bundles
whose `Info.plist` carries the `AudioComponents` array a modern host registers
from. That plist change and the entry point are the useful half of what
`plugins/MacSignedAU` is, and they are in these three sources now; the other half
of MacSignedAU is a Header Search Path pointing at a CoreAudio SDK on somebody
else's desktop, which is the problem rather than the answer.

So **all three of these Audio Units do load in a real host**: `scripts/build.sh
--validate` builds them and puts each through `auval`, and all three pass. Read
`au_shim`'s [README](../MacAU/au_shim/README.md) for which of the two headers a
given build gets, and for what each half of the verification does and does not
establish.

That is also why `kDeclickVersion` and `kDehumVersion` now move. They read
`0x00010000` in all 542 Airwindows folders here, and when `foo_dsp_declick` and
`foo_dsp_dehum` went to 1.0.1 these were deliberately left alone as house
pattern rather than a per-plug-in version — correct at the time, because nothing
could build them. Now that they ship, the constant is doing its real job: it is
what the bundle registers with, what a host displays, and what the Component
Manager keys a cached registration on. A shipped 1.0.1 that registers as
`0x00010000` is one a host may keep serving from cache. So the three here carry
their own versions — Declick and Dehum at 1.1.2, ParaEQ at 1.0.2 — and the
other 542 are untouched. The VST2 ports carry the same
three numbers in `kVersion`, for the same reason and with the same consequence
if they drift: see [What version a host shows](#what-version-a-host-shows).

### Checking that they agree

`processDoubleReplacing` is left undithered — that is the standard Airwindows
arrangement, and it also makes it the path to compare. **`declick_vst_verify`**
and **`dehum_vst_verify`** drive `declick::Channel` and `dehum::Channel`
directly with the same `Config`, and require each VST's 64-bit output to match to
the bit. Both report **0.000e+00**. They run on every build, on both
architectures, and need no SDK. See [Verification](#verification).

Under CMake they compile the `WinVST` copy of the wrapper, because that is the
canonical one; the `LinuxVST` and `MacVST` copies are required to be
byte-identical to it by `sync_cores`, so what they establish holds for all three
ports or the mirror check fails first. `build_linuxvst.sh` builds and runs the
same two sources against the `LinuxVST` copies anyway — this CMake project stops
at a `FATAL_ERROR` anywhere but Windows, since what it builds is a foobar2000
component, so that script is the only way to run them on Linux at all.

**`declick_au_verify`** and **`dehum_au_verify`** do the same job for the Audio
Units, which are not mirrors of that wrapper and so need testing in their own
right. One concession: an AU has only a float path, so the requirement is two ULP
rather than the bit. Worst measured **1.36** and **1.41 ULP**. They also carry
the AU-specific assertions — latency and tail in seconds, exactly when
`PropertyChanged` fires and when it must not, `Reset()`, the silence flag,
rendering in place, and the parameter table the host is handed. See
[Verification](#verification).

Those two link the plug-in into the test binary, which is why they can compare
bit for bit — and also why they cannot see the ABI at all.
**`vst_host_verify`** is the other half: it does what a host does and nothing
else — load the module, look up `VSTPluginMain`, read the `AEffect`,
`dispatcher(opcode)`, `processDoubleReplacing`, `effClose` — and never touches
the plug-in's C++ classes through that path. It also links the same plug-in
statically, drives that copy through the identical opening sequence, and requires
the two streams to be equal to the bit. Both copies are the same source compiled
the same way, so any difference between them *is* the ABI, the dispatcher, the
thunks or the calling convention: the four things nothing else here can see.

One file covers both platforms. A `.dll` and a `.so` differ in how the module is
opened and in how a process asks how much memory it is using, and in nothing
else that matters here — which is the same fact that lets one plug-in source
tree serve hosts on both. `build_winvst.ps1` runs it against each DLL it
produces and `build_linuxvst.sh` against each `.so`. All eight pass — six DLLs
and two `.so`s — worst deviation **0.000e+00**. Along the way it checks the things a host would notice
and a compiler would not:

| | |
| --- | --- |
| `sizeof(AEffect)` | 144 on x86, 192 on x64, read out of the loaded module |
| flags | exactly `canReplacing \| programChunks \| canDoubleReplacing`, plus `hasEditor` for ParaEQ and for neither of the others |
| `uniqueID` | `0x64636C6B` `'dclk'`, `0x6468756D` `'dhum'`, `0x70726571` `'preq'` |
| `resvd1`, `resvd2`, `future[56]` | left zeroed, as the host expects |
| the deprecated `process` | a no-op function, not a null pointer, because a host old enough to call it will not check first |
| opcode routing | all seven parameter names arrive at the right index; displays and labels stay inside `kVstMaxParamStrLen` in a buffer bigger than promised |
| the latency contract | Declick declares 6192 samples at 44.1 kHz with `effGetTailSize` matching, and calls `audioMasterIOChanged` twice for the two structural parameters; Dehum declares 0 and calls it **never** |
| `main` | exported as well as `VSTPluginMain`, for hosts that predate the rename, and reaching the same plug-in when called. On Windows the `.def` makes them one address and that is asserted too; the `.so` gets a forwarder instead, because there is no `.def` to alias with |
| `effClose` | 24 open/close cycles do not accumulate private bytes. This is a contract, not a nicety: the host frees nothing, so a shim that forgets the `delete` leaks the whole instance on every plug-in scan, and Dehum carries ~400 kB of analysis state per channel |

### Building the VST2 plug-ins

```powershell
.\scripts\build_winvst.ps1
```

All three plug-ins, both architectures, into `..\dist\winvst\`:

| File | For |
| --- | --- |
| `Declick32.dll`, `Dehum32.dll`, `ParaEQ32.dll` | 32-bit hosts |
| `Declick64.dll`, `Dehum64.dll`, `ParaEQ64.dll` | 64-bit hosts |

A VST2 host identifies a plug-in by its `uniqueID` rather than its filename, so
both architectures can live in the same VST folder. Install by copying.

```sh
scripts/build_linuxvst.sh
```

Both plug-ins into `../dist/linuxvst/`, as `Declick.so` and `Dehum.so` — the
names `plugins/LinuxVST/CMakeLists.txt` would give them. Copy them where your
host looks; `~/.vst` is the usual place. It checks the mirrors first, then runs
`declick_vst_verify`, `dehum_vst_verify` and `vst_host_verify` over what it
produced, so nothing ships from here unverified either. `CXX` and `OUTDIR` are honoured, so
`CXX="g++ -m32" OUTDIR=... scripts/build_linuxvst.sh` builds a 32-bit plug-in on
a machine with multilib installed. Two flags this one adds over the Windows
build, both about being loaded by somebody else's program:

* **only two symbols are exported**, `VSTPluginMain` and `main`, via
  `-fvisibility=hidden` and a version script. Some hosts `dlopen` with
  `RTLD_GLOBAL`, and one that does would otherwise get the plug-in's whole C++
  surface in its global namespace.
* **`-static-libstdc++ -static-libgcc`**, which is the same call `/MT` makes on
  Windows. A plug-in linked against the build machine's libstdc++ refuses to
  load on a distribution with an older one, and the user's host is where they
  would find that out.

Steinberg's `vst2.x` sources are not redistributable and are not here —
`plugins/AirwindowsWinVSTTemplate.txt` says so and adds "so you're on your own".
That left `plugins/WinVST` as source nobody could build without a copy of a
discontinued SDK, so **`plugins/WinVST/vst2_shim`** is a clean-room
implementation of the VST2 ABI: the `AEffect` structure, the opcode dispatcher
and `VSTPluginMain`, written from the published description of the interface and
MIT licensed with the rest of the tree. JUCE, Ardour and LMMS all arrived at the
same place. There is no MIDI, no offline processing and no speaker arrangements;
those opcodes answer "not supported", which is what the stock Airwindows plug-ins
answered by not overriding them.

There **is** an editor, as of ParaEQ: `AEffEditor` plus the four `eff*Edit*`
opcodes a host actually sends, with no VSTGUI and no idea what is drawn inside.
A plug-in that never calls `setEditor()` is exactly the plug-in it was before,
which is why Declick and Dehum still assert that no editor is claimed.

The build does not go through each plug-in's `VSTProject.vcxproj`. Those ask for
Windows SDK 8.1 and expect the SDK at a path outside this tree, so
`build_winvst.ps1` drives `cl.exe` directly. The `.vcxproj`, `.sln` and `.def`
files are as Airwindows ships them apart from two settings, toolset v141 in
place of v140 and C++17: Visual Studio 2017 is the oldest one supported, and
`declick_core.h` uses aligned `new`. The documented "drag the folder into
VSTProject and press build" route still works for anyone who does have the real
SDK.

`build_linuxvst.sh` avoids `plugins/LinuxVST/CMakeLists.txt` for the same
reason. That project builds all five hundred plug-ins and wants Steinberg's
sources in `LinuxVST/include/vstsdk`, so it is left alone and keeps working for
anyone who has them — including for these two, which are registered in it with
`add_airwindows_plugin(Declick)` and `add_airwindows_plugin(Dehum)` like every
other plug-in there. The one change to it is that Declick builds as C++17
rather than the project's C++14, for the same aligned `new`.

#### The part to be suspicious of

An ABI is not an interface you get to design. Every byte offset in `AEffect` and
every opcode number was fixed in 1999 by hosts that are still in use, and
getting one wrong does not fail to compile — it loads, and then a host reads a
function pointer out of the middle of an integer field and jumps to it. Three
things push back:

* **Every field offset is `static_assert`ed**, along with `sizeof(AEffect)` — 144
  bytes on 32-bit, 192 on 64-bit. Those two numbers are the one thing an outsider
  can check the shim against without reading it.
* **The opcode enums keep their deprecated entries.** `effGetVu` is unused and
  deleting it would silently move the ten opcodes after it. The values the
  plug-ins depend on are additionally asserted as literals.
* **`vst_host_verify` loads the finished plug-in** — see above.

What none of that proves is that 144 and 192 and `effGetChunk == 23` are
themselves right, because the shim and its test read the same header and so agree
by construction. Only a real host settles that.

### Building the Audio Units

```sh
plugins/MacAU/scripts/build.sh --validate
```

All three, universal (x86_64 + arm64), into `../dist/au/mac/`:

| File | For |
| --- | --- |
| `Declick.component`, `Dehum.component`, `ParaEQ.component` | any AU host — Logic, Live, Reaper, Audio Hijack |

macOS only, and it needs nothing installed but the Xcode command line tools: on
the first run it fetches Apple's AudioUnitSDK into `plugins/MacAU/external/`
(git-ignored, pinned to 1.3.0 — 1.4.0 wants `std::expected` and so an Xcode
16.3 libc++). It runs the three `*_au_verify` suites before it builds anything,
and with `--validate` it installs the result into
`~/Library/Audio/Plug-Ins/Components` and runs `auval` over each one. The
components come out ad-hoc signed, which is not a nicety: `clang` signs an arm64
slice as it links it and `lipo` discards that, so an unsigned universal
component is not merely unsigned on Apple Silicon — it does not load.

The build does not go through each plug-in's `.xcodeproj`, for the same reason
`build_winvst.ps1` does not go through `VSTProject.vcxproj` — those expect an SDK
at a path outside this tree — and the projects are left exactly as the Airwindows
template ships them, so the documented route still works for anyone who has it.
There is also no `Rez` step: Component Manager registration has been dead since
10.7 and building a `.rsrc` would need `AUResources.r` from the same missing SDK,
so the `.r` files are left in place unbuilt and the `AudioComponents` array in
each `Info.plist` does the registering.

```sh
plugins/MacAU/scripts/package.sh --sign "Developer ID Installer: ..." \
                                --codesign "Developer ID Application: ..." \
                                --notarize --notary-profile ShellacFilters
```

One `ShellacFilters-AudioUnits-<version>.pkg` beside them, installing all three
into the user's `~/Library/Audio/Plug-Ins/Components` — a home-directory install,
so it asks for no administrator password. The notarisation arrangement is the
same one `plugins/vdjplugin/scripts/package.sh` uses and the options are spelled
the same way, including the refusal to take an app-specific password as a
command-line argument where it would land in the shell history; `--help` on
either script has the three ways to supply credentials. The three plug-ins are
versioned independently — `k<Name>Version` in each folder, which is what the
bundle registers with and what a host displays — and the package takes the
highest of them, because a package carrying a 1.0.1 Declick while calling itself
1.0.0 would be describing its oldest contents rather than itself. Each bundle's
registration integer and `CFBundleShortVersionString` are checked against its own
constant on the way past, those being three hand-written copies of one number.

---

## Known limitations

* **Dehum needs a few seconds of a track before it acts**, its detection is per
  channel, and a buzz whose fundamental is weak will not be found by searching
  for the fundamental — see [What Dehum will not do](#what-dehum-will-not-do).
* **No dark mode in three of the four configuration dialogs.** DeCrackle,
  Declick and Dehum are plain Win32 dialogs full of standard controls, and
  following foobar2000 2.x's dark mode with those means `fb2k::CDarkModeHooks`,
  which pulls in libPPUI and WTL — WTL is not in the SDK archive and would
  have to be downloaded separately. They were kept dependency-free instead. The
  equaliser's editor is owner-drawn and does follow dark mode, because painting
  it took two colours from the host rather than a dependency.
* **The equaliser draws its curve for the playing file's sample rate**, read
  from the track's own metadata. If a resampler sits ahead of it in the DSP
  chain, the plot is drawn for the wrong rate — visibly only within an octave
  or so of Nyquist, where the bilinear transform warps most, and the audio is
  unaffected either way.
* **DeCrackle does not flush its tail.** Like the VST, the last few
  milliseconds sitting in its delay line at end of playback are not emitted.
  Flushing them would add samples to the stream and break gapless playback.
  (Declick does flush, so its stream length is preserved.)
* **Declick's C++ core is not a line-by-line port of the Python prototype** in
  `scratchpad/tune/`. It is an independent implementation of the same method
  with a different block and noise-estimate structure; the two agree on roughly
  the same clicks but not sample for sample. The C++ version is the one that
  was calibrated, and the figures above are its own.
* **Declick has not been evaluated on stereo vinyl**, only on mono 78s. It
  should work — the detector is per-channel and format-agnostic — but the
  thresholds were tuned on shellac.
* **The body stage reaches about 45% of what the decrackled version takes from
  1–2 kHz under a held note, and its key is causal.** The *squirt* is gone by
  ear, but the key needs about 0.1 s of crackle history. It opens a little
  later at the start of a stream than the centred key the prototype used.
  Without its 2 s release it flickered under steady crackle and reached 40%.
  On a record whose crackle is too sparse to open the key, the bodies
  stay — see [The body stage](#the-body-of-dense-crackle-the-body-stage).
* **Declick's audio thread can still allocate in one case:** a caller pushing
  more than `Config::maxBlock` — 16384 samples — between pulls. That grows the
  output ring once, and then never again. Neither wrapper comes near it.
  Everything else is reserved in `configure()`; see
  [Real-time safety](#real-time-safety).
* **A format change rebuilds Declick's channels from `on_chunk`.** A different
  channel count or sample rate mid-stream constructs new `Channel` objects,
  which allocates, on whatever thread foobar2000 called it from — the one route
  left that does, now that a control move reuses the channels in hand. It happens at
  track boundaries rather than during steady playback, and foobar2000's own
  `insert_chunk()` allocates on every chunk regardless, so the DSP is not the
  binding constraint there. The VST has no equivalent path: it reconfigures in
  place.
* **ARM64EC is wired up in CMake but untested.**

---

## Licence

The DeCrackle algorithm is © Chris Johnson / Airwindows, MIT licensed — see
`LICENSE` at the repository root and <https://www.airwindows.com/>. The
foobar2000 SDK is covered by its own licence, included in the downloaded
archive as `sdk-license.txt`; it is not redistributed here.
