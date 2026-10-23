#!/usr/bin/env bash
# Build Wine's PE ("builtin") DLLs with llvm-mingw and stage them in
# wine-pe/out/builtin.
#
#   wine-pe/build.sh [--jobs N] [--only name1,name2] [--clean] [-h]
#
# Steps: (1) make sure wine-src exists and the patches are applied,
# (2) configure Wine once into wine-pe/build (cached, re-done only when the
# configure arguments or compiler change), (3) build every module listed in
# dlls.txt with one `make -k` run (falling back to a per-module run to get a
# separate log for each failure), (4) copy the PE files flat into
# out/builtin together with MANIFEST.txt and LICENSE.wine, (5) print a
# summary table. Re-runnable and incremental.
#
# Environment:
#   LLVM_MINGW          llvm-mingw root (default /opt/llvm-mingw)
#   WINE_PE_NO_CCACHE=1 do not wrap the cross compiler in ccache
#
# Exit status: 0 if the core modules (ntdll, kernelbase, kernel32) built,
# 1 otherwise. Failures of other modules are reported but not fatal.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/wine-src"
BUILD="$HERE/build"
OUT="$HERE/out"
LOGS="$BUILD/logs"
LIST="$HERE/dlls.txt"
PATCH_DIR="$HERE/patches"
CORE=(ntdll kernelbase kernel32)
ARCH_DIR="x86_64-windows"

JOBS="$(nproc 2>/dev/null || echo 4)"
ONLY=""
CLEAN=0

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs|-j) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --jobs=*)  JOBS="${1#*=}"; shift ;;
        --only)    ONLY="${2:?--only needs a comma separated list}"; shift 2 ;;
        --only=*)  ONLY="${1#*=}"; shift ;;
        --clean)   CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

now() { date +%s; }
T_START="$(now)"
declare -A T_PHASE

# ---------------------------------------------------------------- toolchain
LLVM_MINGW="${LLVM_MINGW:-/opt/llvm-mingw}"
if [[ -d "$LLVM_MINGW/bin" ]]; then
    export PATH="$LLVM_MINGW/bin:$PATH"
fi
if ! command -v x86_64-w64-mingw32-clang >/dev/null 2>&1; then
    echo "build.sh: x86_64-w64-mingw32-clang not found (set LLVM_MINGW)" >&2
    exit 1
fi
for tool in flex bison make gcc; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "build.sh: host tool '$tool' missing (apt-get install flex bison gettext build-essential)" >&2
        exit 1
    }
done

CROSS_CC="x86_64-w64-mingw32-clang"
if [[ "${WINE_PE_NO_CCACHE:-0}" != 1 ]] && command -v ccache >/dev/null 2>&1; then
    CROSS_CC="ccache $CROSS_CC"
fi

WINE_TAG="$(sed -n 's/^WINE_TAG="\(.*\)"$/\1/p' "$HERE/fetch-wine.sh")"
[[ -n "$WINE_TAG" ]] || { echo "build.sh: cannot read WINE_TAG from fetch-wine.sh" >&2; exit 1; }

# ------------------------------------------------------------ source + patches
T0="$(now)"
"$HERE/fetch-wine.sh"
# Belt and braces: fetch-wine.sh already applied them, verify.
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
    git -C "$SRC" apply --check --reverse "$p" >/dev/null 2>&1 || {
        echo "build.sh: patch $(basename "$p") is not applied to wine-src" >&2
        exit 1
    }
done
T_PHASE[fetch]=$(( $(now) - T0 ))

