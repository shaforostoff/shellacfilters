#!/bin/sh
#
# Builds the LinuxVST ports of Declick and Dehum into loadable .so plug-ins.
#
# Produces:
#
#     plugins/dist/linuxvst/Declick.so
#     plugins/dist/linuxvst/Dehum.so
#
# This is the counterpart of scripts/build_winvst.ps1, and the same two things
# about it are worth knowing before reading it.
#
# It does not use plugins/LinuxVST/CMakeLists.txt. That project builds all five
# hundred plug-ins and wants Steinberg's vst2.x sources dropped into
# LinuxVST/include/vstsdk, which are not redistributable and are not in this
# repository. Rather than change the CMake project - which is what anyone who
# does have the SDK uses, and which now carries add_airwindows_plugin(Declick)
# and add_airwindows_plugin(Dehum) lines like every other plug-in - this drives
# the compiler over the two plug-in folders directly.
#
# It compiles plugins/WinVST/vst2_shim in place of the SDK. That folder is named
# for where the Windows port put it; what is in it is the VST2 ABI, and the ABI
# is not a Windows thing - the AEffect layout, the opcode numbers and the
# calling convention are the same here, which is the whole reason one plug-in
# source tree can serve hosts on both platforms. Read
# plugins/WinVST/vst2_shim/README.md for what the shim is and, more to the
# point, what it does not prove.
#
# Unless --skip-tests is given, two of the harnesses in tests/ are built and run
# against what came out. They are the same two the Windows build runs, and on
# this platform they are the only way to run them at all: the CMake project in
# plugins/foobar2000_dsp builds a foobar2000 component and stops at a
# FATAL_ERROR anywhere but Windows.
#
#   <plugin>_vst_verify   drives the core directly with the same Config and
#                         requires the wrapper's processDoubleReplacing output
#                         to match it to the bit, plus everything the wrapper
#                         has to get right on its own - block patterns, the
#                         latency contract, resume(), the slider mappings, the
#                         preset chunk. Links the plug-in; cannot see the ABI.
#   vst_host_verify       loads the finished .so the way a host does - dlopen,
#                         the entry point, opcodes, the function pointers in
#                         AEffect - and requires its audio to match the same
#                         plug-in linked statically, to the bit. That is the one
#                         that sees the ABI.
#
# Usage:
#   scripts/build_linuxvst.sh                  both plug-ins, both harnesses
#   scripts/build_linuxvst.sh Dehum            just the dehummer
#   scripts/build_linuxvst.sh --skip-tests     build only; not recommended, and
#                                              on this platform there is no
#                                              other way to run either harness
#   scripts/build_linuxvst.sh --clean          wipe the object directory first
#
# Environment:
#   CXX      the compiler. g++ by default; "g++ -m32", with multilib installed,
#            builds a 32 bit plug-in - set OUTDIR as well in that case so it
#            does not overwrite the 64 bit one.
#   OUTDIR   where the .so files go. plugins/dist/linuxvst by default.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)      # plugins/foobar2000_dsp
plugins=$(cd "$root/.." && pwd)             # plugins
linuxvst="$plugins/LinuxVST/src"
shim="$plugins/WinVST/vst2_shim"
testsdir="$root/tests"
build="$root/build/linuxvst"
# alongside build_release.ps1's .fb2k-components and build_winvst.ps1's DLLs,
# one level above this project, because a VST2 plug-in is not a foobar2000
# component
out="${OUTDIR:-$plugins/dist/linuxvst}"
cxx="${CXX:-g++}"

skip_tests=0
clean=0
wanted=""

usage() {
    cat <<'USAGE'
usage: build_linuxvst.sh [Declick|Dehum] [--skip-tests] [--clean]

  Declick, Dehum   which plug-ins to build; both if neither is named
  --skip-tests     build without verifying the result
  --clean          remove the object directory first

  CXX and OUTDIR override the compiler and the output directory; see the
  comment at the top of this script.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-tests) skip_tests=1 ;;
        --clean)      clean=1 ;;
        -h|--help)    usage; exit 0 ;;
        Declick|Dehum) wanted="$wanted $1" ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
[ -n "$wanted" ] || wanted="Declick Dehum"

