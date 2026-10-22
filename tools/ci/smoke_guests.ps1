# Runs every guest program the build bundles through the real host executables and gates on each one's exit
# code, read from the process itself (not through a shell's rendering of it, and not through ctest). Used by the
# windows job of ci.yml (desktop machine folder) and by host-win32-smoke.yml (console-like machine folder, with
# -ConsoleConditions).
#
#   tools/ci/smoke_guests.ps1 -Bin build/bin -Machine build/bin/machine [-LogDir guest-smoke] [-ConsoleConditions] [-RealPrograms] [-Sysinternals]
#
# The table below is the set of guests this repository builds, with the exit code each one returns when it ran to
# completion (tests/guest/README.md, host-uwp-lite/README.md, tests/drivers). A guest in the table that was not built
# or is missing from the machine folder FAILS the run. A guest in the machine folder that is not in the table is
# listed (it is still gated by its ctest entry, if it has one) but does not fail this step, so a new guest never
# breaks CI before someone decides its expected exit code here.
# Rows marked Real = $true are programs that are not ours (Lua, built by tools/ci/build_lua.ps1, and the Sysinternals
# Coreinfo64/PsInfo64, fetched by tools/ci/fetch_sysinternals.ps1, into the machine folder's guest directory); they run, and fail when missing, only with -RealPrograms, which the workflows that
# build them pass. Args = the guest's command-line arguments.
param(
  [Parameter(Mandatory = $true)] [string] $Bin,
  [Parameter(Mandatory = $true)] [string] $Machine,
  [string] $LogDir = 'guest-smoke',
  [switch] $ConsoleConditions,
  [switch] $RealPrograms,
  [switch] $Sysinternals,
  [int] $TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$Bin = (Resolve-Path $Bin).Path
$Machine = (Resolve-Path $Machine).Path
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path $LogDir).Path
$host_exe = Join-Path $Bin 'win10xbox.exe'
$drv_host = Join-Path $Bin 'drv_host.exe'
$drivers = Join-Path $Machine 'Windows\System32\drivers'

# Stage the test drivers where the driver e2e tests put them (tests/drivers/run_driver_e2e.cmake).
New-Item -ItemType Directory -Force -Path $drivers | Out-Null
foreach ($sys in @(Get-ChildItem (Join-Path $Bin 'drivers') -Filter *.sys -ErrorAction SilentlyContinue)) {
  Copy-Item $sys.FullName -Destination $drivers -Force
}

# name, host, guest image (in <machine>\guest), driver (or ''), expected exit code, stdout regex (or ''), Args, Real
$table = @(
  @{ Name = 'guest_hello';     Host = $host_exe; Guest = 'guest_hello.exe';     Driver = '';                  Exit = 42; Out = 'hello from guest' },
  @{ Name = 'guest_cpuid';     Host = $host_exe; Guest = 'guest_cpuid.exe';     Driver = '';                  Exit = 0;  Out = 'cpuid\.1\.ecx=' },
  @{ Name = 'guest_sleep';     Host = $host_exe; Guest = 'guest_sleep.exe';     Driver = '';                  Exit = 7;  Out = 'guest slept' },
  @{ Name = 'guest_smoke_cui'; Host = $host_exe; Guest = 'guest_smoke_cui.exe'; Driver = '';                  Exit = 42; Out = 'guest: done, returning 42' },
  @{ Name = 'guest_smoke';     Host = $host_exe; Guest = 'guest_smoke.exe';     Driver = '';                  Exit = 42; Out = 'guest: done, returning 42' },
  @{ Name = 'drv_client';      Host = $drv_host; Guest = 'drv_client.exe';      Driver = 'OurTestDevice.sys'; Exit = 0;  Out = 'driver_ioctl: all checks passed' },
  @{ Name = 'drv_kmdf_client'; Host = $drv_host; Guest = 'drv_kmdf_client.exe'; Driver = 'OurKmdfDevice.sys'; Exit = 0;  Out = 'kmdf: all checks passed' },
  @{ Name = 'lua_hello';       Host = $host_exe; Guest = 'lua.exe';             Driver = '';                  Exit = 42; Out = 'hello from lua\t42'; Real = $true
     Args = @('-e', "print('hello from lua', 6*7) os.exit(42)") },
  @{ Name = 'lua_smoke';       Host = $host_exe; Guest = 'lua.exe';             Driver = '';                  Exit = 42; Out = 'smoke: done fails=0'; Real = $true
     Args = @('C:\guest\lua_smoke.lua', 'alpha', 'beta gamma') },
  @{ Name = 'coreinfo';        Host = $host_exe; Guest = 'Coreinfo64.exe';      Driver = '';                  Exit = 0;  Out = 'Logical to Physical Processor Map:'; Real = $true
     Args = @('-accepteula'); Sysinfo = $true },
  @{ Name = 'psinfo';          Host = $host_exe; Guest = 'PsInfo64.exe';        Driver = '';                  Exit = 0;  Out = 'Kernel version:'; Real = $true
     Args = @('-accepteula'); Sysinfo = $true }
)
# Sysinternals Coreinfo64/PsInfo64 (tools/ci/fetch_sysinternals.ps1, docs/slices/sysinfo.md) run only with -Sysinternals
# (ci.yml's windows job) and, like the GUI guests, on the builtin-only machine folder the integration.prepare_machine
# fixture leaves at <Bin>/machine-builtin.
if (-not $Sysinternals) { $table = @($table | Where-Object { -not $_.Sysinfo }) }
if (-not $RealPrograms) {
  Write-Host "real programs not run here (no -RealPrograms): $(($table | Where-Object { $_.Real } | ForEach-Object { $_.Name }) -join ', ')"
  $table = @($table | Where-Object { -not $_.Real })
}

