# Handoff — read this first

A continuation of this project starts here. `docs/STATUS.md` is the detailed record (including
every claim that was made and later withdrawn, with the counter-evidence); this file is the
orientation you need before touching anything.

Branch: `claude/youthful-cerf-l7xxsb`. Repo: `user99672223/win10xbox`.

## What this project is

A Windows 10 22H2 compatibility layer for Xbox Series S in Dev Mode, running unmodified Windows
x64 programs **natively** — no CPU emulation. Wine's PE builtins supply the Win32 DLLs; our code
supplies the NT syscall surface beneath them, inside a UWP AppContainer.

## The one-sentence state

**The mechanism is proven on the actual console; the scope is one trivial guest.** On an Xbox
Series S (OS `26100.9445.amd64fre.xb_flt_2608ge.260927-0830`) a genuine unmodified MSVC-built
Windows x64 PE is mapped, has its KERNEL32 imports bound, reaches its entry point, runs, and
exits with its own code 42. That is the core thesis, demonstrated. It is also the *only* program
that has ever run.

## What is actually proven, by evidence tier

| Tier | What | Gating? |
|---|---|---|
| Xbox hardware | one guest, exit 42, 188 calls across **34 distinct syscalls** | manual |
| Real Windows CI | same + a **genuine AppContainer** + the exit-code path; all guest/driver end-to-end tests, M4 KMDF + layout check (WDK), every bundled guest's exit code | ✅ yes |
| Wine on Linux CI | the whole ctest suite, M3 conformance baseline (`wine_conformance`) included | ✅ yes |

## What has never been tested — do not assume otherwise

* **One guest program, and it is ours.** `tests/guest/{hello,cpuid,dll,sleep}.c` and
  `host-uwp-lite/smoke_guest.c`. No real-world application has ever run.
* **34 of 258 syscalls** exercised (~13%). **21 are still `todo`.** 161 are pass-through.
* **No GUI has ever been drawn.** On the hardware run `user32`, `gdi32full`, `shell32` and
  `comctl32` were reported *absent* — nothing loaded them, because the guest was a console
  program. The DLLs do ship in the package; see "The goal" below for what is actually missing.
* **`drivers/` (8,397 lines) has never run on the console.** Only under Wine/mingw.
* **`winexbox/` (993 lines) is a headless stub.** No real display or GPU path.
* No COM, networking, audio, child processes, or graphics (M6 not started).

## The goal: pixels on the Xbox screen

That is the next deliverable. Everything else in this file is context for it.

### What the GUI path actually is

`win32u.dll` in the package is **syscall thunks only** (`dlls/win32u/Makefile.in`: `UNIXLIB =
win32u.so`; `win32u.spec`: 1541 `-syscall` lines, nothing else). The window manager, message
queues and GDI engine are in Wine's unix library plus wineserver, which this project never builds.
So the win32k surface is ours to emulate, as `docs/ARCHITECTURE.md` (E5, "EMULATE: win32k") has
said since M3. It is NOT a second dispatch surface: the dispatcher already owns ids `0x1000..`,
the loader already rewrites the win32u thunks, and dispatch is by name. STATUS.md records both
earlier, wrong statements with the counter-evidence.

The real path (design in `docs/GUI.md`, which is the document for this work):

```
 guest user32/gdi32 (Wine PE)  ->  win32u.dll thunks (ids 0x1000..)
                                        |  core/dispatch, Emulate
                                  core/user: NtUser*/NtGdi* (window manager, queues, GDI objects,
                                             software rasteriser, user-mode callbacks)      <- NEW
                                        |  winexbox::IWinexboxDriver (surfaces, input, display)
                                  headless backend [HAVE]
                                        |  user::ComposeFrame()
                 host-win32: WIN10XBOX_SCREENSHOT=x.bmp        host-uwp: D3D11 swap chain on the CoreWindow
```

