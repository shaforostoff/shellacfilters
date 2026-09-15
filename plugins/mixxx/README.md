# Mixxx — Declick and Dehum

Ports of the two restoration DSPs in this repository into [Mixxx](https://mixxx.org),
on macOS, Windows and Linux.

The DSP is not new. `src/restoration/declick_core.{h,cpp}` and `dehum_core.{h,cpp}`
here are byte-identical copies of the files `foo_dsp_declick` and `foo_dsp_dehum`
build, kept that way by `../foobar2000_dsp/scripts/sync_cores.sh`; everything else
in this directory is glue. For what the algorithms do, how every default was
arrived at, and the measurements behind them, read
[`../foobar2000_dsp/README.md`](../foobar2000_dsp/README.md). This file is about
Mixxx.

---

## This is a patch to Mixxx, not a plug-in

That is the one decision worth understanding before reading anything else, and it
follows from a single requirement: **these filters need to read the whole track.**

Declick is a detect-and-interpolate repairer. To rebuild a damaged sample it needs
the audio on *both* sides of it, so the core holds `Config::latency` samples — 880
at 44.1 kHz with the default Max repair and Model order. A filter that is handed
n samples and must hand back n can only do that by delaying the output. On a deck
that is 20 ms the deck is late against everything it is being mixed with.

Dehum has no latency, but it has an acquisition time: unaided, its detector needs
about 9 s to confirm a line it can see outright and up to 43 s for one only the
coherence route can reach. All of it is record playing with the hum still in it.

Both problems dissolve if the filter can read the decoded track rather than only
the buffer passing through it. **No plug-in format Mixxx supports can do that**,
and neither can a Mixxx built-in effect:

| | What it is handed | Can it read the track? |
| --- | --- | --- |
| VST2 / VST3 / Audio Unit / LV2 | the next n frames | no |
| Mixxx built-in **effect** | `pInput`, `pOutput`, and a `GroupFeatureState` carrying beat length, beat fraction and gain | no — not even the play position |
| **`mixxx::AudioSource`** | asked for any frame range of the decoded track | **yes** |

So the restoration goes where the track is: a `mixxx::AudioSourceProxy` between
the decoder and the rest of Mixxx. Asked for `[a, b)` it reads `[a, b + latency)`
underneath and hands back audio aligned with what was asked for. The lookahead
becomes an extra read instead of a delay — **no delay at all** — and that is not a
claim about intent: `declick_mixxx_verify` requires the served audio to be
bit-identical to the core run straight through the whole track, at block sizes of
8192, 4096, 1000, 512 and 64, in stereo and in mono.

It is the same trick as the READ AHEAD note on `declick::Channel::prime()`, and
the same one the VirtualDJ buffer plug-ins use. It is also, exactly, what the
foobar2000 components do: a player that can scan the file does not have to pay a
declicker's latency.

### Why not LV2, which is at least portable

LV2 is the only third-party plug-in format Mixxx supports on all three platforms,
and it was tried first. Two things killed it, in order of severity:

**It cannot read the track.** An LV2 plug-in gets its n frames like any other, so
Declick would play 20 ms late and Dehum would lose its scout. That alone is the
whole reason this port exists at the layer it does.

**And on macOS it cannot even load.** The shipped Mixxx is signed with the
hardened runtime and the App Sandbox, and without the
`com.apple.security.cs.disable-library-validation` entitlement:

```
$ codesign -dvvv /Volumes/Mixxx/Mixxx.app
CodeDirectory v=20500 flags=0x10000(runtime) ...
TeamIdentifier=JBLRSP95FC
$ codesign -d --entitlements :- /Volumes/Mixxx/Mixxx.app
... com.apple.security.app-sandbox = true ...     (and no disable-library-validation)
```

Library validation refuses to load any binary not signed by the same team, so a
third-party LV2 bundle cannot be opened by the official macOS build however it is
signed — and the sandbox redirects `$HOME`, so lilv would not find it anyway.
(This is also why Mixxx hosts Audio Units on macOS but not LV2 plug-ins: it
instantiates AUs out of process, with `kAudioComponentInstantiation_LoadOutOfProcess`,
which is how a sandboxed host loads somebody else's code.) LV2 does work on
Windows and Linux. It is simply the wrong layer.

---

## What the patch does

Five files, 37 added lines, and nothing rewritten in place. Everything else this
port adds is a file Mixxx does not already have.

| File | Change |
| --- | --- |
| `CMakeLists.txt` | six source lines |
| `src/engine/cachingreader/cachingreaderworker.h` | the settings pointer as a member |
| `src/engine/cachingreader/cachingreader.cpp` | pass the config it already holds to its worker |
| `src/engine/cachingreader/cachingreaderworker.cpp` | wrap the opened `AudioSource`; two includes |
| `src/preferences/dialog/dlgpreferences.cpp` | register the preferences page |

The wrap is the whole integration:

```cpp
m_pAudioSource = mixxx::AudioSourceRestoreProxy::wrap(
        SoundSourceProxy(pTrack).openAudioSource(config),
        restoration::readSettings(m_pConfig));
```

With both filters off, `wrap()` hands the decoder straight back, so the decode
path is exactly what it was before the patch — no extra virtual call, nothing
allocated. That matters because it is what every user who has not switched this on
is running.

**It is wrapped in `CachingReaderWorker` and nowhere else, so this is playback
only.** Three places in Mixxx open an `AudioSource`: that one, `AnalyzerThread`
for waveforms and beats, and `chromaprinter` for fingerprinting. The last two are
left alone, so the waveform still shows the record as it is and an analysis result
never depends on what the restoration settings happened to be when it ran.

Everything runs on `CachingReaderWorker`'s own thread. That is not the audio
callback, so a pipeline restart or a slice of scouting delays a chunk that was
being read *ahead*, not one a deck is waiting on.

---

## Building

```bash
git clone -b 2.5.6 https://github.com/mixxxdj/mixxx.git
scripts/integrate.sh ../mixxx          # copy the files, apply the patch
# then build Mixxx exactly as its own README says
```

`integrate.sh --check` reports what it would do and changes nothing;
`integrate.sh --revert` puts the tree back, files and patch both. Re-running it
after `../foobar2000_dsp/scripts/sync_cores.sh` has pushed a new core out is
expected and safe — it notices the patch is already applied and only copies
files.

Nothing here adds a dependency. The cores are portable C++17 with no library
behind them, which is why this port is one patch for all three platforms rather
than three.

The port targets **Mixxx 2.5.6**, the current stable release. The patch is
generated from that tree and checked against it. On another version `git apply`
will say so rather than guess, and the three edits above are small enough to
redo by hand.

### The harness, without Mixxx

```bash
scripts/build.sh
```

Builds and runs the verification suite on its own — no Mixxx checkout, no Qt, no
decoder, about ten seconds. See *Verification* below.

---

## Controls

**Preferences ▸ Restoration.** Both filters are off until you switch them on: this
is a patch to a DJ program, and a restoration filter that turns itself on for
everybody's whole library is not a default anyone asked for.

Settings are per collection rather than per deck, and **a change reaches a deck the
next time it loads a track**. That is a consequence of where the restoration runs:
its output is what fills `CachingReader`'s decoded chunks, so a parameter that
moved mid-track would mean throwing those chunks away and decoding them again
under a playing deck.

The ranges are exactly the ranges the VST2 and VirtualDJ sliders cover, so every
figure in [`../foobar2000_dsp/README.md`](../foobar2000_dsp/README.md) describes
this page too. They are stored in `mixxx.cfg` under `[Restoration]`, in these
units, so they can be read and edited by hand.

### Declick

| Control | Range | Default | |
| --- | --- | --- | --- |
| Sensitivity | 0–1 → 6.0–2.5 σ trigger | 0.60 (3.9 σ) | Higher finds more, and eventually starts costing music |
| Extent | 0–1 | 0.50 | How far a detection spreads into its own tail |
| Max repair | 0.2–20 ms | 4.0 ms | Longest single repair |
| Depth | 0–1 | 0 | 0 subtracts the calibrated fraction of each click, which adds the least error of its own; 1 replaces damaged samples outright |
| Passes | 1–3 | 2 | A second pass catches clicks the first uncovers; ~50 % more CPU |
| Model order | 8–64 in eights | 64 | The one control where CPU clearly buys quality |
| Dry/Wet | 0–1 | 1 | 0 is a bypass |

### Dehum

| Control | Range | Default | |
| --- | --- | --- | --- |
| Sensitivity | 0–1 → prominence threshold | 0.50 (16 dB) | Above ~0.70, hum-free material starts activating lines |
| Bandwidth | 0.1–5 Hz | 1.00 Hz | Notch half width |
| Search to | 40–500 Hz | 100 Hz | Top of the automatic search. Higher finds sustained musical notes instead |
| Harmonics | 1–8 | 1 | Only raise for a genuine mains buzz; on rumbly transfers the harmonics took 84 % of everything removed |
| Frequency | Automatic, then 10–500 Hz | Automatic | Pinning one turns the search off — and with it the scout |
| Rumble | Off, then 10–200 Hz | 67 Hz | High-pass corner. Wind it back towards 40 where the low end is worth keeping |
| Dry/Wet | 0–1 | 1 | |

When both are on the order is **Declick, then Dehum**. Clicks are broadband
impulses and they land in every hop of Dehum's analysis window, lifting the noise
floor a line's prominence is measured against — so declicking first makes the hum
easier to find. The reverse is not true to any degree that matters: hum is a low
tone, and a tone is what Declick's AR model predicts well, so it barely reaches
the residual the detector thresholds.

---

## What it costs

**Declick's lookahead costs nothing extra to read.** Measured over a whole track,
the decoder is asked for exactly as many frames as the track holds: the readahead
runs inside the slice the pipeline was going to read anyway, and past the end of
the track the pipeline feeds its own silence rather than asking for audio that is
not there. Sequential playback restarts the pipeline once, at the start.

**A seek restarts it.** A restart rewinds `madWindow` — 1323 frames, 30 ms — before
the audio that was asked for and runs the cores over that first, so Declick's noise
estimate is settled by the time anything anyone hears comes out of it. Measured,
a chunk reached by jumping differs from the same chunk played into by 19 % of the
repair; the rest of the restart is free. Small seeks, loops and scratching mostly
never reach this layer at all, because `CachingReader` serves them from decoded
chunks it already has.

**Dehum keeps what it learned across a seek**, unlike Declick. The analysis window
is stale and goes, but the hum on the far side of a seek is the same hum, and
re-acquiring it every time the DJ moves the play position would be worse than
doing nothing.

**The scout is budgeted**, at `kScoutSpeedup` — eight — times the rate audio is
being read, capped at one chunk's worth of work per call. It reads up to 60 s of
the opening, stops as soon as it has a confirmed line and at least 10 s, and then
costs nothing for the rest of the track. Because `CachingReader` fills its cache
as fast as it can when a track is loaded, the scout gets most of its budget
exactly when it is worth the most.

---

## Verification

```bash
scripts/build.sh
```

Two binaries, neither of which needs Mixxx, Qt or a decoder:

| | |
| --- | --- |
| `declick_mixxx_verify` | The readahead claim — served audio bit-identical to the core run straight through the whole track, at five block sizes, stereo and mono; that disabled is bit-exact pass-through; the read cost; restart determinism; that a warmed-up restart recovers most of the repair; and that the last `latency` frames of a track are served rather than left in the pipeline |
| `dehum_mixxx_verify` | The same alignment checks for the zero-latency core; that a pinned line is cancelled; that the scout beats the unaided detector in the acquisition gap; that a pinned frequency leaves the scout idle and reading nothing; that lines survive a seek; and that the two filters chained are exactly Dehum after Declick |

Two measurements those print are the point of the whole port:

```
core latency 880 frames (20.0 ms), paid in reads
read 529200 frames for a 529200 frame track (+0)

41.3 Hz over 1.6-2.6 s: scouted -38.3 dB, unaided -17.0 dB
```

Neither re-tests the DSP. `declick_verify`, `dehum_verify` and the rest in
[`../foobar2000_dsp/tests`](../foobar2000_dsp/tests) do that against independent
references, and these binaries compile the identical cores.

**What is not covered here.** `audiosourcerestoreproxy.cpp`, `restorationconfig.cpp`
and the preferences page are compiled when you build Mixxx, not by
`scripts/build.sh` — they need Mixxx's headers and Qt, and standing up a stub for
those would test the stub. They are thin on purpose for exactly that reason: the
proxy is about ninety lines and holds no state of its own beyond the pipeline, and
everything with any judgement in it lives in `restorationpipeline.h`, which is
tested above. `integrate.sh --check` is what tells you the patch still fits; the
compiler is what tells you the glue does.

---

## Keeping the cores in sync

`src/restoration/declick_core.{h,cpp}` and `dehum_core.{h,cpp}` are **mirrors, not
sources**. An edit made here is an edit that will be overwritten. The canonical
copies live in `../foobar2000_dsp/foo_dsp_declick` and `../foobar2000_dsp/foo_dsp_dehum`;
change those and run

```bash
../foobar2000_dsp/scripts/sync_cores.sh
```

or `sync_cores.ps1` from Windows. `--check` reports drift and exits non-zero,
which is what CI wants. Then re-run `scripts/integrate.sh` to push the new copies
into your Mixxx tree.

---

## Layout

```
patches/
  mixxx-2.5.6.patch          the five-file, 37-line edit that registers everything
src/                         laid out exactly as it lands in Mixxx's src/
  restoration/
    declick_core.{h,cpp}     MIRROR - see above
    dehum_core.{h,cpp}       MIRROR - see above
    restorationsettings.h    the parameters, in the cores' units, Qt-free
    restorationsource.h      random access to the decoded track
    restorationpipeline.h    the readahead, the restarts and the warm-up
    dehumscout.h             reads the opening faster than it plays
    restorationconfig.{h,cpp}  those settings in mixxx.cfg, under [Restoration]
    audiosourcerestoreproxy.{h,cpp}  the mixxx::AudioSourceProxy itself
  preferences/dialog/
    dlgprefrestoration.{h,cpp}, dlgprefrestorationdlg.ui
scripts/
  integrate.sh               copy and patch a Mixxx tree; --check, --revert
  build.sh                   build and run the harness, without Mixxx
tests/
  restore_test_support.h     a track in memory, signal generators, references
  declick_mixxx_verify.cpp
  dehum_mixxx_verify.cpp
```

MIT licensed, like the rest of this tree. Mixxx is GPL-2.0-or-later, which MIT is
compatible with, so a build carrying these files is redistributable as GPL.
