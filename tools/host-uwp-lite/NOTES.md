# host-uwp-lite — side-task snapshot and first Xbox run

This directory is a snapshot of the `host-uwp-lite` side-task project (branch
`claude/youthful-cerf-l7xxsb` on the same `user99672223/win10xbox` repo),
together with the first Xbox-side run of the packaged artifact. **No code
was modified during capture.** Nothing outside `tools/host-uwp-lite/` was
touched on this branch.

## Exact versions pinned

| Thing | Value |
|---|---|
| Side-task source branch | `claude/youthful-cerf-l7xxsb` |
| Side-task source commit | `64687416c449c3526ca71a053de5ccbef36e6505` (short `6468741`) |
| Package consumed | `host-uwp-lite-msix` artifact from CI run `39647136661` of `.github/workflows/uwp-lite.yml` (`uwp-lite` workflow id `391993931`) |
| `.msix` filename | `win10xbox-uwp-lite.msix` (21 774 303 B) |
| `.cer` filename | `win10xbox-uwp-lite.cer` (752 B, self-signed `CN=Dev`) |
| Package Identity (manifest) | `win10xbox.uwp.lite` version `1.0.0.2` arch `x64` publisher `CN=Dev` |
| Installed PackageFullName | `win10xbox.uwp.lite_1.0.0.2_x64__eqad6j9zhtw80` |
| Installed PackageFamilyName | `win10xbox.uwp.lite_eqad6j9zhtw80` |
| AUMID | `win10xbox.uwp.lite_eqad6j9zhtw80!App` |
| Wine PE builtins artifact | `wine-pe-builtin` from CI run `39611835197` of `.github/workflows/wine-pe.yml`, artifact id `12350447777` (20 584 848 B) |
| Wine PE builtins source commit | `ff36f484d68b7dfc0059264a4d1f96badd32593b` |
| Wine source tag inside builtins | `wine-11.9` |
| Builtins toolchain | `llvm-mingw PE x86_64` |
| Full builtin inventory + SHA-256s | `wine-pe/MANIFEST.txt` |
| Wine source license | `wine-pe/LICENSE.wine` (LGPL-2.1+) |

Staged binary sizes inside the `.msix` (`payload/builtin/`):

| DLL | Bytes |
|---|---:|
| `ntdll.dll` | 3 284 992 |
| `kernelbase.dll` | 5 656 576 |
| `kernel32.dll` | 1 691 648 |

(Full set of 23 Wine PE builtins + their SHA-256 digests are in
`wine-pe/MANIFEST.txt`.)

## Device under test

| Field | Value |
|---|---|
| Device name | `XBOX` |
| OS build | `10.0.26100.9445` |
| OS version string | `26100.9445.amd64fre.xb_flt_2608ge.260927-0830` |
| OS edition | SystemOS (Dev Mode) |
| Partition | Game bucket |
| Architecture | x64 |
| Device Portal URL | `https://192.168.1.211:11443` |

## Deploy + pull procedure used

