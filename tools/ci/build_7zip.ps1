# Builds 7-Zip's console program 7zr.exe from the official source with MSVC nmake (CPP/7zip/Bundles/Alone7z) as a
# guest for the layer: a real, multithreaded (LZMA/LZMA2) archiver that is not ours (docs/TESTING.md, "Real programs").
#
#   tools/ci/build_7zip.ps1 [-Out build/bin/machine/guest] [-Work <dir>]     (needs cl, nmake, ml64 on PATH: ilammy/msvc-dev-cmd)
#
# The source tarball (the GitHub release of the official ip7z/7zip repository) is pinned by SHA-256 and verified on
# EVERY run, including when ci.yml restored it from the cache (the cache key carries the same hash).
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'sevenzip-src'),
  [string] $Version = '24.09',
  [string] $Sha256 = '49c05169f49572c1128453579af1632a952409ced028259381dac30726b6133a'
)
$ErrorActionPreference = 'Stop'
$tag = $Version.Replace('.', '')
$url = "https://github.com/ip7z/7zip/releases/download/$Version/7z$tag-src.tar.xz"
$tar = Join-Path $Work "7z$tag-src.tar.xz"
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Tarball {
  (Test-Path $tar) -and ((Get-FileHash -Algorithm SHA256 $tar).Hash.ToLowerInvariant() -eq $Sha256)
}
if (-not (Test-Tarball)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    try {
      Invoke-WebRequest -Uri $url -OutFile $tar -UseBasicParsing
      if (Test-Tarball) { $ok = $true; break }
      Write-Host "attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $tar).Hash.ToLowerInvariant()), expected $Sha256"
    } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download 7z$tag-src.tar.xz with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "7z$tag-src.tar.xz present, SHA-256 verified"
}

$srcRoot = Join-Path $Work 'src'
if (Test-Path $srcRoot) { Remove-Item -Recurse -Force $srcRoot }
New-Item -ItemType Directory -Force -Path $srcRoot | Out-Null
# Relative names from inside $Work: a GNU tar would read "C:\..." as host:file.
Push-Location $Work
try { tar -xJf "7z$tag-src.tar.xz" -C src } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Host '::error::tar failed'; exit 1 }

$bundle = Join-Path $srcRoot 'CPP\7zip\Bundles\Alone7z'
$exe = Join-Path $bundle 'x64\7zr.exe'
Push-Location $bundle
try {
  # The makefiles build with -Wall -WX; the runner's newer SDK headers warn (C4865), so warnings are not fatal.
  $env:_CL_ = '-WX-'
  & nmake /nologo PLATFORM=x64
  if ($LASTEXITCODE -ne 0) { Write-Host '::error::nmake failed'; exit 1 }
} finally { Pop-Location }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out '7zr.exe') -Force
Write-Host "7zr.exe $((Get-Item $exe).Length) bytes -> $(Join-Path $Out '7zr.exe')"
