status: ready

# Slice wt-ntdll-file: Wine's ntdll `file` unit through the layer

Start (docs/slices/wt-ntdll.md): 2854 assertions, 281 failures on the Windows runners. The last full log before the
slice (ci.yml run 39895037227, `windows-Debug-logs/winetest/ntdll/results/file.log`) printed 157 failures: 107 of
them "Test succeeded inside todo block" and 50 real ones (48 distinct lines).

## What the 157 were, and what this slice does about each group

* 107 x "Test succeeded inside todo block". Wine's harness takes the guest for Wine (its ntdll exports
  `wine_get_version`), so every `todo_wine` block expects a failure, and every `broken()` is false; the host is Windows
  and its NTFS passes them. That counts as a failure and can never be fixed from the layer. It also made the seven
  "expected STATUS_PENDING" lines (file.c:635, 4301, 4331, 4357, 5415, 5628, 5672) fail: `broken(status ==
  STATUS_SUCCESS)` is false on "wine", and true on "windows", which accepts the immediate completion of an overlapped
  write that the runner's NTFS gives (the layer passes `NtWriteFile`/`NtReadFile` through; the native run passes them too).
  The `file` unit now runs with
  `--platform=windows` (units of `wine-pe/tests/ntdll.parity`; `wine-pe/tests/ntdll.sh` turns the argument into
  `WINETEST_PLATFORM=windows` before Wine's `main`, which cannot be given through the guest's environment): the checks
  of a Windows machine, no `todo_wine` allowances.
* file.c:400/404/413: `\??\C:\windows\SYSTEM32\cmd.exe` did not exist. `programs/cmd` is in `wine-pe/dlls.txt`; the VFS
  serves `C:\Windows\System32\cmd.exe` from `Windows\builtin` (the same rule as the DLLs). A trailing separator on an
  existing file with `FILE_DIRECTORY_FILE` answers `STATUS_NOT_A_DIRECTORY` (NTFS), without it `STATUS_OBJECT_NAME_INVALID`
  (`vfs::TranslateObjectAttributes` takes the create options). host-win32's `ResolveProgram` asks the VFS before it
  falls back to a host file, otherwise `win10xbox.exe C:\Windows\System32\cmd.exe` ran the host's own cmd.exe.
  Gate: `integration.cmd_builtin` (`cmd.exe /c echo` on the builtin-only machine).
* file.c:467: `FILE_OPEN_BY_FILE_ID` names are binary file IDs relative to a directory handle, and the layer read them
  as a path ("." and ".." components, NAME_INVALID). They go to the host untouched.
* file.c:3969, 4133-4155: `FileNameInformation`, `FileNormalizedNameInformation` and `FileAllInformation` returned the
  host's path (the host wrote it past the guest-length name, `FileAllInformation` was not translated at all). The reply
  is built from a private host reply; length checks, `STATUS_BUFFER_OVERFLOW` and `IoStatus.Information` follow the
  caller's buffer.
* file.c:6310-7236: junctions and symbolic links store an NT name that the host's NTFS resolves on the host's C:.
  `NtFsControlFile` `FSCTL_SET_REPARSE_POINT` maps an absolute `\??\X:\..` target to the host's name (the drive root
  through the VFS, the rest as written: NT names are not normalised, `..` stays invalid), `FSCTL_GET_REPARSE_POINT`
  maps it back; relative targets, volume GUIDs and other tags pass unchanged. Unit test: `tests/unit/test_reparse.cpp`.
