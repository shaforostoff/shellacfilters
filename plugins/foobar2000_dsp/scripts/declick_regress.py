"""Whole-song regression check for the declick core, base build against candidate.

    "C:\\Program Files\\Python314\\python.exe" scripts\\declick_regress.py [--base REV] [--cand REV]

Builds declick_cli and dehum_cli twice - from `git archive` of the base revision
(default HEAD) and of the candidate (default: the working tree) - and runs each
reference song through both, whole, two ways: declick alone, and dehum then
declick, the chain foobar2000 runs when Dehum sits ahead of Declick. For every
song, chain and build it reports

  removal   10 log10(removed energy / input energy), against what declick was given
  clamped   estimates repairRun() pulled back to the local range, first repairs +
            re-repairs (declick_cli prints them; builds older than the counter show -)
  events    stretches where |out| > 1.25 x the input's 10 ms max envelope + 1e-4,
            samples within 5 ms grouped as one; each with its time and length,
            first sample out to last

and exits 1 if the candidate has an out-of-range event longer than 3 samples
that the base does not (or one more than twice as long as the base's at the
same place), or more clamped estimates than --clamp-margin over the base. A bad
repair can cross the limit at only a couple of samples of a 10-sample wiggle,
which is why the length is the span and not the count of samples out.

Why whole songs and the chain: the half repair in 59c398f48 put a 6 ms burst at
the clamp into Voz de tango at 14.37 s after Dehum, and moved whole-song removal
by about 0.1 dB. It did not show on 60 s excerpts, on aggregates, or without
Dehum ahead. See the README, "Before committing a core change".

Needs ffmpeg on PATH, CMake and Visual Studio, and numpy, scipy and soundfile.
Decoded songs and builds are cached under --work; outputs are deleted unless
--keep-audio. Not the "clicks/s above 8 sigma" score: it swings 2x between
near-identical outputs, so nothing here passes or fails on it.
"""

import argparse
import concurrent.futures as cf
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

import numpy as np
import soundfile as sf
from scipy.ndimage import maximum_filter1d

SUBDIR = "plugins/foobar2000_dsp"
SOURCES = ("tools", "foo_dsp_declick", "foo_dsp_dehum")

TROILO = r"C:\TangoTunes\Anibal Troilo\shellac\Anibal Troilo 1938-1950"
TANTURI = r"C:\TangoTunes\Ricardo Tanturi\shellac"
DARIENZO = r"C:\SortedTangoSpanishNames\Juan D'Arienzo\Juan D'Arienzo 1950-1957"
CANARO = r"C:\SortedTangoSpanishNames\Francisco Canaro\Francisco Canaro 1935-1973"

# name -> path. The Troilo .m4a sides carry hum and the .flac sides are the
# same takes without it; Malvon and Olga are clean records; El irresistible and
# Hotel Victoria are crackly MP3s with an empty top band.
REFS = [
    ("tablada.m4a", TROILO + r"\Anibal Troilo - Instrumental - La tablada - 1942 - Tango.m4a"),
    ("tablada.flac", TROILO + r"\Anibal Troilo - Instrumental - La tablada - 1942 - Tango.flac"),
    ("cachirulo.m4a", TROILO + r"\Anibal Troilo - Instrumental - Cachirulo - 1941 - Tango.m4a"),
    ("cachirulo.flac", TROILO + r"\Anibal Troilo - Instrumental - Cachirulo - 1941 - Tango.flac"),
    ("malvon", TANTURI + r"\Ricardo Tanturi - Enrique Campos - Malvon - 1943 - Tango.flac"),
    ("vozdetango", TANTURI + r"\Ricardo Tanturi - Alberto Castillo - Voz de tango (Tango) - 1942 - Tango.m4a"),
    ("olga", DARIENZO + r"\Juan D'Arienzo - Instrumental - Olga - 1956 - Vals.mp3"),
    ("marne", DARIENZO + r"\Juan D'Arienzo - Instrumental - El Marne - 1954 - Tango.mp3"),
    ("irresistible", DARIENZO + r"\Juan D'Arienzo - Instrumental - El irresistible (hissy) - 1954 - Tango.mp3"),
    ("hotel", CANARO + r"\Francisco Canaro - Instrumental - Hotel Victoria (hissy) - 1935 - Tango.mp3"),
]

CHAINS = ("alone", "dehum")

