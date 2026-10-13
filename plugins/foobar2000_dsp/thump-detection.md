# Thump detection — handoff

Status as of 2026-10-12: **no thump remover exists, and none should be built
blind.** The evidence below says thumps are not a common defect on this
collection; the next step is a real example from Nick. Branch
`declick-dense-crackle`, commit `15b064d96`.

## What a thump is

A **thump** is the low-frequency transient a big scratch or a crack leaves:
a click, then a slow swing that dies away over roughly 10–100 ms, from the
tonearm and cartridge being kicked. On a cracked 78 it repeats **once per
revolution**: 0.769 s at 78 rpm (1.333 s at 45, 1.8 s at 33⅓), give or take
the transfer's speed correction (re-transfers in this library differ by up to
~2.5 %).

The declicker cannot touch one, and should not try:

* The tail is a decaying resonance, which is exactly what an AR model
  predicts well. The residual detector sees the click at its onset and
  nothing after it, whatever Extent is set to — measured: more Extent left
  the synthetic thump at ~−6 dB and damaged music.
* It lasts far longer than anything interpolation can fill. `maxRun` is
  4 ms by default and 20 ms at most, and the analysis window is
  `2*pad + 512` samples, `pad = maxRun + 3*order` — 1632 samples (37 ms) at
  the defaults. A 100 ms thump does not fit in it.

## The evidence so far

### The synthetic probe was misleading

The probe pack (`C:\TangoTunes\_decrackle_audition\probe_2026-10`) contains a
"thump" shape that was **invented**: an impulse followed by a 48-sample ring
at ~1.1 kHz, `exp(-t/12) * sin(2πt/40) * 0.4`. On it, at −24 dBFS, the
current defaults remove only −5.9 dB (tone) and −4.3 dB (music).

That shortfall prompted this investigation. It does not correspond to anything
found on real records — see below. Do not tune against that probe shape.

### Real thumps: rare, and the candidates were music

1. **Click-triggered average.** The 150 largest repairs per track (by peak
   of `original − declicked`) on 12 shellac transfers, 1784 events; each
   aligned at the repair onset, sign-normalised by the click's polarity,
   low-passed at 400 Hz, averaged. A consistent thump would survive the
   averaging while unrelated music cancels. Result: **nothing above the
   noise floor** at 0–60 ms (mean |avg| 0.03–0.06 of click peak against a
   standard error of ~0.05).

2. **Per-event energy.** The 60 largest repairs per track on 19 shellac
   transfers (Fresedo *Alas*; eight Caló; four Pugliese; six D'Arienzo
   1935–39 — all unprocessed originals), 1140 events. Band
   energy 5–35 ms after the onset against 30 ms before, compared with the
   same measure at random positions:

   | band | after big clicks: >+10 dB | random positions: >+10 dB |
   | --- | --- | --- |
   | 20–80 Hz | 4.9 % | 1.8 % |
   | 80–300 Hz | 1.2 % | 2.0 % |

   So ~3 % of big clicks are followed by a sub-80 Hz jump that chance would
   not explain, and nothing at all happens at 80–300 Hz.

3. **Looking at the four largest of those jumps** (+20 to +22 dB):

   * Pugliese – *Cafetín* (1947), 80.46 s
   * Pugliese – *Demasiado tarde* (1946), 21.69 s
   * D'Arienzo – *Cabeza de novia* (1938), 12.67 s and 12.68 s

   **All four are music.** Low-passed at 150 Hz, the "tail" is a steady
   ~100 Hz oscillation that keeps going past 200 ms without decaying — a
   bass note or chord whose attack happens to coincide with a click. Use
   these four as **negative controls**: a thump detector that fires on them
   is broken.

The excess in (2) is most likely the same thing at a smaller scale: clicks
are more often detected at note attacks, and attacks bring low-frequency
energy with them. That was not verified event by event.

## What is needed before building anything

**A real example.** Ask Nick for a record with audible thumps — most likely
a cracked or heavily scratched 78, where it should repeat once per turn.
Without one, the shape, duration and frequency range are guesses, and the
synthetic probe above shows how a guess goes wrong.

