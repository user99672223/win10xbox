<#
.SYNOPSIS
  One command on a Windows 11 laptop with Visual Studio 2022: tools, clone, build, full native ctest suite, agent settings.

.DESCRIPTION
  Run it in an EMPTY folder (PowerShell 5.1 or 7). It is idempotent: every step prints [ok] when there is nothing
  to do, [install] while it installs or builds, [fail] and the exact next step when it cannot go on.

    a  tools: git, gh, cmake >= 3.25, ninja, python >= 3.10, node/npm, pwsh, claude (winget / npm)
    b  Visual Studio components (vswhere): VC.Tools.x86.x64 and a Windows 11 SDK; -WithUwp adds the UWP workload
    c  gh auth (gh auth login --web once)
    d  clone .\win10xbox (or git pull --ff-only + submodule update)
    e  MSVC x64 environment, tools\dev\prepare-windows.ps1, configure, build, ctest, tools\ci\check_ctest_run.py
    f  .claude\settings.local.json for non-interactive agents (only if absent)

  -SkipBuild    stop after the clone, the prep and the settings (no configure/build/ctest)
  -SkipInstall  never install anything (winget, VS modify, npm); a missing prerequisite is a [fail]
  -WithUwp      also require/add Microsoft.VisualStudio.ComponentGroup.UWP.VC (for the console package)
#>
[CmdletBinding()]
param(
  [switch] $SkipBuild,
  [switch] $SkipInstall,
  [switch] $WithUwp,
  [switch] $SkipWinetest,
  [string] $Branch = 'claude/youthful-cerf-l7xxsb',
  [string] $Repo = 'user99672223/win10xbox',
  [string] $Dir = (Join-Path (Get-Location).Path 'win10xbox'),
  [string] $Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
$t0 = Get-Date

function Say($tag, $msg) { Write-Host ("[{0}] {1}" -f $tag, $msg) }
function Stop-Here($msg, $next) {
  Write-Host "[fail] $msg"
  if ($next) { Write-Host "       next step: $next" }
  exit 1
}
function Update-SessionPath {
  $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
  $u = [Environment]::GetEnvironmentVariable('Path', 'User')
  $env:Path = (@($m, $u, "$env:APPDATA\npm") | Where-Object { $_ }) -join ';'
}
function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-ToolVersion([string] $text) {
  if ($text -match '(\d+)\.(\d+)(?:\.(\d+))?') { return [version]("{0}.{1}" -f $Matches[1], $Matches[2]) }
  return $null
}

# ---------------------------------------------------------------- a. tools
# name, command, winget id, minimum, how to read the version (empty = presence only)
$tools = @(
  @{ n = 'git';    c = 'git';    id = 'Git.Git';                min = $null;               v = { git --version } },
  @{ n = 'gh';     c = 'gh';     id = 'GitHub.cli';             min = $null;               v = { gh --version } },
  @{ n = 'cmake';  c = 'cmake';  id = 'Kitware.CMake';          min = [version]'3.25';     v = { cmake --version } },
  @{ n = 'ninja';  c = 'ninja';  id = 'Ninja-build.Ninja';      min = $null;               v = { ninja --version } },
  @{ n = 'python'; c = 'python'; id = 'Python.Python.3.12';     min = [version]'3.10';     v = { python --version } },
  @{ n = 'node';   c = 'node';   id = 'OpenJS.NodeJS.LTS';      min = $null;               v = { node --version } },
  @{ n = 'pwsh';   c = 'pwsh';   id = 'Microsoft.PowerShell';   min = $null;               v = { pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString()' } }
)
function Test-Tool($t) {
  $cmd = Get-Command $t.c -ErrorAction SilentlyContinue
  if (-not $cmd) { return $null }
  try { $txt = (& $t.v 2>&1 | Out-String).Trim() } catch { return $null }
  if ($LASTEXITCODE -ne 0) { return $null }      # e.g. the Microsoft Store python stub
  $ver = Get-ToolVersion $txt
  if ($t.min -and (-not $ver -or $ver -lt $t.min)) { return "too old ($txt), need >= $($t.min)" }
  return $txt.Split("`n")[0]
}

Update-SessionPath
foreach ($t in $tools) {
  $r = Test-Tool $t
  if ($r -and $r -notlike 'too old*') { Say 'ok' "$($t.n): $r"; continue }
  $why = if ($r) { $r } else { 'not found' }
  $wg = "winget install --id $($t.id) -e --accept-source-agreements --accept-package-agreements"
  if ($SkipInstall) { Stop-Here "$($t.n) $why" $wg }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Stop-Here "$($t.n) $why and winget is missing" 'install "App Installer" from the Microsoft Store (provides winget), then rerun this script'
  }
  Say 'install' "$($t.n) ($why): $wg"
  & winget install --id $t.id -e --accept-source-agreements --accept-package-agreements | Out-Host
  Update-SessionPath
  $r = Test-Tool $t
  if (-not $r -or $r -like 'too old*') {
    Stop-Here "$($t.n) still not usable after winget (exit $LASTEXITCODE)" "open a NEW PowerShell window (so PATH is refreshed) and rerun this script; if it repeats, run: $wg"
  }
  Say 'ok' "$($t.n): $r"
}

# claude (Claude Code CLI) via npm
if (Get-Command claude -ErrorAction SilentlyContinue) {
  Say 'ok' 'claude: present'
} else {
  if ($SkipInstall) { Stop-Here 'claude (Claude Code CLI) not found' 'npm install -g @anthropic-ai/claude-code' }
  Say 'install' 'claude: npm install -g @anthropic-ai/claude-code'
  & npm install -g '@anthropic-ai/claude-code' | Out-Host
  Update-SessionPath
  if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    Stop-Here "claude not on PATH after npm install (exit $LASTEXITCODE)" 'open a NEW PowerShell window and rerun this script, or run: npm install -g @anthropic-ai/claude-code'
  }
  Say 'ok' 'claude: installed'
}

# ---------------------------------------------------------------- b. Visual Studio
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
  Stop-Here 'Visual Studio is not installed (no vswhere.exe)' 'install Visual Studio 2022 (Community is fine) with the "Desktop development with C++" workload, then rerun this script'
}
$vsPath = (& $vswhere -latest -products '*' -version '[17.0,)' -property installationPath | Select-Object -First 1)
if (-not $vsPath) {
  Stop-Here 'vswhere found no Visual Studio 2022 or newer' 'install Visual Studio 2022 with the "Desktop development with C++" workload, then rerun this script'
}
$vsPath = $vsPath.Trim()
Say 'ok' "Visual Studio: $vsPath"

