#!/usr/bin/env bash
# Builds Wine's programs/expand as wine-pe/out/builtin/expand.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_expand (tests/wineprog/expand.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/expand "$@"
