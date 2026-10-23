#!/usr/bin/env bash
# Builds Wine's programs/icacls as wine-pe/out/builtin/icacls.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_icacls (tests/wineprog/icacls.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/icacls "$@"
