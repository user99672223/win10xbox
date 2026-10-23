# Fetches fastfetch (https://github.com/fastfetch-cli/fastfetch), the system-information tool, as a guest for the
# layer: a real program that is not ours and that we do not build (docs/slices/fastfetch.md). It is the project's
# official prebuilt Windows x64 release (fastfetch-windows-amd64.zip of one pinned tag), checked against a pinned
# SHA-256 on EVERY run, including when ci.yml restored it from the cache (the cache key carries the version), so the
# bytes run are always the ones this was measured with. Only fastfetch.exe goes into the machine folder's guest dir.
#
#   tools/ci/fetch_fastfetch.ps1 [-Out build/bin/machine/guest] [-Work <dir>]
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'fastfetch-src'),
  [string] $Version = '2.70.0',
  [string] $Sha256 = '89e5a20f46ac04b154c7d0ea13dbac97e43f799a98d7ae4c598863ba357a8c7f'
)
$ErrorActionPreference = 'Stop'
$url = "https://github.com/fastfetch-cli/fastfetch/releases/download/$Version/fastfetch-windows-amd64.zip"
$zip = Join-Path $Work "fastfetch-$Version-windows-amd64.zip"
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
  if (-not $ok) { Write-Host "::error::could not download fastfetch $Version with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "fastfetch-$Version-windows-amd64.zip present, SHA-256 verified"
}

$dir = Join-Path $Work "fastfetch-$Version"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
Expand-Archive -Path $zip -DestinationPath $dir -Force
$exe = Join-Path $dir 'fastfetch.exe'
if (-not (Test-Path $exe)) { Write-Host '::error::fastfetch.exe is not in the release zip'; exit 1 }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'fastfetch.exe') -Force
Write-Host "fastfetch.exe $((Get-Item $exe).Length) bytes -> $(Join-Path $Out 'fastfetch.exe')"
