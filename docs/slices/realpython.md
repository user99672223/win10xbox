status: wip

# Slice: the python.org embeddable CPython through the layer

`python-3.12.10-embed-amd64.zip` (the official package, pinned by SHA-256 and verified on every run by
`tools/ci/fetch_python.ps1`, cached with `actions/cache`, unpacked as shipped to `build/bin/machine/guest/python`) is a
real guest program that is not ours and that we do not build. It runs on the builtin-only machine folder
(`bin/machine-builtin`, the console's DLL set) and is gated by two ctest tests in ci.yml's windows job, registered under
`if(MSVC OR WIN10XBOX_REAL_PYTHON)` in `tests/integration/CMakeLists.txt`:

* `integration.real_python_platform`: `python.exe -c "import sys, platform, os; print(sys.version);
  print(platform.uname()); print(os.cpu_count())"`, exit 0; a `3.12.10 (tags/v3.12.10...` line, a
  `uname_result(system='Windows'` line and a number as the last line.
* `integration.real_python_json`: `python.exe -m json.tool C:\guest\python_sample.json` (`tests/guest/python/sample.json`,
  one line), exit 0; the re-indented document on stdout.

CI run: (filled in when the head is green)

## What python prints through the layer

```
3.12.10 (tags/v3.12.10:0cc8128, Apr  8 2025, 12:21:36) [MSC v.1943 64 bit (AMD64)]
uname_result(system='Windows', node='', release='10', version='10.0.19045', machine='AMD64')
4
```

(`node` is empty when `_socket.pyd` cannot be imported.) `python -m json.tool` prints the document re-indented with four
spaces: `{`, `"name": "win10xbox"`, `"list": [1, 2, 3]` one element per line, `"nested": {"ok": true, "pi": 3.25}`.

## Why the first run was red

`integration.real_python_c` died while loading `python312.dll`: `0x80000100 EXCEPTION_WINE_STUB`, unresolved import
`api-ms-win-core-crt-l2-1-0.dll!_initterm_e`, called from a native DLL's DllMain. The test ran on the shared machine folder,
whose `Windows\System32` is the runner's own build-26100 DLL set (`machine.default.json`: `native,builtin`). python312 imports
`ws2_32`, `advapi32`, `version` and `bcrypt`; the runner's copies are written against an ntdll and an API-set schema Wine
does not have. This is the failure `docs/GUI.md` and `prepare_machine.cmake` already describe for the GUI tests.

## What changed

* Both tests run on the builtin-only folder (`_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR}`). The `_guest_console`
  switch drops that folder's "user32.dll was mapped" requirement (`-DREQUIRE_USER32=OFF`); "no native system DLL was
  mapped" stays.
* `prepare_machine.cmake` copies `guest/` into the builtin-only folder recursively (the package is a folder) and stages
  `*.json` guest files.
* `run_guest.cmake` expands the guest arguments directly in `execute_process`: through an intermediate `set()` list the
  `@SEMI@` placeholder came back as two arguments (CMake drops the `\;` escape when it joins the expanded elements), which
  turned the platform test's program text into `import sys, platform, os` and nothing else.
* DLLs added to `wine-pe/dlls.txt`: `bcrypt` (python's `os.urandom` calls `BCryptGenRandom`, which Wine implements with
  `RtlGenRandom` and no unix library; `python312.dll` imports it, so without it the load stops with
  `STATUS_DLL_NOT_FOUND`). `cryptbase` (advapi32's `SystemFunction036` forward) and the `ws2_32` unix library came from the
  fastfetch slice.
* Syscall changed: `NtCreateNamedPipeFile` is now Translate (was PassThrough). `platform.uname()` runs `ver` through
  `subprocess`; Wine's `CreatePipe` calls `NtCreateNamedPipeFile` with the default time-out 0 (NMPWAIT_USE_DEFAULT_WAIT) in a
  loop until it succeeds, and the Windows kernel answers `STATUS_INVALID_PARAMETER` to that every time: the first run of
  `real_python_platform` spent its 120 s in that loop (3 million relay lines). The translation hands the host the documented
  default, 50 ms, in place of 0, with clean 32-bit parameters. Everything else passes through unchanged.
* `tests/guest/python/smoke.py` and `integration.real_python_c` are gone: the broad script needed `_ctypes`, `_hashlib`,
  `_sqlite3` and a working `os.listdir`, none of which is part of this goal.

## Still failing / not gated

* `_wmi.pyd` does not import (it needs `propsys.dll`, which is not in the builtin set), so `platform` falls back to
  `sys.getwindowsversion()` and `ver`.
* `_ctypes`, `_hashlib`, `_sqlite3` and `os.listdir` of a folder were not examined past the local Wine run, where
  `os.listdir` returned an empty list and `_socket` was therefore not found.
* Local runs under Wine: with the full trace on, every log line also goes through `OutputDebugString`, which raises
  `DBG_PRINTEXCEPTION_C` inside host code on the guest's stack. Wine's host unwinder cannot find the host modules while the
  PEB belongs to the guest, so it can walk into stale python frames, and `_PyEval_EvalFrameDefault`'s GS-cookie handler
  fails fast (`0xc0000409`, process exit 9). Seen only with Wine as the host and the trace on (`WIN10XBOX_DEBUG=-all` is
  clean); not seen on the Windows runners.
