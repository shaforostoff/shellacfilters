#!/usr/bin/env bash
#
# Builds the components that have a macOS port and packages each one as an
# installable .fb2k-component in plugins/dist/. The counterpart of
# scripts\build_release.ps1.
#
# Configures and builds with CMake, runs the test suites, pulls the debug info
# out into a .dSYM, strips and signs each bundle, then assembles one archive
# per component:
#
#     foo_dsp_dehum-<version>-mac.fb2k-component
#       mac/foo_dsp_dehum.component      universal: Apple Silicon and Intel
#
# Three of the four components are ported: declick, dehum and paraeq.
# Decrackle is Windows only.
#
# mac/ is where foobar2000 for Mac looks, and every other platform ignores a
# subfolder it does not understand - which is what --merge is for. Pointed at
# the directory scripts\build_release.ps1 wrote its archives to, it unpacks each
# component's Windows archive alongside the macOS bundle and writes one
# component that installs on both:
#
#     foo_dsp_dehum-<version>.fb2k-component
#       foo_dsp_dehum.dll                32 bit Windows, foobar2000 1.5 - 2.x
#       x64/foo_dsp_dehum.dll            64 bit Windows, foobar2000 2.x
#       mac/foo_dsp_dehum.component      macOS, foobar2000 for Mac
#
# Debug symbols go into a separate archive that is NOT part of the component,
# as on Windows - keep it so a crash report can be resolved.
#
# Usage:
#     scripts/build_release_macos.sh [options] [component...]
#
#     component             foo_dsp_declick, foo_dsp_dehum or foo_dsp_paraeq,
#                           or the same without the foo_dsp_ prefix.
#                           Default: all three.
#     -a, --arch <list>     architectures to build, comma separated:
#                           arm64, x86_64, or both. Default: both, which is
#                           what makes the bundles universal.
#     -c, --config <name>   CMake build type. Default: Release.
#     -j, --jobs <n>        compile jobs to run at once. Default: one fewer
#                           than the machine has cores, so the build leaves a
#                           core - and the memory that goes with it - for
#                           everything else running.
#     -s, --sign <identity> codesign identity. Default: - , which is an ad-hoc
#                           signature. See "Signing" below.
#     -m, --merge <dir>     fold each macOS bundle into the Windows archive of
#                           the same component and version found in <dir>,
#                           producing one archive per component that installs
#                           on Windows and macOS alike. The Windows script
#                           writes to plugins/dist, so that is usually <dir>;
#                           merging an archive over itself is safe. A missing
#                           archive is an error, caught before anything builds.
#         --skip-tests      do not run the verification harnesses. Not recommended.
#         --clean           wipe the build directory first.
#
# There is no counterpart to build_release.ps1's -Win7, -Toolset, -Generator or
# -WindowsSdk: they all pick between Microsoft toolchains. Nor to
# -InstructionSet: the x86_64 slice keeps the SSE2 baseline, and the code that
# can use AVX picks it at run time.
#
# Signing
# -------
# The components are ad-hoc signed by default, which is what the SDK's own
# Xcode project does and what a component needs: on Apple Silicon the kernel
# refuses to map unsigned code at all, so an unsigned bundle would simply fail
# to load on half the machines it is meant for. Cross-building a universal
# binary does not sign it for you, so this is not optional.
#
# A Developer ID and notarization are NOT needed. foobar2000 for Mac runs under
# the hardened runtime but ships com.apple.security.cs.disable-library-validation,
# which is exactly the entitlement that lets it load code signed by somebody
# else - or by nobody. Pass -s "Developer ID Application: ..." if you have one
# and would rather ship signed components anyway; nothing here requires it.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # plugins/foobar2000_dsp
plugins="$(cd "$root/.." && pwd)"                           # plugins
# One level above this project, alongside build_release.ps1's archives - which
# is also what makes `--merge "$plugins/dist"` the natural thing to ask for.
dist_dir="$plugins/dist"

all_components="foo_dsp_declick foo_dsp_dehum foo_dsp_paraeq"

arch_list="arm64,x86_64"
configuration="Release"
jobs=""
sign_identity="-"
merge_dir=""
skip_tests=0
clean=0
components=""

