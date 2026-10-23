#!/usr/bin/env bash
# Fetch the pinned Wine source into wine-pe/wine-src and apply our patches.
#
#   wine-pe/fetch-wine.sh
#
# * Clones WINE_TAG shallowly if wine-src does not exist yet.
# * Applies wine-pe/patches/*.patch (sorted by name) with `git apply`.
#   Idempotent: a patch that is already applied is skipped, a patch that
#   neither applies nor is already applied aborts with an error.
#
# The pin lives here and in third_party/PINS.md; change both in one commit.
set -euo pipefail

WINE_TAG="wine-11.9"
WINE_URL="https://github.com/wine-mirror/wine"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/wine-src"
PATCH_DIR="$HERE/patches"

if [[ ! -d "$SRC/.git" ]]; then
    echo "fetch-wine: cloning $WINE_URL @ $WINE_TAG (shallow) into $SRC"
    rm -rf "$SRC"
    git clone --depth 1 --branch "$WINE_TAG" "$WINE_URL" "$SRC"
else
    # Warn (do not fail) if the checkout is not at the pinned tag; a local
    # tree with applied patches is expected to be dirty, so only the tag is
    # compared.
    have="$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || echo unknown)"
    if [[ "$have" != "$WINE_TAG" ]]; then
        echo "fetch-wine: WARNING wine-src is at '$have', pin is '$WINE_TAG'" >&2
        echo "fetch-wine:         delete wine-pe/wine-src to re-fetch" >&2
    fi
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
for p in "${patches[@]}"; do
    name="$(basename "$p")"
    if git -C "$SRC" apply --check --reverse "$p" >/dev/null 2>&1; then
        echo "fetch-wine: $name already applied"
    elif git -C "$SRC" apply --check "$p" >/dev/null 2>&1; then
        git -C "$SRC" apply "$p"
        echo "fetch-wine: applied $name"
    else
        # A restored cache can hold an older revision of a patch. Reset the tracked files to the tag and
        # reapply the whole series in order.
        echo "fetch-wine: $name neither applies nor is applied; resetting wine-src to $WINE_TAG and reapplying" >&2
        git -C "$SRC" checkout -- .
        for q in "${patches[@]}"; do
            git -C "$SRC" apply "$q" || { echo "fetch-wine: ERROR $(basename "$q") does not apply to $WINE_TAG" >&2; exit 1; }
            echo "fetch-wine: applied $(basename "$q")"
        done
        break
    fi
done
echo "fetch-wine: $WINE_TAG ready, ${#patches[@]} patch(es) in place"
