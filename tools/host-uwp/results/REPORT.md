# host-uwp on Xbox Series S — run of 2026-10-22

**Verdict: `RESULT: PASS (the guest exited with the expected code 42)`**. That PASS needs one
manual step: the CI package shipped without the Wine PE builtins (see "CI defect" below), so
they were staged into LocalState by hand. With nothing added, the package fails before the
guest starts (run 1).

## What was tested

| | |
|---|---|
| Source branch / headSha | `claude/youthful-cerf-l7xxsb` @ `3ae78049c09dd800642b897beee0bb123948a7b2` |
| CI run | `host-uwp.yml` run `39809498419`, artifact `host-uwp-msix` (id `12439152644`) |
| Artifact contents | `win10xbox-uwp.msix` (1 446 483 B), `win10xbox-uwp.cer` (752 B), `win10xbox-uwp.exe`, `win10xbox-uwp.map` (7 105 502 B), `win10xbox-uwp.pdb` (25 505 792 B) |
| Package | `win10xbox.uwp_1.0.0.8_x64__eqad6j9zhtw80`, AUMID `win10xbox.uwp_eqad6j9zhtw80!App` |
| Coexisting package (left untouched) | `win10xbox.uwp.lite_1.0.0.4_x64__eqad6j9zhtw80` |
| Console | `XBOX`, Xbox Series S, SystemOS (Dev Mode) |
| Console OS build | `26100.9445.amd64fre.xb_flt_2608ge.260927-0830` |
| Wine PE builtins staged (runs 2–3) | `wine-pe-builtin` artifact `12350447777` from `wine-pe.yml` run `39611835197` (wine-11.9, source `ff36f48`); SHA-256 of ntdll/kernelbase/kernel32 match `tools/host-uwp-lite/wine-pe/MANIFEST.txt` |
| NLS staged (runs 2–3) | `l_intl.nls` (9 926 B), `c_10000.nls` (66 082 B) from the laptop's `C:\Windows\System32`, as in the host-uwp-lite run (not committed) |

## The three runs

| Dir | Launch config | What was in LocalState | Result |
|---|---|---|---|
| `run1-packaged-default/` | packaged default | only what the package stages | `RESULT: FAIL (no Wine PE builtins in the machine folder (Windows\builtin\ntdll.dll is missing))` |
| `run2-builtins-staged/` | packaged default | + 25 builtins in `machine\Windows\builtin`, + 2 NLS | `RESULT: PASS (the guest exited with the expected code 42)` |
| `run3-diagnostic-trace/` | `launch.json` (committed): `trace=+dispatch,+modules`, `winedebug=warn+module` | same as run 2 | `RESULT: PASS (the guest exited with the expected code 42)` |

Run 2 is the main result. Run 3 only adds trace channels, because two of the requested lines are
not logged at the default level: the startup `TEB.TlsSlots` line is `TRACE(dispatch)` and Wine's
`No implementation for` is `warn:module`. `launch.json` was deleted from the console afterwards.

`crash.txt` and `crash.dmp` were absent in all three runs (no crash). `entry-reached.txt` was absent
in run 1 and present in runs 2 and 3.

## Requested lines (verbatim)

**1. Guest exit code / PASS** (run 2 `status.txt`)
```
RESULT: PASS (the guest exited with the expected code 42)
guest.exit_code=42 (0x0000002a)
```
trace.log: `16:07:10.627 host: guest exited with code 42 (0x0000002a) after 78 ms`

**2. "host path of image file ... unavailable"**: **N = 5** (ERROR_ACCESS_DENIED) on every line. There are three in run 2, and kernelbase.dll has none:
```
0fbc:warn:modules:CreateImageSectionFromFile host path of image file ntdll.dll unavailable (Win32 error 5): sections are told apart by contents
0fbc:warn:modules:CreateImageSectionFromFile host path of image file <exe> unavailable (Win32 error 5): sections are told apart by contents
0868:warn:modules:CreateImageSectionFromFile host path of image file KERNEL32.dll unavailable (Win32 error 5): sections are told apart by contents
```

**3. dbghelp**: bound (every run):
```
16:07:10.414 host: shell: crash capture installed (last-chance vectored handler added, unhandled-exception filter set); crash thread 0x1b60 ready; dbghelp.dll bound: crash.dmp enabled; its one-line crash summaries go to trace.log
```

