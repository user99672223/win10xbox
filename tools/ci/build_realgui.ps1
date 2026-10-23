# Builds real Win32 GUI programs with MSVC as guests for the layer (docs/TESTING.md, "Real GUI programs"):
#
#   hellowin  Microsoft's LearnWin32 "Hello World" (Windows-classic-samples, MIT): RegisterClass, CreateWindowEx
#             with WS_OVERLAPPEDWINDOW, ShowWindow, GetMessage loop, WM_PAINT with BeginPaint/FillRect. One source file.
#   putty     PuTTY 0.85 (MIT), the official source tarball, cmake + cl, /MT. `putty.exe -telnet ...` skips the
#             configuration dialog and creates the terminal window directly.
#
#   tools/ci/build_realgui.ps1 [-Out build/bin/machine/guest] [-Work <dir>]      (needs cl and cmake on PATH)
#
# Every download is pinned by SHA-256 and verified on EVERY run (also when restored from the cache): the bytes
# compiled are always the ones this was measured with. A mismatch fails the step.
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'realgui-src')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Work, $Out | Out-Null

function Get-Pinned([string] $Url, [string] $File, [string] $Sha256) {
  $ok = (Test-Path $File) -and ((Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant() -eq $Sha256)
  if ($ok) { Write-Host "$File present, SHA-256 verified"; return }
  foreach ($attempt in 1..3) {
    try {
      Invoke-WebRequest -Uri $Url -OutFile $File -UseBasicParsing
      if ((Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant() -eq $Sha256) { return }
      Write-Host "attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant()), expected $Sha256"
    } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  Write-Host "::error::could not download $Url with the pinned SHA-256"; exit 1
}

# ---- hellowin ----
$hwSrc = Join-Path $Work 'hellowin.cpp'
Get-Pinned 'https://raw.githubusercontent.com/microsoft/Windows-classic-samples/main/Samples/Win7Samples/begin/LearnWin32/HelloWorld/cpp/main.cpp' `
  $hwSrc 'a994644463d59064e2fd60b567b464d204a3652474edf7e93209be1aabc662b5'
$hwObj = Join-Path $Work 'hw-obj'
New-Item -ItemType Directory -Force -Path $hwObj | Out-Null
& cl /nologo /O2 /MT /W3 /EHsc "/Fo$hwObj\" "/Fe$(Join-Path $Work 'hellowin.exe')" $hwSrc user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS
if ($LASTEXITCODE -ne 0) { Write-Host '::error::cl hellowin failed'; exit 1 }
Copy-Item (Join-Path $Work 'hellowin.exe') (Join-Path $Out 'realgui_hellowin.exe') -Force

# ---- putty ----
$ver = '0.85'
$tgz = Join-Path $Work "putty-$ver.tar.gz"
Get-Pinned "https://the.earth.li/~sgtatham/putty/$ver/putty-$ver.tar.gz" $tgz '13fd4db2936d03b73812a7bcc2a658e4dd29cc776a56c3670a7fc6f1a0ee8af8'
$srcRoot = Join-Path $Work "putty-$ver"
if (Test-Path $srcRoot) { Remove-Item -Recurse -Force $srcRoot }
Push-Location $Work
try { tar -xzf "putty-$ver.tar.gz" } finally { Pop-Location }   # relative: GNU tar reads "C:\..." as host:file
if ($LASTEXITCODE -ne 0) { Write-Host '::error::tar failed'; exit 1 }
$bld = Join-Path $Work 'putty-build'
if (Test-Path $bld) { Remove-Item -Recurse -Force $bld }
& cmake -S $srcRoot -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { Write-Host '::error::putty cmake configure failed'; exit 1 }
& cmake --build $bld --target putty
if ($LASTEXITCODE -ne 0) { Write-Host '::error::putty build failed'; exit 1 }
$exe = Get-ChildItem $bld -Recurse -Filter putty.exe | Select-Object -First 1
if (-not $exe) { Write-Host '::error::putty.exe not found'; exit 1 }
Copy-Item $exe.FullName (Join-Path $Out 'realgui_putty.exe') -Force
Write-Host "realgui_hellowin.exe, realgui_putty.exe ($((Get-Item $exe.FullName).Length) bytes) -> $Out"
