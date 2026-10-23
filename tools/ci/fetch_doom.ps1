# Fetches Chocolate Doom (https://github.com/chocolate-doom/chocolate-doom), the source port of Doom, and the shareware
# doom1.wad as guests for the layer: the first game that is not ours and that we do not build (docs/slices/doom.md).
# Both are pinned by SHA-256 and checked on EVERY run, including when ci.yml restored them from the cache (the cache key
# carries the versions), so the bytes run are always the ones this was measured with:
#   * chocolate-doom-3.1.1-win64.zip, the project's official Windows x64 release (chocolate-doom.exe, SDL2.dll,
#     SDL2_mixer.dll, SDL2_net.dll, SDL3.dll and the MinGW DLLs libfluidsynth-3.dll imports);
#   * doom1.wad, the id Software shareware IWAD 1.9 (4196020 bytes), freely redistributable. Mirrors are tried in order:
#     ibiblio's SliTaz source mirror, then the Debian doom-wad-shareware tarball on archive.org (the WAD inside it).
# chocolate-doom.exe, every DLL of the zip and doom1.wad go into the machine folder's guest directory.
#
#   tools/ci/fetch_doom.ps1 [-Out build/bin/machine/guest] [-Work <dir>]
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'doom-src'),
  [string] $Version = '3.1.1',
  [string] $ZipSha256 = '58c34c61ae954493fce5ff01fd553898240a0de25658aa97b43ac9510c49581f',
  [string] $WadSha256 = '1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Hash([string] $path, [string] $want) {
  (Test-Path $path) -and ((Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant() -eq $want)
}
function Get-Pinned([string[]] $urls, [string] $dest, [string] $want, [string] $what) {
  if (Test-Hash $dest $want) { Write-Host "$what present, SHA-256 verified"; return }
  foreach ($url in $urls) {
    foreach ($attempt in 1..3) {
      try {
        Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
        if (Test-Hash $dest $want) { Write-Host "$what downloaded from $url, SHA-256 verified"; return }
        Write-Host "${url} attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $dest).Hash.ToLowerInvariant()), expected $want"
      } catch { Write-Host "${url} attempt ${attempt}: $_" }
      Start-Sleep -Seconds (5 * $attempt)
    }
  }
  Write-Host "::error::could not download $what with the pinned SHA-256"; exit 1
}

# --- Chocolate Doom ---
$zip = Join-Path $Work "chocolate-doom-$Version-win64.zip"
Get-Pinned @("https://github.com/chocolate-doom/chocolate-doom/releases/download/chocolate-doom-$Version/chocolate-doom-$Version-win64.zip") `
  $zip $ZipSha256 "chocolate-doom-$Version-win64.zip"
$dir = Join-Path $Work "chocolate-doom-$Version"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
Expand-Archive -Path $zip -DestinationPath $dir -Force
$exe = Join-Path $dir 'chocolate-doom.exe'
if (-not (Test-Path $exe)) { Write-Host '::error::chocolate-doom.exe is not in the release zip'; exit 1 }

# --- doom1.wad ---
$wad = Join-Path $Work 'doom1.wad'
if (-not (Test-Hash $wad $WadSha256)) {
  $direct = @('https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad')
  $ok = $false
  foreach ($url in $direct) {
    foreach ($attempt in 1..2) {
      try {
        Invoke-WebRequest -Uri $url -OutFile $wad -UseBasicParsing
        if (Test-Hash $wad $WadSha256) { $ok = $true; Write-Host "doom1.wad downloaded from $url, SHA-256 verified"; break }
        Write-Host "${url} attempt ${attempt}: SHA-256 $((Get-FileHash -Algorithm SHA256 $wad).Hash.ToLowerInvariant()), expected $WadSha256"
      } catch { Write-Host "${url} attempt ${attempt}: $_" }
      Start-Sleep -Seconds (5 * $attempt)
    }
    if ($ok) { break }
  }
  if (-not $ok) {
    # The Debian doom-wad-shareware 1.9 source tarball (a gzip'd tar of doom1.wad), mirrored on archive.org.
    $tgz = Join-Path $Work 'doom-wad-shareware.tar.gz'
    $tgzUrl = 'https://archive.org/download/doom-wad-shareware_1.9_11apr2025-mirror/doom-wad-shareware_1.9.fixed.orig.tar.gz'
    $tgzSha = 'a2d5964a84235eb60119f213b2cdf7f748672744'   # archive.org's published SHA-1 of the tarball; the WAD itself is pinned by SHA-256 below
    foreach ($attempt in 1..3) {
      try {
        Invoke-WebRequest -Uri $tgzUrl -OutFile $tgz -UseBasicParsing
        if ((Get-FileHash -Algorithm SHA1 $tgz).Hash.ToLowerInvariant() -eq $tgzSha) { break }
        Write-Host "tarball attempt ${attempt}: unexpected SHA-1"
      } catch { Write-Host "tarball attempt ${attempt}: $_" }
      Start-Sleep -Seconds (5 * $attempt)
    }
    $x = Join-Path $Work 'wad-x'
    if (Test-Path $x) { Remove-Item -Recurse -Force $x }
    New-Item -ItemType Directory -Force -Path $x | Out-Null
    tar.exe -xzf $tgz -C $x
    $found = Get-ChildItem -Recurse -Path $x -Filter 'doom1.wad' | Select-Object -First 1
    if ($found) { Copy-Item $found.FullName $wad -Force }
  }
  if (-not (Test-Hash $wad $WadSha256)) { Write-Host "::error::could not get doom1.wad with the pinned SHA-256 $WadSha256"; exit 1 }
} else {
  Write-Host 'doom1.wad present, SHA-256 verified'
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe (Join-Path $Out 'chocolate-doom.exe') -Force
$dlls = Get-ChildItem -Path $dir -Filter '*.dll'
foreach ($d in $dlls) { Copy-Item $d.FullName (Join-Path $Out $d.Name) -Force }
Copy-Item $wad (Join-Path $Out 'doom1.wad') -Force
Write-Host "chocolate-doom.exe $((Get-Item $exe).Length) bytes, $($dlls.Count) DLLs, doom1.wad $((Get-Item $wad).Length) bytes -> $Out"