function Test-VsComponent([string] $id) {
  $p = (& $vswhere -latest -products '*' -version '[17.0,)' -requires $id -property installationPath | Select-Object -First 1)
  return [bool]$p
}
$add = @()
if (Test-VsComponent 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64') { Say 'ok' 'VS component VC.Tools.x86.x64' }
else { $add += 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }

$sdkIds = 'Microsoft.VisualStudio.Component.Windows11SDK.26100', 'Microsoft.VisualStudio.Component.Windows11SDK.22621', 'Microsoft.VisualStudio.Component.Windows11SDK.22000'
$sdkOk = $false
foreach ($s in $sdkIds) { if (Test-VsComponent $s) { Say 'ok' "VS component $s"; $sdkOk = $true; break } }
if (-not $sdkOk) {
  # A standalone Windows 11 SDK (10.0.22000 or newer) is as good as the VS component.
  $inc = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
  $newer = @(Get-ChildItem $inc -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^10\.0\.(\d+)\.' -and [int]$Matches[1] -ge 22000 -and (Test-Path (Join-Path $_.FullName 'um\windows.h')) })
  if ($newer.Count -gt 0) { Say 'ok' "Windows SDK $($newer[-1].Name) (standalone)"; $sdkOk = $true }
}
if (-not $sdkOk) { $add += 'Microsoft.VisualStudio.Component.Windows11SDK.22621' }

if ($WithUwp) {
  if (Test-VsComponent 'Microsoft.VisualStudio.ComponentGroup.UWP.VC') { Say 'ok' 'VS component group UWP.VC' }
  else { $add += 'Microsoft.VisualStudio.ComponentGroup.UWP.VC' }
}
if ($add.Count -gt 0) {
  $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
  $addArgs = ($add | ForEach-Object { "--add $_" }) -join ' '
  $cmdText = "`"$setup`" modify --installPath `"$vsPath`" $addArgs --passive --norestart"
  if ($SkipInstall) { Stop-Here "Visual Studio is missing: $($add -join ', ')" "run (elevated): $cmdText" }
  if (-not (Test-Admin)) {
    Stop-Here "Visual Studio is missing: $($add -join ', '), and adding components needs elevation" "rerun this script from an elevated PowerShell (Start menu, right-click PowerShell, Run as administrator); it will run: $cmdText"
  }
  Say 'install' "Visual Studio components: $($add -join ', ') (the installer window shows progress)"
  $argList = @('modify', '--installPath', $vsPath) + ($add | ForEach-Object { '--add'; $_ }) + @('--passive', '--norestart')
  $proc = Start-Process -FilePath $setup -ArgumentList ($argList | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -Wait -PassThru
  if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) {
    Stop-Here "the Visual Studio installer exited $($proc.ExitCode)" "run it by hand and read its log in %TEMP% (dd_installer*.log): $cmdText"
  }
  foreach ($c in $add) { if ($c -notlike '*Windows11SDK*' -and -not (Test-VsComponent $c)) { Stop-Here "component $c still missing after the installer" "run: $cmdText" } }
  Say 'ok' "Visual Studio components added$(if ($proc.ExitCode -eq 3010) { ' (a reboot is pending; continuing)' })"
}

# ---------------------------------------------------------------- c. gh auth
& cmd.exe /d /c 'gh auth status >nul 2>&1'
if ($LASTEXITCODE -eq 0) { Say 'ok' 'gh: logged in' }
else {
  Say 'install' 'gh auth login --web (interactive, once: follow the browser prompt)'
  & gh auth login --web --git-protocol https | Out-Host
  & cmd.exe /d /c 'gh auth status >nul 2>&1'
  if ($LASTEXITCODE -ne 0) { Stop-Here 'gh is not logged in' 'run: gh auth login --web   (agents need it to push and to download the wine-pe artifacts)' }
  Say 'ok' 'gh: logged in'
}
& cmd.exe /d /c 'gh auth setup-git >nul 2>&1'      # git pushes of agents use gh's token

# ---------------------------------------------------------------- d. clone
$url = "https://github.com/$Repo"
if (-not (Test-Path (Join-Path $Dir '.git'))) {
  if ((Test-Path $Dir) -and @(Get-ChildItem $Dir -Force -ErrorAction SilentlyContinue).Count -gt 0) {
    Stop-Here "$Dir exists, is not empty and is not a git clone" "move or delete $Dir, then rerun"
  }
  Say 'install' "git clone -b $Branch --recurse-submodules $url $Dir"
  & git -c core.longpaths=true clone -b $Branch --recurse-submodules $url $Dir | Out-Host
  if ($LASTEXITCODE -ne 0) { Stop-Here "git clone failed (exit $LASTEXITCODE)" "check the network and run: git clone -b $Branch --recurse-submodules $url $Dir" }
} else {
  Say 'install' "git pull --ff-only in $Dir"
  & git -C $Dir pull --ff-only | Out-Host
  if ($LASTEXITCODE -ne 0) { Stop-Here "git pull --ff-only failed (exit $LASTEXITCODE)" "resolve the local changes in $Dir (git status), then rerun" }
  & git -C $Dir submodule update --init | Out-Host
  if ($LASTEXITCODE -ne 0) { Stop-Here "git submodule update failed (exit $LASTEXITCODE)" "run in $Dir : git submodule update --init" }
}
& git -C $Dir config core.longpaths true
# ci.yml: "Nested submodules we need (zycore only ...)"
& git -C $Dir submodule update --init --recursive third_party/zydis | Out-Host
if ($LASTEXITCODE -ne 0) { Stop-Here "zydis submodule failed (exit $LASTEXITCODE)" "run in $Dir : git submodule update --init --recursive third_party/zydis" }
$head = (& git -C $Dir rev-parse --short HEAD).Trim()
Say 'ok' "clone at $Dir ($Branch @ $head)"

# ---------------------------------------------------------------- f. agent settings (before the long build)
$claudeDir = Join-Path $Dir '.claude'
$settings = Join-Path $claudeDir 'settings.local.json'
if (Test-Path $settings) {
  Say 'ok' "$settings exists (left alone)"
} else {
  New-Item -ItemType Directory -Force -Path $claudeDir | Out-Null
  $json = @'
{
  "permissions": {
    "allow": [
      "Read", "Edit", "Write", "Glob", "Grep",
      "Bash(git:*)", "Bash(gh:*)", "Bash(cmake:*)", "Bash(ctest:*)", "Bash(ninja:*)",
      "Bash(python:*)", "Bash(pwsh:*)", "Bash(powershell:*)", "Bash(dumpbin:*)", "Bash(llvm-objdump:*)"
    ],
    "deny": [
      "Bash(git push * origin claude/youthful-cerf-l7xxsb)",
      "Bash(git push origin claude/youthful-cerf-l7xxsb)",
      "Bash(git push * origin main)",
      "Bash(git push origin main)",
      "Bash(git push --force:*)",
      "Bash(git push -f:*)",
      "Bash(rm:*)",
      "Bash(Remove-Item:*)"
    ]
  }
}
'@
  [IO.File]::WriteAllText($settings, $json, (New-Object Text.UTF8Encoding($false)))
  $excl = Join-Path $Dir '.git\info\exclude'
  if ((Test-Path $excl) -and -not (Select-String -Path $excl -Pattern 'settings.local.json' -Quiet)) { Add-Content -Path $excl -Value '.claude/settings.local.json' }
  Say 'ok' "wrote $settings (allow list + deny list; see docs/dev/WINDOWS-AGENTS.md for what pattern matching cannot enforce)"
}

# ---------------------------------------------------------------- e. MSVC environment, prep, build, test
$counts = $null
$exit = 0
if ($SkipBuild) {
  Say 'ok' '-SkipBuild: no MSVC environment, prep, configure, build or ctest'
} else {
  $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path $vcvars)) { Stop-Here "$vcvars missing" 'add the VC.Tools.x86.x64 component (rerun elevated without -SkipInstall)' }
  $tmp = Join-Path ([IO.Path]::GetTempPath()) ('vcvars-' + [guid]::NewGuid().ToString('N') + '.cmd')
  Set-Content -Path $tmp -Encoding ASCII -Value @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b 1', 'set')
  $envLines = & $tmp
  $rc = $LASTEXITCODE
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
  if ($rc -ne 0 -or -not $envLines) { Stop-Here "vcvars64.bat failed (exit $rc)" "open 'x64 Native Tools Command Prompt for VS 2022' once to see the error" }
  foreach ($l in $envLines) {
    if ($l -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
  }
  if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { Stop-Here 'cl.exe not on PATH after vcvars64' 'check the VC.Tools.x86.x64 component' }
  Say 'ok' 'MSVC x64 environment (cl on PATH)'

  Set-Location $Dir
  $prep = Join-Path $Dir 'tools\dev\prepare-windows.ps1'
  $prepArgs = @{ RepoRoot = $Dir; GitHubRepo = $Repo }
  if ($SkipWinetest) { $prepArgs.SkipWinetest = $true }
  try {
    $pre = & $prep -Phase Pre @prepArgs
  } catch { Stop-Here "prepare-windows.ps1 (Pre) failed: $($_.Exception.Message)" "read the message above; fix and rerun this script (finished steps are cached under $env:LOCALAPPDATA\win10xbox\cache)" }
  if ($pre -is [array]) { $pre = $pre[-1] }
  $env:WIN10XBOX_IMGUI_DIR = $pre.ImguiDir

  Say 'install' "cmake configure ($Config, Ninja, cl)"
  & cmake -S . -B build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DWIN10XBOX_DDK_INCLUDE=$($pre.WdkKm)" | Out-Host
  if ($LASTEXITCODE -ne 0) { Stop-Here "cmake configure failed (exit $LASTEXITCODE)" 'read the first error above; rerun this script after fixing' }
  Say 'install' 'cmake --build build (the first build takes a while)'
  & cmake --build build | Out-Host
  if ($LASTEXITCODE -ne 0) { Stop-Here "cmake --build failed (exit $LASTEXITCODE)" 'run: cmake --build build 2>&1 | Select-String "error"' }
  Say 'ok' 'build'

  try { & $prep -Phase Post -RepoRoot $Dir | Out-Null } catch { Stop-Here "prepare-windows.ps1 (Post) failed: $($_.Exception.Message)" 'fix and rerun this script (finished steps are skipped)' }

  # ci.yml "Test": WIN10XBOX_DEBUG=+all
  $env:WIN10XBOX_DEBUG = '+all'
  Say 'install' 'ctest (full suite, 300 s per-test timeout)'
  Remove-Item (Join-Path $Dir 'build\ctest-junit.xml') -Force -ErrorAction SilentlyContinue
  & ctest --test-dir build --output-on-failure --timeout 300 --output-junit ctest-junit.xml | Out-Host
  $ctestRc = $LASTEXITCODE
  Remove-Item Env:\WIN10XBOX_DEBUG -ErrorAction SilentlyContinue

  $gate = @('-I', 'tools/ci/check_ctest_run.py', '--build', 'build', '--junit', 'ctest-junit.xml',
            '--require-label', 'guest_e2e', '--require-label', 'driver_e2e=2', '--require-label', 'drivers=2')
  if ($pre.WinetestBuilt) { $gate += @('--require-label', 'winetest_ntdll') }
  & python @gate | Out-Host
  $gateRc = $LASTEXITCODE

  $junit = Join-Path $Dir 'build\ctest-junit.xml'
  if (Test-Path $junit) {
    $x = [xml](Get-Content $junit -Raw)
    $cases = @($x.SelectNodes('//testcase'))
    $nSkip = @($cases | Where-Object { $_.skipped -ne $null }).Count
    $nFail = @($cases | Where-Object { $_.failure -ne $null }).Count
    $counts = [pscustomobject]@{ Total = $cases.Count; Passed = $cases.Count - $nSkip - $nFail; Failed = $nFail; Skipped = $nSkip }
  }
  if ($ctestRc -ne 0) { $exit = 1; Say 'fail' "ctest exited $ctestRc (the failing tests are listed above; rerun one with: ctest --test-dir build -R <name> --output-on-failure)" }
  if ($gateRc -ne 0) { $exit = 1; Say 'fail' 'tools/ci/check_ctest_run.py: a test was skipped, not run, or a required label lost its tests (see above)' }
}

# ---------------------------------------------------------------- summary
Write-Host ''
if ($counts) {
  Write-Host ("ctest: {0} tests, {1} passed, {2} failed, {3} skipped" -f $counts.Total, $counts.Passed, $counts.Failed, $counts.Skipped)
}
if ($exit -eq 0) {
  Say 'ok' ("done in {0:N0} min" -f ((Get-Date) - $t0).TotalMinutes)
  Write-Host ''
  Write-Host 'Run a one-shot agent (log in once first: run `claude`, or set ANTHROPIC_API_KEY):'
  Write-Host "  cd `"$Dir`""
  Write-Host '  claude -p "<brief>"'
  Write-Host 'Brief template and rules: docs\dev\WINDOWS-AGENTS.md. Agents push only to claude/youthful-cerf-l7xxsb-<slice>.'
}
exit $exit
