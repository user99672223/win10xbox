# Builds TinyCC 0.9.27 for Windows x64 from the upstream tarball with MSVC (win32/build-tcc.bat -c cl) and copies
# tcc.exe + include/ + lib/ to <Out> (default build/bin/machine/guest/tcc), so a guest C:\guest\tcc\tcc.exe can
# compile C:\guest\tcc\hello.c inside the layer (integration.real_tcc).
#   tools/ci/build_tcc.ps1 [-Out build/bin/machine/guest/tcc] [-Work <dir>]   (needs cl on PATH)
# The tarball is pinned by SHA-256 and verified on every run.
param(
  [string] $Out = 'build/bin/machine/guest/tcc',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'tcc-src'),
  [string] $Sha256 = 'de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c'
)
$ErrorActionPreference = 'Stop'
$urls = @('https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2',
          'https://download-mirror.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2',
          'https://mirrors.kernel.org/nongnu/tinycc/tcc-0.9.27.tar.bz2')
$tbz = Join-Path $Work 'tcc-0.9.27.tar.bz2'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
function Test-Tarball { (Test-Path $tbz) -and ((Get-FileHash -Algorithm SHA256 $tbz).Hash.ToLowerInvariant() -eq $Sha256) }
if (-not (Test-Tarball)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    foreach ($u in $urls) {
      try {
        Invoke-WebRequest -Uri $u -OutFile $tbz -UseBasicParsing
        if (Test-Tarball) { $ok = $true; break }
        Write-Host "${u}: bad SHA-256"
      } catch { Write-Host "${u}: $_" }
    }
    if ($ok) { break }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host '::error::could not download tcc-0.9.27.tar.bz2 with the pinned SHA-256'; exit 1 }
}
$root = Join-Path $Work 'tcc-0.9.27'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
Push-Location $Work
try { tar -xjf 'tcc-0.9.27.tar.bz2' } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Host '::error::tar failed'; exit 1 }
$w32 = Join-Path $root 'win32'
Push-Location $w32
try { cmd /c 'build-tcc.bat -c cl' } finally { Pop-Location }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $w32 'tcc.exe'))) { Write-Host '::error::build-tcc.bat failed'; exit 1 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item (Join-Path $w32 'tcc.exe') $Out -Force
if (Test-Path (Join-Path $w32 'libtcc.dll')) { Copy-Item (Join-Path $w32 'libtcc.dll') $Out -Force }
Copy-Item -Recurse -Force (Join-Path $w32 'include') $Out
Copy-Item -Recurse -Force (Join-Path $w32 'lib') $Out
Get-ChildItem -Recurse $Out | Select-Object -First 40 | ForEach-Object { Write-Host $_.FullName }
