#!/usr/bin/env bash
# Builds Wine's programs/whoami as wine-pe/out/builtin/whoami.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_whoami (tests/wineprog/whoami.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/whoami "$@"
