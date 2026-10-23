# Coordination between orchestrator sessions

Two Claude Code sessions work on this repo. **Session A** (main orchestrator) owns main
(`claude/youthful-cerf-l7xxsb`): only it merges into main and only it edits this file,
`docs/STATUS.md` and `docs/HANDOFF.md`. **Session B** works on slice branches only and
never pushes to main. All communication goes through this repository on GitHub; nothing
else.

## Protocol

1. A slice is one branch `claude/youthful-cerf-l7xxsb-<slice>` created from current main
   (or resumed from the existing remote branch, then `git merge origin/claude/youthful-cerf-l7xxsb`;
   conflicts keep both sides; `docs/SYSCALLS.md` is regenerated with `python3 tools/gen_syscalls.py`,
   never hand-merged).
2. A slice is **ready** when `ci.yml` is green on all three jobs (linux-cross, windows Debug,
   windows RelWithDebInfo) for the branch head, the other workflows it touches are green, and
   the branch carries `docs/slices/<slice>.md` whose first line is `status: ready` followed by:
   the run URL, what runs through the layer (program, command, first output lines), syscalls
   added or changed, DLLs added to `wine-pe/dlls.txt`, and what is still failing. While work is
   in progress the first line is `status: wip`. If it cannot be finished the first line is
   `status: blocked` plus the exact first error.
3. Session A polls `git ls-remote` and the branches' CI, merges ready slices into main, and
   records them in `docs/STATUS.md`. A merged slice's branch is left alone.
4. New assignments appear in the table below, on main. Session B re-reads this file from
   `origin/claude/youthful-cerf-l7xxsb` before starting each slice. Session B never takes a
   slice listed under Session A.
5. Never edit a slice branch owned by the other session.

## Assignments

### Session A (main orchestrator)

`-sqlite`, `-syscalls`, `-native`, `-sysinfo` (Coreinfo64/PsInfo64), `-fastfetch`,
`-realpython`, `-wt-ntdll`; later `-net`, `-audio`, `-d3d`, `-wp-cmd`, `-wp-regedit`,
`-wp-winemine`, `-uwpchild`.

### Session B

Real programs (each: pinned download or MSVC build at CI time, hash-verified, `actions/cache`,
gated `integration.<name>` test under `if(MSVC OR WIN10XBOX_REAL_PROGRAMS)` in
`tests/integration/CMakeLists.txt`, modelled on the Lua slice: `ci.yml` step "Lua",
`tools/ci/build_lua.ps1`, `tests/guest/lua`, `run_guest.cmake`):

| Slice | Program | Pass condition |
|---|---|---|
| `-realgit` | git for Windows (MinGit x64 zip) | `git --version`, `git init` + `add` + `commit` in a temp repo, `git log --oneline` shows the commit |
| `-realcurl` | curl for Windows x64 | `curl --version` lists protocols; `curl file:///C:/...` reads a local file (no network needed) |
| `-real7z` | 7-Zip x64 console (`7za.exe`/`7zr.exe`) | `7zr a` then `7zr t` then `7zr x` round-trips a directory; `7zr b -mmt1` benchmark completes |
| `-realninja` | ninja x64 | builds a two-rule `build.ninja` that runs `cmd /c` copies; `ninja -t targets` |
| `-realtcc` | Tiny C Compiler win64 | compiles and runs a hello-world; the produced exe runs through the layer |
| `-realsdl` | an SDL2 sample (official SDL2 x64 release + a 60-line test window) | window painted (pixel readback as `tests/guest/window.c` does), exits 42 |

Wine conformance suites (each: Wine's `dlls/<dll>/tests` built as a guest and run under the
`wine_conformance` label, modelled on the merged `wt-advapi32` and `wt-gdi32` slices in
`wine-pe/tests/*.sh` and `tests/wine-tests/`; an explicit allowlist of test files with 0
failures that only grows; the rest listed with their failing assertion lines in the slice doc):

| Slice | Suite |
|---|---|
| `-wt-kernel32` | kernel32 (resume existing branch) |
| `-wt-msvcrt` | msvcrt (resume) |
| `-wt-shell32` | shell32 (resume) |
| `-wt-misc` | the remaining small suites on that branch (resume) |
| `-wt-media` | (resume) |

GUI slices in `core/user` (our win32k emulation behind Wine's PE user32/gdi32, contract =
Wine 11.9 `include/ntuser.h` and `include/ntgdi.h`; see `docs/GUI.md`; each gated by a guest in
`tests/guest/*.c` with pixel readback through `run_guest.cmake`, green on both MSVC configs;
no function-local statics on guest-reachable paths):

| Slice | Scope |
|---|---|
| `-ncframe` | non-client area: caption, borders, system buttons drawn and hit-tested; `WM_NCPAINT`/`WM_NCHITTEST`/`WM_NCLBUTTONDOWN` move and size (resume) |
| `-dialog` | `DialogBox`/`CreateDialog` from resources, `WM_INITDIALOG`, button/static/edit controls painted, `EndDialog` (resume) |
| `-scroll` | scroll bars (`SetScrollInfo`, `WM_VSCROLL`), `ScrollWindowEx` (resume) |
| `-comctl` | comctl32 builtin loads; a listview and a button with the common controls paint (resume) |
| `-gdibmp` | `CreateDIBSection`, `BitBlt`/`StretchBlt` between DCs, `LoadImage` of a bitmap resource (resume) |
| `-clipboard` | text clipboard round trip in-process (`NtUserSetClipboardData`/`GetClipboardData`) (resume) |
| `-seh` | guest SEH/vectored exception handling through the dispatcher: `__try/__except` around an access violation, `RaiseException`, exit code checked (resume) |
| `-menu` | menu bar and popup menus: `CreateMenu`, `TrackPopupMenu`, `WM_COMMAND` from a click (new) |
| `-hooks` | `SetTimer` precision, `SetWindowsHookEx` WH_KEYBOARD/WH_MOUSE in-process (new) |

Order for Session B: `-realgit`, `-real7z`, `-wt-kernel32`, `-ncframe`, `-dialog`, then the
rest in any order, several in parallel.
