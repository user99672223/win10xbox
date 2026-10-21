<#
.SYNOPSIS
  Populate a win10xbox machine folder with Windows user-mode DLLs.

.DESCRIPTION
  The machine folder is the virtual PC's C:\. This script copies the guest DLL
  set from a real Windows installation (your own Windows 10 22H2 install is
  the intended source; on CI the runner's System32 is used for mechanics only)
  into <Machine>\Windows\System32, and writes machine.json if missing.
  Microsoft binaries are never committed to the repository.

.EXAMPLE
  .\machine\import_windows_dlls.ps1 -Source C:\Windows\System32 -Machine D:\xbox-machine
#>
param(
  [Parameter(Mandatory)] [string] $Source,
  [Parameter(Mandatory)] [string] $Machine,
  [switch] $CI,
  [string] $ListFile = (Join-Path $PSScriptRoot "guest_dlls.txt"),
  [string] $NlsListFile = (Join-Path $PSScriptRoot "nls_files.txt")
)
$ErrorActionPreference = "Stop"
$sys32 = Join-Path $Machine "Windows\System32"
New-Item -ItemType Directory -Force -Path $sys32, (Join-Path $Machine "Windows\Temp"),
  (Join-Path $Machine "Users\User\AppData\Local\Temp"), (Join-Path $Machine "Program Files"),
  (Join-Path $Machine "Program Files (x86)"), (Join-Path $Machine "ProgramData"),
  (Join-Path $Machine "Windows\System32\config") | Out-Null

$names = Get-Content $ListFile | Where-Object { $_ -and -not $_.StartsWith("#") }
$missing = @()
foreach ($n in $names) {
  $src = Join-Path $Source $n
  if (Test-Path $src) { Copy-Item -Force $src (Join-Path $sys32 $n) } else { $missing += $n }
}
if ($missing.Count) {
  Write-Host "import_windows_dlls: $($missing.Count) DLLs not found in ${Source}: $($missing -join ', ')"
  if (-not $CI) { Write-Warning "Programs importing those DLLs will fall back to Wine builtins if available." }
}

# NLS data files (code pages, casemap, locale, normalization, sort keys). The
# emulated NtGetNlsSectionPtr/NtInitializeNlsFiles read them from the machine
# folder; the locale init of Wine's ntdll fails without them. Source layout is
# <Windows>\System32 plus <Windows>\Globalization\Sorting\sortdefault.nls.
$sorting = Join-Path $Machine "Windows\Globalization\Sorting"
New-Item -ItemType Directory -Force -Path $sorting | Out-Null
$winDir = Split-Path -Parent ($Source.TrimEnd('\', '/'))
$nlsCopied = 0
$nlsMissing = @()
if (Test-Path $NlsListFile) {
  $entries = Get-Content $NlsListFile | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object { $_.Trim() }
  foreach ($e in $entries) {
    if ($e.StartsWith("Globalization\", [System.StringComparison]::OrdinalIgnoreCase)) {
      # Globalization\Sorting\x.nls: <Windows>\Globalization\Sorting (kept there), else System32 (fallback copy)
      $leaf = Split-Path -Leaf $e
      $hit = @(Get-ChildItem -Path (Join-Path $winDir $e) -File -ErrorAction SilentlyContinue)
      $dest = $sorting
      if (-not $hit.Count) {
        $hit = @(Get-ChildItem -Path (Join-Path $Source $leaf) -File -ErrorAction SilentlyContinue)
        $dest = $sys32
      }
    } else {
      $hit = @(Get-ChildItem -Path (Join-Path $Source $e) -File -ErrorAction SilentlyContinue)
      $dest = $sys32
    }
    if (-not $hit.Count) { $nlsMissing += $e; continue }
    foreach ($f in $hit) { Copy-Item -Force $f.FullName (Join-Path $dest $f.Name); $nlsCopied++ }
  }
} else {
  Write-Warning "import_windows_dlls: NLS list $NlsListFile not found"
}
if ($nlsMissing.Count) {
  Write-Host "import_windows_dlls: $($nlsMissing.Count) NLS files not found in ${Source}: $($nlsMissing -join ', ')"
  if (-not $CI) { Write-Warning "Wine's ntdll locale init needs the NLS data files; guest programs will be skipped or fail to start." }
}
Write-Host "import_windows_dlls: $nlsCopied NLS files -> $sys32, $sorting"
$cfg = Join-Path $Machine "machine.json"
if (-not (Test-Path $cfg)) {
  Copy-Item (Join-Path $PSScriptRoot "machine.default.json") $cfg
}
$ver = (Get-Item (Join-Path $Source "ntdll.dll")).VersionInfo
Write-Host "import_windows_dlls: source ntdll $($ver.FileVersion) -> $sys32 ($($names.Count - $missing.Count) files)"
