#!/usr/bin/env bash
# Builds Wine's programs/sort as wine-pe/out/builtin/sort.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_sort (tests/wineprog/sort.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/sort "$@"
