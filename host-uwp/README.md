# host-uwp

The **real** win10xbox UWP host (milestone **M5**). It is a C++/WinRT `CoreApplication` shell around the Host
(`host.h`): it runs the program named in `launch.json` through the win10xbox layer on an Xbox in Dev Mode, puts what the
guest draws on the TV (the presenter, below), and leaves its evidence as plain files in the app's `LocalState`.

It is **not** the throwaway validator. That is `host-uwp-lite` (see `host-uwp-lite/README.md`), which only smoke-tests the
loader and the dispatcher. `host-uwp` replaces it as the real host, and the two install side by side.

> **Status: built and packaged by CI (`.github/workflows/host-uwp.yml`); the console-only (headless) shell has passed on
> an Xbox Series S. The presenter is NEW and has NOT been run on a console.** CI proves its D3D11 upload + copy on a WARP
> device (`host_uwp.blit`, gating). In the non-gating `run` job, a windows-latest runner installed the package and
> launched it, and the presenter created a hardware device and a CoreWindow swap chain and presented
> (`display.presenter=D3D11 hardware swap chain ... presents=2`). Only the console can show the Game-bucket device rule
> and the TV output. Whether a guest window actually appears also depends on core/user (the NtUser*/NtGdi* emulation),
> which composes the frame.

## What happens when you launch it

1. The app activates its `CoreWindow` and lets activation settle. It creates the Host (whose winexbox display backend is
   the headless one, handed to core/user), brings up the presenter (D3D11 device on its own thread, swap chain and a first
   dark-grey frame on the UI thread), then starts the Host's worker thread. The UI thread only pumps events, forwards
   input and presents (`main.cpp`).
2. The shell installs the crash capture first thing (`crash.txt`, `crash.dmp`, `entry-reached.txt`; see Outputs below).
   The worker then opens the evidence trail: `trace.log` and `status.txt`.
3. It reads `launch.json`: `LocalState\launch.json` if there is one, else the packaged default `payload\launch.json`, else
   the built-in defaults. A file that exists but cannot be read or parsed is an error, not a silent fallback.
4. It stages the machine folder `LocalState\machine` (the guest's `C:\`) and points the guest's stdout and stderr at
   `LocalState\stdout.txt`.
5. `win10xbox::Initialize`, then `process::Launch` of the guest. With `exit_when_done` (default `true`) the app closes
   itself when the guest has exited; the packaged default sets it to `false`, so the last frame stays on the TV until you
   close the app.
6. The verdict is in `status.txt`, which is rewritten at each phase until the run ends.

## The presenter: pixels on the TV

The guest's `user32`/`gdi32` (Wine's PE builtins) reach core/user through `win32u` syscalls. core/user allocates window
surfaces from the winexbox backend and composes the desktop into one BGRA image (`win10xbox::user::ComposeFrame`,
`core/include/win10xbox/user.h`). The presenter (`presenter.h`, `presenter.cpp`, `blit.h`, `blit.cpp`) copies that image to
the screen:

- a D3D11 device (`D3D11CreateDevice`, hardware with BGRA support, WARP if that fails);
- a DXGI swap chain for the CoreWindow (`IDXGIFactory2::CreateSwapChainForCoreWindow`): `B8G8R8A8_UNORM`, flip-sequential,
  2 buffers, `DXGI_SCALING_STRETCH`, alpha ignored, sized to the CoreWindow in physical pixels;
- one default-usage BGRA texture the size of the virtual screen. Each pump round (about 60 Hz), if `user::FrameSerial()`
  moved, or at least every 500 ms, it composes into a CPU buffer, `UpdateSubresource`s the texture, copies it into the back
  buffer (only the overlapping region if the sizes differ) and calls `Present(1, 0)`. Until the first composed frame the
  screen is dark grey, which shows that the presenter itself works.

