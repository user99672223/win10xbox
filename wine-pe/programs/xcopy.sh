#!/usr/bin/env bash
# Builds Wine's programs/xcopy as wine-pe/out/builtin/xcopy.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_xcopy (tests/wineprog/xcopy.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/xcopy "$@"
