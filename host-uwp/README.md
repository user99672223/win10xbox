# host-uwp

The **real** win10xbox UWP host (milestone **M5**). It is a headless C++/WinRT `CoreApplication` shell around the Host
(`host.h`): it runs the program named in `launch.json` through the win10xbox layer on an Xbox in Dev Mode, and leaves its
evidence as plain files in the app's `LocalState`.

It is **not** the throwaway validator. That is `host-uwp-lite` (see `host-uwp-lite/README.md`), which only smoke-tests the
loader and the dispatcher. `host-uwp` replaces it as the real host, and the two install side by side.

> **Status: built and packaged by CI (`.github/workflows/host-uwp.yml`). It has NOT been run on a console.**
> Running it is the owner's job; this README says how.

## What happens when you launch it

1. The app starts headless: no renderer and no D3D, the display is the headless winexbox backend. It activates its
   `CoreWindow`, then starts the Host's worker thread. The UI thread only pumps events (`main.cpp`).
2. The shell installs the crash capture first thing (`crash.txt`, `crash.dmp`, `entry-reached.txt`; see Outputs below).
   The worker then opens the evidence trail: `trace.log` and `status.txt`.
3. It reads `launch.json`: `LocalState\launch.json` if there is one, else the packaged default `payload\launch.json`, else
   the built-in defaults. A file that exists but cannot be read or parsed is an error, not a silent fallback.