**Thread rules.** `D3D11CreateDevice` hangs when called on the UI thread of a Game-bucket app (measured on the console,
`docs/DECISIONS.md`), so it runs on a thread of its own. The swap chain is created and presented on the UI thread, from
the shell's pump. The shell waits up to 5 s for the device, pumping events, and does all of this **before** the Host's
worker starts, so the D3D/DXGI DLLs are loaded before the guest takes over the PEB's loader fields. If the device turns up
later it is still used. Nothing in the presenter can end the app: if the device, the swap chain or `Present` fails, the
app keeps running headless. A `display:` line in `trace.log` gives the reason and the HRESULT, and `display.presenter` in
`status.txt` gives the state.

**Sizes.** `CoreWindow::Bounds()` is in view pixels. The shell converts it with
`DisplayInformation::RawPixelsPerViewPixel()` (or `LogicalDpi / 96`), so the swap chain, the virtual monitor of the
backend (`HostInfo.display`) and `ComposeFrame` all use the same physical pixel size, and pointer positions are scaled to
match.

**After the guest exits** the presenter stops composing and keeps presenting the last frame, while the app stays open
(`"exit_when_done": false`).

**`screen.bmp`.** `LocalState\screen.bmp` is rewritten every 5 s while the guest runs, through a handle opened before the
guest started (no path is opened while the guest owns the PEB). It is written once more with `user::WriteScreenshot` when
the guest has exited, falling back to the last composed frame. Fetch it in Device Portal to see what was on screen without
looking at the TV. If nothing was ever composed, there is no `screen.bmp`.

**To see the window on the TV:** install the package and launch it. The packaged `launch.json` runs `window.exe`, a
640x400 window filled with solid blue, which quits after 3 s and exits with 42. The app then stays open on that last frame
until you close it. To run another guest, write `LocalState\launch.json` with `"guest": "<name>.exe"`. The bundled
guests are listed below.

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

`ctest` runs two console programs that need no layer, WinRT or package:

- `host_uwp_selftest` checks the launch.json parser, and also the packaged `payload/launch.json` and
  `payload/launch.example.json`.
- `host_uwp_blittest` runs the presenter's upload + copy (`blit.cpp`) on a WARP device. It uses frames of several sizes and
  strides, copies them into a 64x64 render target, reads the pixels back through a staging texture and checks every one.

Cache options (all optional):

| Option | Default | Meaning |
|--------|---------|---------|
| `HOST_UWP_PACKAGE_VERSION` | `1.0.0.0` | Package version `a.b.c.d`. CI passes `1.0.0.<run number>`, so each build installs over the last and keeps `LocalState`. |
| `HOST_UWP_BUILTIN_DIR` | `<repo>/wine-pe/out/builtin` | Wine PE builtin DLLs to bundle (the `wine-pe-builtin` artifact). Without them the package is still made; copy the DLLs into `LocalState\machine\Windows\builtin` by hand. |
| `HOST_UWP_SKIP_WINMD` | `OFF` | Skips `midl`, so the package has no WinRT metadata and the Xbox launcher refuses it. Debugging only. |

