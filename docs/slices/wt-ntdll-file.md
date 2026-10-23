status: wip

# Slice wt-ntdll-file: Wine's ntdll `file` unit through the layer

Goal: the `file` unit of `wine-pe/tests/ntdll.sh` joins `wine-pe/tests/ntdll.allowlist` with a measured floor. Start
(docs/slices/wt-ntdll.md): 2854 assertions, 281 failures on the Windows runners. The last full log (ci.yml run
39895037227, `windows-Debug-logs/winetest/ntdll/results/file.log`) printed 157 failures, of which 107 were
"Test succeeded inside todo block" and 50 real ones (48 distinct lines).

## What the 157 were, and what this slice does about each group

* 107 x "Test succeeded inside todo block". Wine's harness takes the guest for Wine (its ntdll exports
  `wine_get_version`), so every `todo_wine` block expects a failure; the host is Windows and its NTFS passes them.
  That counts as a failure and can never be fixed from the layer. The `file` unit now runs with `--platform=windows`
  (`tests/winetest/run_ntdll.cmake`, `_windows_platform_units`; `wine-pe/tests/ntdll.sh` turns the argument into
  `WINETEST_PLATFORM=windows` before Wine's `main`): the same checks as on a Windows machine, no `todo_wine` allowances.
* file.c:400/404/413: `\??\C:\windows\SYSTEM32\cmd.exe` did not exist. `programs/cmd` is in `wine-pe/dlls.txt`; the VFS
  serves `C:\Windows\System32\cmd.exe` from `Windows\builtin` (the same rule as the DLLs). A trailing separator on an
  existing file with `FILE_DIRECTORY_FILE` answers `STATUS_NOT_A_DIRECTORY` (NTFS), without it `STATUS_OBJECT_NAME_INVALID`
  (`vfs::TranslateObjectAttributes` takes the create options). host-win32's `ResolveProgram` asks the VFS before it falls
  back to a host file, otherwise `win10xbox.exe C:\Windows\System32\cmd.exe` would have run the host's own cmd.exe.
  Gate: `integration.cmd_builtin` (`cmd.exe /c echo` on the builtin-only machine).
* file.c:467: `FILE_OPEN_BY_FILE_ID` names are 8-byte binary IDs relative to a directory handle, and the layer read them
  as a path ("." and ".." components, NAME_INVALID). They go to the host untouched.
* file.c:3969, 4133-4155: `FileNameInformation`, `FileNormalizedNameInformation` and `FileAllInformation` returned the
  host's path (the host wrote it past the guest-length name, `FileAllInformation` was not translated at all). The reply
  is built from a private host reply; length checks, `STATUS_BUFFER_OVERFLOW` and `IoStatus.Information` follow the
  caller's buffer.
* file.c:6310-7236: junctions and symbolic links store an NT name that the host's NTFS resolves on the host's C:.
  `NtFsControlFile` `FSCTL_SET_REPARSE_POINT` maps an absolute `\??\X:\..` target to the host's name (the drive root
  through the VFS, the rest as written: NT names are not normalised, `..` stays invalid), `FSCTL_GET_REPARSE_POINT`
  maps it back; relative targets, volume GUIDs and other tags pass unchanged. Unit test: `tests/unit/test_reparse.cpp`.
* file.c:635, 4301, 4331, 4357, 5415, 5628, 5672: an overlapped write answers `STATUS_PENDING` on a TestBot and
  `STATUS_SUCCESS` from the runners' NTFS. The layer passes `NtWriteFile` through; see "What still fails" once measured.

## Files

core/dispatch/nt_file.cpp (NtQueryInformationFile, NtFsControlFile, by-file-id, create options), core/vfs/vfs.cpp +
core/include/win10xbox/vfs.h, core/dispatch/syscall_policy.inc, docs/SYSCALLS.md (regenerated), host-win32/main.cpp,
wine-pe/dlls.txt, wine-pe/tests/ntdll.sh, tests/winetest/run_ntdll.cmake, tests/integration/CMakeLists.txt,
tests/unit/test_reparse.cpp, docs/DISPATCHER.md.
