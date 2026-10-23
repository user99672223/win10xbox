# Handoff — read this first

`docs/STATUS.md` is the detailed record (including every claim later withdrawn, with the
counter-evidence). This file is the orientation to read before touching anything.

Main: `claude/youthful-cerf-l7xxsb`. Repo: `user99672223/win10xbox`.

## What this project is

A Windows 10 22H2 compatibility layer for Xbox Series S in Dev Mode. Unmodified Windows x64
programs run **natively**, with no CPU emulation. Wine's PE builtins supply the Win32 DLLs; our code
supplies the NT syscall surface beneath them, inside a UWP AppContainer.

## What works on main (measured)

* **Console:** package 1.0.0.333 (run 39981119649) on an Xbox Series S, OS 26100.9445: `window.exe` painted its
  blue window and red square on the TV and `guest_xinput.exe` read the real controller (exit 42). Needed by hand:
  `l_intl.nls` and `c_10000.nls` uploaded into LocalState (the package did not bundle NLS; `-nlspkg`). One guest per
  app run (`-relaunch`); `rdrand` faults (`-rdrand`). Details: STATUS.md "On the console".
* **Loader** (`core/loader`): PE mapping, import binding, Wine builtin stub rewrite, scanner.
* **Dispatcher** (`core/dispatch`): by-name table generated from Wine's headers; `Emulate` / `PassThrough` /
  `Todo` per syscall; privileged calls answered as an unprivileged caller.
