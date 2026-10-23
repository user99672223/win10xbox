# Downloads the Sysinternals system-information tool Coreinfo64.exe (a program that is not ours; docs/slices/sysinfo.md)
# into the machine folder's guest directory, where integration.coreinfo and tools/ci/smoke_guests.ps1 run it through
# the layer. -IncludePsInfo also fetches PsInfo64.exe, which no test runs yet (it needs ws2_32, see the slice notes).
#
#   tools/ci/fetch_sysinternals.ps1 [-Out build/bin/machine/guest] [-Work <dir>] [-IncludePsInfo]
#
# The files come from https://live.sysinternals.com/ and are pinned by SHA-256, verified on EVERY run, including
# when ci.yml restored them from the cache (the cache key carries the same hash), so the bytes run are always
# the ones this was measured with. live.sysinternals.com serves the current release: when Microsoft publishes a new
# one the hash check fails loudly here (re-pin deliberately: new hash here and in the ci.yml cache key).
param(
  [switch] $IncludePsInfo,   # also fetch PsInfo64.exe (not gated by any test yet: docs/slices/sysinfo.md)
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'sysinternals')
)
$ErrorActionPreference = 'Stop'
$files = [ordered]@{
  'Coreinfo64.exe' = '4be1e87e17499e1237d0a444aae8e775ca876f983862c5fe27e376cd4bce4030'
}
if ($IncludePsInfo) { $files['PsInfo64.exe'] = '983270daa5a2ffa9c5c0c0a8d0f572bc5b6e68e5151ae84bdea7cf76a1d711b9' }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
New-Item -ItemType Directory -Force -Path $Out | Out-Null

foreach ($name in $files.Keys) {
  $sha = $files[$name]
  $path = Join-Path $Work $name
  $good = { (Test-Path $path) -and ((Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant() -eq $sha) }
  if (-not (& $good)) {
    $ok = $false
    foreach ($attempt in 1..3) {
      try {
        Invoke-WebRequest -Uri "https://live.sysinternals.com/$name" -OutFile $path -UseBasicParsing
        if (& $good) { $ok = $true; break }
        Write-Host "attempt ${attempt}: $name SHA-256 $((Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant()), expected $sha"
      } catch { Write-Host "attempt ${attempt}: $_" }
      Start-Sleep -Seconds (5 * $attempt)
    }
    if (-not $ok) { Write-Host "::error::could not download $name with the pinned SHA-256"; exit 1 }
  } else {
    Write-Host "$name present, SHA-256 verified"
  }
  Copy-Item $path (Join-Path $Out $name) -Force
  Write-Host "$name $((Get-Item $path).Length) bytes -> $(Join-Path $Out $name)"
}
