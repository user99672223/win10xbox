#!/usr/bin/env bash
# Builds Wine's programs/more as wine-pe/out/builtin/more.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_more (tests/wineprog/more.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/more "$@"
