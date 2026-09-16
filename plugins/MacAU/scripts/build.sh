#!/bin/sh
#
# Builds the three ShellacFilters Audio Units on macOS.
#
# Produces, as universal (x86_64 + arm64) components:
#
#     plugins/dist/au/mac/Declick.component   detect-and-interpolate declicker
#     plugins/dist/au/mac/Dehum.component     hum removal that scouts the record
#     plugins/dist/au/mac/ParaEQ.component    console channel equaliser
#
# Not through the .xcodeproj files beside the sources. Those are the Airwindows
# template's, they reach for Apple's CoreAudio AUPublic and PublicUtility
# sources under $(SYSTEM_DEVELOPER_DIR), and no Xcode has put anything there
# since 3.2.6. This compiles the same sources against Apple's current
# AudioUnitSDK instead - fetched by scripts/get_sdk.sh, Apache 2.0, and the
# supported descendant of exactly those files - with ../au_compat/AUEffectBase.h
# in front of it to reconcile the handful of spellings that changed. The
# wrappers themselves are not edited for it.
#
# What that buys, beyond building at all: the result is an AudioComponents
# bundle, which is what a host on Apple Silicon actually looks for. The .r
# resource files in each folder are the Component Manager's, they need
# AUResources.r from the same missing SDK, and nothing has read one since
# macOS 10.7 - so they are not built here and the Info.plist carries the
# registration instead. That is the same move AirwindowsAUToSignedAUProcess.txt
# describes making by hand, and it is the useful half of what MacSignedAU is.
#
# Unless --skip-tests is given, declick_au_verify, dehum_au_verify and
# paraeq_au_verify are built and run first. Those link the wrapper classes
# directly against ../au_shim and check that each one adds no DSP of its own to
# the core it shares with foo_dsp_*: see ../au_shim/README.md.
#
# Usage:
#   scripts/build.sh                  fetch the SDK if needed, verify, build
#   scripts/build.sh --skip-tests     build only; not recommended
#   scripts/build.sh --clean          wipe the build tree first
#   scripts/build.sh --no-sign        leave the components unsigned; see below
#   scripts/build.sh --install        copy them to ~/Library/Audio/Plug-Ins/Components
#   scripts/build.sh --validate       run auval on what was built (implies --install)
#
# Environment:
#   ARCHS      architectures to build, space separated. "x86_64 arm64".
#   MACOS_MIN  deployment target. 11.0, which is where arm64 starts.
#   AUSDK      an AudioUnitSDK checkout to use instead of external/AudioUnitSDK
#   OUTDIR     where the components go. plugins/dist/au/mac by default.

set -eu
export LC_ALL=C

case "$(uname -s)" in
    Darwin) ;;
    *) echo "build.sh builds macOS Audio Units and needs clang and the" >&2
       echo "CoreAudio frameworks. There is no Audio Unit to build elsewhere." >&2
       exit 1 ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)      # plugins/MacAU
plugins=$(cd "$root/.." && pwd)             # plugins
build="$root/build/mac"
out="${OUTDIR:-$plugins/dist/au/mac}"
sdk="${AUSDK:-$root/external/AudioUnitSDK}"

archs="${ARCHS:-x86_64 arm64}"
macos_min="${MACOS_MIN:-11.0}"

skip_tests=0
clean=0
# Ad-hoc signed by default, which is not belt and braces. clang ad-hoc signs an
# arm64 slice as it links it, and lipo then throws that away: a universal binary
# comes out of lipo unsigned, and macOS will not load unsigned arm64 code at
# all. So on Apple Silicon an unsigned component is not "unsigned", it is
# broken - and on Intel it works, which is how that ships. --no-sign is there
# for the one case that wants it: handing the bundles to something that will
# sign them itself.
sign=1
install=0
validate=0

for arg in "$@"; do
    case "$arg" in
        --skip-tests) skip_tests=1 ;;
        --clean)      clean=1 ;;
        --sign)       sign=1 ;;          # kept: it used to be the opt-in
        --no-sign)    sign=0 ;;
        --install)    install=1 ;;
        --validate)   validate=1; install=1 ;;
        -h|--help)    sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v clang++ >/dev/null 2>&1 || {
    echo "clang++ was not found on PATH. Install the Xcode command line tools:" >&2
    echo "  xcode-select --install" >&2
    exit 1
}