With an example in hand, first characterise it: duration, frequency content,
decay time constant, how similar successive occurrences are, and whether
they repeat at the rotation period. Then build a probe from **that** shape.

## Proposed design (once there is an example)

### Detection

Candidates are big clicks the declicker already finds; a thump never comes
without one. Confirm with all of:

* **A low-frequency onset.** 20–150 Hz energy jumps at the click.
* **Decay, not sustain.** The envelope falls monotonically with a time
  constant in the measured range, and the oscillation has no stable pitch.
  This is what separates a thump from the four bass notes above, which
  sustain at a fixed ~100 Hz.
* **Periodicity, where present.** Successive candidates spaced by the
  rotation period (search 0.70–0.85 s for 78s, and the other speeds) are
  near-certain thumps; music does not repeat at the disc's rotation rate.
  Crack thumps also tend to look alike from one turn to the next, which
  allows a template.

### Removal

Interpolation cannot work over 10–100 ms. Subtract a fitted model of the
thump instead:

* **Damped sinusoids.** Fit 1–3 exponentially decaying sinusoids to the low
  band from the onset onwards by least squares, and subtract. Cheap, since
  it only runs at detected events.
* **Template.** For a periodic crack, average the aligned occurrences into a
  template, then fit its gain and offset per occurrence. The averaging
  cancels the music, as in the click-triggered average above.
* The literature: Vaseghi's template-based thump removal; Godsill & Rayner's
  two-process model (music AR plus thump AR, separated with a Kalman
  smoother) is the principled version, and much more expensive.

### Where it lives

Probably **not inside `declick::Channel`**: it needs ~100–200 ms of
lookahead, which its window does not have, and enlarging the window grows
latency for every click repair. Options:

* A separate stage after declick: its own buffer, or a new component.
  foobar2000 reads ahead, so latency is free there. The VST2/AU ports would
  have to declare it through `setInitialDelay` / `GetLatency`, and the
  VirtualDJ port's 0.25 s warm-up already exceeds it.
* Or a second, slower path inside the core fed by the declicker's large
  repairs. More coupling, one component.

It runs only at rare events, so CPU is negligible next to the declicker
(43× real time mono at the defaults on the dev machine, an estimated 6×
stereo on the 2014 MacBook Air that playback actually runs on).

### Test plan

1. Synthetic thumps **matching the real example's measured shape**, injected
   at known positions (and once per revolution) into the three 78-era
   excerpts the crackle benchmark uses.
   Score removal and harm exactly, as for the crackle benchmark.
2. The four negative controls above must come through untouched.
3. The real example, by ear.

## Things learned the hard way

* **Check which binary is measured.** The `declick_cli.exe` in
  `build/x64/tools/Release` was once stale against HEAD; rebuild before
  measuring, and keep a clean HEAD build to compare against.
* **Modern recordings are not a clean base for 78 work.** A peak-limited
  modern master is spikier in the AR residual than shellac crackle is.
* **The commercial `(decrackle)` deltas include damage.** It carves 20–60
  sample bumps out of marcato attacks; its output is a reference, not ground
  truth. 79 of the 87 `(decrackle)` files are sample-aligned with their
  original (lag 0, ρ ≥ 0.99); the other 8 are re-transfers at a different
  speed.
* **Different pressings never align sample for sample**: shellac/vinyl
  overlaps, `(2)` versions, and `C:\SortedTangoSpanishNamesFLAC` against
  `C:\TangoTunes`. Speed differs by ~2 % and drifts within a track; even
  time-warped, local correlation is ~0.6. They can label *events* (present
  in one pressing, absent from the other) but not samples.
* **Core edits go in `foo_dsp_declick/`**, then
  `scripts/sync_cores.ps1` pushes them to the five mirrors;
  `-Check` must report "every mirror matches". The VST2/AU/VirtualDJ
  wrappers hard-code default slider positions (Order 0.4677 = 128 on a
  16–256 slider), and their tests check the mapping.
* **Several wrapper files are Latin-1** (the © in the header). Edit them
  byte-wise; a UTF-8 round trip turns © into U+FFFD.
* **`declick_rt_verify` forbids allocation** on the audio path: size any
  new buffer in `configure()` from the `buf*` envelope.