The PE-side contract is two Wine headers, `include/ntuser.h` and `include/ntgdi.h` ("not
compatible with Windows" by Wine's own comment: we implement Wine's contract because the DLLs are
Wine's). user32 initialisation is one syscall (`NtUserInitializeClientPfnArrays`); window
procedures run through `PEB.KernelCallbackTable[NtUserCallWinProc]` and end with
`NtCallbackReturn`, both emulated in-process.

### Shortest path to something visible

1. **`tests/guest/window.c` green under Wine locally** (`integration.guest_window`): class,
   `CreateWindowExW`, `WM_PAINT` with two `FillRect`s, `GetPixel` read-back, exit 42, plus two
   pixels of the host's screenshot. Frameless window, solid colours, no font engine.
2. **The same test green on both Windows configs in `ci.yml`.**
3. **The presenter**: host-uwp copies `user::ComposeFrame()` into a D3D11 swap chain on the
   CoreWindow (device on a worker thread: `D3D11CreateDevice` hangs the UI thread of a Game-bucket
   app; swap chain and `Present` on the UI thread's 60 Hz pump). `LocalState\screen.bmp` is the
   same frame for Device Portal.
4. **Input, after pixels.** The driver's `InjectInput` is already fed from the CoreWindow;
   `core/user` turns it into `WM_MOUSEMOVE`/`WM_KEYDOWN`.

## Traps that have already cost real time — do not rediscover these

1. **Guest tests SKIP silently without the Windows NLS data.** `ctest` reports `100% tests
   passed` while the guest tests never ran. Stage NLS exactly as `ci.yml:80-83` does:
   copy `/usr/share/wine/nls/*.nls` and `$WINEPREFIX/drive_c/windows/system32/*.nls` into
   `build-mingw/bin/machine/Windows/System32`, and `sortdefault.nls` into
   `Windows/Globalization/Sorting`. **Always check the skip count, not just the pass count.**
2. **Wine-green does not mean Windows-green.** mingw uses `__cxa_guard` (no TLS); MSVC's
   thread-safe-static guard reads TLS. That single difference hid a crash for weeks and the
   gating Linux job was structurally blind to it. Four real bugs were found the first time the
   layer ran on real Windows.
3. **A crash record is useless without the host module table.** While the guest runs, `PEB->Ldr`
   is Wine's, so host modules are absent and an address cannot be attributed. This caused two
   successive wrong attributions. `host-win32/crash.cpp` now snapshots the host's modules before
   the takeover; `host-uwp-lite` and `host-uwp` copies still need that port (task #13).
4. **Reason from artifacts, not plausibility.** Several confident hypotheses here were refuted by
   simply reading a log or checking whether a file existed. All withdrawals are recorded in
   `STATUS.md` with their counter-evidence; that honesty is deliberate — keep it.
5. **Tags cannot be pushed** from this environment (HTTP 403, branch-scoped credential).
   `m0`–`m5` exist locally only; `STATUS.md` records every milestone SHA instead.

## How to build and test

```bash
tools/dev/build-mingw.sh            # configure + build into build-mingw/
# stage NLS (see trap 1), then:
ctest --test-dir build-mingw --output-junit ctest-junit.xml      # everything (wine_conformance needs wine-pe/fetch-wine.sh)
python3 tools/ci/check_ctest_run.py --build build-mingw --junit ctest-junit.xml --require-label guest_e2e   # skips = FAIL
```

Workflows: `ci.yml` (gates), `host-win32-smoke.yml` (real-Windows guest, gates),
`host-win32-appcontainer.yml` (**genuine AppContainer**, gates), `host-uwp.yml` (M5 `.msix`),
`uwp-lite.yml` (throwaway validator), `wine-pe.yml` (builds the Wine PE builtins).

Test hooks that simulate conditions we cannot otherwise reach, all latched in `Initialize`
before the PEB takeover: `WIN10XBOX_TEST_NO_HOST_PATHS`, `WIN10XBOX_TEST_TLS_INDEX`,
`WIN10XBOX_TEST_TLS_CLOBBER`, `WIN10XBOX_TEST_GUEST_VIEW_VIOLATION`,
`WIN10XBOX_TEST_LEAK_GUEST_ACTCTX`, `WIN10XBOX_NORMAL_EXIT`.

## Recommended priority order

1. **Pixels.** The four steps under "Shortest path to something visible". This is the goal; it is
   not blocked on anything below.
2. ~~**Close the `guest_e2e` Windows gate** (task #12)~~ **Done 2026-10-22**: stdio was captured all along;
   two real bugs were hiding behind the non-blocking step (`docs/STATUS.md`, top). What the old text said: those tests
   run on Windows but cannot fail the build, because guest stdio is not captured there. That is
   the exact hole that let four bugs sit in "green" code — but it is a standing-protection task,
   so fold it in rather than letting it hold the screen hostage.
3. **Run a guest that is not ours.** Something small, real, console-only. This teaches you more
   about the remaining 87% of the syscall surface than any other single action.
4. **Then the 21 `todo` syscalls**, driven by what steps 1 and 3 actually hit — not speculatively.
5. **M6 graphics proper** (accelerated D3D for guest apps, as opposed to the presenter in step 3
   of the pixel path). Known gaps already recorded: child-process resume, D3D feature level 11_0
   vs 12_1, GLOn12/Dozen/Vulkan bundling, WoW64 far jump.

### Standing testing rule

Test the **whole** project on GitHub's free Windows runners, continuously, not just the piece you
are touching. Windows Server 2025 runners are build 26100 — the *same* build family the console
reports (`26100.9445...`), which is why Windows CI predicts console behaviour and Linux-Wine CI
does not. Concretely: bundle every guest in the smoke package (including the `drivers/` client),
get `drivers/` running somewhere other than mingw, and **watch the ctest skip count, not just the
pass count** (trap 1). When a hardware trip is needed, combine everything into one run — each trip
costs the user real time.

Open tasks worth reading: #12 is closed (2026-10-22, `STATUS.md`), #13 (port the host module table
to the other two `crash.cpp` copies), #15 (use NT paths so `HostPathOfHandle` works inside an
AppContainer — measured to work, would replace byte-comparison identity).

## Two loose ends

* `FlsGetValue2` is missing from our `kernelbase` builtin, and
  `api-ms-win-appmodel-runtime-l1-1-2.dll` fails to load. Neither blocked the hardware run, but
  the second is API-set plumbing that graphics work will likely need.
* `integration.host_version` is environment-sensitive: Wine's `wine32 is missing` noise can
  precede the `^win10xbox` anchored regex. It passes in some environments and fails in others.