Build outputs in `build-uwp`: `bin\win10xbox-uwp.exe` (the host), the guests (see *The bundled guests*),
`bin\host_uwp_selftest.exe`, `bin\host_uwp_blittest.exe`, `winmd\Win10XboxUwp.winmd`, and, once packaged, `AppX\`
(the package layout) and `AppPackages\`.

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
4. The packaged default runs `window.exe`: a solid blue window should appear on the TV and stay there after the guest
   exits (after about 3 s). The verdict expects exit code 42, and the run ends after 600 seconds at the latest. Close the
   app when you have seen it. If the verdict is not PASS, see *If it fails* below. `screen.bmp` shows what was on screen.

## Outputs (all in the app's `LocalState`)

| File | Content |
|------|---------|
| `status.txt` | The verdict and facts. The first line is `RESULT: PASS`, `FAIL ...`, `EXITED ...` (no `expect_exit_code` set), `INCOMPLETE ...` (still starting, or the process died) or `FAIL (the host process crashed)`. A `hint` key says what to look at next. Read it first. |
| `trace.log` | Host lines (`host:`) and the core's trace, as selected by `trace`. Replaced at each start. Capped at `trace_log_max_mb` MiB (default 64); lines past the cap are dropped and a note says so. |
| `stdout.txt` | The guest's stdout and stderr. Stdin is an empty file. |
| `launch.example.json` | A copy of the packaged `payload\launch.example.json`, replaced at each start. It documents every `launch.json` key with its default. |
| `screen.bmp` | What was on the screen: the composed desktop as a 32-bit BMP. Rewritten every 5 s while the guest runs, and written with `user::WriteScreenshot` when it has exited. Absent if nothing was ever composed. Replaced at each start. |
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
| `guest` | string | `hello.exe` (the packaged default names `window.exe`) | A bare file name, looked up in `C:\guest`, or an absolute guest path such as `"C:\\Program Files\\App\\app.exe"`. |
| `args` | array or string | `[]` | Arguments, each quoted as needed; or one string appended verbatim. |
| `cwd` | string | `""` | The guest's current directory, an absolute guest path. `""` means `C:\Users\<user>`. |
| `trace` | string | `""` | A `WIN10XBOX_DEBUG` spec, e.g. `"+loader,+process"`. `""` is quiet (err and fixme only). You do **not** need to include `+modules`: the host appends it, so `entry-reached.txt` is written whatever you set here. |
| `winedebug` | string | `""` | A `WIN10XBOX_WINEDEBUG` spec for Wine's own channels, e.g. `"+loaddll,warn+module"`. |
| `trace_log_max_mb` | integer | `64` | Size cap of `trace.log` in MiB. `0` means unlimited. |
| `timeout_sec` | integer | `0` | Watchdog: end the run after this many seconds. `0` waits for the guest. |
| `exit_when_done` | boolean | `true` | Close the app when the guest has exited. |
| `expect_exit_code` | integer | (absent) | When set, `status.txt` says PASS or FAIL against this guest exit code. |
| `stage_native` | array of strings | `[]` | Bare DLL names, copied BY NAME from the console's `C:\Windows\System32` into the machine folder. |
| `input_pump` | boolean | `true` | Forward CoreWindow keyboard and pointer input to the winexbox display backend. |

The packaged default (`payload\launch.json`) is used when there is no `LocalState\launch.json`. It runs `window.exe`, sets
`expect_exit_code` to 42, `trace` to `+user,+gdi`, `exit_when_done` to `false` and `timeout_sec` to 600. To switch the
guest, write `LocalState\launch.json`, for example `{ "guest": "hello.exe", "expect_exit_code": 42 }`.

## The bundled guests

Every guest of the repo is in the package (`payload\guest\`) and is copied into `machine\guest\` (`C:\guest`), so
`"guest": "<name>"` is enough:

| Guest | Source | Kind | Exit code |
|-------|--------|------|-----------|
| `window.exe` | `tests/guest/window.c` if it exists, else the placeholder `guest/window.c` | GUI: a 640x400 window filled with solid blue, quits after 3 s. The packaged default. | 42 only if it was painted and its timer quit the loop (placeholder: 3 = the message loop ended otherwise, 4 = never painted, 2 = no window) |
| `hello.exe` | `guest/hello.c` | GUI subsystem, no window: facts to stdout | 42 |
| `hello_cui.exe` | `guest/hello.c` | the same as a console program | 42 |
| `smoke_cui.exe` | `host-uwp-lite/smoke_guest.c` | console: OS version, CPUID, clock, DLL facts | 42 |
| `guest_hello.exe` | `tests/guest/hello.c` | console: prints `hello from guest` | 42 |
| `guest_cpuid.exe` | `tests/guest/cpuid.c` | console: CPUID leaf 1 ECX and the vendor | 0 |
| `guest_sleep.exe` | `tests/guest/sleep.c` | console: sleeps (reaches `NtDelayExecution`) | 7 |
| `drv_client.exe` | `tests/drivers/drv_client.c` | console: talks to `\\.\OurTestDevice` | 0 with the driver host; 2 here |
| `guest_dll.dll` | `tests/guest/dll.c` | a DLL for loader tests, not a program | - |

`drv_client.exe` needs the driver host (`tests/drivers` `drv_host`, with `OurTestDevice.sys` loaded), which host-uwp does
not run. Under host-uwp it reports that the device cannot be opened and exits with 2. It is bundled so that it is already on
the console when the driver host is.

## If it fails

1. Read `status.txt`, then `trace.log`. If `entry-reached.txt` exists, the guest was running: read `crash.txt` and the end
   of `trace.log`.
2. A missing Wine builtin is reported in `status.txt`: copy the Wine PE DLLs into `LocalState\machine\Windows\builtin`.
3. A program that is not found: put it into `LocalState\machine\guest` (`C:\guest` in the guest), or change `guest` in
   `launch.json`.
4. More trace, without a rebuild: set `"trace": "+process,+loader,+vfs"` (`+modules` is appended for you) and, for Wine's own
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
configures the standalone project, builds it, runs `package.cmd`, runs both selftests (launch.json parser and payload
files; the D3D11 blit on WARP) and uploads artifact `host-uwp-msix` (the `.msix`, `.cer`, `.map`, `.pdb` and `.exe`,
kept 14 days). Fetching the Wine builtins from the latest successful `wine-pe` run is mandatory. The package assertion
checks the builtins, the package size, every bundled guest and that the packaged `launch.json` runs `window.exe`. It is
separate from `ci.yml` on purpose.

## Known limits and design notes

- **The presenter is not run on hardware.** CI proves that it builds and packages, that the launch.json selftest passes,
  and that the D3D11 upload + copy works on WARP. The CoreWindow swap chain, the hardware device and the hang rule can only
  be checked on the console.
- **The crash capture is a copy of host-uwp-lite's** (`crash.cpp`, `crash.h`, byte-identical, same namespace so the two
  stay diffable). The two gaps that used to exist here are fixed: `host_diag.cpp` now *chains* the capture's
  unhandled-exception filter instead of replacing it, so a hard crash still produces the UNHANDLED record, the dump and
  the FAIL status rewrite; and crash summaries now reach `trace.log`, because the shell opens the log first and hands the
  capture that handle. The capture runs before `host_diag`'s own report, so a near-exhausted stack is still handed to the
  capture's dedicated crash thread.
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
main.cpp              CoreApplication view; presenter bring-up; starts the worker; 60 Hz pump; exits when done
presenter.h / .cpp    D3D11 device thread, CoreWindow swap chain, Present, screen.bmp
blit.h / .cpp         the D3D11 upload + copy of the composed frame (also in the blit test)
pch.h                 umbrella include for the C++/WinRT translation units
host.h / host.cpp     the Host: launch.json -> staging -> Initialize -> process::Launch -> status.txt
host_diag.h / .cpp    trace.log, status.txt, the unhandled-exception filter
host_shell.h / .cpp   crash capture wiring, publication of launch.example.json
host_stage.h / .cpp   machine folder staging in LocalState
host_util.h / .cpp    small Win32 helpers
launch_config.h / .cpp  the launch.json parser (portable; also built into the selftest)
crash.h / crash.cpp   crash.txt / crash.dmp / entry-reached.txt (copied from host-uwp-lite)
guest/hello.c         the sample guest: facts to stdout, returns 42 (hello.exe / hello_cui.exe)
guest/window.c        placeholder GUI guest (window.exe) until tests/guest/window.c exists
payload/launch.json   the packaged default launch configuration
payload/launch.example.json  every key documented (copied to LocalState at each start)
selftest/selftest.cpp off-device checks of the launch.json parser and the payload files (ctest)
selftest/blit_test.cpp the presenter's blit on a WARP device, pixels read back (ctest)
assets/               tile images
```
