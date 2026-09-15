#!/bin/sh
#
# Puts Declick and Dehum into a Mixxx source tree.
#
# This port is not a plug-in - Mixxx has no plug-in interface that can read the
# track, and reading the track is the whole point - so installing it means
# building Mixxx with these files in it. That is two steps, and this script is
# both:
#
#   1. copy src/restoration and the preferences page into the tree, and
#   2. apply patches/mixxx-<version>.patch, which is the five-file, 37-line edit
#      that registers them: four source lines in CMakeLists.txt, the settings
#      carried as far as the reader, one wrapped call where a track is opened,
#      and the preferences page added to the dialog.
#
# Everything it copies is a file Mixxx does not have, and everything it patches
# is listed above. Nothing is generated and nothing is rewritten in place, so
# --revert really does put the tree back.
#
# Usage:
#   scripts/integrate.sh <mixxx-source-dir>            copy and patch
#   scripts/integrate.sh --check <mixxx-source-dir>    report only, change nothing
#   scripts/integrate.sh --revert <mixxx-source-dir>   take it back out
#
# After integrating, build Mixxx the way its own README says to. Nothing here
# adds a dependency: the cores are portable C++17 with no library behind them,
# which is why this port is the same on macOS, Windows and Linux.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)     # plugins/mixxx

# The Mixxx release this port is written against. The patch is generated from
# that tree and checked against it; a different one may still take it, and
# git apply will say so rather than guess.
want_version="2.5.6"
patch_file="$root/patches/mixxx-$want_version.patch"

mode="apply"
target=""

for arg in "$@"; do
    case "$arg" in
        --check)  mode="check" ;;
        --revert) mode="revert" ;;
        -h|--help)
            sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)  target="$arg" ;;
    esac
done

if [ -z "$target" ]; then
    echo "usage: $0 [--check|--revert] <mixxx-source-dir>" >&2
    exit 2
fi

mixxx=$(cd "$target" 2>/dev/null && pwd) || {
    echo "not a directory: $target" >&2
    exit 1
}

# --- is this actually Mixxx, and which one? --------------------------------
if [ ! -f "$mixxx/CMakeLists.txt" ] || [ ! -d "$mixxx/src/engine/cachingreader" ]; then
    echo "$mixxx does not look like a Mixxx source tree." >&2
    echo "Clone one: git clone -b $want_version https://github.com/mixxxdj/mixxx.git" >&2
    exit 1
fi

got_version=$(sed -n 's/^project(mixxx VERSION \([0-9.]*\)).*/\1/p' "$mixxx/CMakeLists.txt" | head -1)
[ -n "$got_version" ] || got_version="unknown"
if [ "$got_version" != "$want_version" ]; then
    echo "note: this tree is Mixxx $got_version; the patch was made against $want_version."
    echo "      It may still apply. If it does not, the three edits are small enough"
    echo "      to make by hand - see README.md, 'What the patch does'."
fi

if [ ! -f "$patch_file" ]; then
    echo "missing patch: $patch_file" >&2
    exit 1
fi

# --- the files this port adds ----------------------------------------------
# Listed rather than globbed, so --revert removes exactly what --apply added and
# a stray file in the source tree is never deleted from somebody's Mixxx.
restoration_files="audiosourcerestoreproxy.cpp audiosourcerestoreproxy.h \
declick_core.cpp declick_core.h dehum_core.cpp dehum_core.h dehumscout.h \
restorationconfig.cpp restorationconfig.h restorationpipeline.h \
restorationsettings.h restorationsource.h"

prefs_files="dlgprefrestoration.cpp dlgprefrestoration.h dlgprefrestorationdlg.ui"

copy_one() {
    src="$1"
    dst="$2"
    if [ "$mode" = "check" ]; then
        if [ ! -f "$dst" ]; then
            echo "  would add    ${dst#"$mixxx"/}"
        elif cmp -s "$src" "$dst"; then
            echo "  same         ${dst#"$mixxx"/}"
        else
            echo "  would update ${dst#"$mixxx"/}"
        fi
        return
    fi
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
    echo "  copied       ${dst#"$mixxx"/}"
}

case "$mode" in
apply|check)
    echo "=== files ==="
    for f in $restoration_files; do
        copy_one "$root/src/restoration/$f" "$mixxx/src/restoration/$f"
    done
    for f in $prefs_files; do
        copy_one "$root/src/preferences/dialog/$f" "$mixxx/src/preferences/dialog/$f"
    done

    echo "=== patch ==="
    # Already applied is not an error: this script is meant to be re-run after
    # scripts/sync_cores.sh has pushed a new core out, and only the files change
    # then.
    if git -C "$mixxx" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        echo "  already applied"
    elif [ "$mode" = "check" ]; then
        if git -C "$mixxx" apply --check "$patch_file" >/dev/null 2>&1; then
            echo "  would apply cleanly"
        else
            echo "  WOULD NOT APPLY - see README.md, 'What the patch does'" >&2
            exit 1
        fi
    else
        git -C "$mixxx" apply "$patch_file"
        echo "  applied"
    fi

    if [ "$mode" = "check" ]; then
        echo
        echo "check only; nothing was changed"
        exit 0
    fi

    echo
    echo "Done. Build Mixxx as usual, then switch the filters on in"
    echo "Preferences > Restoration. They are off until you do."
    ;;

revert)
    echo "=== patch ==="
    if git -C "$mixxx" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        git -C "$mixxx" apply --reverse "$patch_file"
        echo "  reverted"
    else
        echo "  not applied, or the tree has moved on - left alone"
    fi

    echo "=== files ==="
    for f in $restoration_files; do
        rm -f "$mixxx/src/restoration/$f" && echo "  removed      src/restoration/$f"
    done
    rmdir "$mixxx/src/restoration" 2>/dev/null || true
    for f in $prefs_files; do
        rm -f "$mixxx/src/preferences/dialog/$f" &&
            echo "  removed      src/preferences/dialog/$f"
    done
    ;;
esac
