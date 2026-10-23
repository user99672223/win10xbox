# Fetches ninja (https://github.com/ninja-build/ninja), the build system, as a guest for the layer: a real program
# that is not ours and that we do not build (docs/slices/realninja.md). It is the project's official prebuilt Windows
# x64 release (ninja-win.zip of one pinned tag), checked against a pinned SHA-256 on EVERY run, including when ci.yml
# restored it from the cache (the cache key carries the hash), so the bytes run are always the ones this was measured
# with. Only ninja.exe goes into the machine folder's guest dir.
#
#   tools/ci/fetch_ninja.ps1 [-Out build/bin/machine/guest] [-Work <dir>]
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'ninja-src'),
  [string] $Version = '1.12.1',
  [string] $Sha256 = 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a'
)
$ErrorActionPreference = 'Stop'
$url = "https://github.com/ninja-build/ninja/releases/download/v$Version/ninja-win.zip"
$zip = Join-Path $Work "ninja-$Version-win.zip"
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Zip {
  (Test-Path $zip) -and ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant() -eq $Sha256)
}
if (-not (Test-Zip)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    try {
      Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
      if (Test-Zip) { $ok = $true; break }
      Write-Host "attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()), expected $Sha256"
    } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download ninja $Version with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "ninja-$Version-win.zip present, SHA-256 verified"
}

$dir = Join-Path $Work "ninja-$Version"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
Expand-Archive -Path $zip -DestinationPath $dir -Force
$exe = Join-Path $dir 'ninja.exe'
if (-not (Test-Path $exe)) { Write-Host '::error::ninja.exe is not in the release zip'; exit 1 }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'ninja.exe') -Force
Write-Host "ninja.exe $((Get-Item $exe).Length) bytes -> $(Join-Path $Out 'ninja.exe')"