* file.c:6705, 6751, 7116: Wine's advapi32 did not know `SeCreateSymbolicLinkPrivilege` (its name table stopped at
  SeCreateGlobalPrivilege): `wine-pe/patches/0005-privilege-names-past-create-global.patch` names the privileges
  Windows 10 defines up to 36. The guest token (a standard user's) does not hold it, so the unit skips its symbolic
  link tests as it does for any unprivileged Windows user. (Symbolic links stay out of the gate; junctions are covered.)

## Parity gate

Windows itself fails assertions of this unit on the runner image (windows-2025-vs2026, 20261021.278.1). Run natively,
without the layer, `ntdll_test.exe file --platform=windows` prints 29 failures (ci.yml run 39941046185, Debug and
RelWithDebInfo alike): 26 are `todo_wine ok(io.Status == 0xdeadbeef)` (file.c:1719, 1782, 1816, 2190, 2223, 2348,
2484, 2587, 2621, 3001, 3034, 3105, 3136, 3205: the runner's NTFS writes the status block of a failed
`FileRenameInformation`/`FileLinkInformation`, the Wine test expects the older behaviour), and three are the runner's
8.3 profile name `RUNNER~1` (file.c:6476, 6980) and `STATUS_NOT_A_DIRECTORY` for a relative symbolic link to a file
with a trailing separator (file.c:6751). So the unit cannot reach 0 failures here, and is not on
`wine-pe/tests/ntdll.allowlist`. `wine-pe/tests/ntdll.parity` lists it instead, with a floor of 1300 assertions:
`tests/winetest/run_ntdll.cmake` runs the unit through the layer twice and natively once on the same runner, and fails
when the unit does not complete, executes less than the floor, or fails (in both layer runs) a source line the native
run passes. Through the layer the unit printed `file: 2722 tests executed (0 marked as todo, 0 as flaky, 26 failures),
3 skipped.` in both configurations, all 26 being lines the native run fails too.

## Run

* ci.yml on `a257d8e` (the code of this slice; the later commits only merge main and edit this file):
  https://github.com/user99672223/win10xbox/actions/runs/39944856100 - linux-cross, windows (Debug) and windows
  (RelWithDebInfo) green, `integration.cmd_builtin` and `integration.winetest_ntdll` passed on both Windows configs
  (87/87 tests each, nothing skipped). wine-pe.yml for the same head (patch 0005, `programs/cmd`):
  https://github.com/user99672223/win10xbox/actions/runs/39944855992 (green).
* First run with `programs/cmd` and the file-name/reparse/by-file-id changes (`6fb640f`):
  https://github.com/user99672223/win10xbox/actions/runs/39933901886, `file: 2867 tests executed (0 marked as todo,
  0 as flaky, 29 failures), 2 skipped.` on both configs (29 = 26 + the three symbolic-link lines 6705, 6751, 7116).
* What `cmd.exe` printed through the layer (`win10xbox.exe C:\Windows\System32\cmd.exe /c echo cmd runs through the
  layer`, exit 0): `cmd runs through the layer`.
* host-uwp.yml was red on `a257d8e` because main's host-uwp/CMakeLists.txt carried a duplicate `foreach` from the hooks
  and menu merges (Configure error, "Flow control statements are not properly nested"); main fixed it in b9bc38c and
  469293c, which this branch merged afterwards. Main now runs host-uwp, uwp-lite and the host-win32 smoke jobs on main only.

## Syscalls added or changed

* `NtFsControlFile`: new Translate (PassThrough before): `FSCTL_SET_REPARSE_POINT` / `FSCTL_GET_REPARSE_POINT` map the
  target of a junction or absolute symbolic link between the guest's and the host's name.
* `NtQueryInformationFile`: `FileNameInformation`, `FileNormalizedNameInformation`, now also `FileAllInformation`;
  the name is the guest view and the reply follows the caller's buffer (private host reply).
* `NtCreateFile`, `NtOpenFile`: `FILE_OPEN_BY_FILE_ID` names are not translated; the create options reach the VFS
  (`file\` on an existing file with `FILE_DIRECTORY_FILE` is `STATUS_NOT_A_DIRECTORY`).

## DLLs and programs added to wine-pe/dlls.txt

`programs/cmd` (cmd.exe, imports shell32, user32, advapi32, kernelbase: all already built). wine-pe/patches: 0005 (names
of the privileges past SeCreateGlobalPrivilege).

## What still fails

* Through the layer the unit fails 26 assertions on the runners, all of them also failing natively (the 14 lines of the
  `io.Status` group above). Nothing the layer does fixes them; the unit stays out of the allowlist and is gated by
  parity.
* The unit executes 2722 assertions through the layer and 2868 natively: the 146 fewer are the symbolic-link tests
  (the guest's token is a standard user's and skips them, the runner's native token runs them). Symbolic links (`FSCTL_SET_REPARSE_POINT` with a relative target) therefore have
  no gate here; one of them, a relative link that climbs past the drive root (file.c:7116), cannot be answered the way
  Windows does while the guest's C: is a directory of the host (the host resolves `..` against its own volume root).
* `NtWriteFile` / `NtReadFile` on an overlapped handle answer what the host answers (immediate completion for a
  cached write on this runner image); the unit accepts it through `broken()` when run as Windows.

## Files

core/dispatch/nt_file.cpp (NtQueryInformationFile, NtFsControlFile, by-file-id, create options), core/vfs/vfs.cpp +
core/include/win10xbox/vfs.h, core/dispatch/syscall_policy.inc, docs/SYSCALLS.md (regenerated), host-win32/main.cpp,
wine-pe/dlls.txt, wine-pe/patches/0005-*.patch, wine-pe/tests/ntdll.sh, wine-pe/tests/ntdll.parity,
tests/winetest/run_ntdll.cmake + CMakeLists.txt, tests/integration/CMakeLists.txt, tests/unit/test_reparse.cpp,
docs/DISPATCHER.md.