# The out-of-range scan.
ENV_MS = 10.0
RATIO = 1.25
FLOOR = 1e-4
GROUP_MS = 5.0
MIN_SAMPLES = 3       # an event fails the run only if it is longer than this

CMAKELISTS = r"""cmake_minimum_required(VERSION 3.15)
project(declick_regress CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
# The release flags of plugins/foobar2000_dsp/CMakeLists.txt.
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /EHsc /GR- /utf-8 /Gw /Zc:inline")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Oi /Ot /DNDEBUG")
set(D "${CMAKE_CURRENT_SOURCE_DIR}/plugins/foobar2000_dsp")
add_executable(declick_cli "${D}/tools/declick_cli.cpp" "${D}/foo_dsp_declick/declick_core.cpp")
add_executable(dehum_cli "${D}/tools/dehum_cli.cpp" "${D}/foo_dsp_dehum/dehum_core.cpp")
"""


def log(*a):
    print(*a, flush=True)


def git(root, *args, binary=False):
    out = subprocess.run(["git", "-C", root] + list(args), check=True,
                         stdout=subprocess.PIPE).stdout
    return out if binary else out.decode("utf-8", "replace").strip()


# --- builds ------------------------------------------------------------------

def tree_hash(src):
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            h.update(os.path.relpath(p, src).replace("\\", "/").encode())
            with open(p, "rb") as fh:
                h.update(hashlib.sha256(fh.read()).digest())
    return h.hexdigest()


def stage_sources(root, rev, dst):
    """The three source folders of `rev` - or of the working tree, rev None -
    under dst, at their paths in the repository."""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    paths = [SUBDIR + "/" + s for s in SOURCES]
    if rev is None:
        for p in paths:
            shutil.copytree(os.path.join(root, p), os.path.join(dst, p))
    else:
        tar = git(root, "archive", "--format=tar", rev, *paths, binary=True)
        with tarfile.open(fileobj=io.BytesIO(tar)) as t:
            t.extractall(dst, filter="data")


def build(root, rev, label, work, patch=None):
    """Build both CLIs for `rev` (None: the working tree). Returns the folder
    holding declick_cli.exe and dehum_cli.exe. A build is reused while its
    sources and patch hash the same."""
    stage = os.path.join(work, "stage-" + label)
    stage_sources(root, rev, stage)
    if patch:
        subprocess.run(["git", "apply", os.path.abspath(patch)], cwd=stage, check=True)
    with open(os.path.join(stage, "CMakeLists.txt"), "w", newline="\n") as f:
        f.write(CMAKELISTS)
    key = tree_hash(stage)[:16]
    bdir = os.path.join(work, "build-" + key)
    exe = os.path.join(bdir, "bin", "Release")
    if all(os.path.isfile(os.path.join(exe, n + ".exe")) for n in ("declick_cli", "dehum_cli")):
        log("  %s: %s, cached build %s" % (label, rev or "working tree", key))
        shutil.rmtree(stage)
        return exe
    src = os.path.join(work, "src-" + key)
    if os.path.isdir(src):
        shutil.rmtree(src)
    os.rename(stage, src)
    log("  %s: %s, building %s" % (label, rev or "working tree", key))
    # From PowerShell, as the README's recipe does: Git Bash rewrites /m into a
    # path, and anything started from it inherits that.
    ps = ("$ErrorActionPreference = 'Stop'; "
          "cmake -S '{src}' -B '{b}' -A x64 "
          "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE='{b}/bin/Release' | Out-Null; "
          "if ($LASTEXITCODE) {{ exit $LASTEXITCODE }}; "
          "cmake --build '{b}' --config Release -- /m /v:minimal /nologo; "
          "exit $LASTEXITCODE").format(src=src, b=bdir)
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.stdout.write(r.stdout.decode("utf-8", "replace"))
        raise SystemExit("build of %s failed" % label)
    return exe


# --- audio -------------------------------------------------------------------

def decode(path, work):
    """Mono float WAV at the file's own rate, cached by path, size and mtime."""
    st = os.stat(path)
    key = hashlib.sha1(("%s|%d|%d" % (path, st.st_size, int(st.st_mtime))).encode()).hexdigest()[:12]
    dst = os.path.join(work, "decoded", key + ".wav")
    if not os.path.isfile(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".part.wav"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-vn", "-ac", "1",
                        "-c:a", "pcm_f32le", tmp], check=True)
        os.replace(tmp, dst)
    return dst


