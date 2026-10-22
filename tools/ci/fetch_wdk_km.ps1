# Puts the WDK's kernel-mode headers (Include\<ver>\km: ntddk.h, wdm.h, ...) on a GitHub windows runner and exports
# their directory as WDK_KM (forward slashes) through GITHUB_ENV, for -DWIN10XBOX_DDK_INCLUDE=$env:WDK_KM.
#
# Why: tests/drivers builds OurKmdfDevice.sys, its client and layout_check against Microsoft's ntddk.h. With
# llvm-mingw that is the mingw-w64 ddk; with MSVC it is the WDK, and the windows-latest image does not have it (ci.yml
# run 39823615327 listed C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0 with no km directory). Microsoft
# publishes the WDK as the NuGet package Microsoft.Windows.WDK.x64; only its km headers (17 MB of the 110 MB package)
# are extracted. Pinned version + SHA-256, so the bytes are the ones this was measured with.
#
#   tools/ci/fetch_wdk_km.ps1 [-Dest <dir>]     (default: $env:RUNNER_TEMP\wdk; reused if already populated)
param(
  [string] $Dest = (Join-Path $env:RUNNER_TEMP 'wdk'),
  [string] $Version = '10.0.26100.6584',
  [string] $Sha256 = 'c393d03dfb640b5c92f546b32f6770ef68cd3aaf691956e7d66d8e2c28a1b55e'
)
$ErrorActionPreference = 'Stop'
$inc = ($Version -split '\.')[0..2] -join '.'
$inc = "$inc.0"                                  # 10.0.26100.6584 -> Include\10.0.26100.0
$km = Join-Path $Dest "Include\$inc\km"
if (-not (Test-Path (Join-Path $km 'ntddk.h'))) {
  New-Item -ItemType Directory -Force -Path $Dest | Out-Null
  $pkg = Join-Path $Dest 'wdk.nupkg'
  $url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$Version/microsoft.windows.wdk.x64.$Version.nupkg"
  $ok = $false
  foreach ($attempt in 1..3) {
    try {
      Invoke-WebRequest -Uri $url -OutFile $pkg -UseBasicParsing
      $got = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLowerInvariant()
      if ($got -eq $Sha256) { $ok = $true; break }
      Write-Host "attempt ${attempt}: SHA-256 $got, expected $Sha256"
    } catch { Write-Host "attempt ${attempt}: $_" }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download Microsoft.Windows.WDK.x64 $Version with the pinned SHA-256"; exit 1 }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::OpenRead($pkg)
  $prefix = "c/Include/$inc/km/"
  $n = 0
  try {
    foreach ($e in $zip.Entries) {
      $name = [Uri]::UnescapeDataString($e.FullName)
      if (-not $name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $name.EndsWith('/')) { continue }
      $out = Join-Path $km ($name.Substring($prefix.Length) -replace '/', '\')
      New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
      [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
      $n++
    }
  } finally { $zip.Dispose() }
  Remove-Item $pkg -Force
  Write-Host "extracted $n km headers of Microsoft.Windows.WDK.x64 $Version into $km"
} else {
  Write-Host "WDK km headers already present in $km"
}
if (-not (Test-Path (Join-Path $km 'ntddk.h'))) { Write-Host "::error::$km\ntddk.h missing"; exit 1 }
$fwd = $km -replace '\\', '/'
if ($env:GITHUB_ENV) { Add-Content -Path $env:GITHUB_ENV -Value "WDK_KM=$fwd" }
Write-Host "WDK_KM=$fwd"
