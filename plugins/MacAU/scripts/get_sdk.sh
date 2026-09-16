#!/bin/sh
#
# Downloads Apple's AudioUnitSDK into external/AudioUnitSDK.
#
# This is the folder AirwindowsAUToSignedAUProcess.txt calls CA_SDK and tells
# you to drag onto your desktop. It is not in this repository and it is not
# something you have on disk already: the AUPublic and PublicUtility sources the
# .xcodeproj files reach for under $(SYSTEM_DEVELOPER_DIR) last shipped with
# Xcode 3.2.6, they are Apple's and not redistributable, and all 545 plug-in
# folders beside this one need them to build.
#
# Apple's replacement is AudioUnitSDK, the same base classes maintained in the
# open under Apache 2.0:
#
#     https://github.com/apple/AudioUnitSDK
#
# It is about 12,000 lines, needs nothing installed but git and the Xcode
# command line tools, and - with ../au_compat/AUEffectBase.h in front of it -
# compiles the Airwindows-era sources unmodified. scripts/build.sh calls this on
# its own when the folder is missing, so you normally never run it by hand.
#
# Why the version is pinned, and to 1.3.0 rather than the newest. 1.4.0 uses
# std::expected, which is C++23 and needs a libc++ from Xcode 16.3 or newer;
# 1.3.0 needs C++20, which Xcode 14 has. Pinning also means a build here does
# not change because a dependency did. Override with --version to try another:
#
#     scripts/get_sdk.sh --version AudioUnitSDK-1.4.0
#
# Usage:
#   scripts/get_sdk.sh [destination] [--force] [--version TAG]

set -eu
export LC_ALL=C

root=$(cd "$(dirname "$0")/.." && pwd)      # plugins/MacAU
dest="$root/external/AudioUnitSDK"
repo="https://github.com/apple/AudioUnitSDK.git"
tag="AudioUnitSDK-1.3.0"
force=0

while [ $# -gt 0 ]; do
    case "$1" in
        --force)   force=1 ;;
        --version) shift; tag="${1:-}" ;;
        -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)        echo "usage: $0 [destination] [--force] [--version TAG]" >&2; exit 2 ;;
        *)         dest="$1" ;;
    esac
    shift
done

# The marker is a header rather than the directory: a clone that was
# interrupted leaves the directory behind and nothing in it.
if [ "$force" -eq 0 ] && [ -f "$dest/include/AudioUnitSDK/AUEffectBase.h" ]; then
    echo "AudioUnitSDK already at $dest"
    exit 0
fi

command -v git >/dev/null 2>&1 || {
    echo "git was not found on PATH. Install the Xcode command line tools:" >&2
    echo "  xcode-select --install" >&2
    exit 1
}

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"

echo "=== fetching $tag ==="
git clone --depth 1 --branch "$tag" "$repo" "$dest" 2>&1 | sed 's/^/  /'

[ -f "$dest/include/AudioUnitSDK/AUEffectBase.h" ] || {
    echo "the clone finished but $dest does not look like AudioUnitSDK." >&2
    exit 1
}

# The tree is not this repository's to track, and a nested .git would make it a
# submodule nobody asked for.
rm -rf "$dest/.git"

echo "AudioUnitSDK $tag at $dest"
