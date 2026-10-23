status: ready

# Slice: realninja

ninja 1.12.1 (the official prebuilt `ninja-win.zip`, SHA-256 pinned and verified on every run by
`tools/ci/fetch_ninja.ps1`, cached with `actions/cache`) is a real guest program that is not ours and that we do not
build. It runs on the shared machine folder next to the Lua build, gated in ci.yml's windows jobs:

* `integration.ninja_version`: `C:\guest\ninja.exe --version`, exit 0, prints `1.12.1`.
* `integration.ninja_targets`: `ninja -C C:\guest -f ninja_build.ninja -t targets all`, exit 0, lists the three outputs.
* `integration.ninja_build`: the same build.ninja with two rules (`copy`, `concat`) and three edges, exit 0,
  `[1/3]`..`[3/3]` on stdout, `C:\guest\ninja_out\ab.txt` exists afterwards. ninja on Windows does not prepend
  `cmd /c` (src/subprocess-win32.cc) and the layer has no cmd.exe, so the rules run `C:\guest\lua.exe
  C:\guest\ninja_tool.lua` (tests/guest/ninja).
* `integration.ninja_noop`: a second run prints `ninja: no work to do.`

CI run: https://github.com/user99672223/win10xbox/actions/runs/39889154701 (head f699206, before the merge of main):
linux-cross, windows (Debug) and windows (RelWithDebInfo) all green, "Gate - nothing skipped" passed on all three,
51 of 51 tests passed on both Windows jobs. The four ninja tests among them (RelWithDebInfo 0.28 s, 0.28 s, 0.90 s,
0.28 s; Debug 0.88 s, 0.86 s, 2.70 s, 0.87 s). An earlier run (39880335902, head 65e6ccb) had the same Windows result
with linux-cross dead on an apt timeout. ctest prints no output for passing tests, so the regexes above (exact
`[1/3]`, `[2/3]`, `[3/3] CONCAT ninja_out/ab.txt`, `ninja: no work to do.`) are the record of what ninja printed.
The head after the merge of main is checked by its own ci run (see COORDINATION.md for that run).

Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: none.

## What had to change

Nothing in core/: ninja's CreateProcess children, overlapped named pipes with an I/O completion port, GetFileAttributesEx
and `-C` (SetCurrentDirectory, inherited by the child) all worked through the existing syscalls. Added:
`tools/ci/fetch_ninja.ps1` and the two ci.yml steps, `tests/guest/ninja/{ninja_build.ninja,ninja_tool.lua}`, the four
tests in tests/integration/CMakeLists.txt, and `*.ninja` in prepare_machine.cmake's guest glob.

## Still open

Only a single-process-per-edge build was measured (3 edges, one rule command each); no depfile (`deps = msvc`/`gcc`) edge.
