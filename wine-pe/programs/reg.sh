#!/usr/bin/env bash
# Builds Wine's programs/reg as wine-pe/out/builtin/reg.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_reg (tests/wineprog/reg.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/reg "$@"