4. It stages the machine folder `LocalState\machine` (the guest's `C:\`) and points the guest's stdout and stderr at
   `LocalState\stdout.txt`.
5. `win10xbox::Initialize`, then `process::Launch` of the guest. With `exit_when_done` (default `true`) the app closes
   itself when the guest has exited.
6. The verdict is in `status.txt`, which is rewritten at each phase until the run ends.

## Build and package

Requirements: Windows with MSVC, Ninja and CMake 3.25 or newer (`CMakeLists.txt:38`), run from an x64 Native Tools prompt
(CI uses `ilammy/msvc-dev-cmd`). It also needs the Windows SDK with `midl.exe`, the platform metadata (`*.winmd`) and the
C++/WinRT headers. `sdk.cmake` finds `midl`, `makeappx`, `makepri`, `signtool` and the metadata. No Visual Studio UWP
workload and no NuGet.

It is a standalone CMake project. Configure it directly, **not** through the top-level tree: the top-level
`-DWIN10XBOX_BUILD_UWP=ON` is refused on purpose (`host-uwp/CMakeLists.txt:41`).

```bat
cmake -S host-uwp -B build-uwp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build-uwp
ctest --test-dir build-uwp --output-on-failure
host-uwp\package.cmd build-uwp
```

`ctest` runs `host_uwp_selftest`, which checks the launch.json parser off-device: no layer, WinRT or package is needed.

Cache options (all optional):

| Option | Default | Meaning |
|--------|---------|---------|
| `HOST_UWP_PACKAGE_VERSION` | `1.0.0.0` | Package version `a.b.c.d`. CI passes `1.0.0.<run number>`, so each build installs over the last and keeps `LocalState`. |
| `HOST_UWP_BUILTIN_DIR` | `<repo>/wine-pe/out/builtin` | Wine PE builtin DLLs to bundle (the `wine-pe-builtin` artifact). Without them the package is still made; copy the DLLs into `LocalState\machine\Windows\builtin` by hand. |
| `HOST_UWP_SKIP_WINMD` | `OFF` | Skips `midl`, so the package has no WinRT metadata and the Xbox launcher refuses it. Debugging only. |

Build outputs in `build-uwp`: `bin\win10xbox-uwp.exe` (the host), `bin\hello.exe` and `bin\hello_cui.exe` (the sample
guests), `bin\host_uwp_selftest.exe`, `winmd\Win10XboxUwp.winmd`, and, once packaged, `AppX\` (the package layout) and
`AppPackages\`.

### What `package.cmd` does

`host-uwp\package.cmd [BUILDDIR]` turns the staged layout into a signed package. `BUILDDIR` defaults to `..\build-uwp`.

1. Checks that `BUILDDIR\AppX\win10xbox-uwp.exe` and `AppxManifest.xml` exist. If not, it asks for `cmake --build` first.
2. Finds `makepri`, `makeappx` and `signtool` through `BUILDDIR\sdk_tools.cmd` (written by CMake), falling back to PATH.
3. `makepri` writes `AppX\resources.pri` from the manifest and the tile assets.
4. `makeappx pack` writes `BUILDDIR\AppPackages\win10xbox-uwp.msix`.
5. Signing uses `BUILDDIR\sign\win10xbox-uwp.pfx`. If it is missing, a self-signed code-signing certificate `CN=Dev` is
   created (PowerShell `New-SelfSignedCertificate`) and reused afterwards.
6. `signtool sign /fd SHA256` signs the `.msix`. The public `win10xbox-uwp.cer` is copied to `BUILDDIR\AppPackages\`.

It prints the package family name `win10xbox.uwp_eqad6j9zhtw80` and the AUMID `win10xbox.uwp_eqad6j9zhtw80!App`.
`package.cmd` must stay CRLF (see `.gitattributes`).

### Install

- **Console (Dev Mode):** Device Portal at `https://<console-ip>:11443`, then Apps, Add: upload the `.cer` first, then the
  `.msix`. The manifest declares `Windows.Universal` and `Windows.Xbox`, so one package is meant for either bucket.
- **Laptop:** as administrator, `Import-Certificate -FilePath <the .cer> -CertStoreLocation Cert:\LocalMachine\Root`, then
  `Add-AppxPackage -Path <the .msix>`. `package.cmd` prints these lines.

## Run it on the console

1. Install as above, then launch **win10xbox UWP host** (AUMID `win10xbox.uwp_eqad6j9zhtw80!App`).
2. Keep it in the foreground until it closes itself. A UWP app that goes to the background is suspended.
3. Read `status.txt` first. In Device Portal: File explorer, `LocalAppData`, the `win10xbox.uwp_..._x64__eqad6j9zhtw80`
   folder, `LocalState`.
4. The packaged default expects the sample guest to exit with 42 and ends the run after 300 seconds. If the verdict is not
   PASS, see *If it fails* below.

## Outputs (all in the app's `LocalState`)

| File | Content |
|------|---------|
| `status.txt` | The verdict and facts. The first line is `RESULT: PASS`, `FAIL ...`, `EXITED ...` (no `expect_exit_code` set), `INCOMPLETE ...` (still starting, or the process died) or `FAIL (the host process crashed)`. A `hint` key says what to look at next. Read it first. |
| `trace.log` | Host lines (`host:`) and the core's trace, as selected by `trace`. Replaced at each start. Capped at `trace_log_max_mb` MiB (default 64); lines past the cap are dropped and a note says so. |
| `stdout.txt` | The guest's stdout and stderr. Stdin is an empty file. |
| `launch.example.json` | A copy of the packaged `payload\launch.example.json`, replaced at each start. It documents every `launch.json` key with its default. |
| `entry-reached.txt` | Exists only if control reached the guest (written when the core logs `guest entry reached`). If it exists and the run still failed, the guest was running. If it does not exist, the failure came before the guest (staging, loader or ntdll init). |
| `crash.txt` | Exception records (first-chance and UNHANDLED): code, address as module+offset, registers, bytes at RIP, stack words and the module list (the guest's list while it runs). At most 40 records. |
| `crash.dmp` | A minidump of the first fatal-looking exception on a guest thread, or of the unhandled one. Needs `dbghelp.dll` bound inside the AppContainer and a successful dump call; otherwise it is absent. It has no code pages: take the images from `machine\`. |
| `machine\` | The guest's `C:\`: `machine.json`, `Windows\builtin\`, `guest\`, the NLS data and any `stage_native` DLLs. Kept between runs; files already there are reused, not copied again. Leave it. |
| `launch.json` | Yours to write. The package never creates or overwrites it. |

`crash.txt`, `crash.dmp` and `entry-reached.txt` are deleted when a new run starts.

## The launch.json configuration

`LocalState\launch.json` changes the run. It is JSON with `//` and `/* */` comments and trailing commas, and every key is
optional. An **unknown key is an error**, not a warning: the message lists the known keys. Keys beginning with `$` or `_`
are annotations and are ignored. `LocalState\launch.example.json` lists every key with its default.

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `guest` | string | `hello.exe` | A bare file name, looked up in `C:\guest`, or an absolute guest path such as `"C:\\Program Files\\App\\app.exe"`. |
| `args` | array or string | `[]` | Arguments, each quoted as needed; or one string appended verbatim. |
| `cwd` | string | `""` | The guest's current directory, an absolute guest path. `""` means `C:\Users\<user>`. |
| `trace` | string | `""` | A `WIN10XBOX_DEBUG` spec, e.g. `"+loader,+process"`. `""` is quiet (err and fixme only). Keep `+modules` in it, or `entry-reached.txt` is not written. |
| `winedebug` | string | `""` | A `WIN10XBOX_WINEDEBUG` spec for Wine's own channels, e.g. `"+loaddll,warn+module"`. |
| `trace_log_max_mb` | integer | `64` | Size cap of `trace.log` in MiB. `0` means unlimited. |
| `timeout_sec` | integer | `0` | Watchdog: end the run after this many seconds. `0` waits for the guest. |
| `exit_when_done` | boolean | `true` | Close the app when the guest has exited. |
| `expect_exit_code` | integer | (absent) | When set, `status.txt` says PASS or FAIL against this guest exit code. |
| `stage_native` | array of strings | `[]` | Bare DLL names, copied BY NAME from the console's `C:\Windows\System32` into the machine folder. |
| `input_pump` | boolean | `true` | Forward CoreWindow keyboard and pointer input to the winexbox display backend. |

The packaged default (`payload\launch.json`) is used when there is no `LocalState\launch.json`. It runs `hello.exe`, sets
`expect_exit_code` to 42, `trace` to `+modules` and `timeout_sec` to 300.

## The sample guests

- `hello.exe` (`guest/hello.c`, a GUI-subsystem program) is the default guest. `hello_cui.exe` is the same source built as
  a console program, for an A/B comparison. Both come from the package into `machine\guest\`.
- The guest writes a few facts to stdout and returns 42, so a first run ends in `RESULT: PASS` or `FAIL` in `status.txt`.

## If it fails

1. Read `status.txt`, then `trace.log`. If `entry-reached.txt` exists, the guest was running: read `crash.txt` and the end
   of `trace.log`.
2. A missing Wine builtin is reported in `status.txt`: copy the Wine PE DLLs into `LocalState\machine\Windows\builtin`.
3. A program that is not found: put it into `LocalState\machine\guest` (`C:\guest` in the guest), or change `guest` in
   `launch.json`.
4. More trace, without a rebuild: set `"trace": "+process,+loader,+vfs,+modules"` (keep `+modules`) and, for Wine's own
   channels, `"winedebug": "+loaddll,warn+module"`. Then relaunch.
5. If the guest does not exit, `timeout_sec` ends the run, and `status.txt` points at the last `relay` lines in `trace.log`.

## How it differs from host-uwp-lite

| | `host-uwp` (M5) | `host-uwp-lite` (validator) |
|---|---|---|
| Package identity | `win10xbox.uwp` | `win10xbox.uwp.lite` |
| Display name | `win10xbox UWP host` | `win10xbox UWP smoke` |
| Executable | `win10xbox-uwp.exe` | `win10xbox-uwp-lite.exe` |
| WinRT namespace | `Win10XboxUwp` (`Win10XboxUwp.App`, `Win10XboxUwp.winmd`) | `Win10XboxLite` (`Win10XboxLite.App`, `Win10XboxLite.winmd`) |
| What runs | The Host, driven by `launch.json` | A fixed smoke: `smoke.exe` by default (`guest.txt` can change it) |
| Verdict file | `status.txt` | `result.txt` |
| Capabilities | `codeGeneration`, `internetClient`, `internetClientServer`, `privateNetworkClientServer` | `codeGeneration` only |
| Workflow / artifact | `.github/workflows/host-uwp.yml` / `host-uwp-msix` | `.github/workflows/uwp-lite.yml` / `host-uwp-lite-msix` |

Because the identity, the executable and the WinRT namespace all differ, the two packages install side by side, each with
its own `LocalState`.

## CI

`.github/workflows/host-uwp.yml` (workflow `host-uwp`, job `msix`) runs on `windows-latest` with RelWithDebInfo. It
configures the standalone project, builds it, runs `package.cmd`, runs the launch.json selftest and uploads artifact
`host-uwp-msix` (the `.msix` and the `.cer`, kept 14 days). It fetches the Wine builtins from the latest successful
`wine-pe` run when one exists; without them the package is still produced. It is separate from `ci.yml` on purpose.

## Known limits and design notes

- **Not run on hardware.** CI proves that it builds, packages and passes the launch.json selftest, nothing more.
- **The crash capture is a copy of host-uwp-lite's** (`crash.cpp`, `crash.h`, same namespace so the two stay diffable).
  `host_shell.cpp` documents two differences: `host_diag.cpp` installs its own unhandled-exception filter, so an exception
  on a host thread that nothing handled gets no UNHANDLED record or dump from the capture; and crash summaries do not reach
  `trace.log`. The vectored handler still records first-chance exceptions.
- **`crash.dmp` needs `dbghelp.dll` inside the AppContainer.** It is delay-loaded and bound at startup. If the console
  refuses it, the package still starts and `crash.txt` says why no dump was written.
- **The guest runs in this process.** `process::Launch` takes over PEB fields (loader lists, process parameters, the loader
  lock), so the worker starts only after the window is activated and settled. Closing the window while the guest runs asks
  the worker to stop; if it does not react, EmergencyExit ends the process (`main.cpp`).
- **Capabilities:** `codeGeneration` (RWX for the loader); the three internet capabilities (the guest's socket syscalls
  pass through to the host kernel); no `broadFileSystemAccess`. System32 files are opened by name only, never listed
  (`Package.appxmanifest`, `host_stage.h`).

## Files

```
CMakeLists.txt        standalone project (layer + UWP exe + guests + selftest + winmd + staging)
sdk.cmake             finds midl/makeappx/makepri/signtool and the platform winmds
stage.cmake           assembles <build>/AppX (the package layout)
package.cmd           makepri + makeappx + signtool -> AppPackages\*.msix + .cer (keep CRLF)
Package.appxmanifest  AppContainer manifest, Universal + Xbox families, capabilities
App.idl               runtimeclass Win10XboxUwp.App -> Win10XboxUwp.winmd (midl)
App.h / App.cpp       IFrameworkViewSource + wWinMain (CoreApplication::Run)
main.cpp              headless CoreApplication view; starts the worker; exits when done
pch.h                 umbrella include for the C++/WinRT translation units
host.h / host.cpp     the Host: launch.json -> staging -> Initialize -> process::Launch -> status.txt
host_diag.h / .cpp    trace.log, status.txt, the unhandled-exception filter
host_shell.h / .cpp   crash capture wiring, publication of launch.example.json
host_stage.h / .cpp   machine folder staging in LocalState
host_util.h / .cpp    small Win32 helpers
launch_config.h / .cpp  the launch.json parser (portable; also built into the selftest)
crash.h / crash.cpp   crash.txt / crash.dmp / entry-reached.txt (copied from host-uwp-lite)
guest/hello.c         the sample guest: facts to stdout, returns 42 (hello.exe / hello_cui.exe)
payload/launch.json   the packaged default launch configuration
payload/launch.example.json  every key documented (copied to LocalState at each start)
selftest/selftest.cpp off-device checks of the launch.json parser (ctest)
assets/               tile images
```
