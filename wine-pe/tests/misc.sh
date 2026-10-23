#!/usr/bin/env bash
# Build the Wine conformance tests of one small DLL (dlls/<dll>/tests: psapi userenv
# cabinet xmllite mpr bcrypt setupapi dbghelp wintrust ncrypt crypt32 msxml3) as one
# PE x64 guest and run its units through the layer (win10xbox.exe). Generic over the
# DLL, the same way wine-pe/tests/advapi32.sh does advapi32.
#
#   wine-pe/tests/misc.sh build --dll <dll> --out <dir> [--gen-include <dir>]
#   wine-pe/tests/misc.sh run   --dll <dll> --out <dir> --host <win10xbox.exe> --machine <dir>
#                                   [--emulator <wine>] [--units "<unit> ..."]
#                                   [--timeout <s>] [--baseline <file>]
#   wine-pe/tests/misc.sh all   (build, then run; takes the options of both)
#
# Units default to every .c in the suite's Makefile.in SOURCES. Link libraries: the
# suite's IMPORTS mapped to -l<lib> plus the standard set. .rc sources are compiled
# with windres when available; .idl sources need widl (WIDL or PATH), otherwise the
# build skips (exit 77) when a source includes the header it would generate.
# build: compiles the <dll> test sources of wine-pe/wine-src (fetch-wine.sh;
#   LGPL, never copied into this repository) plus a generated test list into
#   <out>/<dll>_test.exe with llvm-mingw, the same way tests/wine-tests does
#   (Wine's headers, STANDALONE test.h). Needs Wine's widl-generated headers
#   (wine-pe/build/include, made by wine-pe/build.sh or the wine-pe-include CI
#   artifact) because registry.c includes them.
# run: copies the guest to <machine>/guest-wine/ and runs
#   `C:\guest-wine\<dll>_test.exe <unit>` once per unit with a per-unit cap.
#   Wine's harness ends a unit with "N tests executed (T marked as todo, F as
#   flaky, K failures), S skipped."; no such line = crashed, exit 124 = timeout.
#   One line per unit goes to <out>/<dll>.tsv and to stdout as
#   "WINETEST <dll>:<unit> status=... executed=... failures=...".
#
# Gate (the baseline file, default wine-pe/tests/<dll>.baseline, lines
# "<unit> <run|crash|timeout> <max_failures>"):
#   run    the unit must complete with failures <= max (fewer prints IMPROVED)
#   crash|timeout  must not complete; completing prints IMPROVED (record it)
#   and at least one assertion must have executed over all units.
# Exit: 0 baseline holds, 1 regression, 2 usage, 77 prerequisites missing
# (the Wine sources, the builtins or the NLS data): ctest SKIP.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="${WINE_SRC:-$ROOT/wine-pe/wine-src}"
LLVM_MINGW="${LLVM_MINGW:-/opt/llvm-mingw}"
[[ -d "$LLVM_MINGW/bin" ]] && export PATH="$LLVM_MINGW/bin:$PATH"

MODE="${1:-}"; shift || true
DLL=""; OUT=""; GEN="$ROOT/wine-pe/build/include"; HOST=""; MACHINE=""; EMULATOR=""
UNITS=""; TIMEOUT=30; BASELINE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dll) DLL="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --gen-include) GEN="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --machine) MACHINE="$2"; shift 2 ;;
        --emulator) EMULATOR="$2"; shift 2 ;;
        --units) UNITS="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --baseline) BASELINE="$2"; shift 2 ;;
        *) echo "misc.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[[ -n "$OUT" && -n "$DLL" ]] || { echo "misc.sh: --dll and --out are required" >&2; exit 2; }
[[ -n "$BASELINE" ]] || BASELINE="$HERE/$DLL.baseline"
mkdir -p "$OUT"
EXE="$OUT/${DLL}_test.exe"
TDIR="$SRC/dlls/$DLL/tests"
sources() { sed -n '/^SOURCES/,/^$/p' "$TDIR/Makefile.in" | tr -d '\\' | sed 's/^SOURCES[ \t]*=//' | tr ' \t' '\n\n' | grep -E '\.(c|rc|idl)$'; }
unit_names() { sources | grep '\.c$' | sed 's/\.c$//'; }

skip() { echo "SKIP: $*"; exit 77; }

