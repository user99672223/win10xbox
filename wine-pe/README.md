# wine-pe: Wine's PE DLLs, built with llvm-mingw

win10xbox can use Wine's "builtin" DLLs (ntdll, kernelbase, kernel32, user32,
win32u, ...) as guest system DLLs wherever the user has not supplied the
genuine Microsoft ones. This directory contains the scripts and patches that
produce those DLLs from a pinned Wine tag. Nothing in here is Wine source:
the source is fetched on demand into `wine-src/` (gitignored).

## Quick start

```sh
wine-pe/fetch-wine.sh        # shallow-clone the pinned tag, apply patches/
wine-pe/build.sh             # configure (cached), build dlls.txt, stage to out/
wine-pe/build.sh --only ntdll,kernel32 --jobs 4
wine-pe/build.sh --clean     # wipe build/ and out/, then rebuild from scratch
```

Result: `wine-pe/out/builtin/<name>.dll|exe` (flat), `MANIFEST.txt` (name,
size, sha256, Wine tag) and the Wine license (`out/LICENSE.wine`, also copied
into `out/builtin/`). `build.sh` prints a built / failed table; the exit status
is non-zero only if a core module (ntdll, kernelbase, kernel32) fails.
Per-module logs: `wine-pe/build/logs/<name>.log`.

## How it works

* `fetch-wine.sh` holds the pin (`WINE_TAG="wine-11.9"`, mirrored in
  `third_party/PINS.md`). It clones `--depth 1 --branch $WINE_TAG` if
  `wine-src/` is absent and then applies `patches/*.patch`.
* `build.sh` configures Wine out of tree into `build/` once. The configure
  arguments, the compiler string and the Wine tag are recorded in
  `build/.win10xbox-configure-stamp`; a change triggers a reconfigure.
  The configuration is minimal (`--enable-win64 --enable-archs=x86_64
  --disable-tests --with-mingw`, everything needing a system library is
  `--without-*`). Wine's configure always configures the unix side too, which
  only needs the host `gcc`, `make`, `flex`, `bison` (and `gettext` for the
  `.mo` files). We never build or ship the unix side.
* The PE files of Wine 11.x land in
  `build/<dir>/x86_64-windows/<name>.dll` (programs: `<name>.exe`). They are
  built by naming that file as a make target, e.g.
  `make -C build dlls/ntdll/x86_64-windows/ntdll.dll`. `make` resolves all
  dependencies itself (winebuild/widl/wrc/..., import libraries, generated
  headers), so there is no separate `make tools` step and the unix side is
  not built. All modules are passed to a single `make -k -j N`; any missing
  output is then rebuilt serially to get its own log.
