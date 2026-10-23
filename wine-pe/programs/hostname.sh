#!/usr/bin/env bash
# Builds Wine's programs/hostname as wine-pe/out/builtin/hostname.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_hostname (tests/wineprog/hostname.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/hostname "$@"
