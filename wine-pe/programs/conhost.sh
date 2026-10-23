#!/usr/bin/env bash
# Wine's conhost.exe (programs/conhost) as a PE x64 builtin. Evaluated as a
# console host against win10xbox's own console path (docs/DECISIONS.md).
#
# Imports (programs/conhost/Makefile.in): user32 gdi32 advapi32; delay
# import: comctl32. Extra arguments go to wine-pe/build.sh (e.g. --jobs 4).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/build-program.sh" conhost \
    ntdll,kernelbase,kernel32,msvcrt,ucrtbase,user32,gdi32,advapi32,win32u,sechost,comctl32,imm32 \
    "$@"
