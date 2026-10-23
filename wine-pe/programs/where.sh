#!/usr/bin/env bash
# Builds Wine's programs/where as wine-pe/out/builtin/where.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_where (tests/wineprog/where.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/where "$@"
