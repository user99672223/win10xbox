#!/usr/bin/env bash
# Build Wine's programs/cmd (cmd.exe) as a PE x64 guest program and stage it.
#
#   wine-pe/programs/cmd.sh [--jobs N] [--machine DIR] [-h]
#
# Steps: (1) wine-pe/build.sh --only programs/cmd (fetches and configures Wine
# if needed; programs/cmd is listed in wine-pe/dlls.txt, so the CI artifact
# wine-pe-builtin carries cmd.exe too), (2) check that the result is a PE32+
# x86-64 console image, (3) with --machine, copy it to
# DIR/Windows/builtin/cmd.exe, which is where tests/integration/prepare_machine.cmake
# puts every builtin (the guest sees it as C:\Windows\builtin\cmd.exe).
#
# Output: wine-pe/out/builtin/cmd.exe. Exit status: 0 if cmd.exe was built and
# is PE x64, 1 otherwise.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="$(nproc 2>/dev/null || echo 4)"
MACHINE=""

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs|-j)   JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --jobs=*)    JOBS="${1#*=}"; shift ;;
        --machine)   MACHINE="${2:?--machine needs a directory}"; shift 2 ;;
        --machine=*) MACHINE="${1#*=}"; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "cmd.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

grep -qE '^[[:space:]]*programs/cmd[[:space:]]*(#.*)?$' "$HERE/dlls.txt" || {
    echo "cmd.sh: programs/cmd is not listed in $HERE/dlls.txt" >&2
    exit 1
}

# build.sh exits non-zero only for a core failure; cmd's own status is checked below.
"$HERE/build.sh" --jobs "$JOBS" --only programs/cmd

EXE="$HERE/out/builtin/cmd.exe"
if [[ ! -f "$EXE" ]]; then
    echo "cmd.sh: $EXE was not built (log: $HERE/build/logs/cmd.log)" >&2
    exit 1
fi

# PE32+ x86-64: e_lfanew at 0x3c, then "PE\0\0", Machine 0x8664, optional header magic 0x20b.
read_u16() { od -An -tu2 -j "$2" -N2 "$1" | tr -d ' '; }
read_u32() { od -An -tu4 -j "$2" -N4 "$1" | tr -d ' '; }
pe="$(read_u32 "$EXE" 60)"
sig="$(od -An -c -j "$pe" -N2 "$EXE" | tr -d ' ')"
machine="$(read_u16 "$EXE" $((pe + 4)))"
magic="$(read_u16 "$EXE" $((pe + 24)))"
subsystem="$(read_u16 "$EXE" $((pe + 24 + 68)))"
if [[ "$sig" != "PE" || "$machine" != 34404 || "$magic" != 523 ]]; then
    echo "cmd.sh: $EXE is not a PE32+ x86-64 image (sig=$sig machine=$machine magic=$magic)" >&2
    exit 1
fi
[[ "$subsystem" == 3 ]] || echo "cmd.sh: warning: subsystem $subsystem, expected 3 (console)" >&2
echo "cmd.sh: $EXE: PE32+ x86-64, subsystem $subsystem, $(stat -c %s "$EXE") bytes"

if [[ -n "$MACHINE" ]]; then
    mkdir -p "$MACHINE/Windows/builtin"
    cp -f "$EXE" "$MACHINE/Windows/builtin/cmd.exe"
    echo "cmd.sh: staged as $MACHINE/Windows/builtin/cmd.exe (C:\\Windows\\builtin\\cmd.exe)"
fi
