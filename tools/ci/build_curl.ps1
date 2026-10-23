# Builds curl's command-line tool (curl.exe, libcurl linked in statically, static CRT) with CMake + MSVC as a guest
# for the layer: a real network client that is not ours (docs/TESTING.md, "Real programs"). No TLS at all
# (-DCURL_ENABLE_SSL=OFF, -DCURL_USE_SCHANNEL=OFF) and no optional dependency (zlib, brotli, zstd, nghttp2, libpsl,
# libssh2, libidn2, LDAP, SSPI), so the only system DLLs it imports are the Win32 base set and ws2_32.
#
#   tools/ci/build_curl.ps1 [-Out build/bin/machine/guest] [-Work <dir>]      (needs cl, cmake, ninja: ilammy/msvc-dev-cmd)
#
# The release tarball is pinned by SHA-256 and verified on EVERY run, including when ci.yml restored it from the cache
# (the cache key carries the same hash), so the bytes compiled are always the ones this was measured with.
param(
  [string] $Out = 'build/bin/machine/guest',
  [string] $Work = (Join-Path $env:RUNNER_TEMP 'curl-src'),
  [string] $Version = '8.11.1',
  [string] $Sha256 = 'a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80'
)
$ErrorActionPreference = 'Stop'
$tag = 'curl-' + $Version.Replace('.', '_')
$urls = @(
  "https://github.com/curl/curl/releases/download/$tag/curl-$Version.tar.gz",
  "https://curl.se/download/curl-$Version.tar.gz"
)
$tgz = Join-Path $Work "curl-$Version.tar.gz"
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Test-Tarball {
  (Test-Path $tgz) -and ((Get-FileHash -Algorithm SHA256 $tgz).Hash.ToLowerInvariant() -eq $Sha256)
}
if (-not (Test-Tarball)) {
  $ok = $false
  foreach ($attempt in 1..3) {
    foreach ($url in $urls) {
      try {
        Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
        if (Test-Tarball) { $ok = $true; break }
        Write-Host "attempt ${attempt} ${url}: SHA-256 $((Get-FileHash -Algorithm SHA256 $tgz).Hash.ToLowerInvariant()), expected $Sha256"
      } catch { Write-Host "attempt ${attempt} ${url}: $_" }
    }
    if ($ok) { break }
    Start-Sleep -Seconds (5 * $attempt)
  }
  if (-not $ok) { Write-Host "::error::could not download curl-$Version.tar.gz with the pinned SHA-256"; exit 1 }
} else {
  Write-Host "curl-$Version.tar.gz present, SHA-256 verified"
}

$srcRoot = Join-Path $Work "curl-$Version"
if (Test-Path $srcRoot) { Remove-Item -Recurse -Force $srcRoot }
# Relative names from inside $Work: a GNU tar would read "C:\..." as host:file.
Push-Location $Work
try { tar -xzf "curl-$Version.tar.gz" } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Host '::error::tar failed'; exit 1 }

$bld = Join-Path $Work 'build'
if (Test-Path $bld) { Remove-Item -Recurse -Force $bld }
$cfg = @(
  '-S', $srcRoot, '-B', $bld, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
  '-DCMAKE_C_COMPILER=cl',
  '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON', '-DBUILD_STATIC_CURL=ON', '-DCURL_STATIC_CRT=ON',
  '-DBUILD_CURL_EXE=ON', '-DBUILD_TESTING=OFF', '-DBUILD_EXAMPLES=OFF',
  '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF', '-DENABLE_CURL_MANUAL=OFF',
  '-DCURL_ENABLE_SSL=OFF', '-DCURL_USE_SCHANNEL=OFF', '-DCURL_WINDOWS_SSPI=OFF',
  '-DCURL_ZLIB=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF', '-DUSE_NGHTTP2=OFF', '-DUSE_NGTCP2=OFF',
  '-DUSE_LIBIDN2=OFF', '-DUSE_WIN32_IDN=OFF', '-DCURL_USE_LIBPSL=OFF', '-DCURL_USE_LIBSSH2=OFF',
  '-DCURL_USE_LIBSSH=OFF', '-DCURL_USE_GSSAPI=OFF', '-DCURL_DISABLE_LDAP=ON', '-DCURL_DISABLE_LDAPS=ON',
  '-DCURL_USE_PKGCONFIG=OFF', '-DENABLE_UNICODE=OFF'
)
& cmake @cfg
if ($LASTEXITCODE -ne 0) { Write-Host '::error::curl cmake configure failed'; exit 1 }
& cmake --build $bld --target curl
if ($LASTEXITCODE -ne 0) { Write-Host '::error::curl build failed'; exit 1 }

$exe = Get-ChildItem -Path $bld -Recurse -Filter curl.exe | Select-Object -First 1
if (-not $exe) { Write-Host '::error::curl.exe not found in the build tree'; exit 1 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item $exe.FullName (Join-Path $Out 'curl.exe') -Force
Write-Host "curl.exe $($exe.Length) bytes -> $(Join-Path $Out 'curl.exe')"
# The DLLs it imports (what the machine folder must provide), and the build's own idea of what it is.
& dumpbin /nologo /dependents $exe.FullName
& $exe.FullName --version
