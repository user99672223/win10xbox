# Fetches the official CPython embeddable package (python.org, x64) and unpacks it into the machine folder as a
# guest: a real program that is not ours (docs/TESTING.md, "Real programs"). Nothing is compiled: python.exe,
# python312.dll, vcruntime140.dll, the .pyd extension modules, libcrypto/libssl/libffi/sqlite3 and the zipped
# standard library run exactly as python.org ships them.
#
#   tools/ci/fetch_python.ps1 [-Out build/bin/machine/guest/python] [-Work <dir>]
#
# The zip is pinned by SHA-256 and verified on EVERY run, including when ci.yml restored it from the cache (the
# cache key carries the same hash).
param(
  [string] $Out = 'build/bin/machine/guest/python',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'python-embed'),
  [string] $Version = '3.12.10',
  [string] $Sha256 = '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'
)
$ErrorActionPreference = 'Stop'
$url = "https://www.python.org/ftp/python/$Version/python-$Version-embed-amd64.zip"
$zip = Join-Path $Work "python-$Version-embed-amd64.zip"
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
  if (-not $ok) { Write-Host "::error::could not download python-$Version-embed-amd64.zip with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "python-$Version-embed-amd64.zip present, SHA-256 verified"
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Expand-Archive -Path $zip -DestinationPath $Out -Force
$exe = Join-Path $Out 'python.exe'
if (-not (Test-Path $exe)) { Write-Host "::error::python.exe missing after unpacking"; exit 1 }
Write-Host "python $Version embeddable -> $Out ($((Get-ChildItem $Out).Count) files)"
