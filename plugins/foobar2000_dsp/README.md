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
| `foo_dsp_declick-1.0.0.fb2k-component` | Same, for the declicker |
| `foo_dsp_dehum-1.0.0.fb2k-component` | Same, for the dehummer |
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
.\scripts\check_win7.ps1 ..\dist\foo_dsp_declick-1.0.0.fb2k-component
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
an `AVX2` build loads fine on a CPU without AVX2 and then crashes.

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

---

## Declick parameters

Add *Declick (AR interpolation)* to the chain and press **Configure selected**.

| | |
| --- | --- |
| **Sensitivity** | The one to reach for. Raise it until the crackle goes, then back off as soon as the music starts to dull. Maps onto the trigger threshold in robust sigmas: 0 → 6.0σ, 1 → 2.5σ. |
| **Extent** | How far a detection spreads into its own tail before the repair stops. Raise it if repairs leave a residual tick behind them. |
| **Max repair** | Longest single repair. Anything longer is treated as music and left alone. 4 ms suits 78s. |
| **Passes** | A second pass catches clicks the first one uncovers; the model is refitted in between. Small but real gain, roughly 50% more CPU. |
| **Model order** | 8–256, default **64**. The one control where spending CPU clearly buys quality: 128 measurably beats 64 and 256 beats that again. Neither is the default because they cost roughly 3.6× and 10× the CPU of order 32, where 64 costs about a third more — but at 50× realtime, order 128 is affordable on far weaker hardware than it used to be. See [Does more CPU help?](#does-more-cpu-help). |
| **Repair depth** | How much of each click to subtract. 0 removes the calibrated fraction that adds the least error of its own; 1 replaces the damaged samples outright. See [Repair depth](#repair-depth). |
| **Dry/Wet** | 0 bypasses. |

Measured over 20 s excerpts of four 78 rpm transfers, at the default repair
depth of 0. The unprocessed originals sit at **71** impulsive events/s.

| Sensitivity | samples repaired | events/s | collateral damage | HF change | precision |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 0.7 % | 39 | −60.8 dB | −0.3 dB | 0.91 |
| 0.30 | 1.5 % | 26 | −50.2 dB | −0.5 dB | 0.85 |
| **0.60** (default) | 3.4 % | 21 | −46.5 dB | −0.8 dB | 0.72 |
| 0.70 | 4.6 % | 20 | −45.4 dB | −1.0 dB | 0.66 |
| 0.85 | 7.6 % | 19 | −43.2 dB | −1.3 dB | 0.55 |
| 1.00 | 14.0 % | 18 | −41.8 dB | −1.7 dB | 0.43 |

Note how flat the right-hand end is: quadrupling the number of samples touched
(3.4 % → 14.0 %) only takes 21 events/s down to 18. Past about 0.7, sensitivity
mostly buys collateral damage. **Repair depth is the more effective lever**.

Throughput: ~180× realtime at the default, measured end to end over 60 s of
44.1 kHz mono including file I/O — well under 1 % of one core on a modern
desktop. See [Where the time goes](#where-the-time-goes-and-what-was-done-about-it).
Latency ~18 ms, reported to foobar2000 so visualisations stay in sync. Note
that `config().latency` is a buffering delay, not a shift: that many samples
must go in before the first block comes out, but the emitted stream is aligned
with the input. The component reads ahead rather than holding the output back —
it feeds every chunk in and emits whatever the pipeline has finished — so what
those 18 ms describe is how far ahead of the listener the decoder is running,
which is exactly what `get_latency()` exists to tell the core.

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

### Real-time safety

An audio callback must not allocate: `malloc` may take a lock, and a lock held
by a lower-priority thread is a dropout. `declick_rt_verify` replaces global
`operator new` and requires the count to be **zero** across the processing path,
every parameter change, `reset()`, `prime()` and `drain()`, in both access
patterns — one sample at a time as the VST does it, and whole-chunk-then-drain
as foobar2000 does.

Getting there took two fixes, neither of which was visible until counted:

* **The output FIFO** was a `std::vector` that grew by `push_back` to 65536
  samples and was then compacted with `erase()`. About a second and a half into
  *every stream* the audio thread took a **720 kB reallocation**. It is now a
  fixed-capacity ring, sized for `maxBlock + 2 * kBlock`.
* **The per-click interpolation scratch** was `assign()`ed to a length that
  varies with the run being repaired, so it grew whenever a longer click turned
  up than any seen so far — a long tail of reallocations that never quite
  stopped. It is now reserved for the worst case the detector can produce.

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
the largest single item — are **not reserved at all** unless
`Config::wienerAlpha` is greater than zero, which by default it is not. With the
Wiener path deliberately engaged it is 1.2 MB per channel.

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
Subtracting a fixed **0.45** of the discrepancy is what ships. The **Repair
depth** control scales from there towards outright replacement:

| depth | click reduction | collateral harm | whole-file error | events/s left |
| --- | --- | --- | --- | --- |
| **0.00** (default) | 1.79 dB | −42.4 dB | **+0.60 dB** | 28 |
| 0.25 | 2.00 dB | −40.1 dB | +0.07 dB | 21 |
| 0.50 | 1.98 dB | −38.2 dB | −0.70 dB | 17 |
| 1.00 (full replacement) | 1.29 dB | −35.4 dB | −2.52 dB | 15 |

Two things worth noting. Full replacement is worse at *removing clicks* than a
partial subtraction is (1.29 dB against 2.00 dB) — its own interpolation error
partly undoes the repair. And the clean master itself measures 27 impulsive
events per second; the default lands at 28, essentially back at that natural
level, while higher depths drive it *below* the master, which means they are
removing real musical transients. Hence the conservative default. Raise it if
you would rather trade some added error for less audible crackle — the numbers
above say 0.25–0.5 is the sensible range to explore.

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
| **flat 0.45** (ships) | **1.79 dB** | **−42.4 dB** | **+0.60 dB** | **28** |

The machinery is still in the tree, off by default (`Config::wienerAlpha = 0`,
so the recursion is not even run) and sweepable from `declick_cli --alpha`.
Material with different click statistics — louder clicks on stereo vinyl, say —
might land somewhere else, and the code costs nothing while it is off.

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

The defaults are conservative. On the same four transfers (originals at 71 events/s):

| | events/s | collateral damage | HF change |
| --- | --- | --- | --- |
| default (s 0.60, depth 0) | 21 | −46.5 dB | −0.8 dB |
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
| order 64 (ships), w 0.45 | 2.47 | −41.2 | +0.76 | 28 |
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

Measured end to end over 60 s of 44.1 kHz mono including file I/O:

| order | x64 | x86 | before optimisation | 2014 MacBook Air, stereo (estimated) |
| --- | --- | --- | --- | --- |
| **32** (default) | **181×** | 156× | 84× | ~25× |
| 64 | 110× | 99× | 39× | ~15× |
| 128 | **50×** | 45× | 16× | ~7× |
| 256 | 18× | 15× | 5× | ~2.5× |

The last column is an estimate, not a measurement: a 1.4 GHz Haswell i5-4260U
is roughly 3–4× slower per core than the machine above, and stereo doubles the
work. After the optimisation below, **order 128 is viable on that machine** —
it was not before.

`kMaxOrder` is 256 and **the default is 64** — where paying for the one lever
that clearly pays is still cheap, at about a third more CPU than 32, which
leaves plenty of headroom under a real-time deadline. Going further is the
user's call rather than a silent change; if you have the headroom, order 128 is
the setting to reach for, and offline nothing has a deadline at all.

#### Where the time goes, and what was done about it

Profiled by the difference between a clean file and a crackly one, the
per-click interpolation is only **2 % of the run time at order 32 and 7 % at
order 256**. Almost everything is the model fit and the two prediction-residual
passes. So an earlier note here, suggesting the per-click residual be shared
across clicks and patched per gap, was aimed at the wrong 5 % and has been
dropped.

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

Latency is **zero**: the detector reads the signal but does not sit in the path,
so the component processes in place and needs no FIFO. Throughput is **159×
realtime** on x64 and 146× on x86, measured end to end over 177 s of 44.1 kHz
mono including file I/O. Memory is **2.0 MB per channel** at 44.1 kHz and 3.9 MB
at 96 kHz, fixed at `configure()` and independent of the parameters.

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
somewhere else entirely.

| | |
| --- | --- |
| ← → | The selected band's frequency, a semitone a press. |
| shift + ← → | The same, four semitones a press. |
| ctrl + ← → | Select the previous or next band, wrapping. |
| ↑ ↓ | Its gain, 0.5 dB a press. On the low cut, its slope — **down** for more of it. |
| shift + ↑ ↓ | The same, 2 dB a press. |
| ctrl + ↑ ↓ | The same, an eighth of a dB, for placing rather than finding. |
| page up / down | Its Q. On the low cut, its slope again, down for more; on a shelf, the shelf/bell switch. |
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
holds on the arrows, the wheel, page up and down, and the readout cell, so the
gesture means the same thing wherever it is made.

**Q**, the width of a peaking band, is on four of those routes, because it is
the control with no handle of its own — the two axes of the curve are already
the frequency and the gain — and so the one thing here somebody has to be able
to find rather than be told about:

| | |
| --- | --- |
| shift + wheel | Over the curve. Q moves geometrically, 24 notches from 0.5 to 8. |
| page up / down | The same, on the selected band. |
| Drag the **Q** readout cell | Vertically, for a value you can name. |
| Right-click → **Bandwidth (Q)** | The current value, the two keys, and a list of widths to pick from. |

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
`solveBandedToeplitz()` against dense elimination (**3.8e-15**), and
`bandedInverseDiagonal()` against a full dense inverse (**1.6e-15**), over
random positive-definite systems across the whole order and run-length range.
The single-missing-sample posterior variance is pinned separately against its
closed form `1/Σaₖ²`, which anchors the scale of the rest. It also covers
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
and after so a regression shows up in the log. All zero, 2073 kB either side.

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

**`declick_au_verify`** and **`dehum_au_verify`** do the same job for the MacAU
ports — see [Sharing a core with the other plug-in
formats](#sharing-a-core-with-the-other-plug-in-formats). An Audio Unit renders
into 32-bit float and the house dither is on the way out, so there is no
undithered path to compare and the requirement is the core driven directly to
within **two ULP** rather than to the bit: measured **1.36 ULP** worst for the
declicker and **1.41 ULP** for the dehummer. What that still catches is any
difference that is not rounding, and the gross ones cannot hide in a ULP — a
wrong pre-roll makes the core zero-fill mid-stream. Around it, the things an AU
has to get right that a VST does not:

* `GetLatency()` and `GetTailTime()` are **seconds**, not samples, and both have
  to come back out as `cfg.latency` at the rate in force — 880 samples at the
  shipping default, 784 after dropping to order 32, 1088 at 96 kHz;
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

Plus what the host is told about the controls: seven parameters, named, in the
VST's order, all 0..1, readable and writable, with the advertised default equal
to the one the constructor actually set — the classic template slip — and a bad
index or a non-global scope refused. Both are built against
`plugins/MacAU/au_shim` and need no SDK. Read that folder's
[README](../MacAU/au_shim/README.md) first: unlike `vst2_shim` it is **not an
ABI**, nothing loads it, and **these two plug-ins have not been opened by a real
host**.

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
                                  the modal dialog and by the UI element
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
  declick_au_verify.cpp           the MacAU declicker vs. the core it shares
  dehum_au_verify.cpp             the MacAU dehummer vs. the core it shares
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

The stand-in the two `*_au_verify` tests build against lives with its plug-ins
too, and exists for the same reason — Apple's CoreAudio sources are not
redistributable either:

```
../MacAU/au_shim/
  AUEffectBase.h                  AUBase / AUEffectBase, the CoreAudio types the
                                  two plug-ins name, and COMPONENT_ENTRY
  README.md                       what it is not: an ABI, or a real host
```

`decrackle_core.{h,cpp}` deliberately knows nothing about foobar2000, VST or
Win32, which is what lets the test harness compare it against the original
source directly.

---

## Sharing a core with the other plug-in formats

Two of the four cores have further consumers — builds of the same algorithms for
hosts on three platforms, in two plug-in formats:

| | |
| --- | --- |
| `plugins/WinVST/Declick`, `plugins/WinVST/Dehum` | VST2, `.dll`, 32 and 64 bit |
| `plugins/LinuxVST/src/Declick`, `plugins/LinuxVST/src/Dehum` | VST2, `.so` |
| `plugins/MacVST/Declick`, `plugins/MacVST/Dehum` | VST2, `.vst` bundle |
| `plugins/MacAU/Declick`, `plugins/MacAU/Dehum` | Audio Unit, `.component` |

They all compile `declick_core.{h,cpp}` and `dehum_core.{h,cpp}` — not
reimplementations of them, and not translations. There is exactly one copy of
each piece of maths in this repository that anything is allowed to diverge from,
and it is the one under `foo_dsp_declick/` or `foo_dsp_dehum/`.

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

`paraeq_core.{h,cpp}` has no mirrors in this repository and so is not in
`sync_cores`. It does have one **outside** it: the core was written for
EmbraceNG, which carries its own copy under `Source/`. This port added the
`curveTrig()` / `curveDb()` drawing path and simplified the flush-to-zero
setter, and both files have since been copied back, so the two are
byte-identical as things stand. **This tree is the canonical one** — it is
where the core is developed and where the harness that exercises it lives —
so a future sync copies foobar2000 to EmbraceNG and not the other way.

The tests are a copy rather than a mirror: `tests/paraeq_verify.cpp` and
EmbraceNG's `Tests/ParaEQCoreTests.cpp` hold the same checks in the same order
and differ only in their opening comment and in `M_PI` against a local constant,
which MSVC needs and clang does not. Keeping them diffable is deliberate —
anything added to one should be added to the other.

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
| **`Channel::retune()`** | foobar2000 has no automation, so rebuilding the pipeline on a preset change is acceptable there. A DAW moves sliders while audio runs, and `configure()` reallocates, which resets. `retune()` swaps in a config that needs the same buffers — everything except Max repair and Model order — with no discontinuity. |
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
interface. Same seven sliders, same mappings, same defaults, same core, same
dither — the differences are all in how the host is talked to:

| | |
| --- | --- |
| **`GetLatency()`, `GetTailTime()`** | Seconds, not samples, so both are `(1.0/GetSampleRate())*cfg.latency` and Airwindows' house `*0.0` for Dehum. There is no `setInitialDelay()`: the host asks. |
| **`PropertyChanged(kAudioUnitProperty_Latency)`** | The AU's `ioChanged()`. Declick calls it when Max repair or Model order resizes the pipeline; Dehum never calls it at all. |
| **`Reset()`** | Where `resume()` went. Declick starts the next take from silence, Dehum flushes and keeps the lines — the same split as the VST, at a different entry point. |
| **`Initialize()`** | The one thing an AU makes *easier*. It is told its sample rate before it renders, which a VST is not, so the reallocation a rate change forces happens here instead of on the first render call. |
| **`kAudioUnitRenderAction_OutputIsSilence`** | Cleared by both. Declick holds `cfg.latency` samples of pipeline and Dehum's notches are integrators that ring on into a gap, so silence in is not silence out for either, and a host that trusts the flag would clip that off. |
| **No `getChunk`/`setChunk`** | An AU's parameters *are* its state; the host serialises them. So there is no preset chunk to get wrong, and no equivalent of the VST tests' chunk round trip. |
| **No `getParameterDisplay`** | The cost of keeping every slider generic 0..1 like the other 540 AUs in the tree — and of keeping the two formats interchangeable, since `paramsFromControls()` is then the VST's unchanged. Max repair reads `0.2` in an AU host rather than `4.0 ms`. The mapping tables in [Declick parameters](#declick-parameters) and [Dehum parameters](#dehum-parameters) are what to read it against. |

Two things to know about building them.

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
the same hole `plugins/WinVST/vst2_shim` fills on the Windows side;
`plugins/MacAU/au_shim` fills enough of it to compile the wrappers and drive
them from a test. It is **not** an ABI and nothing loads it, so unlike the VST2
side there is no `vst_host_verify` equivalent and **these two Audio Units have
not been opened by a real host**. Read that folder's
[README](../MacAU/au_shim/README.md) before trusting anything about them beyond
their DSP.

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
produces and `build_linuxvst.sh` against each `.so`. All six pass, worst
deviation **0.000e+00**. Along the way it checks the things a host would notice
and a compiler would not:

| | |
| --- | --- |
| `sizeof(AEffect)` | 144 on x86, 192 on x64, read out of the loaded module |
| flags | exactly `canReplacing \| programChunks \| canDoubleReplacing`, no editor |
| `uniqueID` | `0x64636C6B` `'dclk'`, `0x6468756D` `'dhum'` |
| `resvd1`, `resvd2`, `future[56]` | left zeroed, as the host expects |
| the deprecated `process` | a no-op function, not a null pointer, because a host old enough to call it will not check first |
| opcode routing | all seven parameter names arrive at the right index; displays and labels stay inside `kVstMaxParamStrLen` in a buffer bigger than promised |
| the latency contract | Declick declares 880 samples at 44.1 kHz with `effGetTailSize` matching, and calls `audioMasterIOChanged` twice for the two structural parameters; Dehum declares 0 and calls it **never** |
| `main` | exported as well as `VSTPluginMain`, for hosts that predate the rename, and reaching the same plug-in when called. On Windows the `.def` makes them one address and that is asserted too; the `.so` gets a forwarder instead, because there is no `.def` to alias with |
| `effClose` | 24 open/close cycles do not accumulate private bytes. This is a contract, not a nicety: the host frees nothing, so a shim that forgets the `delete` leaks the whole instance on every plug-in scan, and Dehum carries ~2 MB of analysis state per channel |

### Building the VST2 plug-ins

```powershell
.\scripts\build_winvst.ps1
```

Both plug-ins, both architectures, into `..\dist\winvst\`:

| File | For |
| --- | --- |
| `Declick32.dll`, `Dehum32.dll` | 32-bit hosts |
| `Declick64.dll`, `Dehum64.dll` | 64-bit hosts |

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
same place. There is no editor, no MIDI, no offline processing and no speaker
arrangements; those opcodes answer "not supported", which is what the stock
Airwindows plug-ins answered by not overriding them.

The build does not go through each plug-in's `VSTProject.vcxproj`. Those ask for
toolset v140 and Windows SDK 8.1 and expect the SDK at a path outside this tree,
so `build_winvst.ps1` drives `cl.exe` directly and the `.vcxproj`, `.sln` and
`.def` files are left exactly as Airwindows ships them — the documented "drag the
folder into VSTProject and press build" route still works for anyone who does
have the real SDK.

`build_linuxvst.sh` avoids `plugins/LinuxVST/CMakeLists.txt` for the same
reason. That project builds all five hundred plug-ins and wants Steinberg's
sources in `LinuxVST/include/vstsdk`, so it is left alone and keeps working for
anyone who has them — including for these two, which are registered in it with
`add_airwindows_plugin(Declick)` and `add_airwindows_plugin(Dehum)` like every
other plug-in there.

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
* **Declick's audio thread can still allocate in one case:** a caller pushing
  more than `Config::maxBlock` — 16384 samples — between pulls. That grows the
  output ring once, and then never again. Neither wrapper comes near it.
  Everything else is reserved in `configure()`; see
  [Real-time safety](#real-time-safety).
* **A format change rebuilds Declick's channels from `on_chunk`.** A different
  channel count or sample rate mid-stream constructs new `Channel` objects,
  which allocates, on whatever thread foobar2000 called it from. It happens at
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