# ---------------------------------------------------------------------------
# the same compiler settings for every target
#
# Deliberately not a copy of what the LinuxVST CMake project uses - that is
# "-O2 -D__cdecl=", and the define is for a spelling in Steinberg's headers that
# the shim does not use. What is here is what matters for something another
# program is going to load:
#
#   -fPIC                       a shared object has no choice
#   -fvisibility=hidden, and the version script written below: the only symbols
#                               this .so exports are the entry point and its
#                               legacy alias. Some hosts dlopen with
#                               RTLD_GLOBAL, and one that does would otherwise
#                               get the plug-in's whole C++ surface, and its
#                               copy of libstdc++'s, in the global namespace.
#   -static-libstdc++ -static-libgcc
#                               the same call /MT makes on Windows: a plug-in
#                               linked against this machine's libstdc++ would
#                               refuse to load on a distribution with an older
#                               one, and the user's host is where they would
#                               find that out.
#   -Wl,--no-undefined          a symbol the shim does not define is a link
#                               error here, not a dlopen failure later.
#   no -ffast-math, ever        it changes the arithmetic, and the bit
#                               comparisons in vst_host_verify are what say the
#                               ports are running the same DSP.

# C++17 for declick_core.h, which allocates its filter buffers with aligned new.
std="-std=c++17"
common="-O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DVST_FORCE_DEPRECATED"
warn="-Wall -Wextra"
# The wrapper follows the Airwindows house pattern - a four character code as an
# integer constant, and getChunk()/setChunk() ignoring arguments on purpose - so
# it warns exactly where the other five hundred plug-ins do. Those are not ours
# to fix; MSVC is told the same thing with /wd4244 /wd4305 /wd4100 in
# tests/CMakeLists.txt.
warn_wrapper="$warn -Wno-multichar -Wno-unused-parameter"

warnings=0

# Compiler diagnostics go to a file so a warning can be surfaced without the
# successful compile printing anything, and so a failing one prints everything.
# sh has no local variables, so everything these three set is prefixed - the
# loop below has its own $src and $objdir and they are not these.
report() {   # report <logfile>
    if [ -s "$1" ]; then
        sed -n 's/^/      /p' "$1"
        _found=$(grep -c 'warning:' "$1" || true)
        warnings=$((warnings + _found))
    fi
}

compile() {  # compile <object> <source> <warnflags> [more flags...]
    _obj="$1"; _src="$2"; _warn="$3"; shift 3
    if ! $cxx $std $common $_warn "$@" -c "$_src" -o "$_obj" >"$build/cc.log" 2>&1; then
        cat "$build/cc.log" >&2
        echo "compiling $_src failed" >&2
        exit 1
    fi
    report "$build/cc.log"
}

link() {     # link <output> [flags and objects...]
    _target="$1"; shift
    if ! $cxx $std $common "$@" -o "$_target" >"$build/ld.log" 2>&1; then
        cat "$build/ld.log" >&2
        echo "linking $_target failed" >&2
        exit 1
    fi
    report "$build/ld.log"
}

# ---------------------------------------------------------------------------

echo
echo "mirrored files"
"$root/scripts/sync_cores.sh" --check || {
    echo "a mirror does not match its canonical copy - fix that before" \
         "building anything" >&2
    exit 1
}

if [ "$clean" -eq 1 ] && [ -d "$build" ]; then
    echo
    echo "removing $build"
    rm -rf "$build"
fi
mkdir -p "$build" "$out"

# Two names, and nothing else. Written here rather than committed because it is
# a property of this build, not of the plug-ins.
cat > "$build/vst2.map" <<'MAP'
{
    global:
        VSTPluginMain;
        main;
    local:
        *;
};
MAP

echo
$cxx --version | head -1

results=""

