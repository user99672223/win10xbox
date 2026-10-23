status: ready (host-uwp https://github.com/user99672223/win10xbox/actions/runs/40030941646 green, console report dirtree verdict=PASS (exit code 42 = directory-install); ci https://github.com/user99672223/win10xbox/actions/runs/40030929226 green, integration.guest_dirtree passed on windows Debug/RelWithDebInfo and linux-cross Wine; head ae8a5be)

# Directory installs: a program that lives in a folder tree

Games and tools ship as a directory: `Game\bin\Game.exe` with `Game\data\*.pak` beside it, started with the working
directory somewhere inside. The launcher only knew flat guests (`C:\guest\x.exe`). This slice adds directory entries to
host-uwp and a guest that proves what such a program needs.

## What changed

* `.launch` gains `cwd=<dir relative to the entry's directory>`; `exe=` may be a relative path with subdirectories
  (`bin\guest_dirtree.exe`). Both are validated by `NormalizeRelativePath` (`host-uwp/guest_sidecar.cpp`): leaf-name
  components only, no `..`, no drive, no absolute or empty component; `/` is accepted and stored as `\`.
* A subdirectory `<dir>\` of `payload\guest` or `LocalState\guests` that holds `<dir>.launch` is one picker entry named
  `<dir>` (`host.cpp` `add_dir_entry`). Picking it copies the whole tree (`CopyTree`, `\\?\` prefixed) to `C:\guest\<dir>\`
  and runs `C:\guest\<dir>\<exe>` with the working directory `C:\guest\<dir>\<cwd>` (the directory itself when `cwd=` is unset).
  A directory whose `exe=` is missing is not offered. The bundled one wins over a LocalState one of the same name.
* A flat `.launch` honours `cwd=` too, relative to `C:\guest`.
* Not changed: the guest-visible file system, the loader, the dispatcher.

## Guest and tests

`tests/guest/dirtree.c` (`guest_dirtree`) with `tests/guest/dirtree/` (`dirtree.launch`: `exe=bin\guest_dirtree.exe`,
`cwd=data`; `data/file.pak`). Staged as `bin/guest/dirtree/` (target `stage_guest_dirtree`) and `payload/guest/dirtree/`
(`stage.cmake` `DIRTREE_EXE`). It is deliberately not a flat guest: run flat it would fail its own cwd and module-path
checks. It checks, and exits 42 only if all hold:

1. `GetModuleFileNameW` is `C:\guest\dirtree\bin\guest_dirtree.exe`
2. `GetCurrentDirectoryW` is `C:\guest\dirtree\data`
3. relative opens (`file.pak`, `.\file.pak`, `..\dirtree.launch`), an open built from the module path
   (`<exe dir>\..\data\file.pak`) and an absolute open
4. letter case: `FILE.PAK`, `File.Pak`, `..\DATA\file.pak`, `C:\GUEST\DIRTREE\DATA\FILE.PAK`, `c:\Guest\...`,
   `GetFileAttributesW` on `..\BIN\GUEST_DIRTREE.EXE`, `FindFirstFileW` on `..\DATA\*.PAK`
5. a 398-character `\\?\` path: five nested 70-character directories, a file written, read back, listed, removed

Tests: `integration.guest_dirtree` (ctest; `run_guest.cmake -DCWD=` passes the host's `--cwd C:\guest\dirtree\data`),
`host_uwp.selftest` (`cwd=`, relative `exe=`, rejected `..`, drive, absolute and empty components; 225 checks),
and on the console the "Run every program" report: `dirtree` has a row in `expected_exit.txt` (`42=directory-install`)
and the workflow asserts its report block holds `dirtree: all checks passed`.

## What was already there (not re-implemented)

The test is the proof; nothing in `core/vfs` was touched.

* Case-insensitive lookup. `vfs.cpp` maps `C:\` onto the machine folder and keeps the guest's letter case in the host
  path (the trace shows `...\guest\dirtree\data\FILE.PAK` for a guest open of `FILE.PAK`); it has no case handling of
  its own. The host file system folds case: NTFS on Windows and the console, and under the Linux ctest run Wine's own
  lookup. Proof: checks 4, which pass on both.
* Paths over `MAX_PATH`. `MaybeExtend` turns any host path of 248 characters or more into a `\\?\` path, and a guest
  `\\?\` path (`ext_input`) is passed on extended. Proof: check 5 (398 characters).
* `GetModuleFileNameW` / `GetCurrentDirectoryW`. The host hands the guest path as the image path and `--cwd` /
  launch.json `cwd` as the process's current directory, so both read in the `C:\guest\...` form
  (trace: `image=C:\guest\dirtree\bin\guest_dirtree.exe ... curdir=C:\guest\dirtree\data\`). Proof: checks 1 and 2.

## Limits

* Local run is Linux (llvm-mingw + Wine as the host), so check 4 there proves Wine's case folding, not NTFS's; the
  console run (report block) is the NTFS proof.
* The picker lists the directory by name (`dirtree`), not by a file name with an extension; `autoselect` and the report
  use that name.
* A directory install is copied whole on every pick (overwriting); there is no size limit or delta copy.
