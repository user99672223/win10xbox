# Builds the Lua 5.4.7 interpreter with MSVC as a guest for the layer: the first program that is not ours
# (docs/STATUS.md, "Real programs"). One static executable (/MT: no CRT DLL), console subsystem, every .c file of
# the distribution except luac.c compiled together with lua.c.
#
#   tools/ci/build_lua.ps1 [-Out build/bin/machine/guest] [-Work <dir>]      (needs cl on PATH: ilammy/msvc-dev-cmd)
#
# The tarball is pinned by SHA-256 and verified on EVERY run, including when ci.yml restored it from the cache
# (the cache key carries the same hash), so the bytes compiled are always the ones this was measured with.
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'lua-src'),
  [string] $Version = '5.4.7',
  [string] $Sha256 = '9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30'
)
$ErrorActionPreference = 'Stop'
$url = "https://www.lua.org/ftp/lua-$Version.tar.gz"
$tgz = Join-Path $Work "lua-$Version.tar.gz"
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Tarball {
  (Test-Path $tgz) -and ((Get-FileHash -Algorithm SHA256 $tgz).Hash.ToLowerInvariant() -eq $Sha256)
}
if (-not (Test-Tarball)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    try {
      Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
      if (Test-Tarball) { $ok = $true; break }
      Write-Host "attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $tgz).Hash.ToLowerInvariant()), expected $Sha256"
    } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download lua-$Version.tar.gz with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "lua-$Version.tar.gz present, SHA-256 verified"
}

$srcRoot = Join-Path $Work "lua-$Version"
if (Test-Path $srcRoot) { Remove-Item -Recurse -Force $srcRoot }
# Relative names from inside $Work: a GNU tar would read "C:\..." as host:file.
Push-Location $Work
try { tar -xzf "lua-$Version.tar.gz" } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Host '::error::tar failed'; exit 1 }
$src = Join-Path $srcRoot 'src'

$files = Get-ChildItem $src -Filter *.c | Where-Object { $_.Name -ne 'luac.c' } | ForEach-Object { $_.FullName }
$obj = Join-Path $Work 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$exe = Join-Path $Work 'lua.exe'
& cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS "/Fo$obj\" "/Fe$exe" @files
if ($LASTEXITCODE -ne 0) { Write-Host '::error::cl failed'; exit 1 }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'lua.exe') -Force
Write-Host "lua.exe $((Get-Item $exe).Length) bytes -> $(Join-Path $Out 'lua.exe')"