# ---------------------------------------------------------------- module list
# Parallel arrays: DIRS[i] = source/build dir, FILES[i] = output file name.
DIRS=(); FILES=()
while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    line="${line//[[:space:]]/}"
    [[ -n "$line" ]] || continue
    spec="${line%%=*}"
    file=""
    [[ "$line" == *=* ]] && file="${line#*=}"
    if [[ "$spec" == */* ]]; then dir="$spec"; else dir="dlls/$spec"; fi
    if [[ -z "$file" ]]; then
        base="$(basename "$dir")"
        if [[ "$dir" == programs/* ]]; then file="$base.exe"; else file="$base.dll"; fi
    fi
    if [[ -n "$ONLY" ]]; then
        match=0
        IFS=',' read -r -a want <<< "$ONLY"
        for w in "${want[@]}"; do
            [[ "$w" == "${file%.*}" || "$w" == "$file" || "$w" == "$spec" ]] && match=1
        done
        [[ $match -eq 1 ]] || continue
    fi
    DIRS+=("$dir"); FILES+=("$file")
done < "$LIST"
if [[ ${#DIRS[@]} -eq 0 ]]; then
    echo "build.sh: nothing to build (empty list or --only matched nothing)" >&2
    exit 1
fi

# ------------------------------------------------------------------ configure
# Minimal Wine: PE side only matters to us; the unix side must still
# configure. Anything that needs a system library is switched off.
CONFIGURE_ARGS=(
    --enable-win64 --enable-archs=x86_64 --disable-tests --with-mingw
    --without-x --without-freetype --without-fontconfig --without-gnutls
    --without-pulse --without-alsa --without-dbus --without-usb --without-udev
    --without-v4l2 --without-gstreamer --without-opengl --without-vulkan
    --without-sdl --without-cups --without-gphoto --without-krb5
    --without-netapi --without-pcap --without-sane --without-xinput2
    --without-wayland --without-capi --without-ffmpeg --without-gssapi
    --without-opencl --without-oss --without-pcsclite --without-coreaudio
    --without-hwloc
)
# The builtins carry no Wine debug machinery: TRACE/WARN/FIXME are compiled
# out by wine/debug.h itself (WINE_NO_TRACE_MSGS, WINE_NO_DEBUG_MSGS), and
# patches/0005 makes WINE_NO_DEBUG_MSGS silence ERR as well, so neither the
# message strings nor the __wine_dbch_* channel names reach the binaries.
# No -g: DWARF would carry source paths and Wine function names, and the
# prefix maps keep the checkout path out of __FILE__ (assert, critical-section
# names). These flags replace the "-g -O2" Wine's configure picks for the
# cross compiler.
PE_CFLAGS="-O2 -DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS -ffile-prefix-map=$SRC=. -ffile-prefix-map=$BUILD=."
STAMP="$BUILD/.win10xbox-configure-stamp"
WANT_STAMP="$WINE_TAG|$CROSS_CC|$PE_CFLAGS|${CONFIGURE_ARGS[*]}"

if [[ $CLEAN -eq 1 ]]; then
    echo "build.sh: --clean: removing $BUILD and $OUT"
    rm -rf "$BUILD" "$OUT"
fi
mkdir -p "$BUILD" "$LOGS"

T0="$(now)"
if [[ ! -f "$BUILD/Makefile" || "$(cat "$STAMP" 2>/dev/null || true)" != "$WANT_STAMP" ]]; then
    echo "build.sh: configuring Wine ($WINE_TAG), compiler: $CROSS_CC"
    (
        cd "$BUILD"
        x86_64_CC="$CROSS_CC" x86_64_CFLAGS="$PE_CFLAGS" "$SRC/configure" "${CONFIGURE_ARGS[@]}"
    ) > "$LOGS/configure.log" 2>&1 || {
        echo "build.sh: configure failed, tail of $LOGS/configure.log:" >&2
        tail -30 "$LOGS/configure.log" >&2
        exit 1
    }
    printf '%s' "$WANT_STAMP" > "$STAMP"
else
    echo "build.sh: configure cached"
fi
T_PHASE[configure]=$(( $(now) - T0 ))

# ----------------------------------------------------------------------- build
# Wine's top-level Makefile has one target per output file; make resolves
# every dependency (tools, import libs, generated headers) by itself.
T0="$(now)"
targets=()
for i in "${!DIRS[@]}"; do
    targets+=("${DIRS[$i]}/$ARCH_DIR/${FILES[$i]}")
done

echo "build.sh: building ${#targets[@]} module(s) with -j$JOBS"
make -C "$BUILD" -k -j"$JOBS" "${targets[@]}" > "$LOGS/make-all.log" 2>&1 || true

# Per-module pass: modules that already exist are a no-op, failed ones are
# rebuilt serially so each gets its own log with the real error.
declare -A STATUS
for i in "${!DIRS[@]}"; do
    f="${FILES[$i]}"
    t="${targets[$i]}"
    log="$LOGS/${f%.*}.log"
    if [[ -f "$BUILD/$t" ]] && make -C "$BUILD" -q "$t" >/dev/null 2>&1; then
        STATUS[$f]="built"
        rm -f "$log"
    elif make -C "$BUILD" "$t" > "$log" 2>&1 && [[ -f "$BUILD/$t" ]]; then
        STATUS[$f]="built"
    else
        STATUS[$f]="FAILED"
    fi
done
T_PHASE[build]=$(( $(now) - T0 ))

# ---------------------------------------------------------------------- stage
T0="$(now)"
mkdir -p "$OUT/builtin"
MANIFEST="$OUT/builtin/MANIFEST.txt"
{
    echo "# wine-pe builtin DLLs, wine tag $WINE_TAG, llvm-mingw PE x86_64"
    echo "# name size sha256 wine_tag"
} > "$MANIFEST.tmp"
for i in "${!DIRS[@]}"; do
    f="${FILES[$i]}"
    [[ "${STATUS[$f]}" == built ]] || continue
    cp -f "$BUILD/${targets[$i]}" "$OUT/builtin/$f"
    # The COFF symbol table is not mapped at run time but is part of the file:
    # it names every __wine_spec_* / wine_* symbol. Exports, relocations and
    # the .pdata unwind data do not depend on it.
    "$LLVM_MINGW/bin/llvm-strip" --strip-all "$OUT/builtin/$f"
done
# Manifest covers everything staged (also modules from earlier --only runs).
for f in "$OUT/builtin/"*.dll "$OUT/builtin/"*.exe; do
    [[ -f "$f" ]] || continue
    printf '%s %s %s %s\n' "$(basename "$f")" "$(stat -c %s "$f")" \
        "$(sha256sum "$f" | cut -d' ' -f1)" "$WINE_TAG" >> "$MANIFEST.tmp"
done
mv -f "$MANIFEST.tmp" "$MANIFEST"
cp -f "$SRC/LICENSE" "$OUT/LICENSE.wine"
# Also next to the DLLs so the uploaded artifact carries its license.
cp -f "$SRC/LICENSE" "$OUT/builtin/LICENSE.wine"
T_PHASE[stage]=$(( $(now) - T0 ))

# -------------------------------------------------------------------- summary
echo
printf '%-20s %-8s %10s  %s\n' MODULE STATUS SIZE NOTE
printf '%-20s %-8s %10s  %s\n' -------------------- -------- ---------- ----
nbuilt=0; nfailed=0; core_ok=1
for i in "${!DIRS[@]}"; do
    f="${FILES[$i]}"
    note=""
    size="-"
    if [[ "${STATUS[$f]}" == built ]]; then
        nbuilt=$((nbuilt + 1))
        size="$(stat -c %s "$BUILD/${targets[$i]}")"
    else
        nfailed=$((nfailed + 1))
        note="see $LOGS/${f%.*}.log"
        for c in "${CORE[@]}"; do
            [[ "$f" == "$c.dll" ]] && { core_ok=0; note="CORE FAILURE; $note"; }
        done
    fi
    printf '%-20s %-8s %10s  %s\n' "$f" "${STATUS[$f]}" "$size" "$note"
done
echo
echo "built: $nbuilt  failed: $nfailed  staged in: $OUT/builtin"
printf 'timings (s): fetch %s, configure %s, build %s, stage %s, total %s\n' \
    "${T_PHASE[fetch]}" "${T_PHASE[configure]}" "${T_PHASE[build]}" \
    "${T_PHASE[stage]}" "$(( $(now) - T_START ))"

if [[ $core_ok -ne 1 ]]; then
    echo "build.sh: a core module failed" >&2
    exit 1
fi
