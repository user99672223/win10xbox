# Builds the SQLite command-line shell (sqlite3.exe) with MSVC from the official amalgamation, as a guest
# program for the layer: a real, unmodified Windows x64 console program that this repository did not write.
#
#   tools/ci/build_sqlite.ps1 [-OutDir build/bin/machine/guest] [-CacheDir <dir>] [-WorkDir <dir>]
#
# Needs cl.exe on PATH (ilammy/msvc-dev-cmd in ci.yml). The archive is pinned by URL AND SHA-256 below and
# verified on EVERY run, whether it was just downloaded or came out of the actions cache: a mismatch is a
# hard failure, never a re-download-and-trust. To move to a newer release, change the three pins together and
# the cache key in ci.yml.
#
# How the pin was made: https://www.sqlite.org/download.html lists the current release (3.54.1, 2026) with
# its SHA3-256 (92281c86...); the file was downloaded once and hashed with SHA-256 (0a9c8e34...); the
# SHA3-256 of that same download equals the one the project publishes, so the pin is of the genuine archive.
param(
  [string] $OutDir = 'build/bin/machine/guest',
  [string] $CacheDir = '',
  [string] $WorkDir = ''
)
$ErrorActionPreference = 'Stop'

$Version = '3.54.1'
$Archive = 'sqlite-amalgamation-3540100.zip'
$Url     = "https://www.sqlite.org/2026/$Archive"
$Sha256  = '0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9'

$tmp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
if (-not $CacheDir) { $CacheDir = Join-Path $tmp 'sqlite-download' }
if (-not $WorkDir)  { $WorkDir  = Join-Path $tmp 'sqlite-build' }
New-Item -ItemType Directory -Force -Path $CacheDir, $OutDir | Out-Null
$zip = Join-Path $CacheDir $Archive

function Get-Sha256([string] $path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }

if (Test-Path $zip) {
  $have = Get-Sha256 $zip
  if ($have -eq $Sha256) {
    Write-Host "sqlite: cached $Archive matches the pin"
  } else {
    Write-Host "::warning::sqlite: cached $Archive has SHA-256 $have, not the pin; discarding it"
    Remove-Item -Force $zip
  }
}
if (-not (Test-Path $zip)) {
  for ($try = 1; ; $try++) {
    try {
      Write-Host "sqlite: downloading $Url (attempt $try)"
      Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
      break
    } catch {
      if ($try -ge 4) { throw }
      Start-Sleep -Seconds (5 * $try)
    }
  }
}
$got = Get-Sha256 $zip
if ($got -ne $Sha256) {
  Remove-Item -Force $zip
  throw "sqlite: $Archive has SHA-256 $got, the pin says $Sha256. Refusing to build it."
}
Write-Host "sqlite: $Archive SHA-256 $got verified"

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Expand-Archive -LiteralPath $zip -DestinationPath $WorkDir
$src = Join-Path $WorkDir ([IO.Path]::GetFileNameWithoutExtension($Archive))
foreach ($f in 'shell.c', 'sqlite3.c', 'sqlite3.h') {
  if (-not (Test-Path (Join-Path $src $f))) { throw "sqlite: $f missing from $Archive" }
}
if (-not (Select-String -Path (Join-Path $src 'sqlite3.h') -Pattern "#define SQLITE_VERSION\s+`"$([regex]::Escape($Version))`"" -Quiet)) {
  throw "sqlite: sqlite3.h is not version $Version"
}

# The documented one-liner for the shell: cl shell.c sqlite3.c, optimised, static CRT (the guest must not
# depend on the VC redistributable being in the machine folder), console subsystem (the default for main()).
Push-Location $src
try {
  & cl.exe /nologo /O2 /MT shell.c sqlite3.c /Fe:sqlite3.exe
  if ($LASTEXITCODE -ne 0) { throw "sqlite: cl.exe failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }

$exe = Join-Path $src 'sqlite3.exe'
Copy-Item -Force $exe -Destination (Join-Path $OutDir 'sqlite3.exe')
$dest = Join-Path (Resolve-Path $OutDir).Path 'sqlite3.exe'
Write-Host ("sqlite: built {0} ({1:N0} bytes, SHA-256 {2})" -f $dest, (Get-Item $dest).Length, (Get-Sha256 $dest))