**4. TEB.TlsSlots**: slot 23 at startup and at guest entry.
Startup (run 3 only; `TRACE(dispatch)`):
```
01b8:trace:dispatch:Initialize ThreadState in TEB.TlsSlots[23] (TEB+0x1538); PEB.TlsBitmap=00007FFA8580F220
1918:trace:dispatch:ReserveGuestTlsSlot guest TLS bitmap 000000017009D3E8 (bits 000000B0EB933080, PEB.TlsBitmapBits 000000B0EB933080, 64 slots): slot 23 already in use, allocators share the PEB bits (first guest syscall)
```
Guest entry (identical in runs 2 and 3):
```
0868:trace:modules:NoteGuestEntry guest entry: TEB.TlsSlots[23] still holds our ThreadState
```

**5. "guest view" / "host state reached under the guest TLS view"**: **none** in any run. That
message is an unconditional `ERR(dispatch, ...)` in `core/dispatch/dispatcher.cpp`, so it not appearing
means the violation counter stayed at zero. The `TEB.TlsSlots[%lu] was overwritten` ERR is also absent.

**6. Guest entry**: reached, and `entry-reached.txt` **exists** (runs 2 and 3):
```
0868:trace:modules:NoteGuestEntry guest entry reached (RtlUserThreadStart at 00000001700568B4)
```

**7. "No implementation for KERNEL32"**: **0** lines in all runs, including run 3 with
`warn+module` on. The only Wine `module` warnings in run 3 are:
```
1918:warn:wine:module:LdrGetProcedureAddress "FlsGetValue2" (ordinal 0) not found in L"C:\\windows\\system32\\kernelbase.dll"
1918:warn:wine:module:load_dll Failed to load module L"api-ms-win-appmodel-runtime-l1-1-2.dll"; status=c0000135
```
The 92 unresolved KERNEL32 imports seen in the host-uwp-lite run (`tools/host-uwp-lite/results/DIAGNOSIS.md`) do not appear in this run.

Guest stdout (run 2): `hello from guest` … `guest: OS version 10.0 build 19045 (RtlGetVersion)` … `guest: done, returning 42`.

Other warnings worth noting (run 3): `warn:dispatch:MapImageView C:\Windows\System32\ntdll.dll: stub NtGetTickCount not rewritten`.

## Debugger

Nothing crashed or stopped, so there was no exception, faulting address, stack or register set to
symbolize against the `.map`/`.pdb`. What was in place:

- **Device Portal user-mode crash dumps** turned on for the package
  (`POST /api/debug/dump/usermode/crashcontrol?packageFullName=…`, then GET returned `{"CrashDumpEnabled" : true}`).
  After all three runs `/api/debug/dump/usermode/dumps` listed **no** dumps for `win10xbox.uwp`.
  It is still on.
- **The host's own crash capture** (last-chance vectored handler + unhandled-exception filter
  writing crash.txt/crash.dmp through dbghelp): installed and bound in every run, never fired.
- **Not available: a live first-chance debugger.** The Visual Studio remote debugger was not
  listening on the console (TCP 4024 and 4026 closed), and starting it requires pairing with the
  Visual Studio IDE. The laptop has no cdb/WinDbg. So first-chance access violations could not be
  trapped. The guest ran for 78 ms, too short for a Device Portal live dump
  (`/api/debug/dump/usermode/live`).

## CI defect found: the package is built without the Wine builtins

`host-uwp.yml` job "msix" (run `39809498419`), step *Fetch wine-pe builtin DLLs (optional)*,
logged:
```
no successful wine-pe run yet; the package will be built WITHOUT the Wine builtins
```
although `wine-pe.yml` run `39611835197` on the same branch succeeded and its `wine-pe-builtin`
artifact has not expired. The step's `gh run list --workflow wine-pe.yml ... --status success`
returned nothing. The REST API with `status=success` also returned 0 runs for this repo when tried
from the laptop, while the same query without the filter lists completed/success runs. Because the step is
`continue-on-error`, the job still went green and shipped a 1.4 MB msix with no builtins. Possible fix: select
on `conclusion == "success"` from an unfiltered list, and fail the step (or at least the run summary) when 0 DLLs are fetched.

## Device Portal notes (update to `tools/host-uwp-lite/NOTES.md`)

- `POST /api/taskmanager/app` **does** launch the app on this console when it carries the
  `X-CSRF-Token` header and base64-encoded `appid` / `package` parameters. The 403 recorded earlier
  was most likely the missing CSRF token, so launching from Dev Home is no longer required.
- `crashcontrol` accepts only `packageFullName=` (capital N). `packagefullname=` returns 400.

## Not committed

`.msix`, `.cer`, `.exe`, `.map`, `.pdb` (all in CI artifact `12439152644`), the Wine builtins (artifact
`12350447777`), and the two Microsoft NLS files.
