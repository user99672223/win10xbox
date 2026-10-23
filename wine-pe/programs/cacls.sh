#!/usr/bin/env bash
# Builds Wine's programs/cacls as wine-pe/out/builtin/cacls.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_cacls (tests/wineprog/cacls.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/cacls "$@"
