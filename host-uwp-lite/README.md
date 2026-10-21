# host-uwp-lite

A **throwaway** on-console validator for **M1 (loader)** and **M2 (dispatcher)**.
It is a minimal UWP C++/WinRT AppContainer app that, when launched on an Xbox
Series S in Dev Mode, runs the same smoke sequence as `host-win32/main.cpp`

```
Initialize(cfg) -> process::Launch(C:\guest\smoke.exe) -> dispatch::Shutdown() -> Shutdown()
```

on a worker thread and leaves its evidence as plain files in the app's
`LocalState`. It exists to prove the loader and the by-name syscall dispatch work
on genuine console silicon early; the real `host-uwp` stays milestone **M5**.
It only *drives* the public core API (`core.h`, `config.h`, `dispatch.h`,
`trace.h`); it contains no loader, dispatcher or kernel code, and it changes
nothing outside this directory except the separate workflow
`.github/workflows/uwp-lite.yml`.

> **Status: built and packaged by CI only. It has NOT been run on a console.**
> Running it is the owner's job; this README says how.

## What happens when you launch it

1. The app starts headless (no renderer, same shape as the capability probe's
   `tools/probe/uwp-host`), activates its `CoreWindow`, and starts a worker thread.
2. **Staging** into `LocalState\machine` (the guest's `C:\`):
   * `Windows\builtin\*.dll` - the Wine PE builtins (`ntdll`, `kernelbase`,
     `kernel32`, ...), shipped *inside the package* (CI downloads the
     `wine-pe-builtin` artifact, exactly like `ci.yml`). `ntdll`/`kernelbase`/
     `kernel32` are always the builtin (decision E1).
   * `Windows\System32\*.nls`, `Windows\Globalization\Sorting\sortdefault.nls` -
     the genuine NLS data Wine's ntdll needs, read **by name** from the
     console's own `C:\Windows\System32`.
   * `Windows\System32\ucrtbase.dll`, `msvcrt.dll` - genuine DLLs read by name,
     the ones the guest looks at (everything else a guest imports resolves to the
     Wine builtin through the VFS's `native,builtin` default).
   * `guest\smoke.exe`, `smoke_cui.exe` and `machine.json` (the repo's
     `machine.default.json`, i.e. the 22H2 defaults) from the package.
   * A read-only *inventory* of which `machine/guest_dlls.txt` modules can be
     opened by name in System32 (nothing copied): the D17 question for M5.
3. `stdout`/`stderr` of the host are pointed at `LocalState\stdout.txt` (a plain
   file handle, avoiding the real-Windows pipe-capture problem in `docs/STATUS.md`)
   and the core's trace sink at `LocalState\trace.log` (`WIN10XBOX_DEBUG=+all`, so
   the per-syscall `relay` channel is on; `+modules` is always appended, see below;
   `WIN10XBOX_WINEDEBUG=+loaddll,warn+module` adds Wine's own native-vs-builtin load
   lines and its "No implementation for KERNEL32.dll.<name>" warnings).
4. `Initialize` with the **defaults** (22H2 version spoof ON, D18 CPU identity,
   `AuthenticAMD`, hypervisor hidden; nothing is switched off), then `Launch`.
   The guest prints facts and returns **42**.
5. `result.txt` gets the verdict, then the app closes itself.

Two things run around that sequence (both in `crash.cpp`):

* **Crash capture**, installed first thing: a last-chance vectored handler, the
  unhandled-exception filter and a dump thread. They write `crash.txt` /
  `crash.dmp` (below). The first on-console run ended with the process simply gone
  right after `NtContinue` into the guest; this is what makes the next one readable.
* **Guest-entry marker.** The core logs `guest entry reached` on its `modules`
  trace channel immediately before `NtContinue` switches to the guest's
  `RtlUserThreadStart`; the trace sink sees that line and writes
  `entry-reached.txt` on the spot. The harness appends `+modules` to
  `WIN10XBOX_DEBUG` (whatever `smoke.env` says) so the marker cannot be switched off
  by accident.

## Outputs (all in the app's `LocalState`)

| File | Content |
|------|---------|
| `result.txt` | First line `RESULT: PASS` / `FAIL ...` / `INCOMPLETE ...`, then `key=value` facts: `initialize`, `exit_code`, `launch_status`, staging summaries, which NLS/DLL names could not be read, `system32.readable_by_name`, `hint`. PASS iff `Initialize == 0` and the guest exited with 42. It is rewritten at every phase boundary: if the process dies, it still says which `phase` it reached (and the unhandled-exception filter rewrites it as `RESULT: FAIL (the host process crashed)` with the exception, then the state before the crash). New keys: `guest.entry_reached`; `wine.no_implementation_warnings` (how many imports Wine's loader could not resolve: the first on-console run had 92 for `KERNEL32.dll`, a working run has **0**; it counts Wine's `warn+module` lines, which the harness switches on by default); and `crash.records` / `crash.dmp` when something fatal-looking was recorded. |
| `stdout.txt` | Everything the guest wrote to stdout/stderr: `hello from guest`, the OS version as the guest sees it (PEB / `GetVersionExW` / `RtlGetVersion`, expect 10.0.19045), `GetSystemInfo`, CPUID vendor / family / hypervisor bit / leaf `0x40000000` / brand string, rdtsc and QPC progress, and for `ucrtbase.dll` / `msvcrt.dll` whether the VFS handed out the staged **genuine** copy or the **Wine builtin**, with its file version. |
| `trace.log` | Harness lines (`harness:`, with timestamps) interleaved with the core's trace: `process`, `loader`, `vfs`, `modules` (module-load diagnostics: sections, views, the file-identity decisions, the guest's module list at entry), `relay` (every syscall with arguments and NTSTATUS), Wine's `loaddll`. Can be large. Near the top: whether the crash capture is armed (and whether dbghelp could be bound), and one `harness: module ...` line per `kernel32`, `user32`, `gdi32full`, `shell32`, `comctl32`: the path `GetModuleFileNameW` reports **in the host process** (or `absent`), taken before the guest launches. |
| `entry-reached.txt` | Exists only if control reached the guest: written the moment the core announces it is about to enter the guest (see above). Holds the marker line, a timestamp, the thread id and the host module paths. **If this file exists and the run still fails, the guest was running** (look at `crash.txt` and the end of `trace.log`); if it does not exist, the failure is in the staging / loader / ntdll initialisation before the guest. `result.txt` repeats it as `guest.entry_reached`. |
| `crash.txt` | Exists only if something fatal-looking happened. Readable exception records, appended: exception code and name, address (as `module+offset` or the memory region it is in), access-violation details, registers, the bytes at RIP, the stack words with `module+offset` for return addresses, the process's loader module list (while the guest runs that is the guest's list: `smoke.exe`, `ntdll.dll`, `kernelbase.dll`, ... with bases and sizes), the host module paths and `result.txt` as it was. For Wine's `EXCEPTION_WINE_STUB` (a call through an import that did not resolve) it names the DLL and function. Two kinds of record: *first-chance* (seen by the last vectored handler; the guest's own SEH may still handle it, so a record is not proof of a crash) and *UNHANDLED* (the process is about to die). At most 40 records. |
| `crash.dmp` | A minidump (`MiniDumpWriteDump`, written by a thread created at startup) of the **first** fatal-looking exception on a thread that runs guest code, or of the unhandled exception if nothing was dumped before. Contains the thread stacks and register state, thread and module lists, data segments and all private read-write memory (heaps, the guest's `.data`/`.bss` and stacks). It does **not** contain code pages: open it in WinDbg / Visual Studio and take the guest images from `machine\Windows\builtin` and `machine\guest` (`crash.txt` lists their bases). Can be tens of MB. |
| `machine\` | The staged machine folder; leave it, the next run reuses it (files whose size matches are skipped). |

## Build and package

CI (`.github/workflows/uwp-lite.yml`, separate from `ci.yml`) does it on
`windows-latest` and uploads artifact **`host-uwp-lite-msix`** (`.msix` + `.cer`).
Locally, in an **x64 Native Tools prompt**:

```bat
cmake -S host-uwp-lite -B build-uwp-lite -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build-uwp-lite
host-uwp-lite\package.cmd build-uwp-lite
rem -> build-uwp-lite\AppPackages\win10xbox-uwp-lite.msix  and  .cer (self-signed CN=Dev)
```

It is a standalone CMake project (its own `project()`, `add_subdirectory` of
`third_party/` and `core/` as the probe does with `probe-core`); the top-level
`CMakeLists.txt` is untouched. Needs MSVC + a Windows 10/11 SDK with its UWP
parts (`midl.exe`, platform metadata, C++/WinRT headers); no UWP Visual Studio
workload and no NuGet.

* `midl.exe` turns `App.idl` into `Win10XboxLite.winmd`, shipped next to the exe:
  the probe lesson is that the Xbox launcher silently refuses an exe whose
  manifest `EntryPoint` has no matching WinRT metadata. The C++ `App` is a
  hand-written `IFrameworkViewSource` (nothing activates it through a WinRT
  factory), so no cppwinrt code generation is needed.
* The CRT is static everywhere, so the package has no `Microsoft.VCLibs`
  dependency. `package.cmd` runs `makepri` + `makeappx` + `signtool`.
* The Wine builtins are bundled from `wine-pe/out/builtin` if present. If the
  `wine-pe-builtin` artifact was unavailable the package is still produced;
  `result.txt` then says `builtin.ntdll=MISSING` and you can drop the DLLs into
  `LocalState\machine\Windows\builtin` yourself. They are LGPL-2.1+ Wine 11.x PE
  builds (`payload\builtin\LICENSE.wine` and `MANIFEST.txt` travel with them).

## Run it on the console

One-time console setup (Dev Mode, Device Portal user/password, remote access) is
in `tools/probe/README.md`. Then, in Device Portal (`https://<console-ip>:11443`):

1. Apps -> **Add**: upload `win10xbox-uwp-lite.cer` first (it is the self-signed
   `CN=Dev` certificate from the same CI run), then the `.msix`. Pick the **Game**
   bucket (the probe's numbers were measured there). Each CI run has a higher
   version (`1.0.0.<run number>`), so a new package installs over the old one and
   keeps `LocalState`. (`tools/probe/deploy_xbox.ps1` calls the same Device Portal
   REST endpoints, but its package names are hard-wired to the probe; use the
   Device Portal UI or adapt the names.)
2. Launch **win10xbox UWP smoke** (AUMID `win10xbox.uwp.lite_eqad6j9zhtw80!App`;
   the publisher hash is the probe's, same `CN=Dev`).
3. **Keep it in the foreground** until it closes itself; a UWP app that goes to
   the background is suspended. How long a run takes is not known until it has been
   run on a console (staging copies tens of MB and the full relay trace is slow);
   the guest watchdog is 10 minutes (`UWP_LITE_TIMEOUT_SEC`), after which the
   harness writes the verdict and terminates the process.
4. Read `result.txt` first. In Device Portal: File explorer -> `LocalAppData` ->
   `win10xbox.uwp.lite_..._x64__eqad6j9zhtw80` -> `LocalState`, or over REST (the
   same endpoint the probe's deploy script uses):

   ```
   curl -k -u USER:PASS "https://<console-ip>:11443/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=win10xbox.uwp.lite_1.0.0.<N>_x64__eqad6j9zhtw80&path=\LocalState&filename=result.txt"
   ```

## If it fails: knobs without a rebuild

Create these text files in `LocalState` (Device Portal can upload them), then relaunch:

| File | Effect |
|------|--------|
| `guest.txt` | `<file in machine\guest> [args]`. Default `smoke.exe`. Try `smoke_cui.exe` (same program, console subsystem), `smoke.exe min` (facts only, no DLL probe) or `smoke.exe load` (also a real `LoadLibraryW` of the genuine `ucrtbase.dll`/`msvcrt.dll`, running their `DllMain` on the layer). |
| `smoke.env` | `NAME=VALUE` lines applied before `Initialize`, e.g. `WIN10XBOX_DEBUG=+relay,+loader`, `WIN10XBOX_WINEDEBUG=+loaddll,+module`, `UWP_LITE_TIMEOUT_SEC=120`. |
| `stage_extra.txt` | One extra System32 file name per line to copy by name into the machine folder (e.g. `advapi32.dll`). |

Anything else the guest needs can be dropped into `LocalState\machine\...` by hand.

## Known limits and design notes

* **Not run on hardware.** The CI job proves it builds and packages, nothing more.
* `smoke.exe` is a **GUI-subsystem** program as a precaution (a hypothesis, not a
  measurement): the AppContainer host has no console (`ConsoleHandle` is NULL), and
  the repo records trouble with Wine's kernelbase console path on real Windows
  (`docs/STATUS.md`; the comment in `BuildProcessParameters` in
  `core/dispatch/process.cpp` even notes a fault when `ConsoleHandle` is nulled).
  `smoke_cui.exe` is the same source built as a console program, for an A/B
  comparison. The guest writes with `WriteFile` straight to the inherited handle,
  not through CRT stdio.
* `process::Launch` runs the guest *in this process* and temporarily takes over
  PEB fields (loader lists, process parameters, loader lock). That is fine for a
  smoke but is why the real host-uwp is M5: shell-side WinRT work on other threads
  during that window is not guarded. The worker therefore starts only after the
  window is activated and settled, and the UI thread only pumps events meanwhile.
* The harness uses `Launch(wait=false)` + `WaitForGuest(timeout)` instead of
  `Launch(wait=true)`: identical (`Launch(wait)` just calls `WaitForGuest(INFINITE)`)
  plus a watchdog.
* `crash.dmp` needs `dbghelp.dll` inside the AppContainer, which nobody has tried on a
  console. It is **delay-loaded** (`/DELAYLOAD:dbghelp.dll`) and bound at startup with
  `__HrLoadAllImportsForDll`, so if the console refuses it the package still starts and
  `trace.log` says `dbghelp.dll could not be bound (HRESULT ...)`; `crash.txt` and
  `entry-reached.txt` do not depend on it. If `MiniDumpWriteDump` itself fails, the error
  is in `crash.txt` and `result.txt` (`crash.dmp=FAILED ...`).
* The crash handlers only use raw Win32 and fixed buffers (no CRT formatting, no heap),
  run with the host's TLS/FLS in the TEB (`HostModeScope`), read foreign memory only
  through `VirtualQuery`-checked copies, and fall back to a thread with a stack of its
  own when the faulting thread is nearly out of stack (a runaway recursion through the
  dispatcher is exactly that). A fail-fast (`int 0x29`, e.g. a CRT startup failure) does
  not run any user-mode handler at all: then `entry-reached.txt` plus the end of
  `trace.log` are all there is.
* The probe saw `C:\Windows\system32\kernel32.dll` not openable on the console
  while `ntdll.dll` was. It does not matter here (ntdll/kernelbase/kernel32 are
  never read from the console), but `system32.not_readable` in `result.txt` lists
  exactly which other names behave like that.
* Capabilities: only `codeGeneration` (RWX for the loader). No
  `broadFileSystemAccess`: individual System32 files open by name with default
  AppContainer rights (probe `A/map-system-dll-as-data`, `F/read:system32-read-ntdll`).

## Files

```
CMakeLists.txt        standalone project (core + UWP exe + guests + winmd + staging)
sdk.cmake             finds midl/makeappx/makepri/signtool and the platform winmds
stage.cmake           assembles <build>/AppX (the package layout)
package.cmd           makepri + makeappx + signtool -> AppPackages\*.msix + .cer (keep CRLF)
Package.appxmanifest  AppContainer manifest, Universal + Xbox families, codeGeneration
App.idl               runtimeclass Win10XboxLite.App -> Win10XboxLite.winmd (midl)
App.h / App.cpp       IFrameworkViewSource + wWinMain (CoreApplication::Run)
main.cpp              headless IFrameworkView; starts the worker; exits when done
smoke.h / smoke.cpp   staging, stdout/trace redirection, Initialize/Launch, result.txt
crash.h / crash.cpp   crash.txt / crash.dmp / entry-reached.txt, host module paths (dbghelp delay-loaded)
smoke_guest.c         the guest: facts to stdout, returns 42 (smoke.exe / smoke_cui.exe); also compiled
                      by .github/workflows/host-win32-smoke.yml to check import resolution on real Windows
pch.h                 umbrella include for the two C++/WinRT files
assets/               tile images (the probe's)
```