* **VFS** (`core/vfs`): virtual `C:\` over the machine folder. **Registry** (`core/registry`): REGF hives
  read-only plus an in-memory overlay. **NLS** (`core/dispatch/nt_nls.cpp`): `.nls` sections from the
  machine folder; a missing file fails, nothing is faked.
* **core/user:** windows, GDI and message queues (234 win32u syscalls, Wine 11.9 contract). Gated:
  `integration.guest_window`, `integration.guest_window2`. Input delivery (`integration.guest_window_input`,
  `guest_window_input2`) is gated on main since `d9f7639`.
* **D3D11 presenter** in `host-uwp`: `ComposeFrame()` into a swap chain on the CoreWindow. Writes
  `LocalState\screen.bmp` every 5 s.
* **Nine bundled guests** in the host-uwp package: `window.exe` (default), `hello.exe`, `hello_cui.exe`,
  `guest_hello`, `guest_cpuid`, `guest_sleep`, `guest_dll`, `smoke_cui.exe`, `drv_client.exe`.
* **AppContainer finding (measured, non-gating):** a genuine AppContainer can load the native
  user32/gdi32, create and paint a window and read pixels back (run 39827778745). Not measured on the
  console. Route B (forward to the host's win32u) stays open because of this.

## What does not work yet

Glyph text, menus, clipboard, hooks, window-frame dragging, networking, audio, guest child processes,
D3D for guest apps (M6), and 21 `todo` syscalls. Only the console's one guest has run on hardware; the
package's `window.exe` has not been seen on the TV.

## In-flight slices

Branches `claude/youthful-cerf-l7xxsb-<slice>`, each from main. Full table with scopes in `STATUS.md` (top).

* Existing on origin: `-native`, `-text`, `-texttest`, `-realprog`, `-sqlite`,
  `-sys1`, `-sys2`, `-syscalls`, `-present`, `-ci`, `-gui-probe`, `-user`.
* Named for next, **no remote branch found yet**: `-ncframe` (non-client frame), `-menu`, `-dialog`,
  `-clipboard`, `-hooks` (timers and hooks), `-guiapp` (real GUI program), `-net` (ws2_32/AFD), `-audio`,
  `-uwpchild` (child processes in UWP), `-d3d` (ImGui on d3d11 via route B).

### Package contents

The host-uwp .msix (artifact host-uwp-msix) carries `payload/builtin/*.dll` (Wine PE builtins from the wine-pe run),
`payload/guest/*` (every guest of host-uwp/CMakeLists.txt HOST_UWP_GUESTS: window, hello, hello_cui, guest_hello,
guest_cpuid, guest_sleep, guest_dll.dll, smoke_cui, smoke, drv_client, guest_window2, guest_window_input,
guest_window_input2, guest_window_text) and `payload/guests.txt` (one file name per line, written by stage.cmake).
host-uwp.yml step "Verify package contents + size report" opens the .msix, fails if any guests.txt entry or any
tests/guest/*.c guest is missing, if no builtins are present, or if the package exceeds a 500 MB sideload budget, and
writes a per-directory size table to the job summary. Gaps: NLS files are not bundled; tests/guest/lua is not bundled.

## Install the package on the console

1. Artifact `host-uwp-msix` from the latest green `host-uwp.yml` run on main. Latest green as of this
   writing: [39837028686](https://github.com/user99672223/win10xbox/actions/runs/39837028686) (head `fb86712`).
   Main has moved to `1a6fb74`; its run [39846504150](https://github.com/user99672223/win10xbox/actions/runs/39846504150)
   was still in progress.
2. Device Portal: install the `.cer` first, then the `.msix`.
3. The default `launch.json` runs `window.exe` (args `30`, expects exit 42) and keeps the last frame on screen.
   Verdict: `RESULT: PASS` or `RESULT: FAIL` at the top of `LocalState\status.txt`.
4. `LocalState\screen.bmp` holds the last composed frame. A `LocalState\launch.json` overrides the default.

## Traps that have already cost real time

1. **Guest tests SKIP silently without the Windows NLS data.** `ctest` reports `100% tests passed` while the
   guest tests never ran. Stage NLS as `ci.yml:78-87` does: `/usr/share/wine/nls/*.nls` and
   `$WINEPREFIX/drive_c/windows/system32/*.nls` into `build-mingw/bin/machine/Windows/System32`,
   `sortdefault.nls` into `Windows/Globalization/Sorting`. **Check the skip count, not just the pass count.**
2. **Wine-green does not mean Windows-green.** mingw's `__cxa_guard` has no TLS; MSVC's guard reads TLS.
3. **A crash record needs the host module table.** While the guest runs `PEB->Ldr` is Wine's.
   `host-win32/crash.cpp` snapshots it; the `host-uwp` and `host-uwp-lite` copies still need the port (task #13).
4. **Reason from artifacts, not plausibility.** Withdrawals are recorded in `STATUS.md`; keep that honesty.
5. **Tags cannot be pushed** from this environment (HTTP 403). `STATUS.md` records milestone SHAs instead.

## Build and test

```bash
tools/dev/build-mingw.sh            # configure + build into build-mingw/
# stage NLS (trap 1), then:
ctest --test-dir build-mingw --output-junit ctest-junit.xml
python3 tools/ci/check_ctest_run.py --build build-mingw --junit ctest-junit.xml --require-label guest_e2e   # skips = FAIL
```

Workflows: `ci.yml` (gates), `host-win32-smoke.yml`, `host-win32-appcontainer.yml` (gates),
`host-uwp.yml` (`.msix`), `uwp-lite.yml`, `wine-pe.yml`. Test hooks (`WIN10XBOX_TEST_*`) are latched in
`Initialize`; see `docs/TESTING.md`.

## Priorities

1. Land the in-flight slices that unblock visible GUI: `-text` (glyphs, gates `guest_window_text`), then the
   `-native` route decision. Input is on main.
2. Run a guest that is not ours (`-realprog`, `-sqlite` are the first candidates) and drive the remaining
   `todo` syscalls from what it hits.
3. Get the packaged `window.exe` seen on the TV and recorded in `STATUS.md` with the screenshot.
4. M6 graphics proper: child-process resume, D3D feature level 11_0 vs 12_1, GLOn12/Dozen/Vulkan bundling,
   WoW64 far jump.

### Launcher (host-uwp on-console guest picker)

`host-uwp/launcher.{h,cpp}` is a list of guest programs drawn on the TV by the existing D3D11 presenter
(`Presenter::SetFrameSource`: while the launcher is active it supplies the frame instead of
`user::ComposeFrame`; text uses a built-in 5x7 bitmap font). Navigation: gamepad through
Windows.Gaming.Input (D-pad or left stick, A launches, B goes back; the `VK_GAMEPAD_*` keys are used only
if no pad is ever seen) and keyboard (arrows, Enter/Space, Esc/Backspace).

* **When it shows.** `launch.json` has `"picker": true`, or names `"autoselect"`, or there is no
  `launch.json` at all (neither `LocalState` nor the package). The packaged `payload/launch.json` still
  exists and does not say `"picker"`, so a fresh install keeps running `window.exe` as before; to make the
  picker the fresh-install default add `"picker": true` to it and give the `run` job in `host-uwp.yml` a
  LocalState `launch.json` that pins `window.exe` (it asserts `RESULT: PASS`).
* **The list** is `payload\guest\*.exe` (bundled) plus any `.exe` in `LocalState\guests\`. A LocalState
  guest (and the `.dll` files beside it) is copied into `C:\guest` (`LocalState\machine\guest`) right
  before launch. The cursor starts on the `"guest"` of `launch.json`. Arguments and `expect_exit_code`
  written for another guest are dropped.
* **Sidecar files** next to a guest (package `payload\guest\` or `LocalState\guests\`; parsed by
  `host-uwp/guest_sidecar.{h,cpp}`, covered by `host_uwp.selftest`). Blank lines and lines starting with `#` are
  skipped everywhere; CRLF is fine.
  * `<name>.args`: one argument per line, verbatim (no splitting, no quote handling). When present it replaces
    `launch.json` `args` for that launch.
  * `<name>.env`: `KEY=VALUE` per line. Set in the host's environment (the guest inherits it) for that launch
    only, then the previous values are restored. A malformed file is ignored (logged).
  * `<name>.title`: first non-blank line is shown in the list instead of `<name>.exe`.
  * `<name>.launch`: an extra list entry that runs another guest with its own settings, no copy of the exe.
    Keys, one per line: `exe=<leaf name>` (required, must be in the list), `title=<text>`, `args=<words>`
    (whitespace separated, `"double quotes"` group a word; repeatable, appended), `env=KEY=VALUE` (repeatable).
    An entry whose `exe` is not present is not offered. `autoselect` matches the entry name (`doom-timedemo.launch`).
* **Doom.** The `msix` job runs `tools/ci/fetch_doom.ps1` (pinned) into `payload\guest\` (flat: `chocolate-doom.exe`,
  the SDL2 DLLs, `doom1.wad`, about 10 MB; `stage.cmake` `EXTRA_GUEST_DIR` / CMake `HOST_UWP_EXTRA_GUEST_DIR`).
  Shipped sidecars (`host-uwp/payload/guest/`): `chocolate-doom.args` = `-iwad doom1.wad -fullscreen -nomouse`,
  `chocolate-doom.env` = `SDL_RENDER_DRIVER=software` (the hardware path is a later slice), `chocolate-doom.title`
  = `Doom`, and `doom-timedemo.launch` (`-timedemo demo1 -nosound -nomouse -nogui -window`, listed as
  "Doom (timedemo)"; the engine ends it through `I_Error`, exit code -1, so the result screen says EXITED, not PASS).
  The picker therefore shows "Doom" and "Doom (timedemo)"; play uses the pad through XInput (SDL2). Not proven on
  hardware: any of it. The iwad is found next to the exe (`C:\guest`).
* **After the guest exits** the result screen shows its exit code, the verdict and the last 20 lines of
  `stdout.txt`; B/Esc/Enter returns to the list. The same data is written to
  `LocalState\launcher_result.txt` (`guest=`, `ran=`, `exit_code=`, `verdict=`, `  | <line>`).
* **Test hook.** `"autoselect": "hello_cui.exe"` launches that entry without input, writes
  `launcher_result.txt`, and (with `exit_when_done`, default true) ends the app after one run.
* **Gate.** `host-uwp.yml` job `launcher` (needs `msix`, not continue-on-error): installs the `.msix` on
  the runner, writes `LocalState\launch.json` with `picker` + `autoselect`, activates the app on the
  runner's CoreWindow and asserts `guest=hello_cui.exe`, `ran=yes`, `exit_code=42` and
  `hello from guest` in `launcher_result.txt`. Parser keys are covered by `host_uwp.selftest`.
* **Not proven.** Gamepad/keyboard navigation (CI cannot press buttons) and running a SECOND guest in the
  same process after the first one's `win10xbox::Shutdown()` (the worker loops back to the list and
  re-runs the whole `RunImpl`; `Initialize` is re-entrant on paper, nothing has run it twice on hardware).
  If a second run misbehaves, that is the first thing to look at; `crash.txt` will say.


### Standing rule

Test the whole project on GitHub's Windows runners (build 26100, the same family as the console). Watch the
ctest skip count, not just the pass count. Combine changes into one hardware trip when one is needed.

## Loose ends

* `FlsGetValue2` is missing from our `kernelbase` builtin; `api-ms-win-appmodel-runtime-l1-1-2.dll` fails to load.
* `integration.host_version` is environment-sensitive (Wine's `wine32 is missing` noise can precede the anchored regex).
