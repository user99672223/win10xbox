#!/usr/bin/env bash
# Builds Wine's programs/choice as wine-pe/out/builtin/choice.exe (PE x64, llvm-mingw).
# Test: ctest -R integration.wineprog_choice (tests/wineprog/choice.cmake).
exec "$(dirname "${BASH_SOURCE[0]}")/../build.sh" --only programs/choice "$@"
