# Fetches the pinned walbourn/directx-sdk-samples commit (Direct3D11Tutorials/Tutorial07, docs/slices/d3dsample.md) into -Out.
# Read-only, public GitHub source; the tree is only compiled on the runner, never committed. Retries because the clone
# is the one network step of the job.
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$sha = '1ad8f0f6a3e4d9be7e54ca52640ac12b6565ab0c'
$probe = Join-Path $Out 'Direct3D11Tutorials/Tutorial07/Tutorial07.cpp'
if (-not (Test-Path $probe)) {
  for ($i = 1; $i -le 4; $i++) {
    if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
    New-Item -ItemType Directory -Force $Out | Out-Null
    git -C $Out init -q
    git -C $Out remote add origin https://github.com/walbourn/directx-sdk-samples
    git -C $Out fetch -q --depth 1 origin $sha
    if ($LASTEXITCODE -eq 0) { git -C $Out checkout -q FETCH_HEAD }
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Seconds (5 * $i)
  }
}
if (-not (Test-Path $probe)) { throw "directx-sdk-samples $sha not fetched" }
$head = (git -C $Out rev-parse HEAD).Trim()
if ($head -ne $sha) { throw "directx-sdk-samples is at $head, expected $sha" }
foreach ($f in 'Tutorial07.cpp', 'DDSTextureLoader.cpp', 'Tutorial07.fxh', 'seafloor.dds', 'Tutorial07.rc') {
  if (-not (Test-Path (Join-Path $Out "Direct3D11Tutorials/Tutorial07/$f"))) { throw "Tutorial07/$f missing from the fetched tree" }
}
"WIN10XBOX_D3DSAMPLE_DIR=$Out" >> $env:GITHUB_ENV
Write-Host "directx-sdk-samples $sha ready in $Out"
