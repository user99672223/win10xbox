status: wip

# Slice: an ordinary program that looks for Wine does not find it

Goal: the checks a Windows program makes to find out whether it runs on Wine come back empty, measured by one gated
guest (`tests/guest/notells.c`, `integration.guest_notells` on the builtin-only machine folder and
`integration.guest_notells_shared` on the shared one), without breaking the builtins. Scope is minimal: exports, version
resources, registry keys, environment, drives, OS version. Everything below is what this slice found and did.

## What changed

wine-pe patches (applied by `wine-pe/build.sh` like the others; wine-pe.yml rebuilds on `wine-pe/` changes):

* `0005-no-wine-exports.patch`: ntdll `wine_get_version`, `wine_get_build_id`, `wine_get_host_version` and kernel32
  `wine_get_unix_file_name`, `wine_get_dos_file_name` leave the `.spec` files (`-private` would keep them in the export
  table; only deleting the line removes the name). Importers among the modules of `dlls.txt` (found by grepping the sources
  and confirmed with the import tables of the built DLLs): only shell32 (`recyclebin.c`: `wine_get_host_version`,
  `wine_get_unix_file_name`, `wine_get_dos_file_name`; `shfldr_fs.c`: `wine_get_dos_file_name`). shell32 now answers "no unix
  counterpart" itself (`dos_to_unix_name` / `unix_to_dos_name` in `shell32_main.h`, `is_macos()` is FALSE): those paths need
  a unix host (the recycle bin's XDG trash directory, which needs `WINEHOMEDIR`, and `\\?\unix\` display names) and were
  already dead here. user32 (`SC_ABOUTWINE`) and shell32's About box look `wine_get_version` / `wine_get_build_id` up with
  `GetProcAddress` and test for NULL. Nothing in the layer calls these names on the guest's ntdll
  (`core/dispatch/process.cpp` and `nt_file.cpp` look `wine_get_version` up in the HOST's ntdll to detect a Wine host).
* `0006-neutral-version-info.patch`: the strings of the version resources (see the table).

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

## Registry, environment, drives (no change needed, measured by the guest)

* Registry: `core/registry` has no `Software\Wine` key in its defaults and no hive is staged in the machine folder
  (`Windows\System32\config` is empty); the guest asserts `HKCU\Software\Wine`, `HKLM\Software\Wine` (default, 32-bit and
  64-bit view) and `HKCU\Software\Wine\Drivers` answer `ERROR_FILE_NOT_FOUND`.
* Environment: the guest's block is `DefaultEnvironment()` (a fixed Windows list); a host-supplied block and the child
  process path pass through `IsHostOwnedVariable` (`WIN10XBOX_*` and `WINE*` never reach a guest). The guest asserts no
  variable starts with `WINE` or `WIN10XBOX`.
* Drives: the VFS has `C:` only. The guest asserts no `Z:` in `GetLogicalDrives`, `GetDriveTypeW`, `GetFileAttributesW`,
  `CreateFileW("\\.\Z:")`, `QueryDosDeviceW("Z:")`, and prints (not gated) the `QueryDosDeviceW(NULL)` list, failing only if
  a name contains "wine".

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

`integration.guest_notells` (builtin-only machine folder, console guest): every check below, exit 42.
`integration.guest_notells_shared` (shared folder, `--shared-folder`): getprocaddress, exports (ntdll, kernelbase, kernel32),
registry, environment, drives, os-version. The shared folder maps the runner's native DLLs next to the builtins, so the version
checks (the runner's own version.dll) and the user32/win32u/gdi32 load are not run there; the builtin-only folder is the console's DLL set.

Checks: `getprocaddress` (the five names are NULL), `exports` (no `wine_` name in ntdll, kernelbase, kernel32, win32u, user32,
gdi32 except the three above), `version-ntdll` / `version-kernel32` / `version-user32` (10.0.19045, company, description,
product, internal / original name, no "wine" in the block), `registry`, `environment`, `drives`, `os-version`
(`RtlGetVersion`, `GetVersionExW` with the Windows 10 manifest, `GetVersion`).

## Local build evidence (llvm-mingw, `wine-pe/build.sh --only ntdll,kernelbase,kernel32,user32,shell32,gdi32,msvcrt`)

The patched DLLs link (shell32 included) and carry the strings of the table; `objdump -p` of ntdll lists no `wine_get_*`,
kernel32 and user32 no `wine*` name at all. CI's run is the evidence that counts (below).

## Results

(filled in when ci.yml and wine-pe.yml are green)
