#!/bin/sh
#
# Builds and runs the verification harness for the Mixxx restoration.
#
# This does NOT build Mixxx, and there is nothing here to install. What it
# builds is the part of the port that can be tested on its own: the readahead
# pipeline and the two cores, with a generated track standing in for a decoded
# one. See README.md for what that covers and what it does not.
#
# To put the filters into Mixxx:  scripts/integrate.sh <mixxx-source-dir>
#
# Usage:
#   scripts/build.sh                 configure, build, verify
#   scripts/build.sh --clean         wipe the build tree first
#   scripts/build.sh --skip-tests    build only; not recommended
#
# Environment:
#   BUILD   where the build tree goes. build/ by default.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)     # plugins/mixxx
build="${BUILD:-$root/build}"

clean=0
skip_tests=0

for arg in "$@"; do
    case "$arg" in
        --clean)      clean=1 ;;
        --skip-tests) skip_tests=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v cmake >/dev/null 2>&1 || {
    echo "cmake was not found on PATH. Install CMake 3.16 or newer." >&2
    exit 1
}

[ "$clean" -eq 1 ] && rm -rf "$build"

echo "=== configure ==="
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release

echo "=== build ==="
cmake --build "$build" --parallel

if [ "$skip_tests" -eq 1 ]; then
    echo
    echo "built; tests skipped"
    exit 0
fi

echo "=== verify ==="
ctest --test-dir "$build" --output-on-failure

echo
echo "Next: scripts/integrate.sh <mixxx-source-dir>"
