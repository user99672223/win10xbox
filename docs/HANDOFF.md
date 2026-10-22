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
* **No GUI at all.** `user32`, `gdi32full`, `shell32`, `comctl32` were reported *absent* —
  never loaded.
* **`drivers/` (8,397 lines) has never run on the console.** Only under Wine/mingw.
* **`winexbox/` (993 lines) is a headless stub.** No real display or GPU path.
* No COM, networking, audio, child processes, or graphics (M6 not started).

## The finding that should shape planning

There are **zero `NtUser*`/`NtGdi*` entries in the syscall table.** Those calls are not part of
ntdll — they are the **win32k** surface, reached through `win32u.dll`, and they form a *second,
separate* syscall table with on the order of a thousand entries. Our dispatcher implements the
ntdll table only. So GUI is not "load user32 and go": it is a second dispatch surface plus a
window manager. Budget accordingly.

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

1. **Close the `guest_e2e` Windows gate** (task #12). Those tests run on Windows but cannot fail
   the build, because guest stdio is not captured there. That is the exact hole that let four
   bugs sit in "green" code. Highest value per hour: it converts one-off proofs into standing
   protection.
2. **Run a guest that is not ours.** Something small, real and console-only. This will teach you
   more about the remaining 87% of the syscall surface than any other single action.
3. **Then the 21 `todo` syscalls**, driven by what step 2 actually hits.
4. **Then win32k/win32u** — the second syscall table — if GUI is the goal.
5. **M6 graphics** after that. Known gaps already recorded: child-process resume, D3D feature
   level 11_0 vs 12_1, GLOn12/Dozen/Vulkan bundling, WoW64 far jump.

Open tasks worth reading: #12 (Windows stdio capture / gating), #13 (port the host module table
to the other two `crash.cpp` copies), #15 (use NT paths so `HostPathOfHandle` works inside an
AppContainer — measured to work, would replace byte-comparison identity).

## Two loose ends

* `FlsGetValue2` is missing from our `kernelbase` builtin, and
  `api-ms-win-appmodel-runtime-l1-1-2.dll` fails to load. Neither blocked the hardware run, but
  the second is API-set plumbing that graphics work will likely need.
* `integration.host_version` is environment-sensitive: Wine's `wine32 is missing` noise can
  precede the `^win10xbox` anchored regex. It passes in some environments and fails in others.
