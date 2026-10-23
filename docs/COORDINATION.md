# Coordination between orchestrator sessions

Two Claude Code sessions work on this repo. **Session A** (main orchestrator) owns main
(`claude/youthful-cerf-l7xxsb`): only it merges into main and only it edits this file,
`docs/STATUS.md` and `docs/HANDOFF.md`. **Session B** works on slice branches only and
never pushes to main. All communication goes through this repository on GitHub; nothing
else.

## Acting coordinator

While Session A is away (its usage window is spent) **Session B is the acting coordinator** and
may merge ready slices into main. Session A resumes the role when it is back; it announces that by
removing this paragraph. Merge procedure for the acting coordinator, per ready slice:

1. In the main checkout: `git fetch origin && git merge --no-edit origin/claude/youthful-cerf-l7xxsb-<slice>`.
   Conflicts keep both sides; `docs/SYSCALLS.md` and `core/dispatch/syscall_table.gen.inc` are regenerated,
   never hand-merged: `python3 tools/gen_syscalls.py <wine-src>/dlls/ntdll/ntsyscalls.h core/dispatch/syscall_table.gen.inc core/dispatch/syscall_policy.inc docs/SYSCALLS.md`
   (`wine-pe/fetch-wine.sh` supplies `wine-src`). A new guest in `tests/guest` must also be added to
   `HOST_UWP_GUESTS` and the console-guest `foreach` in `host-uwp/CMakeLists.txt`, or the package check fails.
2. Build and run the local suite before pushing (`tools/dev/build-mingw.sh`, stage NLS as `ci.yml` does,
   `ctest --test-dir build-mingw --label-exclude wine_conformance`); 0 failures, 0 skips.
3. Push main, watch all six workflows for that head (`gh run list --branch claude/youthful-cerf-l7xxsb`), and
   fix anything red on main before the next merge. A failure that is an apt mirror timeout or a runner loss
   is re-run once; anything else is root-caused.
4. Append one line per merged slice to the "Merged" list below with the main SHA and run URL.

Session A's in-flight branches (`-native`, `-realpython`, `-net`, `-probe`, `-om`, `-identity`) were being
driven by Session A's agents, which stop when Session A does. The acting coordinator merges any of them
whose slice doc says `status: ready`, and may take over one whose last push is older than two hours by
adding `owner: B` as the second line of its slice doc; otherwise leaves them alone.

### Merged (acting-coordinator log)