CLAMP_RE = re.compile(r"clamped (\d+) first, (\d+) again")


def run_declick(exe, src, dst):
    r = subprocess.run([os.path.join(exe, "declick_cli.exe"), src, dst],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = r.stdout.decode("utf-8", "replace")
    if r.returncode:
        raise RuntimeError("declick_cli failed on %s:\n%s" % (src, text))
    m = CLAMP_RE.search(text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def run_dehum(exe, src, dst):
    r = subprocess.run([os.path.join(exe, "dehum_cli.exe"), src, dst, "--quiet"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        raise RuntimeError("dehum_cli failed on %s:\n%s" % (src, r.stdout.decode("utf-8", "replace")))


def scan(inp, out):
    """Removal in dB, and the out-of-range events of `out` against `inp`."""
    x, sr = sf.read(inp, dtype="float32")
    y, sr2 = sf.read(out, dtype="float32")
    assert sr == sr2, (inp, out)
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    d = (x - y).astype(np.float64)
    removal = 10.0 * np.log10(np.dot(d, d) / max(np.dot(x.astype(np.float64), x), 1e-30) + 1e-30)
    win = max(1, int(round(ENV_MS * 1e-3 * sr)))
    env = maximum_filter1d(np.abs(x), size=win, mode="nearest")
    lim = RATIO * env + FLOOR
    ay = np.abs(y)
    bad = np.flatnonzero(ay > lim)
    events = []
    if len(bad):
        gap = int(round(GROUP_MS * 1e-3 * sr))
        cuts = np.flatnonzero(np.diff(bad) > gap) + 1
        for g in np.split(bad, cuts):
            s, e = int(g[0]), int(g[-1])
            events.append({"start": s, "end": e, "len": e - s + 1, "n": len(g),
                           "peak": float(np.max(ay[g] / (env[g] + FLOOR)))})
    return removal, events, sr


def process(exe, label, name, decoded, chain, outdir, keep):
    base = os.path.join(outdir, label, name)
    os.makedirs(os.path.dirname(base), exist_ok=True)
    src = decoded
    made = []
    if chain == "dehum":
        src = base + ".dehummed.wav"
        run_dehum(exe, decoded, src)
        made.append(src)
    out = base + ".declick-" + chain + ".wav"
    clamps = run_declick(exe, src, out)
    made.append(out)
    removal, events, sr = scan(src, out)
    if not keep:
        for p in made:
            os.remove(p)
    return {"removal": removal, "clamps": clamps, "events": events, "sr": sr}


# --- comparison --------------------------------------------------------------

def match(ev, others, tol):
    """The events of `others` that overlap `ev`, give or take `tol` samples."""
    return [o for o in others if o["start"] - tol <= ev["end"] and ev["start"] <= o["end"] + tol]


def fmt_event(ev, sr):
    return "%8.3f s  %5d samples long, %4d of them out, peak %.1fx" % (
        ev["start"] / sr, ev["len"], ev["n"], ev["peak"])


def total(c):
    return None if c is None else c[0] + c[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", default="HEAD", help="base revision (default HEAD)")
    ap.add_argument("--cand", default=None,
                    help="candidate revision (default: the working tree)")
    ap.add_argument("--patch-base", help="unified diff applied to the base sources before "
                    "building, e.g. to give an old base the clamp counter")
    ap.add_argument("--patch-cand", help="the same for the candidate")
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "declick_regress"),
                    help="cache for builds and decoded songs (default %%TEMP%%\\declick_regress)")
    ap.add_argument("--only", action="append", default=[],
                    help="only songs whose name contains this; repeatable")
    ap.add_argument("--file", action="append", default=[],
                    help="a song of your own, in place of the reference set; repeatable")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--clamp-margin", type=float, default=20.0,
                    help="clamped estimates the candidate may add per song and chain "
                    "before it fails (default 20) ...")
    ap.add_argument("--clamp-rel", type=float, default=0.5,
                    help="... or this fraction of the base's, if that is more (default 0.5)")
    ap.add_argument("--keep-audio", action="store_true",
                    help="keep the outputs under WORK\\out for listening")
    ap.add_argument("--build-only", action="store_true",
                    help="build both, print where the binaries are, and stop")
    args = ap.parse_args()

    root = git(os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--show-toplevel")
    os.makedirs(args.work, exist_ok=True)

    refs = [(os.path.splitext(os.path.basename(p))[0], p) for p in args.file] or REFS
    if args.only:
        refs = [r for r in refs if any(o.lower() in r[0].lower() for o in args.only)]
    missing = [p for _, p in refs if not os.path.isfile(p)]
    if missing:
        raise SystemExit("not found:\n  " + "\n  ".join(missing))

    def describe(rev):
        if rev is None:
            dirty = git(root, "status", "--porcelain", "--", *[SUBDIR + "/" + s for s in SOURCES])
            return "working tree" + (" (modified)" if dirty else " (= HEAD)")
        return "%s %s" % (git(root, "rev-parse", "--short", rev),
                          git(root, "log", "-1", "--format=%s", rev))

    log("base: " + describe(args.base))
    log("cand: " + describe(args.cand))
    log("builds:")
    exes = {"base": build(root, args.base, "base", args.work, args.patch_base),
            "cand": build(root, args.cand, "cand", args.work, args.patch_cand)}
    if args.build_only:
        for label in ("base", "cand"):
            log("%s %s" % (label, exes[label]))
        return 0

    log("decoding %d songs" % len(refs))
    decoded = {}
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        for (name, path), d in zip(refs, pool.map(lambda r: decode(r[1], args.work), refs)):
            decoded[name] = d

    outdir = os.path.join(args.work, "out")
    log("running %d songs x %d chains x 2 builds, %d at a time" % (len(refs), len(CHAINS), args.jobs))
    jobs = {}
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        for name, _ in refs:
            for chain in CHAINS:
                for label in ("base", "cand"):
                    jobs[(name, chain, label)] = pool.submit(
                        process, exes[label], label, name, decoded[name], chain,
                        outdir, args.keep_audio)
        res = {k: f.result() for k, f in jobs.items()}

    failures = []
    log("")
    log("%-15s %-6s  %9s %9s  %15s %15s  %6s %6s" % (
        "song", "chain", "base dB", "cand dB", "base clamped", "cand clamped", "base ev", "cand ev"))
    for name, _ in refs:
        for chain in CHAINS:
            b, c = res[(name, chain, "base")], res[(name, chain, "cand")]
            cl = lambda r: "-" if r["clamps"] is None else "%d+%d" % r["clamps"]
            log("%-15s %-6s  %9.2f %9.2f  %15s %15s  %6d %6d" % (
                name, chain, b["removal"], c["removal"], cl(b), cl(c),
                len(b["events"]), len(c["events"])))
            tb, tc = total(b["clamps"]), total(c["clamps"])
            if tb is not None and tc is not None:
                allowed = tb + max(args.clamp_margin, args.clamp_rel * tb)
                if tc > allowed:
                    failures.append("%s/%s: %d clamped estimates against the base's %d" % (
                        name, chain, tc, tb))

    log("")
    log("out-of-range events (|out| > %.2f x the input's %g ms max + %g; within %g ms grouped):" % (
        RATIO, ENV_MS, FLOOR, GROUP_MS))
    any_events = False
    for name, _ in refs:
        for chain in CHAINS:
            b, c = res[(name, chain, "base")], res[(name, chain, "cand")]
            sr = c["sr"]
            tol = int(round(GROUP_MS * 1e-3 * sr))
            lines = []
            for ev in c["events"]:
                m = match(ev, b["events"], tol)
                if not m:
                    tag = "NEW"
                elif ev["len"] > 2 * max(o["len"] for o in m) + MIN_SAMPLES:
                    tag = "GREW"
                else:
                    tag = "both"
                if tag != "both" and ev["len"] > MIN_SAMPLES:
                    failures.append("%s/%s: %s event at %.3f s, %d samples long" % (
                        name, chain, tag.lower(), ev["start"] / sr, ev["len"]))
                lines.append((ev["start"], "  cand %-4s %s" % (tag, fmt_event(ev, sr))))
            for ev in b["events"]:
                if not match(ev, c["events"], tol):
                    lines.append((ev["start"], "  base gone %s" % fmt_event(ev, sr)))
            if lines:
                any_events = True
                log("%s / %s" % (name, chain))
                for _, line in sorted(lines):
                    log(line)
    if not any_events:
        log("  none")

    log("")
    if failures:
        log("FAIL: the candidate is worse than the base")
        for f in failures:
            log("  " + f)
        return 1
    log("OK: no new out-of-range events and no more clamped estimates than the base")
    return 0


if __name__ == "__main__":
    sys.exit(main())
