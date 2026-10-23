<#
.SYNOPSIS
  The prep steps of the ci.yml `windows` job, factored for a laptop (PowerShell 5.1 compatible, idempotent).

.DESCRIPTION
  Run from inside the MSVC x64 environment (Developer PowerShell / vcvars64) with the repository as the current
  directory or -RepoRoot. Two phases, in the order ci.yml has them, because the guest programs go into the build
  tree that `cmake --build` makes:

    -Phase Pre   before `cmake -S . -B build`: WDK km headers, wine-pe builtin + NLS artifacts, Dear ImGui v1.93.0,
                 llvm-mingw + Wine's ntdll conformance guest (build\winetest\ntdll\ntdll_test.exe).
                 Returns a hashtable: WdkKm (forward slashes), ImguiDir, WinetestBuilt.
    -Phase Post  after `cmake --build build`: machine\import_windows_dlls.ps1 (both calls, from the laptop's own
                 System32) and every tools\ci\build_*.ps1 / fetch_*.ps1 with CI's arguments.

  Everything downloaded is cached under %LOCALAPPDATA%\win10xbox\cache (-CacheDir). The tools\ci scripts default
  their work directories to $env:RUNNER_TEMP, so that variable is pointed at the cache for the child scripts.
  A step whose output already exists is skipped; -Force redoes all of them.
#>
[CmdletBinding()]
param(
  [ValidateSet('Pre', 'Post')] [string] $Phase = 'Pre',
  [string] $RepoRoot = (Get-Location).Path,
  [string] $BuildDir = 'build',
  [string] $CacheDir = (Join-Path $env:LOCALAPPDATA 'win10xbox\cache'),
  [string] $GitHubRepo = 'user99672223/win10xbox',
  [string] $WinePeBranch = '',
  [switch] $SkipWinetest,
  [switch] $Force
)
$ErrorActionPreference = 'Stop'
$LlvmMingwVer = '20250910'          # ci.yml env.LLVM_MINGW_VER
$ImguiTag = 'v1.93.0'               # ci.yml "Fetch Dear ImGui"

function Say($tag, $msg) { Write-Host ("[{0}] {1}" -f $tag, $msg) }
function Fail($msg) { Write-Host "[fail] $msg"; throw $msg }

$RepoRoot = (Resolve-Path $RepoRoot).Path
Set-Location $RepoRoot
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
$shellExe = (Get-Command pwsh -ErrorAction SilentlyContinue)
if ($shellExe) { $shellExe = $shellExe.Source } else { $shellExe = (Get-Command powershell).Source }

# Run a tools\ci script in a child shell (they are written for pwsh) with the cache as RUNNER_TEMP.
function Invoke-CiScript([string] $script, [string[]] $argList) {
  $old = $env:RUNNER_TEMP
  $env:RUNNER_TEMP = $CacheDir
  try {
    & $shellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot $script) @argList | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "$script exited $LASTEXITCODE" }
  } finally { $env:RUNNER_TEMP = $old }
}

# One guest program: skip when its marker file exists, else run the script with CI's arguments.
function Invoke-Guest([string] $name, [string] $script, [string[]] $argList, [string] $marker) {
  if (-not $Force -and (Test-Path $marker)) { Say 'ok' "$name already in place ($marker)"; return }
  Say 'install' $name
  Invoke-CiScript $script $argList
  if (-not (Test-Path $marker)) { Fail "$name: $marker missing after $script" }
  Say 'ok' $name
}