$saved = @{}
if ($ConsoleConditions) {
  # The console's AppContainer refuses host path lookups (host-win32-smoke.yml explains the hook).
  $saved['WIN10XBOX_TEST_NO_HOST_PATHS'] = $env:WIN10XBOX_TEST_NO_HOST_PATHS
  $env:WIN10XBOX_TEST_NO_HOST_PATHS = '1'
}

$fail = 0
$rows = @()
foreach ($t in $table) {
  $guest_file = Join-Path $(if ($t.Sysinfo) { Join-Path $Bin 'machine-builtin' } else { $Machine }) (Join-Path 'guest' $t.Guest)
  $missing = @()
  if (-not (Test-Path $t.Host)) { $missing += $t.Host }
  if (-not (Test-Path $guest_file)) { $missing += $guest_file }
  if ($t.Driver -and -not (Test-Path (Join-Path $drivers $t.Driver))) { $missing += (Join-Path $drivers $t.Driver) }
  if ($missing.Count -gt 0) {
    Write-Host "::error::$($t.Name): not built or not staged: $($missing -join ', ')"
    $rows += ('{0,-16} NOT RUN (missing: {1})' -f $t.Name, ($missing -join ', '))
    $fail = 1
    continue
  }
  $mach = if ($t.Sysinfo) { Join-Path $Bin 'machine-builtin' } else { $Machine }
  $hostArgs = @('--machine', $mach)
  if ($t.Driver) { $hostArgs += @('--driver', (Join-Path $drivers $t.Driver)) }
  $hostArgs += "C:\guest\$($t.Guest)"
  if ($t.Args) { $hostArgs += $t.Args }
  $out = Join-Path $LogDir "$($t.Name).stdout.log"
  $err = Join-Path $LogDir "$($t.Name).stderr.log"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $t.Host -ArgumentList ($hostArgs | ForEach-Object { '"' + $_ + '"' }) -RedirectStandardOutput $out `
         -RedirectStandardError $err -PassThru -NoNewWindow
  $null = $p.Handle
  $note = ''
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    $p.Kill(); $p.WaitForExit(); $note = " KILLED after $TimeoutSeconds s"
  }
  $sw.Stop()
  $rc = $p.ExitCode
  $stdout = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
  if ($null -eq $stdout) { $stdout = '' }
  $ok = (-not $note) -and ($rc -eq $t.Exit) -and ((-not $t.Out) -or ($stdout -match $t.Out))
  $why = @()
  if ($note) { $why += $note.Trim() }
  if ($rc -ne $t.Exit) { $why += ('exit {0} (0x{1:X8}), expected {2}' -f $rc, [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$rc), 0), $t.Exit) }
  if ($t.Out -and $stdout -notmatch $t.Out) { $why += "stdout lacks '$($t.Out)'" }
  $line = ('{0,-16} exit {1,-4} expected {2,-3} {3,6:N1} s  {4}' -f $t.Name, $rc, $t.Exit, $sw.Elapsed.TotalSeconds, $(if ($ok) { 'ok' } else { 'FAIL: ' + ($why -join '; ') }))
  Write-Host $line
  $rows += $line
  if (-not $ok) {
    $fail = 1
    Write-Host "::error::$($t.Name): $($why -join '; ')"
    Write-Host "--- $($t.Name) stdout (last 40 lines):"
    if (Test-Path $out) { Get-Content $out -Tail 40 | ForEach-Object { Write-Host "  $_" } }
    Write-Host "--- $($t.Name) stderr (last 40 lines):"
    if (Test-Path $err) { Get-Content $err -Tail 40 | ForEach-Object { Write-Host "  $_" } }
  }
}
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }

$known = $table | ForEach-Object { $_.Guest }
if (-not $RealPrograms) { $known += 'lua.exe' }
if (-not $Sysinternals) { $known += 'Coreinfo64.exe', 'PsInfo64.exe' }   # not built here; not "unlisted" either
$extra = @(Get-ChildItem (Join-Path $Machine 'guest') -Filter *.exe -ErrorAction SilentlyContinue |
           Where-Object { $known -notcontains $_.Name } | ForEach-Object { $_.Name })
if ($extra.Count -gt 0) {
  Write-Host "guests without an entry in tools/ci/smoke_guests.ps1 (gated by their ctest entries only): $($extra -join ', ')"
}

$rows | Set-Content (Join-Path $LogDir 'summary.txt')
if ($env:GITHUB_STEP_SUMMARY) {
  Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value "### every bundled guest through the host$(if ($ConsoleConditions) { ' (console conditions)' })"
  Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value '```'
  Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value $rows
  Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value '```'
}
if ($fail) { Write-Host 'guest smoke: FAIL'; exit 1 }
Write-Host "guest smoke: PASS ($($table.Count) guests, every exit code as expected)"
exit 0
