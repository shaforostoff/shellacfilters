# Airwindows foobar2000 / VirtualDJ

Fork of Airwindows trimmed to four plugins: Declick, Dehum, ParaEQ, DeCrackle. Work happens mostly on Declick, Dehum and ParaEQ.

## Layout
- `plugins/{LinuxVST/src,MacAU,MacSignedAU,MacSignedVST,MacVST,WinVST}/<Plugin>`: the VST/AU builds of the four plugins.
- `plugins/foobar2000_dsp`: foobar2000 components (`foo_dsp_declick`, `dehum`, `paraeq`, `decrackle`), tests, CMake. Build: `scripts\build_release.ps1 -Clean`.
- `plugins/vdjplugin`: VirtualDJ plug-ins (`vdj_declick`, `vdj_dehum`) and the NSIS installer. Build: `scripts\build.ps1`.
- Shared infrastructure, do not delete: `WinVST/vst2_shim`, `MacAU/au_shim`, `MacAU/au_compat`, `MacAU/scripts`.
- `docs/`: study notes (alignment, crackle, hard cases). Read only when the task is about that study.

## Rules
- Versions: Declick 1.1.1, Dehum 1.1.1. A bump touches VST `kVersion`, AU version header and plists, foobar2000 `version.h`, vdjplugin `declick_engine.h`, `CMakeLists.txt` and the NSIS script.
- Target hardware is a 2014 MacBook Air; keep DSP CPU cost low.
- Hosts can read the whole track; never give up the first seconds for warm-up.
- Tuning is judged by ear; prefer less music taken over a few clicks left.
- Calibration audio lives outside the repo. A/B the .m4a (hum) against the .flac (control) on every declick/dehum core change.

## Token cost
The `build/` directories under `plugins/foobar2000_dsp` and `plugins/vdjplugin` are large; don't read them. Use `git grep` rather than `grep -r` (slow on this repo).