if ($Phase -eq 'Pre') {
  # ---- WDK km headers (ci.yml: Fetch the WDK km headers) ----
  $wdkDest = Join-Path $CacheDir 'wdk'
  $hit = @(Get-ChildItem (Join-Path $wdkDest 'Include') -Filter ntddk.h -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like '*\km' })
  if ($hit.Count -eq 0) {
    Say 'install' 'WDK km headers (NuGet Microsoft.Windows.WDK.x64, pinned)'
    Invoke-CiScript 'tools/ci/fetch_wdk_km.ps1' @('-Dest', $wdkDest)
    $hit = @(Get-ChildItem (Join-Path $wdkDest 'Include') -Filter ntddk.h -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like '*\km' })
  }
  if ($hit.Count -eq 0) { Fail "WDK km headers: no ntddk.h under $wdkDest" }
  $wdkKm = $hit[0].DirectoryName -replace '\\', '/'
  Say 'ok' "WDK km headers: $wdkKm"

  # ---- wine-pe builtin + NLS artifacts (ci.yml: Fetch wine-pe builtin DLLs; host-uwp.yml adds the NLS) ----
  $sha = (& git rev-parse HEAD).Trim()
  if (-not $WinePeBranch) { $WinePeBranch = (& git rev-parse --abbrev-ref HEAD).Trim() }
  if ($WinePeBranch -eq 'HEAD') { $WinePeBranch = 'main' }
  $stamp = Join-Path $RepoRoot 'wine-pe\out\.fetched-for'
  $ntdll = Join-Path $RepoRoot 'wine-pe\out\builtin\ntdll.dll'
  $nlsDir = Join-Path $RepoRoot 'wine-pe\out\nls'
  $haveNls = @(Get-ChildItem $nlsDir -Filter *.nls -ErrorAction SilentlyContinue).Count -gt 0
  if (-not $Force -and (Test-Path $ntdll) -and $haveNls -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $sha)) {
    Say 'ok' "wine-pe builtin + NLS already fetched for $($sha.Substring(0, 7))"
  } else {
    Say 'install' "wine-pe builtin + NLS artifacts (latest successful wine-pe run; own-commit run waited for; branch $WinePeBranch, then repository)"
    & python -I tools/ci/fetch_wine_pe.py --repo $GitHubRepo --branch $WinePeBranch --sha $sha `
      --artifact 'wine-pe-builtin=wine-pe/out/builtin' --artifact 'wine-pe-nls=wine-pe/out/nls' | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail 'fetch_wine_pe.py failed. Is `gh auth status` green? Without the builtins every guest test would skip.' }
    Set-Content -Path $stamp -Value $sha
    Say 'ok' 'wine-pe builtin + NLS'
  }

  # ---- Dear ImGui (ci.yml: Fetch Dear ImGui) ----
  $imgui = Join-Path $CacheDir 'imgui'
  $imguiMain = Join-Path $imgui 'examples\example_win32_directx11\main.cpp'
  if (-not (Test-Path $imguiMain)) {
    Say 'install' "Dear ImGui $ImguiTag"
    for ($i = 1; $i -le 4; $i++) {
      if (Test-Path $imgui) { Remove-Item -Recurse -Force $imgui }
      & git clone -q --depth 1 --branch $ImguiTag https://github.com/ocornut/imgui $imgui | Out-Host
      if ($LASTEXITCODE -eq 0) { break }
      Start-Sleep -Seconds (5 * $i)
    }
    if (-not (Test-Path $imguiMain)) { Fail "Dear ImGui $ImguiTag not fetched" }
  }
  Say 'ok' "Dear ImGui $ImguiTag at $imgui"
  $env:WIN10XBOX_IMGUI_DIR = $imgui

  # ---- Wine's ntdll conformance guest (ci.yml: Install llvm-mingw + Build Wine's ntdll conformance guest) ----
  $ntdllExe = Join-Path $build 'winetest\ntdll\ntdll_test.exe'
  $winetest = $false
  if ($SkipWinetest) {
    Say 'ok' 'ntdll conformance guest skipped (-SkipWinetest): integration.winetest_ntdll will not be registered'
  } elseif (-not $Force -and (Test-Path $ntdllExe)) {
    Say 'ok' "ntdll conformance guest already built ($ntdllExe)"
    $winetest = $true
  } else {
    $llvmRoot = $null
    $llvmDir = Join-Path $CacheDir "llvm-mingw-$LlvmMingwVer"
    $found = @(Get-ChildItem $llvmDir -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName 'bin\clang.exe') })
    if ($found.Count -eq 0) {
      Say 'install' "llvm-mingw $LlvmMingwVer (ucrt x86_64)"
      $zip = Join-Path $CacheDir "llvm-mingw-$LlvmMingwVer.zip"
      $url = "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmMingwVer/llvm-mingw-$LlvmMingwVer-ucrt-x86_64.zip"
      for ($i = 1; $i -le 3; $i++) {
        & curl.exe -sSL --fail --retry 3 -o $zip $url
        if ($LASTEXITCODE -eq 0) { break }
        Start-Sleep -Seconds 10
      }
      if ($LASTEXITCODE -ne 0) { Fail 'llvm-mingw download failed' }
      if (Test-Path $llvmDir) { Remove-Item -Recurse -Force $llvmDir }
      New-Item -ItemType Directory -Force -Path $llvmDir | Out-Null
      & tar.exe -xf $zip -C $llvmDir
      if ($LASTEXITCODE -ne 0) { Fail 'llvm-mingw extract failed' }
      Remove-Item -Force $zip
      $found = @(Get-ChildItem $llvmDir -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'bin\clang.exe') })
    }
    if ($found.Count -eq 0) { Fail "llvm-mingw: no bin\clang.exe under $llvmDir" }
    $llvmRoot = $found[0].FullName
    Say 'ok' "llvm-mingw at $llvmRoot"

    # Git for Windows' bash (never System32\bash.exe, which is WSL).
    $gitExe = (Get-Command git).Source
    $bash = $null
    $p = Split-Path -Parent $gitExe
    for ($i = 0; $i -lt 3 -and $p; $i++) {
      $c = Join-Path $p 'bin\bash.exe'
      if (Test-Path $c) { $bash = $c; break }
      $p = Split-Path -Parent $p
    }
    if (-not $bash) { Fail "Git for Windows' bin\bash.exe not found next to $gitExe" }

    Say 'install' "Wine ntdll conformance guest (fetch-wine.sh + ntdll.sh build, several minutes the first time)"
    $save = @{}
    foreach ($n in 'GIT_CONFIG_COUNT', 'GIT_CONFIG_KEY_0', 'GIT_CONFIG_VALUE_0', 'GIT_CONFIG_KEY_1', 'GIT_CONFIG_VALUE_1', 'INCLUDE', 'LIB', 'LLVM_MINGW_WIN') {
      $save[$n] = [Environment]::GetEnvironmentVariable($n)
    }
    try {
      $env:GIT_CONFIG_COUNT = '2'
      $env:GIT_CONFIG_KEY_0 = 'core.autocrlf'; $env:GIT_CONFIG_VALUE_0 = 'false'
      $env:GIT_CONFIG_KEY_1 = 'core.longpaths'; $env:GIT_CONFIG_VALUE_1 = 'true'
      $env:INCLUDE = ''; $env:LIB = ''        # clang's mingw driver must not see MSVC's headers (as in ci.yml)
      $env:LLVM_MINGW_WIN = $llvmRoot
      & $bash tools/dev/build-ntdll-guest.sh 'build/winetest/ntdll' | Out-Host
      if ($LASTEXITCODE -ne 0) { Fail 'the ntdll conformance guest did not build (tools/dev/build-ntdll-guest.sh)' }
    } finally {
      foreach ($n in $save.Keys) { [Environment]::SetEnvironmentVariable($n, $save[$n]) }
    }
    if (-not (Test-Path $ntdllExe)) { Fail "$ntdllExe missing" }
    $winetest = $true
    Say 'ok' 'ntdll conformance guest'
  }
  return @{ WdkKm = $wdkKm; ImguiDir = $imgui; WinetestBuilt = $winetest }
}

# ---------------------------------------------------------------- Post
$machine = Join-Path $build 'bin\machine'
$guest = Join-Path $machine 'guest'
$sys32 = Join-Path $env:SystemRoot 'System32'
if (-not (Test-Path (Join-Path $build 'bin'))) { Fail "$build\bin missing: run `cmake --build $BuildDir` before -Phase Post" }

# ci.yml: Populate machine folder from System32 (never committed), then the Direct3D machine's extra natives.
if (-not $Force -and (Test-Path (Join-Path $machine 'Windows\System32\ntdll.dll')) -and (Test-Path (Join-Path $build 'bin\machine-d3d-extra'))) {
  Say 'ok' 'machine folders already populated from System32'
} else {
  Say 'install' "machine folder from $sys32"
  & $shellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'machine\import_windows_dlls.ps1') -Source $sys32 -Machine (Join-Path $build 'bin\machine') -CI | Out-Host
  if ($LASTEXITCODE -ne 0) { Fail 'import_windows_dlls.ps1 (machine) failed' }
  Say 'install' 'Direct3D machine extra natives'
  & $shellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'machine\import_windows_dlls.ps1') -Source $sys32 -Machine (Join-Path $build 'bin\machine-d3d-extra') -CI -ListFile (Join-Path $RepoRoot 'machine\guest_dlls_d3d.txt') | Out-Host
  if ($LASTEXITCODE -ne 0) { Fail 'import_windows_dlls.ps1 (machine-d3d-extra) failed' }
  Say 'ok' 'machine folders'
}

New-Item -ItemType Directory -Force -Path $guest | Out-Null
# Same order and arguments as ci.yml. Out paths are absolute here; the markers are what each script leaves behind.
Invoke-Guest '7-Zip 7zr.exe'        'tools/ci/build_7zip.ps1'        @('-Out', $guest) (Join-Path $guest '7zr.exe')
Invoke-Guest 'SQLite shell'         'tools/ci/build_sqlite.ps1'      @('-OutDir', $guest) (Join-Path $guest 'sqlite3.exe')
Invoke-Guest 'Lua 5.4.7'            'tools/ci/build_lua.ps1'         @('-Out', $guest) (Join-Path $guest 'lua.exe')
Invoke-Guest 'Python embeddable'    'tools/ci/fetch_python.ps1'      @('-Out', (Join-Path $guest 'python')) (Join-Path $guest 'python\python.exe')
Invoke-Guest 'Sysinternals'         'tools/ci/fetch_sysinternals.ps1' @('-Out', $guest) (Join-Path $guest 'PsInfo64.exe')
Invoke-Guest 'fastfetch'            'tools/ci/fetch_fastfetch.ps1'   @('-Out', $guest) (Join-Path $guest 'fastfetch.exe')
Invoke-Guest 'Tiny C Compiler'      'tools/ci/fetch_tcc.ps1'         @('-Out', (Join-Path $guest 'tcc')) (Join-Path $guest 'tcc\tcc.exe')
Invoke-Guest 'ninja'                'tools/ci/fetch_ninja.ps1'       @('-Out', $guest) (Join-Path $guest 'ninja.exe')
Invoke-Guest 'Chocolate Doom'       'tools/ci/fetch_doom.ps1'        @('-Out', $guest) (Join-Path $guest 'doom1.wad')
Invoke-Guest 'real GUI programs'    'tools/ci/build_realgui.ps1'     @('-Out', $guest) (Join-Path $guest 'realgui_putty.exe')
Invoke-Guest 'SDL2 guest'           'tools/ci/build_sdl_guest.ps1'   @('-Out', $guest) (Join-Path $guest 'sdl_rects.exe')
Invoke-Guest 'curl.exe'             'tools/ci/build_curl.ps1'        @('-Out', $guest) (Join-Path $guest 'curl.exe')
Invoke-Guest 'MinGit'               'tools/ci/fetch_mingit.ps1'      @('-Out', (Join-Path $guest 'mingit')) (Join-Path $guest 'mingit\mingw64\bin\git.exe')
Say 'ok' 'prepare-windows (post): all guest programs in place'
