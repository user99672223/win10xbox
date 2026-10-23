status: ready

# Slice: -realgit (Git for Windows through the layer)

Goal: MinGit 2.47.1 x64 (`mingw64/bin/git.exe`, pinned by SHA-256, `tools/ci/fetch_mingit.ps1`) runs on the Wine PE
builtins over the NT syscall dispatcher: `git --version`, then `init`, `add`, `status`, `commit`, `log --oneline` in a
temporary repository (`integration.real_git*`, registered with the Lua tests under `if(MSVC OR WIN10XBOX_REAL_PROGRAMS)`).

First attempt (run 39848918482): both tests died with `EXCEPTION_WINE_STUB` (`api-ms-win-core-crt-l2-1-0!_initterm_e`
called from sechost's DllMain) because the tests ran on the shared machine folder, where the runner's native
advapi32/sechost are mapped. The git tests now run on the builtin-only folder, which `prepare_machine.cmake` fills with
the `guest/mingit` tree.

## Result

Run: https://github.com/user99672223/win10xbox/actions/runs/39889144377 (head cd0e01c; linux-cross, windows Debug and
windows RelWithDebInfo green, "Gate - nothing skipped": 56 registered, 56 ran and passed on both Windows jobs).
wine-pe.yml for the same wine-pe/ content: https://github.com/user99672223/win10xbox/actions/runs/39883366427 (green;
the head's push touched nothing under wine-pe/, so no new run was triggered).

What runs through the layer (MinGit 2.47.1 x64 `mingw64/bin/git.exe`, on the builtin-only machine folder), each a
passing ctest whose expected output regex matched the program's stdout:
- `integration.real_git`: `git --version` -> `git version 2.47.1.windows.1`
- `integration.real_git_init`: `git init C:\guest\gitrepo` -> `Initialized empty Git repository in .../gitrepo/.git/`
- `integration.real_git_add`: `git add hello.txt`
- `integration.real_git_status`: `git status --short` -> `A  hello.txt`
- `integration.real_git_commit`: `git commit -m "layer commit"` -> `[<branch> (root-commit) <sha>] layer commit`
- `integration.real_git_log`: `git log --oneline` -> `<sha> layer commit`
(ctest prints a passing test's output nowhere in the job log; the expected patterns are in tests/integration/CMakeLists.txt.)

Syscalls added or changed:
- `NtQueryObject` (PassThrough -> Translate): `ObjectNameInformation` of a file handle returns `\??\C:\...` (the guest
  path) instead of the host `\Device\HarddiskVolumeN\...` name; Wine's `GetFinalPathNameByHandleW` (git's getcwd)
  accepts only that form.
- `NtOpenFile`: an open with `SYNCHRONIZE` alone (Wine's `SetFileAttributesW`, which git uses to hide `.git`) also asks
  for `FILE_WRITE_ATTRIBUTES`, retrying with the caller's access on access-denied; Windows requires that right for
  `FileBasicInformation`, Wine's server does not.
- `RtlCreateHeap` (forwarded to the host -> core wrapper `w10x_RtlCreateHeap`): a heap with no maximum is created
  `HEAP_GROWABLE` as Wine's ntdll does; the host made msvcrt's private heap a fixed 1 MB heap and git ran out of memory
  in `deflateInit` (unit test `dispatch_create_heap_growable`).
- ws2_32's unix library (git imports ws2_32): landed on main by another slice, taken in the merge.

DLLs added to wine-pe/dlls.txt: `cryptbase` (advapi32 forwards `SystemFunction036` to it; msvcrt's `rand_s`, called by git).

Still failing / not covered: nothing in the pass condition. Not exercised: git network operations (name resolution
answers WSAEOPNOTSUPP), hooks and anything that spawns `sh.exe`/other child processes (MinGit's `usr/bin` is not on
the guest PATH in these tests).
