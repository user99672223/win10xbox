#!/usr/bin/env bash
# Builds Wine's programs/chcp as wine-pe/out/builtin/chcp.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_chcp (tests/wineprog/chcp.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/chcp "$@"
