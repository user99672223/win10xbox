# Fetches Tiny C Compiler 0.9.27 (https://repo.or.cz/w/tinycc.git), the official prebuilt Windows x64 zip
# (tcc-0.9.27-win64-bin.zip from the project's release area), as a guest for the layer: a real program that is not
# ours and that we do not build (docs/slices/realtcc.md). Checked against a pinned SHA-256 on EVERY run, including
# when ci.yml restored it from the cache (the cache key carries the hash). tcc.exe, libtcc.dll, include/, lib/ and
# libtcc/ of the zip's tcc/ directory go to <Out> (default build/bin/machine/guest/tcc), so C:\guest\tcc\tcc.exe finds
# its headers and libtcc1-64.a next to itself.
#
#   tools/ci/fetch_tcc.ps1 [-Out build/bin/machine/guest/tcc] [-Work <dir>]
param(
  [string] $Out = 'build/bin/machine/guest/tcc',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'tcc-src'),
  [string] $Sha256 = '34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2'
)
$ErrorActionPreference = 'Stop'
$name = 'tcc-0.9.27-win64-bin.zip'
$urls = @(
  "https://download.savannah.gnu.org/releases/tinycc/$name",
  "https://download-mirror.savannah.gnu.org/releases/tinycc/$name",
  "https://mirrors.kernel.org/nongnu/tinycc/$name")
$zip = Join-Path $Work $name
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Zip {
  (Test-Path $zip) -and ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant() -eq $Sha256)
}
if (-not (Test-Zip)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    foreach ($url in $urls) {
      try {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 120
        Write-Host "attempt ${attempt}: $url in $($sw.Elapsed.TotalSeconds) s"
        if (Test-Zip) { $ok = $true; break }
        Write-Host "SHA-256 $((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()), expected $Sha256"
      } catch { Write-Host "attempt ${attempt}: $url : $_" }
    }
    if ($ok) { break }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download $name with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "$name present, SHA-256 verified"
}

$dir = Join-Path $Work 'unpacked'
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
Expand-Archive -Path $zip -DestinationPath $dir -Force
$src = Join-Path $dir 'tcc'
foreach ($f in 'tcc.exe', 'libtcc.dll', 'lib/libtcc1-64.a', 'lib/msvcrt.def', 'include/stdio.h') {
  if (-not (Test-Path (Join-Path $src $f))) { Write-Host "::error::$f is not in the release zip"; exit 1 }
}
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
foreach ($f in 'tcc.exe', 'libtcc.dll', 'include', 'lib', 'libtcc') { Copy-Item (Join-Path $src $f) $Out -Recurse -Force }
Write-Host "tcc.exe $((Get-Item (Join-Path $Out 'tcc.exe')).Length) bytes -> $Out"
