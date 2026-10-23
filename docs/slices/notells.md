status: blocked (superseded by Session A's `-nowine`, which covers the same scope and more; this branch is kept for its guest tests/guest/notells.c, the version-string patch and the guest-private \DosDevices in core/dispatch/nt_file.cpp, to be lifted into -nowine if Session A wants them; no further work here)

## Handoff (read this first)
1. Done and green: ci.yml run 39965132603 (all three jobs, code head c936a53), wine-pe.yml run 39961466147. Nothing left to fix; no CI loop needed.
2. Changed: wine-pe patches 0030 (Wine-only exports) and 0031 (version resources); core/dispatch/nt_file.cpp (guest-owned \DosDevices); tests/guest/notells.c = `integration.guest_notells`.
3. Untested on Windows / real hardware: the Xbox AppContainer (needs NtCreateDirectoryObject + NtCreateSymbolicLinkObject allowed; otherwise the old pass-through stays).
4. Local suite was NOT re-run here (llvm-mingw is present, Wine is not on this box at slice time); the linux-cross job is the same suite and passed.
5. Merging main into this branch conflicts (main moved after the last merge; no push was left): nt_file.cpp (keep BOTH blocks, my `\DosDevices` block, then `}` and a blank line, then main's "Reparse points" block; verified with -fsyntax-only), docs/SYSCALLS.md (run `python3 tools/gen_syscalls.py wine-pe/wine-src/dlls/ntdll/ntsyscalls.h core/dispatch/syscall_table.gen.inc core/dispatch/syscall_policy.inc docs/SYSCALLS.md`), host-uwp/CMakeLists.txt, host-uwp.yml, tests/guest/README.md, tests/integration/CMakeLists.txt (union of the guest lists: add guest_notells next to guest_regpersist/guest_cursor/guest_net).
6. host-uwp.yml is red on main too (guest_msctf_stub.exe, guest_net.exe missing from the .msix); guest_notells.exe is packaged.
7. Commands: `wine-pe/build.sh --only ntdll,kernelbase,kernel32,user32,shell32` (patches apply on a fresh wine-src); `ctest -R guest_notells` on a build with the builtin-only machine folder.
8. The doc-only head (ad9f006) re-ran ci.yml: run 39970808334 (windows RelWithDebInfo green when last looked).

# Slice: an ordinary program that looks for Wine does not find it

Goal: the checks a Windows program makes to find out whether it runs on Wine come back empty, measured by one gated
guest (`tests/guest/notells.c`, `integration.guest_notells` on the builtin-only machine folder), without breaking the
builtins. Scope is minimal: exports, version
resources, registry keys, environment, drives, OS version. Everything below is what this slice found and did.

## What changed

wine-pe patches (applied by `wine-pe/build.sh` like the others; wine-pe.yml rebuilds on `wine-pe/` changes):

* `0030-no-wine-exports.patch`: ntdll `wine_get_version`, `wine_get_build_id`, `wine_get_host_version` and kernel32
  `wine_get_unix_file_name`, `wine_get_dos_file_name` leave the `.spec` files (`-private` would keep them in the export
  table; only deleting the line removes the name). Importers among the modules of `dlls.txt` (found by grepping the sources
  and confirmed with the import tables of the built DLLs): only shell32 (`recyclebin.c`: `wine_get_host_version`,
  `wine_get_unix_file_name`, `wine_get_dos_file_name`; `shfldr_fs.c`: `wine_get_dos_file_name`). shell32 now answers "no unix
  counterpart" itself (`dos_to_unix_name` / `unix_to_dos_name` in `shell32_main.h`, `is_macos()` is FALSE): those paths need
  a unix host (the recycle bin's XDG trash directory, which needs `WINEHOMEDIR`, and `\\?\unix\` display names) and were
  already dead here. user32 (`SC_ABOUTWINE`) and shell32's About box look `wine_get_version` / `wine_get_build_id` up with
  `GetProcAddress` and test for NULL. Nothing in the layer calls these names on the guest's ntdll
  (`core/dispatch/process.cpp` and `nt_file.cpp` look `wine_get_version` up in the HOST's ntdll to detect a Wine host).
* `0031-neutral-version-info.patch`: the strings of the version resources (see the table).

Guest (`tests/guest/notells.c`) and registrations: `tests/guest/CMakeLists.txt`, `tests/integration/CMakeLists.txt`,
`host-uwp/CMakeLists.txt` (console foreach and `HOST_UWP_GUESTS`), `host-uwp.yml` (package list).

### Version resources: before (CI build of main) and after

`GetFileVersionInfoW` on System32\ntdll.dll / kernel32.dll / user32.dll. CompanyName was already "Microsoft Corporation"
(Wine's own comment in `ntverp.h.in`: GameGuard depends on it) and stays.

| field | before | after |
|---|---|---|
| ntdll FileDescription | `Wine ntdll` | `NT Layer DLL` |
| kernel32 FileDescription | `Wine kernel DLL` (and a Catalan variant, `DLL de nucli del Wine`, from the .po files) | `Windows NT BASE API Client DLL` |
| user32 FileDescription | `Wine user32` | `Multi-User Windows USER API Client DLL` |
| ProductName (all modules) | `Wine 11.9` | `Microsoft® Windows® Operating System` |
| LegalCopyright (all modules) | `Copyright (c) 1993-2026 the Wine project authors (see the file AUTHORS ...)` | `Licensed under the GNU Lesser General Public License, version 2.1 or later` |
| ntdll file/product version | 6.1.7601.24059 | 10.0.19045.3803 |
| user32 file/product version | 5.1.2600.2180 | 10.0.19045.3803 |
| kernel32 (and every module with no version of its own) | 10.0.19045.5796 | 10.0.19045.3803 |
| FileDescription of other dlls.txt modules | `Wine CRT library`, `Wine graphics library`, ... and the generated default `Wine <name>` | the same without "Wine " (msvcrt, ucrtbase, gdi32, bcrypt, comctl32, mpr, hid, winspool.drv, oleaut32, explorer); the generated default is the module name |

Decisions:

* The strings are product fields of our build, not data about the machine. They do not mention Wine anywhere (the gate
  searches the whole version block, in every translation, case-insensitively).
* 10.0.19045.3803: the OS the layer reports (`machine.default.json` `windows.build` 19045, `ubr` 3803; `RtlGetVersion`,
  the registry and `KUSER_SHARED_DATA` say the same), instead of Wine's own 5796. The numbers are compiled in; a machine.json with
  another `ubr` does not change them.
* LegalCopyright: not a Microsoft claim over LGPL code and not Wine's authors line (which names Wine). The license text and
  the source pointer ship next to the DLLs (`LICENSE.wine`).
* ProductName and CompanyName are what Windows reports for these files. The file version uses the build the layer
  virtualises (19045); it is not compared with a genuine 22H2 file.

## Registry and environment (no change needed, measured by the guest)

* Registry: `core/registry` has no `Software\Wine` key in its defaults and no hive is staged in the machine folder
  (`Windows\System32\config` is empty); the guest asserts `HKCU\Software\Wine`, `HKLM\Software\Wine` (default, 32-bit and
  64-bit view) and `HKCU\Software\Wine\Drivers` answer `ERROR_FILE_NOT_FOUND`.
* Environment: the guest's block is `DefaultEnvironment()` (a fixed Windows list); a host-supplied block and the child
  process path pass through `IsHostOwnedVariable` (`WIN10XBOX_*` and `WINE*` never reach a guest). The guest asserts no
  variable starts with `WINE` or `WIN10XBOX`.

## Drives and DOS devices (core/dispatch/nt_file.cpp changed)

Found while writing the gate: `\DosDevices` was passed through to the HOST's object manager, and Wine's kernelbase builds
`GetLogicalDrives` / `QueryDosDeviceW` on it (`NtOpenDirectoryObject("\DosDevices")` + `NtQueryDirectoryObject`). So a guest saw
the host's letters: a Wine host's `Z:` (the default Wine prefix, i.e. the linux-cross job), a runner's `D:`, and on a Windows
host, whose `\??` is the per-session directory, possibly no `C:` at all. The VFS already answers the file side with `C:` only
(`GetDriveTypeW(Z:\)`, `CreateFileW("\\.\Z:")` fail), so only the enumeration leaked.

Now the guest has a directory object of its own (`BuildDosDevicesLocked`): one symbolic link per VFS drive (`C:` ->
`\Device\HarddiskVolume1`, the numbering `vfs::TranslateNtPath` already resolves), created on first use and held for the
process's life. `NtOpenDirectoryObject` of `\DosDevices` / `\??` / `\GLOBAL??` returns a handle to it; `NtOpenSymbolicLinkObject`,
`NtCreateSymbolicLinkObject` and `NtMakeTemporaryObject` on `\DosDevices\X:` work inside it, so `DefineDosDevice` (create and
remove) keeps working (the link is not permanent, which needs a privilege; a handle of ours keeps it alive until
`NtMakeTemporaryObject`). Other names under `\DosDevices` (PIPE, NUL, ...) are opened as files, not as links, and still reach the
host. If the host refuses to create the directory or any link the old pass-through stays. Policy notes in
`syscall_policy.inc` / `docs/SYSCALLS.md` updated (NtCreateSymbolicLinkObject and NtMakeTemporaryObject are now Translate).

The guest asserts no `Z:` in `GetLogicalDrives`, `GetDriveTypeW`, `GetFileAttributesW`, `CreateFileW("\\.\Z:")`,
`QueryDosDeviceW("Z:")`; that `QueryDosDeviceW("C:")` is a `\Device\HarddiskVolumeN`; that `DefineDosDeviceW` makes `Q:`, it is
listed and queried, and removing it makes it vanish; and prints (not gated) the `QueryDosDeviceW(NULL)` list, failing only if a
name contains "wine".

## What still tells (not changed)

* `wine_server_call`, `wine_server_fd_to_handle`, `wine_server_handle_to_fd` stay in ntdll: `programs/conhost.exe` (in
  `dlls.txt`) imports `wine_server_call`, and Wine's own conformance tests detect Wine with
  `GetProcAddress(ntdll, "wine_server_call")` (`include/wine/test.h` `running_under_wine`), so removing it would flip every
  `todo_wine` in `wine-pe/tests/*.baseline`, `integration.winetest_*` and `tests/wine-tests`. The guest allows exactly these
  three (printed as `info allowed export`). They answer `STATUS_NOT_IMPLEMENTED` / `STATUS_NOT_SUPPORTED` (docs/DISPATCHER.md).
* The `__wine_*` exports of ntdll (and `__wine_ime_wnd_proc` of imm32) stay and are counted, not gated: kernel32, kernelbase,
  msvcrt and ucrtbase import `__wine_dbg_*`; the layer looks `__wine_unix_call_dispatcher` / `__wine_unixlib_handle` up by name;
  `__wine_syscall_dispatcher`, `__wine_unix_spawnvp`, `__wine_ctrl_routine` are Wine's own loader/console plumbing. They are reachable.
* The DOS stub of every builtin carries the string `Wine builtin DLL` (winebuild; Wine's PE `ntdll/loader.c` tests it), the
  `.debug_*` sections carry Wine source paths, `.buildid` is Wine's, and ntdll/kernel32 contain `wine: ...` messages and
  `EXCEPTION_WINE_*` names. The guest prints the DOS-stub marker as `info`.
* No Authenticode signature on any of the builtins (a program that verifies `WinVerifyTrust` on kernel32.dll sees an unsigned file).
* Other modules keep Wine's version numbers (shell32 6.0.2900.6242, msvcrt 7.0.2600.2180, advapi32 10.0.10240.16384, ...);
  kernelbase has no version resource at all (its Makefile.in sets no version variable).
* user32 still contains the strings `wine_get_version` and `Wine %s` (the dead `SC_ABOUTWINE` branch).
* Wine-specific behaviour outside this slice's scope (unimplemented-function messages, Wine's hex stubs 0x80000100, ...).

## Gate

`integration.guest_notells` (builtin-only machine folder, console guest): every check below, exit 42, `FAIL ` in the output fails it.
Builtin-only folder only: the guest imports advapi32 and loads version.dll and user32, and the shared folder maps the
runner's own build-26100 copies of those next to the builtins, which die on Wine's import stubs. A first shared-folder variant
(`integration.guest_notells_shared`) did exactly that (`EXCEPTION_WINE_STUB`, `api-ms-win-core-crt-l2-1-0._initterm_e`, exit
0x80000100, run 39958507943) and was removed rather than hollowed out: the checks that would still run there (exports of the three
core DLLs, registry, environment, drives) run identically here.

Checks: `getprocaddress` (the five names are NULL), `exports` (no `wine_` name in ntdll, kernelbase, kernel32, win32u, user32,
gdi32 except the three above), `version-ntdll` / `version-kernel32` / `version-user32` (10.0.19045, company, description,
product, internal / original name, no "wine" in the block), `registry`, `environment`, `drives` (including C: and the
DefineDosDevice round trip), `os-version` (`RtlGetVersion`, `GetVersionExW` with the Windows 10 manifest, `GetVersion`).

## Local build evidence (llvm-mingw, `wine-pe/build.sh --only ntdll,kernelbase,kernel32,user32,shell32,gdi32,msvcrt`)

The patched DLLs link (shell32 included) and carry the strings of the table; `objdump -p` of ntdll lists no `wine_get_*`,
kernel32 and user32 no `wine*` name at all. CI's run is the evidence that counts (below).

## Results

* ci.yml, all three jobs green on c936a53 (the code and test of this slice; the commit after it only changes this file):
  https://github.com/user99672223/win10xbox/actions/runs/39965132603 (windows Debug, windows RelWithDebInfo, linux-cross; the first
  linux-cross attempt died in the step "Install Wine + ninja", three apt-get timeouts, before any test ran, and was re-run unchanged).
  `integration.guest_notells` Passed in all three, "Gate - nothing skipped" passed (skip count 0).
* wine-pe.yml green with the patches: https://github.com/user99672223/win10xbox/actions/runs/39961466147 (commit 6023581, which is
  the last commit that touches `wine-pe/`; later commits change nothing there, so ci.yml takes these builtins) and run 39958507974 for
  the first push.
* host-uwp.yml (workflow_dispatch on this branch, run 39961794246): red, but not because of this slice: "Verify package contents"
  fails with `guest(s) missing from the .msix: guest_msctf_stub.exe, guest_net.exe`, the same error as main's own host-uwp run
  39948347965 (head of main at the time). `guest_notells.exe` is built, packaged and not in the missing list.

What `guest_notells` printed through the layer (windows Debug, `build/bin/guest_notells.log` of the artifact `windows-Debug-logs`;
the version strings of ntdll and kernel32 are the same shape as user32's, the printed `?` are the two (R) signs narrowed to ASCII):

```
notells: getprocaddress ok
notells: info allowed export ntdll.dll!wine_server_call
notells: info allowed export ntdll.dll!wine_server_fd_to_handle
notells: info allowed export ntdll.dll!wine_server_handle_to_fd
notells: info ntdll.dll: 2584 named exports, 0 wine_ (not allowed) 3 wine_ (allowed) 10 __wine_
notells: info kernelbase.dll: 1439 named exports, 0 wine_ (not allowed) 0 wine_ (allowed) 0 __wine_
notells: info kernel32.dll: 1356 named exports, 0 wine_ (not allowed) 0 wine_ (allowed) 0 __wine_
notells: info win32u.dll: 1540 named exports, 0 wine_ (not allowed) 0 wine_ (allowed) 0 __wine_
notells: info user32.dll: 832 named exports, 0 wine_ (not allowed) 0 wine_ (allowed) 0 __wine_
notells: info gdi32.dll: 531 named exports, 0 wine_ (not allowed) 0 wine_ (allowed) 0 __wine_
notells: info __wine_ exports in total: 10 (not gated)
notells: exports ok
notells: info ntdll.dll file 10.0.19045.3803 product 10.0.19045.3803
notells: version-ntdll ok
notells: info kernel32.dll file 10.0.19045.3803 product 10.0.19045.3803
notells: version-kernel32 ok
notells: info user32.dll file 10.0.19045.3803 product 10.0.19045.3803
notells: info user32.dll 040904b0 CompanyName=Microsoft Corporation
notells: info user32.dll 040904b0 FileDescription=Multi-User Windows USER API Client DLL
notells: info user32.dll 040904b0 LegalCopyright=Licensed under the GNU Lesser General Public License, version 2.1 or later
notells: info user32.dll 040904b0 ProductName=Microsoft? Windows? Operating System
notells: version-user32 ok
notells: info RegOpenKeyExW(HKCU\Software\Wine) = 2            (and the other four keys: 2 = ERROR_FILE_NOT_FOUND)
notells: registry ok
notells: info environment has 28 variables
notells: environment ok
notells: info logical drives: C
notells: info QueryDosDeviceW(C:) = \Device\HarddiskVolume1
notells: info QueryDosDeviceW(Q:) = \??\C:\Windows
notells: info 1 DOS device names: C:
notells: drives ok
notells: info RtlGetVersion 10.0.19045
notells: info GetVersionExW 10.0.19045
notells: os-version ok
notells: info ntdll.dll DOS stub marker "Wine builtin DLL": present
notells: done
```

Notes on those lines:

* ntdll shows 2584 named exports on a Windows host and 1462 on the Wine host (linux-cross): on a Windows host
  `ExtendGuestNtdllExports` (core/dispatch/rtl_forward.cpp) appends the host ntdll's code exports to the guest ntdll's table so
  native DLLs bind, and already skips host names starting with `__wine` / `wine_` (a Wine dev host).
* The first run on the Windows Debug job (before the fixes below) already showed the exports, versions, registry and
  environment right; what it caught: a one-character comparison bug in the guest, a static `version.lib` import that killed the
  shared-folder variant, and, on the linux-cross job, `Z:` coming from the Wine host (see Drives and DOS devices).

Syscalls changed: `NtOpenDirectoryObject` and `NtOpenSymbolicLinkObject` (Translate, more cases), `NtCreateSymbolicLinkObject` and
`NtMakeTemporaryObject` (PassThrough -> Translate), all for `\DosDevices` only; docs/SYSCALLS.md regenerated.
DLLs added to `wine-pe/dlls.txt`: none (patches 0030, 0031 only).

What still fails: nothing in this slice's gate. What still tells is the list above ("What still tells"): `wine_server_*`
exports, `__wine_*` exports, the DOS-stub marker and debug sections, no Authenticode signature, other modules' legacy version
numbers, no kernelbase version resource, the dead `SC_ABOUTWINE` strings in user32.
Not measured: the console (Xbox) host. The private `\DosDevices` directory needs `NtCreateDirectoryObject` and
`NtCreateSymbolicLinkObject` to be allowed in the AppContainer; if the host refuses either, the old pass-through stays (a
`\DosDevices: ...` warning is traced) and the guest sees the host's drive letters again.
