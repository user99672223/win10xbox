#!/usr/bin/env bash
# Builds every Wine console program of wine-pe/programs/cli.txt as a PE x64 exe
# (wine-pe/out/builtin/<name>.exe) through wine-pe/build.sh. Extra arguments
# (--jobs N, --clean) are passed on. The programs are also in dlls.txt, so the
# wine-pe workflow ships them in the wine-pe-builtin artifact.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
list="$(grep -v '^#' "$HERE/cli.txt" | sed '/^$/d; s|^|programs/|' | paste -sd, -)"
exec "$HERE/../build.sh" --only "$list" "$@"
