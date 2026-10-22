# Slice: the python.org embeddable CPython through the layer

Goal: `python-3.12.10-embed-amd64.zip` (pinned by SHA-256, verified on every run, cached with `actions/cache`, unpacked as
shipped to `build/bin/machine/guest/python` by `tools/ci/fetch_python.ps1`) runs on the GitHub Windows runners as two
gated ctest tests, `integration.real_python_platform` and `integration.real_python_json` (`tests/integration/CMakeLists.txt`,
registered under `if(MSVC OR WIN10XBOX_REAL_PYTHON)` like the Lua tests).

## Why the first run was red

`integration.real_python_c` died in `python312.dll`'s load: `0x80000100 EXCEPTION_WINE_STUB`, unresolved import
`api-ms-win-core-crt-l2-1-0.dll!_initterm_e`, called from a native DLL's DllMain. The tests ran on the shared machine
folder, whose `Windows\System32` is the runner's own build-26100 DLL set (`machine.default.json`: `native,builtin`). python312
imports `ws2_32`, `advapi32`, `version` and `bcrypt`; the runner's copies are written against an ntdll and an API-set
schema Wine does not have. This is the failure `docs/GUI.md` and `prepare_machine.cmake` already describe for the GUI tests.

## What changed

* Both tests run on the builtin-only machine folder (`_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR}`, the console's DLL
  set: Wine's builtins + NLS data, `"*": "builtin,native"`). The `_guest_console` switch drops the "user32.dll was
  mapped" requirement of that folder's trace check; the "no native system DLL was mapped" check stays.
* `prepare_machine.cmake` copies `guest/` into the builtin-only folder recursively (the package is a folder), and stages
  `*.json` guest files next to the `*.py` ones.
* `run_guest.cmake` expands `PROGRAM_ARGS` directly in `execute_process`: through an intermediate `set()` list an argument
  that holds a `;` (the program text of the platform test) was split.
* DLL supply: `wine-pe/dlls.txt` gains `bcrypt` (python's `os.urandom` calls `BCryptGenRandom`, which Wine implements with
  `RtlGenRandom` and no unix library) and `cryptbase` (Wine's advapi32 forwards `SystemFunction036` to it). The builtin set
  had neither, so `python312.dll` failed with `STATUS_DLL_NOT_FOUND` before anything ran.
* `core/dispatch/unix_calls.cpp` registers an empty `ws2_32` unix library next to ntdll's. Wine's ws2_32 `DllMain` returns
  FALSE without one (`__wine_init_unix_call`), which aborted the process (`c0000142`). The library carries only name
  resolution (`getaddrinfo`, `gethostby*`, ...); a call into it logs `unix call ws2_32:<n> not implemented` and returns
  `STATUS_NOT_IMPLEMENTED`. No syscall table or policy change.

## What python prints through the layer (Wine host, builtin-only machine)

```
3.12.10 (tags/v3.12.10:0cc8128, Apr  8 2025, 12:21:36) [MSC v.1943 64 bit (AMD64)]
uname_result(system='Windows', node='', release='10', version='10.0.19045', machine='AMD64')
4
```

`python -m json.tool` re-indents a one-line document. `node=''` because `_socket.pyd` does not load (see below).

## Known gaps (not gated)

* `NtCreateUserProcess` is unimplemented: `platform.uname()` tries `subprocess` (`ver`) once and falls back.
* `_socket`, `_ctypes`, `_hashlib`, `_sqlite3` extension modules do not import (their DLL imports are not all in the
  builtin set: iphlpapi, libcrypto/libffi/sqlite3 dependencies not examined). `os.listdir` of a directory returned `[]` in the
  retired broad smoke script. That script (`tests/guest/python/smoke.py`) was removed with the gate it fed.
* Local runs under Wine: with the full trace on, every log line also goes through `OutputDebugString`, which raises
  `DBG_PRINTEXCEPTION_C` inside host code on the guest's stack. Wine's host unwinder cannot find the host modules while the
  PEB belongs to the guest, so it can walk into stale python frames and the GS-cookie handler of
  `_PyEval_EvalFrameDefault` fails fast (`0xc0000409`, process exit 9). Only seen with Wine as the host and the trace on;
  `WIN10XBOX_DEBUG=-all` is clean.