* `-ncframe` merged at 426ec6b (its runs were cancelled by the next push; verdict carried by ae8fc61,
  ci https://github.com/user99672223/win10xbox/actions/runs/39890044877).
* `-realcurl` merged at 253a848 (same; wine-pe for bcrypt: https://github.com/user99672223/win10xbox/actions/runs/39889436184).
* `-dialog` merged at ae8fc61, ci https://github.com/user99672223/win10xbox/actions/runs/39890044877,
  host-uwp https://github.com/user99672223/win10xbox/actions/runs/39890044955 (guest_dialog added to the package).

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

## Fidelity track

The goal is the most faithful Windows a user-mode program can observe. Wine's conformance
suites are the yardstick: every unit that fails points at a kernel behaviour we do not reproduce.
Each slice below grows an allowlist (`wine-pe/tests/<dll>.allowlist`) by the units it fixes and
never fakes a result to pass; a Wine-table-vs-Windows-table data difference (e.g. `rtlstr`
U+037F) is recorded, not patched.

| Slice | Owner | Scope | Yardstick |
|---|---|---|---|
| `-probe` | A | user-pointer probing in the dispatcher: a bad guest pointer gives `STATUS_ACCESS_VIOLATION` (or the status Windows gives) instead of crashing the guest; SEH-guarded parameter access in every Emulate/Translate path that reads or writes caller memory | ntdll `time` (`time.c:129`), `reg.c:390`; the units join the ntdll allowlist |
| `-om` | A | object manager namespace: `\BaseNamedObjects`, `\Sessions\N\BaseNamedObjects`, `\KernelObjects`, directory objects (`NtCreateDirectoryObject`/`NtOpenDirectoryObject`/`NtQueryDirectoryObject`), symbolic links, and named events/mutexes/semaphores/sections shared between a guest and the children it spawns (`NtCreateUserProcess` children are host processes: guest names map onto real host objects under one per-layer prefix inside the AppContainer namespace) | ntdll `om` unit; a guest + child sharing a named event and a named section (new `tests/guest/named_objects.c`) |
| `-identity` | A | one guest asserts the four identity paths agree: CPUID brand/family/model/stepping, registry `CentralProcessor\0`, SMBIOS type-4 via `GetSystemFirmwareTable`, WMI `Win32_Processor`; hypervisor leaf zero; RDRAND usable; `GetVersionEx`/`RtlGetVersion`/`NtQuerySystemInformation(SystemCodeIntegrity..)` consistent with 19045.3803 | new gated `integration.guest_identity` on both Windows configs |
| `-wt-ntdll-reg` | B | registry fidelity: `NtEnumerateKey` loops forever at `reg.c:1987`; status codes for bad parameters (`reg.c:390`); then the rest of the `reg` unit | ntdll `reg` unit joins the allowlist |
| `-wt-ntdll-file` | B | file fidelity: the `file` unit (281 failures) incl. `\??\C:\windows\SYSTEM32\cmd.exe` existing (ship Wine's `programs/cmd` builtin into the machine folder), `NtQueryInformationFile` classes, `NtQueryDirectoryFile` semantics, reparse/attributes statuses | ntdll `file` unit joins the allowlist |
| `-wt-ntdll-rest` | B | after the two above: `exception`, `info`, `pipe`, `sync`, `threadpool`, `virtual`, `wow64` units in that order | each joins the allowlist |

Session B takes the `-wt-ntdll-*` rows after its first five slices are ready.

## Game track (highest priority after the current batch; the acting coordinator may start these)

| Slice | Owner | Scope | Gate |
|---|---|---|---|
| `-d3d` | A (B may start) | guest D3D11/D3D12: the guest loads the host's native `d3d11.dll`/`d3d12.dll`/`dxgi.dll` (in the DLL manifest, `machine/guest_dlls.txt`); their `D3DKMT*` calls arrive as win32u `NtGdiDdDDI*` syscalls and go PassThrough to the host win32u (route B style); `IDXGIFactory2::CreateSwapChainForHwnd` on a guest window is redirected to a swap chain on the host's CoreWindow (host-uwp) or an off-screen swap chain whose back buffer is composed into the presenter frame (host-win32); measure `D3D_FEATURE_LEVEL` in the AppContainer job | `tests/guest/d3d11_tri.c` (clear + one triangle, read back one pixel via a staging texture, exit 42) on both MSVC configs and in `host-win32-appcontainer.yml`; a real program: Microsoft's DirectX-Graphics-Samples `D3D12HelloTriangle` built in CI |
| `-xinput` | B | `xinput1_4`/`xinput9_1_0` and `Windows.Gaming.Input` for guests: the host's gamepad state (host-uwp already reads it for the launcher) served to the guest's XInput; keyboard-to-pad mapping on the runner for the gate | `tests/guest/xinput.c` reads `XInputGetState`, sees the injected state, exit 42 |
| `-audio` | A (B may start) | mmdevapi/WASAPI for guests over the host's `IAudioClient` (an `mmdevapi` unix library the way `-net` did ws2_32), `MMDeviceEnumerator` class registration (fastfetch Sound, exit 1722, is the probe), `xaudio2_9.dll` native on top | `tests/guest/wasapi.c` renders 200 ms of a sine into the default device (render-only, shared mode), exit 42; fastfetch `Sound` module joins its gate |
