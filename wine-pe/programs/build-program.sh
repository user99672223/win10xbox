#!/usr/bin/env bash
# Build one Wine program (programs/<name>) as a PE x64 builtin, together with
# the builtin DLLs it imports, and stage it into wine-pe/out/builtin.
#
#   wine-pe/programs/build-program.sh <name> <dep>[,<dep>...] [build.sh args]
#
# The program and its dependencies must be listed in wine-pe/dlls.txt (the
# wine-pe CI artifact is built from that list, so the integration tests get the
# same files). This is a thin wrapper over wine-pe/build.sh --only; it adds a
# check that the program really came out (build.sh only fails on the core
# DLLs) and prints what the program imports, from the PE, so a missing builtin
# shows here rather than as a loader error in the guest.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WINE_PE="$(dirname "$HERE")"
NAME="${1:?usage: build-program.sh <name> <deps> [build.sh args]}"
DEPS="${2:?usage: build-program.sh <name> <deps> [build.sh args]}"
shift 2

grep -Eq "^[[:space:]]*programs/$NAME([[:space:]#]|$)" "$WINE_PE/dlls.txt" || {
    echo "build-program.sh: programs/$NAME is not in wine-pe/dlls.txt" >&2
    exit 1
}

"$WINE_PE/build.sh" --only "$NAME,$DEPS" "$@"

OUT="$WINE_PE/out/builtin/$NAME.exe"
if [[ ! -f "$OUT" ]]; then
    echo "build-program.sh: $NAME.exe was not built (log: wine-pe/build/logs/$NAME.log)" >&2
    exit 1
fi

LLVM_MINGW="${LLVM_MINGW:-/opt/llvm-mingw}"
OBJDUMP="$LLVM_MINGW/bin/llvm-objdump"
if [[ -x "$OBJDUMP" ]]; then
    missing=0
    while read -r dll; do
        lower="$(echo "$dll" | tr 'A-Z' 'a-z')"
        if [[ -f "$WINE_PE/out/builtin/$lower" ]]; then
            echo "  import $lower: staged"
        else
            echo "  import $lower: MISSING from wine-pe/out/builtin"
            missing=1
        fi
    done < <("$OBJDUMP" -p "$OUT" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u)
    [[ $missing -eq 0 ]] || { echo "build-program.sh: $NAME.exe imports a DLL that is not staged" >&2; exit 1; }
fi
echo "build-program.sh: $OUT ($(stat -c %s "$OUT") bytes)"
