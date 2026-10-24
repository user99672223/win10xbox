status: wip

# Slice -ntdllfile: intermittent crash of Wine's `ntdll:file` unit

Evidence: ci run 40039768246 (main d4bd2c1, windows RelWithDebInfo): `ntdll:file parity FAIL: crashed`.

Finding before any fix (from that run's windows-RelWithDebInfo-logs, ntdll_test.exe rebuilt locally with the same pins):
* `exception.crash.txt` is NOT the file unit's record. The host's crash.txt is only cleared before the diagnosed units
  (exception, ...), so the record is the `exception` unit's deliberate first-chance fault: `ntdll_test.exe+0x27e09` is in
  `func_exception` (the debug-register test: `xor eax,eax; pop [rax]` run from `code_mem`, a 0x10000 rwx VirtualAlloc, then
  `dreg_handler`; the PRIV_INSTRUCTION record is the next sub-test). No APC is involved.
* The `file` unit's own crash is `file: unhandled exception c0000005 at ntdll_test.exe+0x8c5e1`, in
  `test_file_rename_information`: `fni->FileName[fni->FileNameLength / 2] = 0` after an NtQueryInformationFile that failed
  (handle INVALID_HANDLE_VALUE) left `FileNameLength` uninitialised. The root is earlier: `file.c:1675` rename of a file to a
  name that `DeleteFileW(newpath)` was supposed to have freed answers STATUS_OBJECT_NAME_COLLISION (c0000035); every later
  sub-test then cascades (`RemoveDirectoryW failed with 32`, CreateFileW failed) until the write through the garbage length.
* Loop: `.github/workflows/ntdllfile-loop.yml` (push on this branch, or workflow_dispatch) runs `tools/ci/ntdll_file_loop.ps1`.
