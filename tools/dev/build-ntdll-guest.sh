#!/usr/bin/env bash
# The ci.yml windows job's "Build Wine's ntdll conformance guest" step, for Git Bash on a laptop:
# run from the repository root by tools/dev/prepare-windows.ps1 with LLVM_MINGW_WIN set (Windows path of llvm-mingw).
set -euo pipefail
export LLVM_MINGW="$(cygpath -u "$LLVM_MINGW_WIN")"
wine-pe/fetch-wine.sh
wine-pe/tests/ntdll.sh build --out "${1:-build/winetest/ntdll}"
ls -l "${1:-build/winetest/ntdll}"
