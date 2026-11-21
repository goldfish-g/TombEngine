#!/bin/sh
# Builds the browser version of TombEngine inside the image of this folder's
# Dockerfile. Usage: build.sh <build dir> [Release|Debug]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:?build directory}
type=${2:-Release}
if [ ! -f "$build/build.ninja" ]; then
    emcmake cmake -S "$here" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE="$type" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
cmake --build "$build" -- -k 0