die() { echo "error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -a|--arch)    arch_list="${2:-}"; shift 2 ;;
        -c|--config)  configuration="${2:-}"; shift 2 ;;
        -j|--jobs)    jobs="${2:-}"; shift 2 ;;
        -s|--sign)    sign_identity="${2:-}"; shift 2 ;;
        -m|--merge)   merge_dir="${2:-}"; shift 2
                      [[ -n "$merge_dir" ]] || die "--merge wants a directory" ;;
        --skip-tests) skip_tests=1; shift ;;
        --clean)      clean=1; shift ;;
        # Every comment line down to the first that is not one, so the help
        # cannot fall out of step with the header as options grow lines.
        -h|--help)    awk 'NR==1 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' \
                          "${BASH_SOURCE[0]}"; exit 0 ;;
        -*)           die "unknown option: $1" ;;
        *)
            c="$1"; [[ "$c" == foo_dsp_* ]] || c="foo_dsp_$c"
            case " $all_components " in
                *" $c "*) ;;
                *) die "no macOS port of '$1'; expected one of: $all_components" ;;
            esac
            case " $components " in
                *" $c "*) ;;
                *) components="${components:+$components }$c" ;;
            esac
            shift ;;
    esac
done
# A string rather than an array throughout: /bin/bash on macOS is 3.2, where an
# empty array counts as unset and `set -u` stops the script over it.
[[ -n "$components" ]] || components="$all_components"

# --- how much of the machine to use ----------------------------------------
if [[ -z "$jobs" ]]; then
    cores="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
    if [[ "$cores" -gt 1 ]]; then jobs=$(( cores - 1 )); else jobs=1; fi
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "--jobs wants a positive whole number, not '$jobs'"

command -v cmake    >/dev/null || die "cmake was not found on PATH. Install CMake 3.16 or newer."
command -v codesign >/dev/null || die "codesign was not found. Install the Xcode command line tools."
command -v clang    >/dev/null || die "clang was not found. Install the Xcode command line tools."

# --- architectures ---------------------------------------------------------
# One configure covers both: a universal binary is two slices of one build,
# not two builds. Which is also why there is no loop here and there is one in
# the Windows script.
cmake_archs=""
IFS=',' read -r -a requested <<< "$arch_list"
for a in "${requested[@]}"; do
    case "$a" in
        arm64|x86_64) ;;
        *) die "unknown architecture '$a'; expected arm64 or x86_64" ;;
    esac
    case ";$cmake_archs;" in
        *";$a;"*) ;;
        *) cmake_archs="${cmake_archs:+$cmake_archs;}$a" ;;
    esac
done
[[ -n "$cmake_archs" ]] || die "no architectures requested"

if [[ "$cmake_archs" == *";"* ]]; then
    arch_tag=""          # universal, which is the shipping shape
    arch_words="universal (Apple Silicon and Intel)"
else
    arch_tag="-$cmake_archs"
    arch_words="$cmake_archs only - not the shipping configuration"
fi

# --- the cores and the VST wrapper are mirrored into the other formats; catch
# --- drift before we build a component whose maths no longer matches the VSTs'
echo "=== Core sync ==="
sh "$root/scripts/sync_cores.sh" --check || die "mirrors have drifted; see above"

