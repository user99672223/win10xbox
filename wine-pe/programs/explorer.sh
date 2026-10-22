#!/usr/bin/env bash
# Wine's explorer.exe (programs/explorer) as a PE x64 builtin, for
# `explorer.exe /desktop=shell,1280x720`: the virtual desktop window, the
# taskbar (systray.c) and the start menu (startmenu.c).
#
# Imports (programs/explorer/Makefile.in): rpcrt4 user32 gdi32 advapi32 win32u;
# delay imports: comctl32 shell32 oleaut32 ole32 shlwapi.
# Extra arguments go to wine-pe/build.sh (e.g. --jobs 4).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/build-program.sh" explorer \
    ntdll,kernelbase,kernel32,msvcrt,ucrtbase,rpcrt4,user32,gdi32,advapi32,win32u,sechost,comctl32,shell32,oleaut32,ole32,combase,shlwapi,imm32 \
    "$@"
