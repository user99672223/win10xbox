#!/usr/bin/env bash
# Build Wine's programs/regedit as a PE x64 guest (wine-pe/out/builtin/regedit.exe).
# Arguments are passed to wine-pe/build.sh (e.g. --jobs 4). Run through the
# layer by ctest integration.wineprog_regedit.
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-program.sh" regedit "$@"
