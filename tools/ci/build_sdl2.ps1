# Builds SDL2 2.30.9 (upstream release zip, SHA-256 pinned) with CMake + MSVC as a static library
# (video=windows, render=software/direct3d11, /MT) and links tests/guest/sdl2/sdl_rects.c against it into
# <Out>/sdl_rects.exe: the real SDL2 guest for integration.real_sdl2.
#   tools/ci/build_sdl2.ps1 [-Out build/bin/guest] [-Work <dir>]      (needs cl + cmake + ninja on PATH)
param(
  [string] $Out = 'build/bin/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'sdl2-src'),
  [string] $Version = '2.30.9',
  [string] $Sha256 = 'ec855bcd815b4b63d0c958c42c2923311c656227d6e0c1ae1e721406d346444b'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/SDL2-$Version.zip"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$zip = Join-Path $Work "SDL2-$Version.zip"
function Test-Zip { (Test-Path $zip) -and ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant() -eq $Sha256) }
if (-not (Test-Zip)) {
  foreach ($attempt in 1..3) {
    try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing; if (Test-Zip) { break } } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not (Test-Zip)) { Write-Host "::error::could not download SDL2-$Version.zip with the pinned SHA-256"; exit 1 }
}
$src = Join-Path $Work "SDL2-$Version"
if (-not (Test-Path $src)) { Expand-Archive -Path $zip -DestinationPath $Work -Force }
$bld = Join-Path $Work 'build'
$inst = Join-Path $Work 'install'
& cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$inst" `
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_FORCE_STATIC_VCRT=ON `
  -DSDL_AUDIO=OFF -DSDL_HAPTIC=OFF -DSDL_JOYSTICK=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_POWER=OFF `
  -DSDL_RENDER_D3D=ON -DSDL_DIRECTX=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF `
  -DCMAKE_C_COMPILER=cl
if ($LASTEXITCODE -ne 0) { Write-Host '::error::SDL2 configure failed'; exit 1 }
& cmake --build $bld --target install
if ($LASTEXITCODE -ne 0) { Write-Host '::error::SDL2 build failed'; exit 1 }
$lib = Get-ChildItem (Join-Path $inst 'lib') -Filter '*.lib' | Where-Object { $_.Name -notmatch 'main' } | Select-Object -First 1
Write-Host "SDL2 library: $($lib.FullName)"
$inc = Join-Path $inst 'include/SDL2'
$obj = Join-Path $Work 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$exe = Join-Path $Work 'sdl_rects.exe'
& cl /nologo /O2 /MT "/I$inc" "/Fo$obj\" "/Fe$exe" (Join-Path $repo 'tests/guest/sdl2/sdl_rects.c') $lib.FullName `
  user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib uuid.lib advapi32.lib setupapi.lib shell32.lib `
  /link /SUBSYSTEM:CONSOLE
if ($LASTEXITCODE -ne 0) { Write-Host '::error::sdl_rects link failed'; exit 1 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'sdl_rects.exe') -Force
Write-Host "sdl_rects.exe $((Get-Item $exe).Length) bytes -> $(Join-Path $Out 'sdl_rects.exe')"