# --- versions, out of each version.h so archive names match the binaries ----
version_of() {
    local v
    v="$(sed -n 's/^#define[[:space:]]*[A-Z_]*_VERSION_STRING[[:space:]]*"\([^"]*\)".*$/\1/p' \
             "$root/$1/version.h" | head -1)"
    [[ -n "$v" ]] || die "could not read the version out of $1/version.h"
    echo "$v"
}

echo
for c in $components; do
    printf '%-18s %s\n' "$c" "$(version_of "$c")"
done
echo "  architecture: $arch_words"
echo "  jobs: $jobs"

# --- the Windows halves, checked before anything is built ------------------
# Finding out that one is missing after the build would be finding it out after
# the minutes that took.
if [[ -n "$merge_dir" ]]; then
    [[ -d "$merge_dir" ]] || die "no such directory: $merge_dir"
    merge_dir="$(cd "$merge_dir" && pwd)"
    for c in $components; do
        a="$merge_dir/$c-$(version_of "$c").fb2k-component"
        [[ -f "$a" ]] || die "no Windows archive to merge for $c: $a
       Build it with scripts\\build_release.ps1 at the same version, or leave
       --merge off to get a macOS-only component."
    done
fi

build_dir="$root/build/mac${arch_tag:-"-universal"}"
stage="$root/build/_package_mac"
symbols="$root/build/_symbols_mac"

[[ $clean -eq 1 ]] && rm -rf "$build_dir"
rm -rf "$stage" "$symbols"
mkdir -p "$stage" "$symbols" "$dist_dir"

# --- build -----------------------------------------------------------------
echo
echo "=== Configuring ==="
cmake -S "$root" -B "$build_dir" \
      -DCMAKE_BUILD_TYPE="$configuration" \
      -DCMAKE_OSX_ARCHITECTURES="$cmake_archs" \
      -DFOO_DSP_LTO=ON

echo
echo "=== Building ==="
cmake --build "$build_dir" --config "$configuration" --parallel "$jobs"

if [[ $skip_tests -eq 0 ]]; then
    echo
    echo "=== Testing ==="
    # The harnesses are built universal along with everything else, so what
    # runs here is this machine's slice. The other one is compiled and linked
    # but cannot be executed on this hardware, so it is not covered - run this
    # script on a machine of each architecture to cover both.
    ctest --test-dir "$build_dir" -C "$configuration" --output-on-failure
fi

# --- per component: symbols, stage, strip, sign ----------------------------
for c in $components; do
    bundle="$build_dir/$c/$c.component"
    binary="$bundle/Contents/MacOS/$c"
    [[ -d "$bundle" && -f "$binary" ]] || die "expected output missing: $bundle"

    echo
    echo "=== $c ==="

    # Symbols are read out of the build tree's own binary, before anything has
    # been taken out of it.
    mkdir -p "$symbols/$c"
    dsymutil "$binary" -o "$symbols/$c/$c.component.dSYM"
    echo "  symbols: $c.component.dSYM"

    # The other platform's payload, if there is one, is unpacked FIRST, so
    # nothing is written to dist/ until everything going into the archive is
    # already on disk. That is what makes it safe to merge an archive in dist/
    # over itself: the Windows build lands there under the name this writes.
    mkdir -p "$stage/$c"
    if [[ -n "$merge_dir" ]]; then
        win="$merge_dir/$c-$(version_of "$c").fb2k-component"
        echo "  merging $(basename "$win")"
        cmake -E chdir "$stage/$c" cmake -E tar xf "$win"

        if [[ ! -f "$stage/$c/$c.dll" && ! -f "$stage/$c/x64/$c.dll" ]]; then
            echo "  warning: no $c.dll in it - is that really a $c component?" >&2
        fi
        # A macOS payload already in there is this script's previous output,
        # and the one being built replaces it rather than merging with it.
        if [[ -d "$stage/$c/mac" ]]; then
            echo "  replacing the macOS payload it already carried"
            rm -rf "$stage/$c/mac"
        fi
        # An `&&` as the last command in the loop body would make the loop's
        # exit status its own, and `set -e` would end the script there on an
        # archive that turned out to hold nothing.
        for f in "$stage/$c"/*; do
            if [[ -e "$f" ]]; then
                echo "    kept: ${f#$stage/$c/}"
            fi
        done
    fi

    mkdir -p "$stage/$c/mac"
    cp -R "$bundle" "$stage/$c/mac/"

    # Strip, then sign, and both on the staged copy rather than the build
    # tree's: stripping is destructive and the build does not know it
    # happened, so a second run over an unchanged tree would find nothing left
    # to pull a .dSYM out of. Signing comes after stripping either way, because
    # taking bytes out of a signed binary is exactly what a signature detects.
    staged="$stage/$c/mac/$c.component"
    xcrun strip -x -S "$staged/Contents/MacOS/$c"
    codesign --force --sign "$sign_identity" --timestamp=none "$staged"
    codesign --verify --strict "$staged" || die "the signature on $c did not verify"
    if [[ "$sign_identity" == "-" ]]; then
        echo "  signed: ad-hoc - no Developer ID, and none is needed; see the note in this script"
    else
        echo "  signed: $sign_identity"
    fi
done

# --- package ---------------------------------------------------------------
# cmake -E tar for the same reason the Windows script uses it, and because a
# plain zip is all a bundle needs: the signature lives in the Mach-O and in
# Contents/_CodeSignature, both of them ordinary files, and the mode bits
# survive the round trip.
echo
echo "=== Package ==="
for c in $components; do
    v="$(version_of "$c")"
    if [[ -n "$merge_dir" ]]; then
        component="$dist_dir/$c-$v.fb2k-component"
    else
        component="$dist_dir/$c-$v-mac$arch_tag.fb2k-component"
    fi
    symbols_zip="$dist_dir/$c-$v-mac$arch_tag-symbols.zip"
    rm -f "$component" "$symbols_zip"

    cmake -E chdir "$stage/$c"   cmake -E tar cf "$component"   --format=zip .
    cmake -E chdir "$symbols/$c" cmake -E tar cf "$symbols_zip" --format=zip .

    printf '  %s  (%d bytes)\n' "$component" "$(stat -f%z "$component")"
    cmake -E tar tf "$component" | sed 's/^/      /'
    printf '  %s  (%d bytes)\n' "$symbols_zip" "$(stat -f%z "$symbols_zip")"
done

cat <<EOF

To install: drag the .fb2k-component file onto foobar2000, or use
Preferences > Components > Install...
EOF

if [[ -n "$arch_tag" ]]; then
    echo
    echo "This is not the release build. It carries $cmake_archs only, so it will"
    echo "not load on the other architecture. Install it to measure or to test,"
    echo "and do not publish it under that name."
fi
