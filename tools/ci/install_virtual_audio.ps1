# Installs a virtual render endpoint on a hosted Windows runner (docs/slices/xaudio2play-device.md), so that guest_wasapi and
# guest_xaudio2 play a real buffer to a device the OS enumerates. Nothing is faked: the device is a signed kernel-mode
# audio driver from an open release, hash-verified; the sound goes to its network / virtual sink (a null device).
#
# Order of preference (each is pinned; the download is checked against the SHA-256 below before anything runs):
#   1. Scream 4.0 virtual audio driver (github.com/duncanthrax/scream, MS-RL): Install-x64.bat's own step, devcon install of Scream.inf.
#   2. VB-CABLE driver pack 43 (vb-audio.com, donationware, free for this use): VBCABLE_Setup_x64.exe -i -h.
# The SHA-256 values are of the files as downloaded on 2026-10-23 (neither project publishes a checksum; they are the pin).
#
# After each attempt: the driver's signing certificate is trusted for the install (TrustedPublisher), Audiosrv is
# restarted, and the device counts as installed only when guest_wasapi (-Probe), run directly on this host, exits 42
# (played). Then WIN10XBOX_CI_AUDIO_ENDPOINT=1 is written to GITHUB_ENV, which turns the audio tests' accepted
# "no endpoint" answer (43) into a failure (tests/integration/audio_test.cmake). If no device could be installed the
# script prints the exact error of each attempt and exits 1; it never reports an endpoint it did not measure.
param(
  [Parameter(Mandatory = $true)][string] $Probe,        # build/bin/guest/guest_wasapi.exe
  [string] $WorkDir = (Join-Path $env:RUNNER_TEMP 'virtual-audio')
)
$ErrorActionPreference = 'Stop'

$Scream = @{
  Name = 'Scream 4.0'
  Url  = 'https://github.com/duncanthrax/scream/releases/download/4.0/Scream4.0.zip'
  Sha256 = 'fa33e25f9a46c61e4e0cd83362c51c3d2a45c6fe4091aad7507e240e40f1a520'
}
$VbCable = @{
  Name = 'VB-CABLE driver pack 43'
  Url  = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip'
  Sha256 = '66fd0a4d9f4896ff41632b7e3d53892c085c4561f53e8ae8d0f0bc10eedd1cdd'
}

function Get-Pinned($pin, $dir) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $zip = Join-Path $dir 'pkg.zip'
  Invoke-WebRequest -Uri $pin.Url -OutFile $zip -UseBasicParsing
  $h = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
  if ($h -ne $pin.Sha256) { throw "$($pin.Name): sha256 $h, pinned $($pin.Sha256) ($($pin.Url))" }
  Write-Host "$($pin.Name): $($pin.Url) sha256 $h verified"
  $x = Join-Path $dir 'x'
  Expand-Archive -Path $zip -DestinationPath $x -Force
  return $x
}

# Runs a program with a time limit and returns its exit code (-1 on a timeout, after killing the process tree). A driver
# install that waits for a prompt a headless runner cannot answer must fail with an error, never hang the job.
function Invoke-Limited($exe, [string[]]$arguments, $dir, $seconds) {
  $o = Join-Path $WorkDir ([guid]::NewGuid().ToString('N') + '.out'); $e = "$o.err"
  $sp = @{ FilePath = $exe; WorkingDirectory = $dir; PassThru = $true; NoNewWindow = $true; RedirectStandardOutput = $o; RedirectStandardError = $e }
  if ($arguments.Count -gt 0) { $sp.ArgumentList = $arguments }
  $p = Start-Process @sp
  $done = $p.WaitForExit($seconds * 1000)
  if (-not $done) { & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null }
  Write-Host ("{0} {1}: {2}" -f (Split-Path $exe -Leaf), ($arguments -join ' '), $(if ($done) { "exit $($p.ExitCode)" } else { "TIMEOUT after ${seconds}s (killed)" }))
  foreach ($f in $o, $e) { if (Test-Path $f) { Get-Content $f -Raw | Write-Host } }
  if (-not $done) { return -1 }
  return $p.ExitCode
}

