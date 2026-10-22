# Handoff — read this first

`docs/STATUS.md` is the detailed record (including every claim later withdrawn, with the
counter-evidence). This file is the orientation to read before touching anything.

Main: `claude/youthful-cerf-l7xxsb`. Repo: `user99672223/win10xbox`.

## What this project is

A Windows 10 22H2 compatibility layer for Xbox Series S in Dev Mode. Unmodified Windows x64
programs run **natively**, with no CPU emulation. Wine's PE builtins supply the Win32 DLLs; our code
supplies the NT syscall surface beneath them, inside a UWP AppContainer.

## What works on main (measured)

* **Console:** one unmodified MSVC x64 guest is mapped, bound, run and exits 42 on an Xbox Series S
  (build 26100). This is the only evidence from hardware so far.
* **Loader** (`core/loader`): PE mapping, import binding, Wine builtin stub rewrite, scanner.
* **Dispatcher** (`core/dispatch`): by-name table generated from Wine's headers; `Emulate` / `PassThrough` /
  `Todo` per syscall; privileged calls answered as an unprivileged caller.
* **VFS** (`core/vfs`): virtual `C:\` over the machine folder. **Registry** (`core/registry`): REGF hives
  read-only plus an in-memory overlay. **NLS** (`core/dispatch/nt_nls.cpp`): `.nls` sections from the
  machine folder; a missing file fails, nothing is faked.
* **core/user:** windows, GDI and message queues (234 win32u syscalls, Wine 11.9 contract). Gated:
  `integration.guest_window`, `integration.guest_window2`. The input sink is on main; its end-to-end gate
  waits for the `-input` slice.
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

* Existing on origin: `-native`, `-text`, `-texttest`, `-input`, `-inputtest`, `-realprog`, `-sqlite`,
  `-sys1`, `-sys2`, `-syscalls`, `-present`, `-ci`, `-gui-probe`, `-user`.
* Named for next, **no remote branch found yet**: `-ncframe` (non-client frame), `-menu`, `-dialog`,
  `-clipboard`, `-hooks` (timers and hooks), `-guiapp` (real GUI program), `-net` (ws2_32/AFD), `-audio`,
  `-uwpchild` (child processes in UWP), `-d3d` (ImGui on d3d11 via route B).

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

1. Land the in-flight slices that unblock visible GUI: `-text` (glyphs), `-input` (gates `guest_window_input`),
   then `-native` route decision.
2. Run a guest that is not ours (`-realprog`, `-sqlite` are the first candidates) and drive the remaining
   `todo` syscalls from what it hits.
3. Get the packaged `window.exe` seen on the TV and recorded in `STATUS.md` with the screenshot.
4. M6 graphics proper: child-process resume, D3D feature level 11_0 vs 12_1, GLOn12/Dozen/Vulkan bundling,
   WoW64 far jump.

### Standing rule

Test the whole project on GitHub's Windows runners (build 26100, the same family as the console). Watch the
ctest skip count, not just the pass count. Combine changes into one hardware trip when one is needed.

## Loose ends

* `FlsGetValue2` is missing from our `kernelbase` builtin; `api-ms-win-appmodel-runtime-l1-1-2.dll` fails to load.
* `integration.host_version` is environment-sensitive (Wine's `wine32 is missing` noise can precede the anchored regex).