[ "$clean" -eq 1 ] && rm -rf "$build"

# Which plug-in owns which core. The name is the class, the bundle, the entry
# point and the folder; the core is the file it shares with foo_dsp_<core>.
plugin_list="Declick:declick Dehum:dehum ParaEQ:paraeq"

# --- the SDK ----------------------------------------------------------------

if [ ! -f "$sdk/include/AudioUnitSDK/AUEffectBase.h" ]; then
    "$root/scripts/get_sdk.sh" "$sdk"
fi

# --- verify -----------------------------------------------------------------
#
# Against ../au_shim, not against the SDK. These tests link the plug-in classes
# in and drive them the way a host would, and the shim is what lets that happen
# without CoreAudio in the picture - so what they establish is about the DSP,
# the latency arithmetic and the parameter table, and nothing about the bundle.
# auval is the other half; see --validate.

if [ "$skip_tests" -eq 0 ]; then
    echo "=== verify ==="
    tests="$plugins/foobar2000_dsp/tests"
    mkdir -p "$build/tests"
    for entry in $plugin_list; do
        name=${entry%%:*}
        core=${entry##*:}
        src="$tests/${core}_au_verify.cpp"
        [ -f "$src" ] || { echo "  ${core}_au_verify.cpp is missing" >&2; exit 1; }
        clang++ -std=c++11 -O2 \
                -I "$root/au_shim" -I "$root/$name" \
                "$src" "$root/$name/$name.cpp" "$root/$name/${core}_core.cpp" \
                -o "$build/tests/${core}_au_verify"
        "$build/tests/${core}_au_verify" || {
            echo "${core}_au_verify failed" >&2
            exit 1
        }
    done
fi

# --- compile ----------------------------------------------------------------
#
# Per architecture, then lipo. The SDK is eleven translation units and the three
# plug-ins are six between them, so the SDK is compiled once per architecture
# and linked into all three rather than rebuilt for each - which is most of the
# wall clock of a clean build.
#
# C++20 because AudioUnitSDK 1.3 needs it (std::span, requires-clauses). The
# cores and the wrappers are C++11 and do not care.

cxxflags="-std=c++20 -O2 -fvisibility=hidden -Wno-deprecated-declarations"
frameworks="-framework AudioToolbox -framework CoreAudio -framework CoreFoundation"

for arch in $archs; do
    obj="$build/$arch/obj"
    mkdir -p "$obj"

    echo "=== compile $arch ==="

    # The SDK, once.
    ausdk_objs=""
    for src in "$sdk"/src/AudioUnitSDK/*.cpp; do
        o="$obj/ausdk_$(basename "$src" .cpp).o"
        ausdk_objs="$ausdk_objs $o"
        [ -f "$o" ] && [ "$o" -nt "$src" ] && continue
        clang++ $cxxflags -arch "$arch" -mmacosx-version-min="$macos_min" \
                -I "$sdk/include" -c "$src" -o "$o" &
    done
    wait

    # The plug-ins. au_compat must come first on the include path: that is where
    # the "AUEffectBase.h" each wrapper asks for is found, and it is what pulls
    # in the SDK behind it.
    for entry in $plugin_list; do
        name=${entry%%:*}
        core=${entry##*:}
        (
            for src in "$root/$name/$name.cpp" "$root/$name/${core}_core.cpp"; do
                clang++ $cxxflags -arch "$arch" -mmacosx-version-min="$macos_min" \
                        -I "$root/au_compat" -I "$sdk/include" -I "$root/$name" \
                        -c "$src" -o "$obj/${name}_$(basename "$src" .cpp).o"
            done
            clang++ $cxxflags -arch "$arch" -mmacosx-version-min="$macos_min" \
                    -bundle -o "$build/$arch/$name" \
                    "$obj/${name}_$name.o" "$obj/${name}_${core}_core.o" \
                    $ausdk_objs $frameworks
        ) &
    done
    wait
done

# --- assemble the bundles ---------------------------------------------------
#
# By hand, because there is no Xcode project driving this. A .component is a
# plain bundle: an Info.plist that registers it, the binary, and the
# localisation. The Info.plist in each folder is the one the .xcodeproj also
# uses, so it still carries Xcode's build variables and they are substituted
# here - CFBundleExecutable and the two spellings of the product name.

echo "=== assemble ==="
mkdir -p "$out"

for entry in $plugin_list; do
    name=${entry%%:*}
    comp="$out/$name.component"

    thin=""
    for arch in $archs; do thin="$thin $build/$arch/$name"; done

    rm -rf "$comp"
    mkdir -p "$comp/Contents/MacOS" "$comp/Contents/Resources/English.lproj"

    # shellcheck disable=SC2086
    lipo -create $thin -output "$comp/Contents/MacOS/$name"

    sed -e "s/\${EXECUTABLE_NAME}/$name/g" \
        -e "s/\${PRODUCT_NAME:identifier}/$name/g" \
        -e "s/\${PROJECTNAMEASIDENTIFIER}/$name/g" \
        "$root/$name/Info.plist" > "$comp/Contents/Info.plist"
    plutil -lint "$comp/Contents/Info.plist" >/dev/null || {
        echo "$name: the substituted Info.plist is not valid" >&2
        exit 1
    }

    sed -e "s/\${EXECUTABLE_NAME}/$name/g" \
        "$root/$name/version.plist" > "$comp/Contents/version.plist"

    cp "$root/$name/English.lproj/InfoPlist.strings" \
       "$comp/Contents/Resources/English.lproj/InfoPlist.strings"

    # 'BNDL' and no creator. A bundle with no PkgInfo loads; one with a wrong
    # one does not, so it is written rather than left out.
    printf 'BNDL????' > "$comp/Contents/PkgInfo"

    if [ "$sign" -eq 1 ]; then
        # Ad-hoc, and after lipo rather than before it, because lipo is what
        # discards the signature clang put on the arm64 slice. It establishes
        # nothing about who built this; anything going to someone else wants a
        # Developer ID and notarising, which is scripts/package.sh.
        codesign --force --sign - "$comp"
    fi

    printf '  %-20s %s\n' "$name.component" \
        "$(lipo -archs "$comp/Contents/MacOS/$name" 2>/dev/null || echo '?')"
done

echo
echo "components in $out"

# --- install and validate ---------------------------------------------------

if [ "$install" -eq 1 ]; then
    dest="$HOME/Library/Audio/Plug-Ins/Components"
    echo "=== install ==="
    mkdir -p "$dest"
    for entry in $plugin_list; do
        name=${entry%%:*}
        rm -rf "$dest/$name.component"
        ditto "$out/$name.component" "$dest/$name.component"
        echo "  $dest/$name.component"
    done
    # The registrar caches what it found last time, so a rebuild at the same
    # path is invisible to a host until it is asked again. Harmless: it is
    # started on demand.
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
fi

if [ "$validate" -eq 1 ]; then
    echo "=== auval ==="
    command -v auval >/dev/null 2>&1 || {
        echo "auval was not found; it ships with macOS in /usr/bin." >&2
        exit 1
    }
    failed=0
    for entry in $plugin_list; do
        name=${entry%%:*}
        # The four-character subtype the plug-in registers under, read from the
        # Info.plist rather than written down again here.
        subtype=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:subtype" \
                  "$out/$name.component/Contents/Info.plist" 2>/dev/null || echo "")
        manuf=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:manufacturer" \
                "$out/$name.component/Contents/Info.plist" 2>/dev/null || echo "")
        [ -n "$subtype" ] && [ -n "$manuf" ] || {
            echo "  $name: no AudioComponents entry in the Info.plist" >&2
            failed=1
            continue
        }
        if auval -v aufx "$subtype" "$manuf" >"$build/auval_$name.log" 2>&1; then
            printf '  %-20s PASS\n' "$name"
        else
            printf '  %-20s FAIL  %s\n' "$name" "$build/auval_$name.log"
            failed=1
        fi
    done
    [ "$failed" -eq 0 ] || exit 1
fi

exit 0