* The cross compiler is `x86_64-w64-mingw32-clang` from llvm-mingw, wrapped in
  ccache when available (`x86_64_CC="ccache x86_64-w64-mingw32-clang"` is the
  variable Wine's configure honours; `CROSSCC` does not exist in 11.x).
  `WINE_PE_NO_CCACHE=1` disables it. `LLVM_MINGW` (default `/opt/llvm-mingw`)
  selects the toolchain root.

## Toolchain pin

llvm-mingw `20250910`, UCRT, clang 21:
`https://github.com/mstorsjo/llvm-mingw/releases/download/20250910/llvm-mingw-20250910-ucrt-ubuntu-22.04-x86_64.tar.xz`
(same cache key scheme as `.github/workflows/ci.yml`).

Host packages (Debian/Ubuntu): `flex bison gettext ccache build-essential`.
Nothing else was needed on a stock image; configure is run with all optional
libraries disabled.

## Adding a DLL

Append a line to `dlls.txt`:

```
winhttp                    # dlls/winhttp  -> winhttp.dll
programs/cmd               # programs/cmd  -> cmd.exe
dlls/foo=bar.dll           # explicit output name when it differs
```

Run `build.sh --only winhttp`. If the module is vital for the layer, add it
to `CORE` in `build.sh` so its failure fails the build. Not-core modules
may fail without aborting the build.

## Patches

`patches/NNNN-description.patch` are applied in order to `wine-src/` with
`git apply`, idempotently (already-applied patches are skipped, a patch that
does not apply aborts). Create one by editing `wine-src/` and writing the
result of `git diff` into a new file (add a human readable header above the
`diff --git` line; `git apply` ignores it). Never write patches by hand, never
copy Wine code from memory. Keep patches minimal; every one is a rebase cost
when the pin moves.

Current patches:

| Patch | Why |
|-------|-----|
| `0001-keep-host-process-heap.patch` | `ntdll!loader_init` only creates a process heap if `PEB->ProcessHeap` is NULL. win10xbox runs Wine's ntdll in a host process that already owns the heap; heap calls are forwarded to the host ntdll. |
| `0002-honour-tick-count-multiplier.patch` | `GetTickCount(64)` (kernelbase, kernel32), `NtGetTickCount` and win32u's message clock apply `KUSER_SHARED_DATA.TickCountMultiplier`. On a Windows host the guest reads the host's page, where `TickCount` is in 15.625 ms ticks; Wine assumed milliseconds (its own multiplier is `1 << 24`, so Wine hosts are unaffected). |
| `0005-no-err-with-no-debug-msgs.patch` | `wine/debug.h` keeps `ERR` even under `WINE_NO_DEBUG_MSGS`; the builtins are built with both `WINE_NO_TRACE_MSGS` and `WINE_NO_DEBUG_MSGS` and must not carry any message text or `__wine_dbch_*` channel name. |
| `0006-export-wine-names-by-ordinal.patch` | Every `wine_*` / `__wine_*` export of the modules in `dlls.txt` is `-noname` (ordinal only; the numbers are written into the spec). The four places that looked such an export up by name now use its ordinal. Generated by `gen-nowine.py`. |
| `0007-windows-version-resources.patch` | File/product version 10.0.19045.3803, Microsoft product name, company and copyright for every module; FileDescription per module from `version_strings.txt`. Generated by `gen-nowine.py`. |
| `0008-neutral-internal-names.patch` | Thread names, a pipe name, the environment variables of the unix side, the registry resource type, the builtin-module signature and two CRT dialog strings lose the word "wine". Generated by `gen-nowine.py`. |

`gen-nowine.py` rebuilds 0006-0008 from `dlls.txt` and `version_strings.txt`
(needs a built `build/tools/winebuild/winebuild`); the checked-in patches are
what the build applies. What the builtins still say is listed in
`docs/slices/nowine.md`, and `tests/guest/nowine.c` (`integration.guest_nowine`)
checks the modules a process loads.

## Updating the Wine pin

Change `WINE_TAG` in `fetch-wine.sh` and the table in `third_party/PINS.md` in
the same commit, delete `wine-pe/wine-src`, run `fetch-wine.sh`; fix any patch
that no longer applies.

## CI

`.github/workflows/wine-pe.yml` (manual, on `wine-pe/**` changes, weekly)
runs the two scripts on ubuntu-latest and uploads `out/builtin/` as the
artifact `wine-pe-builtin` and `build/include/` (the widl-generated headers
some Wine tests need) as `wine-pe-include` (30 days each). Consumers fetch them
with `tools/ci/fetch_wine_pe.py`, which waits for a wine-pe run of the same
commit. Wine source, build tree and ccache are
cached, keyed on the Wine tag and a hash of the scripts, list and patches.

## Licensing

Wine is LGPL-2.1-or-later. The DLLs in `out/` are LGPL-2.1+ binaries and must
stay separate from our MIT code: they are loaded as separate modules, never
linked into `core/`, and distributed together with `LICENSE.wine` and a
pointer to the corresponding source (the pinned tag plus `patches/`). The
scripts and patches in this directory are the only files here that belong to
this repository; patches are derivative of Wine and thus LGPL-2.1+ as well.
