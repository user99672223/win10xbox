#!/usr/bin/env bash
# Build one Wine program (programs/<name>) as a PE x64 guest executable with
# wine-pe/build.sh and check the result is an x86-64 PE.
#
#   wine-pe/programs/build-program.sh <name> [build.sh args...]
#
# The program and the DLLs it imports must be listed in wine-pe/dlls.txt (so
# the wine-pe workflow's artifact carries them too). Output:
# wine-pe/out/builtin/<name>.exe. Exit status non-zero if it was not built or
# is not a PE32+ x86-64 image.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="${1:?usage: build-program.sh <name> [build.sh args...]}"
shift
# The program's own imports that are not core (build them in the same run so
# a fresh tree has them; build.sh skips what is already up to date).
case "$NAME" in
    regedit) DEPS="shcore,comctl32,comdlg32,shell32,shlwapi,advapi32,user32,gdi32" ;;
    taskmgr) DEPS="comctl32,shell32,shlwapi,advapi32,user32,gdi32" ;;
    oleview) DEPS="comctl32,comdlg32,shell32,shlwapi,ole32,oleaut32,combase,advapi32,user32,gdi32" ;;
    view)    DEPS="comdlg32,comctl32,user32,gdi32" ;;
    *)       DEPS="" ;;
esac
ONLY="programs/$NAME${DEPS:+,$DEPS}"

"$HERE/../build.sh" --only "$ONLY" "$@"

EXE="$HERE/../out/builtin/$NAME.exe"
[[ -f "$EXE" ]] || { echo "build-program.sh: $EXE was not built (see wine-pe/build/logs/$NAME.log)" >&2; exit 1; }
# PE signature offset at 0x3c, then "PE\0\0" and Machine 0x8664 (AMD64).
off=$(od -An -tu4 -j60 -N4 "$EXE" | tr -d ' ')
sig=$(od -An -tx1 -j"$off" -N6 "$EXE" | tr -d ' \n')
if [[ "$sig" != "504500006486" ]]; then
    echo "build-program.sh: $EXE is not a PE x86-64 image (signature+machine: $sig)" >&2
    exit 1
fi
echo "build-program.sh: $NAME.exe is PE x64 ($(stat -c %s "$EXE") bytes)"
