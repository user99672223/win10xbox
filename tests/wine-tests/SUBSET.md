# Wine conformance subset: which tests and why

These are tests from Wine's own tree (`wine-pe/wine-src/dlls/<dll>/tests/*.c`,
Wine 11.9, LGPL-2.1+). They are **not** copied into this repository: the
CMake in this directory compiles them in place with llvm-mingw against Wine's
own headers (`wine-pe/wine-src/include`, plus `wine-pe/build/include` for the
`.idl`-generated ones) and links them against the toolchain's import libraries
and `wine_support.c` (our one-function replacement for `__wine_dbg_strdup`).
Each test becomes one guest PE, `bin/guest-wine/<dll>_<test>.exe`, run as
`win10xbox --machine <dir> C:\guest-wine\<dll>_<test>.exe`.

The subset is chosen to exercise what M2 implements (docs/STATUS.md): the
dispatcher's pass-through/translate/emulate paths, the VFS, the registry,
object handles, the virtual clock, NLS and process/thread bootstrap. Numbers
are in docs/TESTING.md ("Wine test subset"); the per-test baseline that the
ctest entries enforce is `baseline.cmake`.

| Test | M2 subsystem it exercises | Why |
|------|---------------------------|-----|
| ntdll/rtlstr, string, rtlbitmap, error, env | Rtl* forwarding (63 process-wide `Rtl*` routines go to the host ntdll), NLS (`NtInitializeNlsFiles`), PEB environment | Mostly pure computation: shows the Wine-ntdll-on-our-dispatcher base is sound. Control group: should be at 0 failures. |
| ntdll/path | VFS path translation (`RtlDosPathNameToNtPathName`, `\??\` handling) | Path conversion is where the machine-folder/guest-view rewrite can show. |
| ntdll/directory | VFS: `NtQueryDirectoryFile`, `NtOpenFile` translation | Directory enumeration and info classes over the machine folder. |
| ntdll/file | VFS + file syscalls (`NtCreateFile`, `NtQueryInformationFile`, ...) | Basic I/O semantics; needs a real `C:\windows\system32` tree. |
| ntdll/om | Object manager: `NtOpenDirectoryObject`, symbolic links, `NtQueryObject`, named objects | Handle/object syscalls are pass-through to the host; checks the namespace the guest sees. |
| ntdll/reg | Registry subsystem (own REGF reader/writer + write overlay): `NtCreateKey`, `NtEnumerateKey`, values | The ntdll-level registry API surface. |
| ntdll/info | `NtQuerySystemInformation`/`NtQueryInformationProcess`/`Thread` emulation | The broadest single check of the emulated information classes. |
| ntdll/time | Virtual clock (`NtQuerySystemTime`, `NtQueryPerformanceCounter`, timer resolution) | Clock subsystem + `KUSER_SHARED_DATA`. |
| kernel32/atom, codepage, format_msg | NLS data + Wine's kernel32 on our layer | Large, deterministic, syscall-light: a second control group. |
| kernel32/environ, path | PEB/process-parameters, `GetFullPathName` / short-name handling over the VFS | |
| kernel32/directory, file | VFS through kernel32: `CreateFile`/`FindFirstFile`/`GetTempPath`/... | The "file.c basics". |
| kernel32/heap | Process heap (`Rtl*Heap` forwarded to the host ntdll) | |
| kernel32/time, locale, version | Virtual clock (`GetSystemTime`, `GetTickCount`), NLS/locale data, OS version reporting (`RtlGetVersion`) | Version/locale results are what an unmodified app sees first. |
| kernel32/virtual | `NtAllocateVirtualMemory`/`NtProtectVirtualMemory`/sections (pass-through + `SEC_IMAGE` emulation) | |
| kernel32/sync, module | Waits/events/mutexes, APC queueing, loader data (`LdrGetDllHandle`, module list) | Sync is pass-through; module checks the loader's view of the PEB module lists. |
| advapi32/registry | Registry through advapi32 (`RegCreateKeyEx`, `RegSetValueEx`, ...) | The "registry.c subset"; needs the `.idl`-generated headers. |

## Selection process

All of the following were built and run once (about 50 binaries: all of
ntdll's `info om rtl rtlstr rtlbitmap string large_int path atom env error
time reg sync virtual file directory pipe thread change exception unwind
threadpool`, kernel32's `file heap environ atom path time virtual sync
directory format_msg profile version locale codepage module loader process
thread timer mailslot pipe volume drive fiber`, advapi32's `registry
security`). The subset keeps those that map to a named M2 subsystem, run in
a few seconds, and between them cover the passing, failing, crashing and
timing-out outcomes so a regression in any class is visible. Tests that
spawn child processes (`process`, `loader`, `exception` ...) or need
threads/timers heavily were left out of the first cut (they hit
`CreateProcess` and exception dispatch, which M2 does not claim).

## Expanding the subset

1. Add `win10xbox_wine_test(<dll> <test> run 9999)` to `baseline.cmake`
   (use `NEEDS_GEN` if the source includes an `.idl`-generated header, i.e.
   it fails to compile without `wine-pe/build/include`).
2. `cmake --build <bld>` then `ctest --test-dir <bld> -R wine_conformance.<dll>.<test> -V`.
3. Read the `WINE_RESULT` line, and replace the baseline with the measured
   outcome (`run <failures>`, `crash 0` or `timeout 0`); record it in
   docs/TESTING.md.

The `START_TEST(<name>)` in the Wine source must equal the file name (all
tests used here do); a test that needs a Wine-built helper DLL
(`testdll`, `rsrc.rc` resources, manifests) cannot be built this way.