for p in $wanted; do
    lower=$(printf '%s' "$p" | tr 'A-Z' 'a-z')
    upper=$(printf '%s' "$p" | tr 'a-z' 'A-Z')
    src="$linuxvst/$p"
    core="$src/${lower}_core.cpp"

    for f in "$shim/audioeffectx.cpp" "$shim/vstplugmain.cpp" \
             "$src/$p.cpp" "$src/${p}Proc.cpp" "$core"; do
        [ -f "$f" ] || { echo "missing source: $f" >&2; exit 1; }
    done

    objdir="$build/$p"
    mkdir -p "$objdir"
    inc="-I$shim -I$src"

    echo
    echo "  $p.so"

    compile "$objdir/audioeffectx.o"  "$shim/audioeffectx.cpp" "$warn"         $inc
    compile "$objdir/vstplugmain.o"   "$shim/vstplugmain.cpp"  "$warn"         $inc
    compile "$objdir/$p.o"            "$src/$p.cpp"            "$warn_wrapper" $inc
    compile "$objdir/${p}Proc.o"      "$src/${p}Proc.cpp"      "$warn_wrapper" $inc
    compile "$objdir/${lower}_core.o" "$core"                  "$warn"         $inc

    so="$out/$p.so"
    link "$so" -shared \
        -Wl,--no-undefined -Wl,--exclude-libs,ALL \
        -Wl,--version-script="$build/vst2.map" \
        -static-libstdc++ -static-libgcc \
        "$objdir/audioeffectx.o" "$objdir/vstplugmain.o" "$objdir/$p.o" \
        "$objdir/${p}Proc.o" "$objdir/${lower}_core.o"

    kb=$(( ($(wc -c < "$so") + 512) / 1024 ))
    if command -v readelf >/dev/null 2>&1; then
        class=$(readelf -h "$so" | sed -n 's/^ *Class: *//p')
        machine=$(readelf -h "$so" | sed -n 's/^ *Machine: *//p')
        echo "      $kb kB, $class, $machine"
    else
        echo "      $kb kB"
    fi

    # Cheap insurance for a --skip-tests build: the host test checks this too,
    # but a .so that exports nothing a host looks for is not worth shipping.
    if command -v nm >/dev/null 2>&1; then
        exported=$(nm -D --defined-only "$so" | awk '$2 == "T" { print $3 }' \
                   | LC_ALL=C sort | tr '\n' ' ')
        [ "$exported" = "VSTPluginMain main " ] || {
            echo "      exports: $exported" >&2
            echo "expected exactly VSTPluginMain and main" >&2
            exit 1
        }
    fi

    # --- the tests ---------------------------------------------------------
    tests="skipped"
    if [ "$skip_tests" -eq 0 ]; then
        tobj="$build/${p}_tests"
        mkdir -p "$tobj"

        # The wrapper against the core it shares with foo_dsp_$lower. This
        # compiles the LinuxVST folder's own copy, deliberately: that is the
        # file this build just made a plug-in out of, so it is the artefact
        # being tested rather than a stand-in for it. Whether that copy is
        # still byte-identical to the canonical one is the mirror check above.
        compile "$tobj/${lower}_vst_verify.o" "$testsdir/${lower}_vst_verify.cpp" \
                "$warn -Wno-multichar" $inc

        vstverify="$tobj/${lower}_vst_verify"
        link "$vstverify" "$tobj/${lower}_vst_verify.o" \
            "$objdir/audioeffectx.o" "$objdir/$p.o" "$objdir/${p}Proc.o" \
            "$objdir/${lower}_core.o"

        echo
        "$vstverify"

        # Both harnesses take -Wno-multichar and no other suppression: the four
        # character code that warns is in the plug-in's header, which they have
        # to include, and nothing in either test file warns on its own.
        #
        # Neither links vstplugmain.cpp, and this one especially must not: it
        # loads the .so *and* links the plug-in in, so that would be a second
        # entry point - and on this platform the legacy alias in it is a symbol
        # literally named main, which is the test's own.
        compile "$tobj/vst_host_verify.o" "$testsdir/vst_host_verify.cpp" \
                "$warn -Wno-multichar" $inc "-DVST_PLUGIN_$upper"

        exe="$tobj/vst_host_verify_$p"
        link "$exe" "$tobj/vst_host_verify.o" \
            "$objdir/audioeffectx.o" "$objdir/$p.o" "$objdir/${p}Proc.o" \
            "$objdir/${lower}_core.o" -ldl

        echo
        "$exe" "$so"
        tests="passed"
    fi

    results="$results$p.so|$kb|$tests
"
done

# ---------------------------------------------------------------------------

echo
echo "=== built ==="
printf '%s' "$results" | while IFS='|' read -r name kb verdict; do
    [ -n "$name" ] || continue
    printf '  %-14s %6s kB   tests %s\n' "$name" "$kb" "$verdict"
done

count=$(printf '%s' "$results" | grep -c . || true)
echo
echo "$count plug-in(s) in $out"
[ "$warnings" -eq 0 ] || echo "$warnings compiler warning(s) above"
echo "copy them where your host looks for VST2 plug-ins - ~/.vst is the usual" \
     "place - to install."
exit 0