The pre-built `.msix` was fetched from GitHub Actions via the `nightly.link`
mirror (public-repo artifacts don't need auth there), then installed via
Device Portal REST. All subsequent reads of `LocalState` were REST GETs.
Writes to `LocalState` require a CSRF token obtained from a prior GET to any
Device Portal page.

### 1. Pull the pre-built artifact

```powershell
# Pin to the exact run so the same binary is reproducible.
$RUN = '39647136661'
Invoke-WebRequest "https://nightly.link/user99672223/win10xbox/actions/runs/$RUN/host-uwp-lite-msix.zip" `
                  -OutFile artifact.zip -UseBasicParsing
Expand-Archive artifact.zip -DestinationPath out
# -> out\win10xbox-uwp-lite.msix, out\win10xbox-uwp-lite.cer
```

### 2. Install to Xbox via Device Portal REST

```powershell
# PS 5.1 cert bypass (DP uses a self-signed cert); user: willy.
Add-Type @"
using System.Net;
using System.Security.Cryptography.X509Certificates;
public class TA : ICertificatePolicy {
    public bool CheckValidationResult(ServicePoint s, X509Certificate c, WebRequest w, int p) { return true; }
}
"@
[System.Net.ServicePointManager]::CertificatePolicy = New-Object TA
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$cred = Get-Credential      # user: willy, password: <your DP password>
$BASE = 'https://192.168.1.211:11443'
$msix = (Resolve-Path out\win10xbox-uwp-lite.msix).Path

# multipart POST of the msix file; package name only, DP picks partition from TDF
$filename = Split-Path $msix -Leaf
$uri = "$BASE/api/app/packagemanager/package?package=$([uri]::EscapeDataString($filename))"
$boundary = [Guid]::NewGuid().ToString()
$bytes = [IO.File]::ReadAllBytes($msix)
$enc = [Text.Encoding]::GetEncoding('iso-8859-1')
$h = "--$boundary`r`nContent-Disposition: form-data; name=`"$filename`"; filename=`"$filename`"`r`nContent-Type: application/octet-stream`r`n`r`n"
$f = "`r`n--$boundary--`r`n"
$body = [byte[]]::new($enc.GetByteCount($h) + $bytes.Length + $enc.GetByteCount($f))
[array]::Copy($enc.GetBytes($h), 0, $body, 0, $enc.GetByteCount($h))
[array]::Copy($bytes,            0, $body, $enc.GetByteCount($h), $bytes.Length)
[array]::Copy($enc.GetBytes($f), 0, $body, $enc.GetByteCount($h) + $bytes.Length, $enc.GetByteCount($f))
Invoke-RestMethod $uri -Method POST -Credential $cred -ContentType "multipart/form-data; boundary=$boundary" -Body $body -TimeoutSec 300

# poll install state until Code=0 (success) or Code<0 (failure)
while ($true) {
  Start-Sleep -s 2
  $s = Invoke-RestMethod "$BASE/api/app/packagemanager/state" -Credential $cred
  Write-Host ("  Code={0} Reason={1}" -f $s.Code, $s.Reason)
  if ($s.Code -eq 0 -or $s.Code -lt 0) { break }
}
```

The `.cer` does not need to be re-uploaded if the same publisher
(`CN=Dev`) was already trusted by an earlier package — e.g. the capability
probe. If first-ever install, upload it the same way to
`$BASE/api/app/packagemanager/certificate` with field name `cert`; this
returns 403 on a second install (benign).

### 3. Launch

Device Portal rejects programmatic launches of UWP apps on Xbox
(`/api/taskmanager/app` returns 403). Must be launched from **Dev Home**
on the console: Apps -> `win10xbox UWP smoke` -> Start. Keep in the
foreground. Internal watchdog is 10 minutes
(`UWP_LITE_TIMEOUT_SEC`).

### 4. Pull outputs from LocalState

All reads are plain GETs. `/api/filesystem/apps/files` listings on Xbox
return `SizeInBytes=0` for files (stale listing bug) — the sizes are
reported correctly only by downloading `apps/file` directly.

```powershell
$PFN = 'win10xbox.uwp.lite_1.0.0.2_x64__eqad6j9zhtw80'
foreach ($name in 'result.txt','stdout.txt','trace.log') {
  Invoke-WebRequest ("$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$PFN&path=\LocalState&filename=$name") `
                    -Credential $cred -OutFile $name -UseBasicParsing
}
```

### 5. Push patch files into LocalState (requires CSRF token)

Needed for the NLS workaround below. `multipart POST` to the same
`apps/file` endpoint, with `$sub` set to the subdirectory:

```powershell
$r = Invoke-WebRequest "$BASE/" -Credential $cred -UseBasicParsing -SessionVariable sess
$csrf = ($sess.Cookies.GetCookies($BASE) | Where-Object { $_.Name -match 'CSRF' } | Select -First 1).Value
$sub = '\LocalState\machine\Windows\System32'
$uri = "$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$PFN&path=$([uri]::EscapeDataString($sub))"
# (same multipart body build as step 2, but Invoke-WebRequest + -WebSession $sess
#  + -Headers @{ 'X-CSRF-Token' = $csrf })
```

Without `X-CSRF-Token`, every write returns `403 Forbidden CSRF Token Invalid`.

## NLS files patched into LocalState by hand (not shipped in this snapshot)

The Xbox's `C:\Windows\System32\` does not contain `l_intl.nls` or
`c_10000.nls` (`stage.cpp` reports both as `MISSING at source (error 2)` and
`nls.required_missing=l_intl.nls`). Both were copied from the laptop's own
`C:\Windows\System32\` into
`LocalState\machine\Windows\System32\` by hand using the CSRF-authenticated
`POST` described above, before the second run.

| Patched file | Source on laptop | Bytes | Target on Xbox |
|---|---|---:|---|
| `l_intl.nls` | `C:\Windows\System32\l_intl.nls` | 9 926 | `LocalState\machine\Windows\System32\l_intl.nls` |
| `c_10000.nls` | `C:\Windows\System32\c_10000.nls` | 66 082 | `LocalState\machine\Windows\System32\c_10000.nls` |

**These two files are deliberately NOT committed here** (Microsoft Windows
data files, licensing). The deploy procedure for a future run must either
re-upload them from a known-good source, or `host-uwp-lite` must grow its
own fallback.

## Prior-run artifacts

Only the second run's outputs (`result.txt`, `trace.log`) are committed in
`results/`. The first run (before the NLS patch) failed earlier with a
`CRASH:` line pointing at `NtGetNlsSectionPtr(l_intl.nls)` returning
`0xC0000034` STATUS_OBJECT_NAME_NOT_FOUND and the host unhandled-exception
filter firing in Wine's ntdll
(`code=0xc0000005 info0=0 info1=0x02 rip=0x170036862`); it is not committed
because `DIAGNOSIS.md` already summarizes it and the second run supersedes
it.

`stdout.txt` was present but zero bytes in both runs — the guest didn't
reach any `WriteFile` before dying.

## Files in this snapshot

```
tools/host-uwp-lite/
├── NOTES.md                     this file
├── README.md                    unmodified from the side-task branch
├── App.cpp / App.h / App.idl    activatable class XboxProbe.App -> Win10XboxLite.winmd
├── main.cpp                     headless IFrameworkView
├── smoke.cpp / smoke.h          Initialize/Launch/stage/result.txt
├── smoke_guest.c                the guest program (compiled to smoke.exe, smoke_cui.exe)
├── CMakeLists.txt               standalone CMake project
├── sdk.cmake                    midl/makeappx/makepri/signtool discovery
├── stage.cmake                  AppX layout assembly
├── package.cmd                  makepri + makeappx + signtool
├── Package.appxmanifest         Universal + Windows.Xbox TDF, codeGeneration
├── pch.h                        C++/WinRT precompiled header
├── .gitattributes               line-endings
├── assets/                      tile images (text-size)
├── wine-pe/
│   ├── MANIFEST.txt             DLL list + sizes + SHA-256 + wine tag
│   └── LICENSE.wine             LGPL-2.1+
└── results/
    ├── result.txt               verdict + facts (INCOMPLETE)
    ├── trace.log                588 lines, 62 004 B, all the way to NtContinue
    ├── SYSTEM32_INVENTORY.md    29 readable / 18 not-readable-by-name on Xbox
    └── DIAGNOSIS.md             92 unresolved KERNEL32 imports, 2 suspected causes
```
