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
| Real Windows CI | same + a **genuine AppContainer** + the exit-code path | ✅ yes |
| Wine on Linux CI | 26 tests, 14 of them guest end-to-end | ✅ yes |
| Wine only | M3 conformance baseline; M4 KMDF + layout checks are mingw-only | ❌ no |

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

An earlier version of this file claimed GUI needs a second `NtUser*`/`NtGdi*` syscall table on
the order of a thousand entries. **That was wrong and it is withdrawn.** Wine's `win32u` is a
userspace PE: it contains the window manager, the message queues and the dibdrv software GDI
engine. It does not call into a win32k driver. There is no second dispatch surface to build.

The real path, from `winexbox/README.md` (read it — it is the design doc for this work):

```
 guest user32/gdi32  ->  win32u (window manager, message queues, dibdrv GDI)   [HAVE, ships in the .msix]
                              |  struct user_driver_funcs   (Wine C ABI)
                        winexbox.drv                                           [MISSING - piece 1]
                              |  winexbox::IWinexboxDriver                     [HAVE, winexbox/include]
              +---------------+----------------+
        headless backend [HAVE]       D3D12 / CoreWindow backend                [MISSING - piece 2]
```

Two pieces are missing, and they were scoped as two deliverables from the start (M3 landed the
interface and the headless backend deliberately, pending console probe data). The interface is
already designed against the real Wine call sites — `winexbox/README.md` has a method-by-method
table mapping every `IWinexboxDriver` entry to the `user_driver_funcs` slot and the win32u source
file that calls it, plus a 6-step binding plan. Follow that table; do not redesign it.

### Shortest path to something visible

Ship the smallest thing that puts light on the TV, then iterate. A solid-colour window with a
title bar **is** success for this step.

1. **`winexbox.drv`** — the C adapter. Copy `null_user_driver`, override the M3-REQUIRED slots,
   each one a trampoline into the global `IWinexboxDriver`. `WINE_GDI_DRIVER_VERSION` is **108**
   in the vendored tree and must match exactly or win32u rejects the driver. win32u selects it in
   `driver.c` (`load_desktop_driver` reads the `GraphicsDriver` registry value, then the
   `NtUserLoadDriver` callback), so register `winexbox` under that value.
2. **Prove it headless, in CI, before touching the console.** Target: a guest that calls
   `CreateWindowExW`, pumps `GetMessage`, does a `GetDC`/`FillRect`, and the test reads the pixel
   back through `IWinexboxDriver::WindowSurface`. That is step 6 of the README's binding plan and
   it runs on both Linux-Wine and real-Windows CI with no hardware.
3. **The presenter.** `CreateWindowSurface` keeps returning system-memory BGRA32 (dibdrv
   rasterises on the CPU — the driver never rasterises). `ISurface::Flush`/`Present` copies dirty
   rects into a texture and composites onto the swap chain of the app's single `CoreWindow`. A UWP
   app has exactly one top-level surface, so guest top-level windows composite into it by z-order
   from `WindowPosChanged`.
4. **Input, after pixels.** CoreWindow + `Windows.Gaming.Input` feed `InjectInput`; gamepad maps
   to synthesized keys or XInput. Do not block the first screen on this.

Known console unknowns to settle while building piece 3: D3D12 feature level (11_0 vs 12_1 on
Series S in Dev Mode), swap chain on `CoreWindow`, and whether `d3d11on12`/GLOn12 are present.
Probe them from inside the running package rather than assuming.

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
ctest --test-dir build-mingw --label-exclude wine_conformance   # 26/26 expected
ctest --test-dir build-mingw -L guest_e2e                       # 14/14 expected
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
2. **Close the `guest_e2e` Windows gate** (task #12) *while* doing 1, not before it. Those tests
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

Open tasks worth reading: #12 (Windows stdio capture / gating), #13 (port the host module table
to the other two `crash.cpp` copies), #15 (use NT paths so `HostPathOfHandle` works inside an
AppContainer — measured to work, would replace byte-comparison identity).

## Two loose ends

* `FlsGetValue2` is missing from our `kernelbase` builtin, and
  `api-ms-win-appmodel-runtime-l1-1-2.dll` fails to load. Neither blocked the hardware run, but
  the second is API-set plumbing that graphics work will likely need.
* `integration.host_version` is environment-sensitive: Wine's `wine32 is missing` noise can
  precede the `^win10xbox` anchored regex. It passes in some environments and fails in others.