do_build() {
    local tdir="$TDIR"
    [[ -f "$SRC/include/wine/test.h" && -d "$tdir" ]] || skip "$SRC missing (run wine-pe/fetch-wine.sh)"
    [[ -f "$GEN/unknwn.h" ]] || skip "$GEN/unknwn.h missing (Wine's generated headers: wine-pe/build.sh or the wine-pe-include artifact)"
    command -v x86_64-w64-mingw32-clang >/dev/null || skip "x86_64-w64-mingw32-clang not found (llvm-mingw)"
    local cc=x86_64-w64-mingw32-clang
    local res; res="$($cc -print-resource-dir)"
    # Every test source the upstream Makefile lists, so the binary is Wine's <dll>_test.exe.
    local srcs=() names=() objs=() f n
    for f in $(sources); do
        case "$f" in
            *.c) srcs+=("$tdir/$f"); names+=("${f%.c}") ;;
            *.rc)
                local wr; wr="$(command -v x86_64-w64-mingw32-windres || command -v llvm-windres || true)"
                if [[ -n "$wr" ]]; then
                    "$wr" -I"$tdir" -I"$SRC/include" -O coff -i "$tdir/$f" -o "$OUT/${f%.rc}_rc.o" \
                        && objs+=("$OUT/${f%.rc}_rc.o") || echo "misc.sh: windres failed for $f (resources skipped)" >&2
                else echo "misc.sh: no windres, $f skipped" >&2; fi ;;
            *.idl)
                local widl="${WIDL:-$(command -v widl || true)}"
                if [[ -n "$widl" ]]; then
                    mkdir -p "$OUT/gen"
                    "$widl" -I"$SRC/include" -I"$tdir" -h -o "$OUT/gen/${f%.idl}.h" "$tdir/$f" \
                        || echo "misc.sh: widl failed for $f" >&2
                fi ;;
        esac
    done
    [[ ${#srcs[@]} -gt 0 ]] || { echo "misc.sh: no SOURCES in $tdir/Makefile.in" >&2; exit 1; }
    local idl
    for idl in $(sources | grep '\.idl$' | sed 's/\.idl$//'); do
        [[ -f "$OUT/gen/$idl.h" || -f "$GEN/$idl.h" ]] || skip "$idl.h needs widl (not on PATH; set WIDL)"
    done
    local libs=() l
    for l in $(sed -n '/^IMPORTS/,/^$/p' "$tdir/Makefile.in" | tr -d '\\' | sed 's/^IMPORTS[ \t]*=//'); do
        [[ "$l" == \$* ]] || libs+=("-l$l")
    done
    local tl="$OUT/${DLL}_testlist.c"
    {
        echo "/* generated by wine-pe/tests/misc.sh */"
        echo "#define WIN32_LEAN_AND_MEAN"
        echo "#include <windows.h>"
        echo "#define STANDALONE"
        echo "#include \"wine/test.h\""
        for n in "${names[@]}"; do echo "extern void func_$n(void);"; done
        echo "const struct test winetest_testlist[] = {"
        for n in "${names[@]}"; do echo "    { \"$n\", func_$n },"; done
        echo "    { 0, 0 }"
        echo "};"
    } > "$tl"
    local flags=(-std=gnu17 -O1 -g0 -w -nostdinc -isystem "$res/include"
        -I"$SRC/include" -I"$GEN" -I"$OUT/gen" -I"$tdir" -I"$SRC/include/msvcrt"
        -D__WINESRC__ -D_NTSYSTEM_ -DWINE_NO_LONG_TYPES -D_UCRT)
    echo "misc.sh: $DLL: compiling ${#srcs[@]} test sources (${names[*]})"
    "$cc" "${flags[@]}" "${srcs[@]}" "$tl" "${objs[@]}" "$ROOT/tests/wine-tests/wine_support.c" -o "$EXE" \
        "${libs[@]}" \
        -lntdll -lkernel32 -luser32 -ladvapi32 -lgdi32 -lshell32 -lole32 -loleaut32 -lversion -lws2_32 -lshlwapi \
        -luuid -lwinmm -lsecur32 -lrpcrt4 -lcrypt32 -lbcrypt -lncrypt -lwintrust -lsetupapi -lcabinet -lcfgmgr32 \
        -lcomctl32 -lcomdlg32 -lnetapi32 -lmpr -lpsapi -luserenv -lxmllite -ldbghelp -limagehlp -lurlmon -lwininet \
        || { echo "misc.sh: $DLL: build failed" >&2; exit 1; }
    echo "misc.sh: built $EXE"
}

baseline_of() {   # <unit> -> "<expect> <max>"
    [[ -f "$BASELINE" ]] || { echo "run 999999"; return; }
    awk -v u="$1" '$1 == u { print $2, $3; found = 1 } END { if (!found) print "run 999999" }' "$BASELINE"
}

do_run() {
    [[ -n "$HOST" && -n "$MACHINE" ]] || { echo "misc.sh: run needs --host and --machine" >&2; exit 2; }
    [[ -f "$EXE" ]] || skip "$EXE missing (build step skipped or failed)"
    [[ -f "$MACHINE/Windows/builtin/ntdll.dll" ]] || skip "$MACHINE/Windows/builtin/ntdll.dll missing (Wine PE builtins)"
    [[ -f "$MACHINE/Windows/System32/c_1252.nls" ]] || skip "$MACHINE/Windows/System32/c_1252.nls missing (NLS data)"
    mkdir -p "$MACHINE/guest-wine"
    cp -f "$EXE" "$MACHINE/guest-wine/${DLL}_test.exe"
    local wmachine="$MACHINE"
    if [[ -n "$EMULATOR" && "$wmachine" == /* ]]; then wmachine="Z:$wmachine"; fi
    wmachine="${wmachine//\//\\}"
    export WIN10XBOX_DEBUG="${WIN10XBOX_DEBUG_SPEC:--all}"
    local tsv="$OUT/$DLL.tsv" total=0 bad=0
    : > "$tsv"
    local re='([0-9]+) tests executed \(([0-9]+) marked as todo, ([0-9]+) as flaky, ([0-9]+) failures?\), ([0-9]+) skipped'
    local unit
    [[ -n "$UNITS" ]] || UNITS="$(unit_names | tr '\n' ' ')"
    for unit in $UNITS; do
        local log="$OUT/${DLL}_$unit.log" t0 rc status=crashed ex=0 todo=0 fl=0 fails=0 sk=0 last=""
        t0=$(date +%s)
        timeout -k 5 "$TIMEOUT" $EMULATOR "$HOST" --machine "$wmachine" "C:\\guest-wine\\${DLL}_test.exe" "$unit" \
            < /dev/null > "$log" 2>&1
        rc=$?
        local secs=$(( $(date +%s) - t0 ))
        local summary; summary="$(grep -aE "^[0-9a-f]+:$unit:.*$re" "$log" | tail -1)"
        [[ -z "$summary" ]] && summary="$(grep -aE "$re" "$log" | tail -1)"
        if [[ $rc -eq 124 || $rc -eq 137 ]]; then
            status=timeout
        elif [[ -n "$summary" && "$summary" =~ $re ]]; then
            status=ok; ex=${BASH_REMATCH[1]}; todo=${BASH_REMATCH[2]}; fl=${BASH_REMATCH[3]}
            fails=${BASH_REMATCH[4]}; sk=${BASH_REMATCH[5]}
        fi
        if [[ $status != ok ]]; then
            last="$(grep -aoE '[A-Za-z0-9_]+\.c:[0-9]+:[^\r]*' "$log" | tail -1 | cut -c1-160)"
        fi
        # Executed counts survive a crash: Wine prints them only at the end, so a
        # crashed unit contributes 0 to the total.
        total=$(( total + ex ))
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$unit" "$status" "$ex" "$todo" "$fl" "$fails" "$sk" \
            "$rc" "$secs" "$last" >> "$tsv"
        echo "WINETEST $DLL:$unit status=$status executed=$ex todo=$todo flaky=$fl failures=$fails skipped=$sk exit=$rc time=${secs}s"
        [[ -n "$last" ]] && echo "  last test line: $last"
        grep -a '^win10xbox: ' "$log" | head -3 | sed 's/^/  /'
        read -r expect max <<<"$(baseline_of "$unit")"
        if [[ $expect == run ]]; then
            if [[ $status != ok ]]; then
                echo "  REGRESSION: baseline is 'completes', now $status"; bad=1
            elif (( fails > max )); then
                echo "  REGRESSION: $fails failures, baseline allows $max"; bad=1
            elif (( fails < max )); then
                echo "  IMPROVED: $fails failures < baseline $max; lower $BASELINE"
            fi
        elif [[ $status == ok ]]; then
            echo "  IMPROVED: now completes ($ex executed, $fails failures), baseline is '$expect'; update $BASELINE"
        else
            echo "  XFAIL: $status as recorded"
        fi
    done
    echo "WINETEST $DLL total executed=$total"
    if (( total < 1 )); then echo "FAIL: no assertion executed in any unit"; exit 1; fi
    (( bad == 0 )) || { echo "FAIL: $DLL conformance below baseline (logs in $OUT)"; exit 1; }
    echo "PASS (baseline holds)"
}

case "$MODE" in
    build) do_build ;;
    run) do_run ;;
    all) do_build; do_run ;;
    *) sed -n '2,/^set -uo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
