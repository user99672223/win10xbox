#!/usr/bin/env bash
# Builds Wine's programs/attrib as wine-pe/out/builtin/attrib.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_attrib (tests/wineprog/attrib.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/attrib "$@"
