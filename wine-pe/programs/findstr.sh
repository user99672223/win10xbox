#!/usr/bin/env bash
# Builds Wine's programs/findstr as wine-pe/out/builtin/findstr.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_findstr (tests/wineprog/findstr.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/findstr "$@"