# Trust the publisher of a driver catalog, so that the install does not stop at the "would you like to install this
# device software" prompt (a headless runner cannot answer it).
function Trust-CatalogSigner($cat) {
  $sig = Get-AuthenticodeSignature -FilePath $cat
  if (-not $sig.SignerCertificate) { throw "$cat has no signer certificate (status $($sig.Status))" }
  Write-Host "$cat signed by: $($sig.SignerCertificate.Subject) (status $($sig.Status))"
  $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher', 'LocalMachine')
  $store.Open('ReadWrite'); $store.Add($sig.SignerCertificate); $store.Close()
  # a signer whose chain does not end in a trusted root (status UnknownError) is also trusted as a root, on this throwaway runner
  $root = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'LocalMachine')
  $root.Open('ReadWrite'); $root.Add($sig.SignerCertificate); $root.Close()
}

function Restart-Audio {
  foreach ($s in 'AudioEndpointBuilder', 'Audiosrv') {
    try { Restart-Service $s -Force -ErrorAction Stop } catch { Write-Host "Restart-Service ${s}: $_" }
  }
  Start-Sleep -Seconds 5
}

# 42 = guest_wasapi played the sound on a device this host enumerates.
function Test-Endpoint {
  for ($i = 1; $i -le 6; $i++) {
    Write-Host "guest_wasapi run directly on the host (try $i)"
    $rc = Invoke-Limited (Resolve-Path $Probe).Path @() (Get-Location).Path 60
    if ($rc -eq 42) { return $true }
    Start-Sleep -Seconds 5
  }
  return $false
}

function Show-Devices($title) {
  Write-Host "=== $title ==="
  Get-Service Audiosrv, AudioEndpointBuilder -ErrorAction Continue | Format-Table Name, Status, StartType | Out-String | Write-Host
  Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.Class -in 'AudioEndpoint', 'MEDIA' } |
    Format-Table Status, Class, FriendlyName, InstanceId | Out-String -Width 200 | Write-Host
}

function Install-Scream($dir) {
  $x = Get-Pinned $Scream $dir
  $drv = Join-Path $x 'Install\driver\x64'
  Trust-CatalogSigner (Join-Path $drv 'scream.cat')
  $devcon = Join-Path $x 'Install\helpers\devcon-x64.exe'
  Push-Location $drv
  try {
    $null = Invoke-Limited $devcon @('remove', '*Scream') $drv 60
    $rc = Invoke-Limited $devcon @('install', 'Scream.inf', '*Scream') $drv 120
    # devcon: 0 = done, 1 = reboot needed (the device is usually usable anyway), anything else failed
    if ($rc -lt 0 -or $rc -gt 1) { throw "devcon install Scream.inf: exit $rc (-1 = timed out waiting for a prompt)" }
  } finally { Pop-Location }
}

function Install-VbCable($dir) {
  $x = Get-Pinned $VbCable $dir
  Trust-CatalogSigner (Join-Path $x 'vbaudio_cable64_win7.cat')
  $rc = Invoke-Limited (Join-Path $x 'VBCABLE_Setup_x64.exe') @('-i', '-h') $x 180
  if ($rc -lt 0) { throw "VBCABLE_Setup_x64.exe -i -h timed out waiting for a prompt" }
}

if (-not (Test-Path $Probe)) { throw "probe $Probe not found" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Show-Devices 'audio before the virtual device'

$errors = @()
$chosen = $null
foreach ($cand in @(@{ Pin = $Scream; Fn = { param($d) Install-Scream $d } }, @{ Pin = $VbCable; Fn = { param($d) Install-VbCable $d } })) {
  $name = $cand.Pin.Name
  Write-Host "::group::install $name"
  try {
    & $cand.Fn (Join-Path $WorkDir ($name -replace '[^A-Za-z0-9]', '_'))
    Restart-Audio
    Show-Devices "audio after installing $name"
    if (Test-Endpoint) { $chosen = $cand.Pin; Write-Host "::endgroup::"; break }
    $errors += "${name}: installed but guest_wasapi did not exit 42 on this host"
  } catch {
    $errors += "${name}: $($_.Exception.Message)"
    Write-Host "install $name failed: $_"
  }
  Write-Host "::endgroup::"
}

if (-not $chosen) {
  foreach ($e in $errors) { Write-Host "::error::virtual audio: $e" }
  exit 1
}
Write-Host "virtual render endpoint: $($chosen.Name) ($($chosen.Url), sha256 $($chosen.Sha256))"
"WIN10XBOX_CI_AUDIO_ENDPOINT=1" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"WIN10XBOX_CI_AUDIO_DEVICE=$($chosen.Name)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
exit 0
