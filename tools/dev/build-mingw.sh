#!/usr/bin/env bash
# Local Linux iteration: cross-compile with llvm-mingw and (optionally) smoke
# test under Wine. Never treat Wine-on-Linux results as truth for the layer.
#   tools/dev/build-mingw.sh            configure+build into build-mingw/
#   tools/dev/build-mingw.sh test       ...and run ctest under Wine
set -euo pipefail
cd "$(dirname "$0")/../.."
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DWIN10XBOX_BUILD_UWP=OFF "${CMAKE_EXTRA_ARGS[@]:-}"
cmake --build build-mingw
if [[ "${1:-}" == "test" ]]; then
  export WINEDEBUG=-all WINEPREFIX="${WINEPREFIX:-$HOME/.wine-win10xbox}"
  ctest --test-dir build-mingw --output-on-failure
fi
