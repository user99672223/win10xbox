# Builds the SDL2 guest for the layer (docs/slices/realsdl.md): tests/guest/sdl2/sdl_rects.c compiled with cl /O2 /MT
# against the OFFICIAL SDL2 2.30.9 prebuilt Windows x64 release (SDL2-devel-2.30.9-VC.zip: headers, SDL2.lib import
# library, SDL2.dll). Both sdl_rects.exe and the unmodified SDL2.dll are copied to <Out>, so the guest loads a real
# third-party DLL from its own directory (C:\guest).
#
#   tools/ci/build_sdl_guest.ps1 [-Out build/bin/machine/guest] [-Work <dir>]      (needs cl on PATH)
#
# The zip is pinned by SHA-256 and verified on EVERY run, including when ci.yml restored it from the cache (the cache
# key carries the same hash), so the bytes used are always the ones this was measured with.
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'sdl2-src'),
  [string] $Version = '2.30.9',
  [string] $Sha256 = '8c91d91e5bcb997d062ec2b553c53832ebf95654d4aa35e8c02a954d4ce752ae'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/SDL2-devel-$Version-VC.zip"
$zip = Join-Path $Work "SDL2-devel-$Version-VC.zip"
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
  if (-not $ok) { Write-Host "::error::could not download SDL2-devel-$Version-VC.zip with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "SDL2-devel-$Version-VC.zip present, SHA-256 verified"
}

$dir = Join-Path $Work "SDL2-$Version-VC"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
Expand-Archive -Path $zip -DestinationPath $dir -Force
$sdl = Join-Path $dir "SDL2-$Version"
$dll = Join-Path $sdl 'lib/x64/SDL2.dll'
$lib = Join-Path $sdl 'lib/x64/SDL2.lib'
foreach ($f in @($dll, $lib, (Join-Path $sdl 'include/SDL.h'))) {
  if (-not (Test-Path $f)) { Write-Host "::error::$f is not in the release zip"; exit 1 }
}
$obj = Join-Path $Work 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$exe = Join-Path $Work 'sdl_rects.exe'
& cl /nologo /O2 /MT "/I$(Join-Path $sdl 'include')" "/Fo$obj\" "/Fe$exe" (Join-Path $repo 'tests/guest/sdl2/sdl_rects.c') `
  $lib user32.lib gdi32.lib /link /SUBSYSTEM:CONSOLE
if ($LASTEXITCODE -ne 0) { Write-Host '::error::sdl_rects link failed'; exit 1 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'sdl_rects.exe') -Force
Copy-Item $dll (Join-Path $Out 'SDL2.dll') -Force
Write-Host "sdl_rects.exe $((Get-Item $exe).Length) bytes, SDL2.dll $((Get-Item $dll).Length) bytes -> $Out"
