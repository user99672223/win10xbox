# Downloads MinGit (Git for Windows, minimal zip) as a guest for the layer:
# integration.real_git* run <Out>/mingw64/bin/git.exe.
#   tools/ci/fetch_mingit.ps1 [-Out build/bin/machine/guest/mingit]
param(
  [string] $Out = 'build/bin/machine/guest/mingit',
  [string] $Version = '2.47.1'
)
$ErrorActionPreference = 'Stop'
$url = "https://github.com/git-for-windows/git/releases/download/v$Version.windows.1/MinGit-$Version-64-bit.zip"
$zip = Join-Path $env:RUNNER_TEMP "MinGit-$Version.zip"
$ok = $false
foreach ($attempt in 1..3) {
  try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing; $ok = $true; break }
  catch { Write-Host "attempt ${attempt}: $_"; Start-Sleep -Seconds (5 * $attempt) }
}
if (-not $ok) { Write-Host "::error::could not download $url"; exit 1 }
Write-Host "MinGit SHA-256 $((Get-FileHash -Algorithm SHA256 $zip).Hash)"
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
Expand-Archive -Path $zip -DestinationPath $Out
$git = Join-Path $Out 'mingw64/bin/git.exe'
if (-not (Test-Path $git)) { Write-Host "::error::$git missing"; exit 1 }
Write-Host "git.exe -> $git"
