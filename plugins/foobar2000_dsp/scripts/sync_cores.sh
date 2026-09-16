#!/bin/sh
#
# Keeps the files that more than one plug-in format compiles byte-identical
# across those formats: the portable DSP cores, and the VST2 wrapper that the
# Windows and Linux ports share.
#
# A POSIX sh port of sync_cores.ps1, for editing from macOS or Linux where there
# is no PowerShell. Same canonical copies, same mirror list, same --check
# semantics and exit codes, so either script can front the other's CI gate.
#
# The reason it exists: these files are now consumed by builds on other
# platforms as well, so they get edited on machines that cannot run the .ps1,
# and the mirrors then drift silently until the next Windows build - which is
# exactly what this script pair exists to prevent.
#
# ADDING A FORMAT means adding its directory to the destination list of every
# file it builds - the third field takes a comma separated list - or a whole new
# line if it wants files no other format does. Either way the same edit has to
# be made to $mirrors in sync_cores.ps1. The list is the only thing duplicated
# between the two; keep them in step.
#
# Out-of-tree consumers - anything that vendors one of these files from outside
# this repository - are not mirrors and are not checked here. They have to be
# updated by hand when the canonical copy changes.
#
# Usage:
#   scripts/sync_cores.sh              push the canonical copies out to every mirror
#   scripts/sync_cores.sh --check      compare only; report and exit 1 on drift

set -eu

plugins=$(cd "$(dirname "$0")/../.." && pwd)     # plugins

check=0
case "${1:-}" in
    --check|-Check) check=1 ;;
    "") ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

# from_dir|file[,file...]|to_dir[,to_dir...], every path relative to plugins/
#
# The cores are canonical in the foobar2000 tree, which is where they are
# developed and where the test harness lives. The VST2 wrapper has no copy
# there - foo_dsp_* is a foobar2000 component, not a VST - so for those three
# files WinVST is the canonical copy and the other VST2 ports mirror it. All
# three compile the same wrapper, so a change to the parameter mapping or the
# latency reporting in one of them belongs in all of them.
#
# MacAU takes the cores but not the wrapper: an Audio Unit is a different
# interface, so MacAU/Declick/Declick.cpp is its own source rather than a copy
# of anybody's. The cores are the only thing it shares. vdjplugin is in the same
# position for the same reason, and takes the cores for both plug-ins it builds
# out of each - the live one and the buffer one share the folder.
#
# ParaEQ mirrors its curve editor as well as its core, which nothing else here
# does, and to WinVST only. paraeq_editor.{h,cpp} is plain Win32 with no SDK in
# it, so the foobar2000 component and the VST compile the same file and the two
# get the same instrument - the alternative was a second owner-drawn editor of
# that size for the VST, which would have diverged inside a month. MacAU is not
# on that list and must not be: an Audio Unit has no Win32 window to put it in.
#
mirrors="
foobar2000_dsp/foo_dsp_declick|declick_core.h,declick_core.cpp|WinVST/Declick,LinuxVST/src/Declick,MacVST/Declick/source,MacAU/Declick,vdjplugin/vdj_declick
foobar2000_dsp/foo_dsp_dehum|dehum_core.h,dehum_core.cpp|WinVST/Dehum,LinuxVST/src/Dehum,MacVST/Dehum/source,MacAU/Dehum,vdjplugin/vdj_dehum
foobar2000_dsp/foo_dsp_paraeq|paraeq_core.h,paraeq_core.cpp|WinVST/ParaEQ,MacAU/ParaEQ
foobar2000_dsp/foo_dsp_paraeq|paraeq_editor.h,paraeq_editor.cpp|WinVST/ParaEQ
WinVST/Declick|Declick.h,Declick.cpp,DeclickProc.cpp|LinuxVST/src/Declick,MacVST/Declick/source
WinVST/Dehum|Dehum.h,Dehum.cpp,DehumProc.cpp|LinuxVST/src/Dehum,MacVST/Dehum/source
"

drifted=0
copied=0

for entry in $mirrors; do
    from=$(printf '%s' "$entry" | cut -d'|' -f1)
    files=$(printf '%s' "$entry" | cut -d'|' -f2 | tr ',' ' ')
    tos=$(printf '%s' "$entry" | cut -d'|' -f3 | tr ',' ' ')

    for file in $files; do
        src="$plugins/$from/$file"

        if [ ! -f "$src" ]; then
            echo "canonical copy missing: $src" >&2
            exit 1
        fi

        for to in $tos; do
            dst="$plugins/$to/$file"
            rel="$to/$file"

            if [ -f "$dst" ]; then
                if cmp -s "$src" "$dst"; then
                    echo "  same     $rel"
                    continue
                fi
                state="differs"
            else
                state="missing"
            fi

            if [ "$check" -eq 1 ]; then
                printf '  %-8s %s\n' "$state" "$rel"
                drifted=$((drifted + 1))
            else
                mkdir -p "$plugins/$to"
                cp -f "$src" "$dst"
                echo "  updated  $rel"
                copied=$((copied + 1))
            fi
        done
    done
done

if [ "$check" -eq 1 ]; then
    if [ "$drifted" -gt 0 ]; then
        echo
        echo "$drifted mirrored file(s) do not match their canonical copy. Run" \
             "scripts/sync_cores.sh to update them, or move the edit into the" \
             "canonical copy first if that is where it belongs." >&2
        exit 1
    fi
    echo
    echo "every mirror matches"
    exit 0
fi

echo
echo "$copied file(s) updated"
exit 0
