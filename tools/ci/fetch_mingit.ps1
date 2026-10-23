# Fetches MinGit (Git for Windows, minimal zip) as a real guest program for the layer: integration.real_git* run
# <Out>\mingw64\bin\git.exe, which finds its DLLs (libiconv, libintl, libpcre2, libwinpthread, zlib) next to itself.
#
#   tools/ci/fetch_mingit.ps1 [-Out build/bin/machine/guest/mingit] [-Work <dir>]
#
# The release is pinned by tag and by SHA-256, and the hash is verified on EVERY run, including when ci.yml restored
# the zip from the cache (the cache key carries the same hash). A mismatch is a hard failure, never a re-download.
param(
  [string] $Out = 'build/bin/machine/guest/mingit',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'mingit-download'),
  [string] $Version = '2.47.1',
  [string] $Sha256 = '50b04b55425b5c465d076cdb184f63a0cd0f86f6ec8bb4d5860114a713d2c29a'
)
$ErrorActionPreference = 'Stop'
$url = "https://github.com/git-for-windows/git/releases/download/v$Version.windows.1/MinGit-$Version-64-bit.zip"
$zip = Join-Path $Work "MinGit-$Version-64-bit.zip"
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
  if (-not $ok) { Write-Host "::error::could not download MinGit-$Version-64-bit.zip with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "MinGit-$Version-64-bit.zip present, SHA-256 verified"
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
Expand-Archive -Path $zip -DestinationPath $Out
$git = Join-Path $Out 'mingw64/bin/git.exe'
if (-not (Test-Path $git)) { Write-Host "::error::$git missing"; exit 1 }
Write-Host "git.exe $((Get-Item $git).Length) bytes -> $git"
