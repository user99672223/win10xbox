#!/usr/bin/env bash
# Builds Wine's programs/timeout as wine-pe/out/builtin/timeout.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_timeout (tests/wineprog/timeout.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/timeout "$@"
