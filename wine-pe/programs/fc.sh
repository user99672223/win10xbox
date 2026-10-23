#!/usr/bin/env bash
# Builds Wine's programs/fc as wine-pe/out/builtin/fc.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_fc (tests/wineprog/fc.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/fc "$@"
