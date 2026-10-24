# Runs Wine's ntdll `file` unit repeatedly (docs/slices/ntdllfile.md): through the layer exactly as
# tests/winetest/run_ntdll.cmake runs it (--machine <machine-builtin> C:\guest-wine\ntdll_test.exe file --platform=windows),
# alone, after the units that precede it in the real run, with the relay trace on, and natively for comparison.
# Every iteration that does not complete (no summary line), and every one that fails an assertion in the rename
# test (file.c:1600-1900, the precursor of the crash), is kept with the host's crash.txt.
#   ntdll_file_loop.ps1 -Bin build/bin -Exe build/winetest/ntdll/ntdll_test.exe -Out ntdllfile-loop [-N 30] [-NSeq 30] [-NTrace 30] [-NNative 30]
param(
  [string]$Bin = 'build/bin', [string]$Exe = 'build/winetest/ntdll/ntdll_test.exe', [string]$Out = 'ntdllfile-loop',
  [int]$N = 30, [int]$NSeq = 30, [int]$NTrace = 30, [int]$NNative = 30, [switch]$Gate
)
$ErrorActionPreference = 'Stop'
$Bin = (Resolve-Path $Bin).Path
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$hostExe = Join-Path $Bin 'win10xbox.exe'
$machine = (Resolve-Path (Join-Path $Bin 'machine-builtin')).Path
New-Item -ItemType Directory -Force -Path (Join-Path $machine 'guest-wine') | Out-Null
Copy-Item $Exe (Join-Path $machine 'guest-wine/ntdll_test.exe') -Force
$units = @(Get-Content (Join-Path (Split-Path $Exe) 'ntdll_test.units') | Where-Object { $_ })
$before = @(); foreach ($u in $units) { if ($u -eq 'file') { break }; $before += $u }
Write-Host "units before file in the real run: $($before -join ' ')"
$re = '(\d+) tests executed \((\d+) marked as todo, (\d+) as flaky, (\d+) failures?\), (\d+) skipped'
$crash = Join-Path $Bin 'crash.txt'
$tmp = Join-Path $env:RUNNER_TEMP 'nfl'; New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$rows = New-Object System.Collections.Generic.List[object]

function Run-Proc([string]$file, [string[]]$a, [string]$so, [string]$se, [int]$to = 120) {
  $p = Start-Process -FilePath $file -ArgumentList $a -RedirectStandardOutput $so -RedirectStandardError $se -PassThru -NoNewWindow
  $null = $p.Handle
  if (-not $p.WaitForExit($to * 1000)) { $p.Kill(); $p.WaitForExit(); return 'timeout' }
  return $p.ExitCode
}
function Save-Tail([string]$path, [string]$dest, [long]$max) {
  if (-not (Test-Path $path)) { return }
  $len = (Get-Item $path).Length
  $fs = [System.IO.File]::OpenRead($path)
  try {
    if ($len -gt $max) { $null = $fs.Seek($len - $max, 'Begin') }
    $gz = [System.IO.File]::Create($dest)
    $z = New-Object System.IO.Compression.GZipStream($gz, [System.IO.Compression.CompressionMode]::Compress)
    $fs.CopyTo($z); $z.Close(); $gz.Close()
  } finally { $fs.Close() }
}
function Measure-File([string]$mode, [int]$i, $rc, [string]$so, [string]$se, [bool]$trace) {
  $text = if (Test-Path $so) { Get-Content $so -Raw } else { '' }; if ($null -eq $text) { $text = '' }
  $best = $null
  foreach ($m in [regex]::Matches($text, $re)) { if ($null -eq $best -or [int]$m.Groups[1].Value -gt [int]$best.Groups[1].Value) { $best = $m } }
  $outcome = if ($best) { 'ok' } elseif ($rc -eq 'timeout') { 'timeout' } else { 'crashed' }
  $fails = @($text -split "`n" | Where-Object { $_ -match 'Test failed' })
  $first = if ($fails.Count) { $fails[0].Trim() } else { '' }
  $precursor = @($fails | Where-Object { $_ -match 'file\.c:(1[6-8]\d\d):' }).Count
  $tag = "$mode-$('{0:d3}' -f $i)"
  $row = [pscustomobject]@{ mode = $mode; i = $i; rc = "$rc"; outcome = $outcome; executed = $(if ($best) { $best.Groups[1].Value } else { 0 });
                            failures = $(if ($best) { $best.Groups[4].Value } else { -1 }); precursor = $precursor; first = $first }
  $rows.Add($row)
  Write-Host ("{0}: rc={1} {2} executed={3} failures={4} precursor={5} first='{6}'" -f $tag, $rc, $outcome, $row.executed, $row.failures, $precursor, $first)
  if ($outcome -ne 'ok' -or $precursor -gt 0) {
    Copy-Item $so (Join-Path $Out "$tag.out.txt") -Force
    if (Test-Path $crash) { Copy-Item $crash (Join-Path $Out "$tag.crash.txt") -Force }
    if ($trace) { Save-Tail $se (Join-Path $Out "$tag.relay.gz") 120MB }
    elseif ((Test-Path $se) -and (Get-Item $se).Length -gt 0) { Copy-Item $se (Join-Path $Out "$tag.err.txt") -Force }
  }
}
function Layer-Unit([string]$u, [string]$so, [string]$se) {
  Remove-Item (Join-Path $machine 'registry-overlay.bin') -ErrorAction SilentlyContinue
  $a = @('--machine', $machine, 'C:\guest-wine\ntdll_test.exe', $u)
  if ($u -eq 'file') { $a += '--platform=windows' }
  return Run-Proc $hostExe $a $so $se
}

$env:WIN10XBOX_DEBUG = '-all'
for ($i = 1; $i -le $N; $i++) {
  Remove-Item $crash -ErrorAction SilentlyContinue
  $so = Join-Path $tmp 'o.txt'; $se = Join-Path $tmp 'e.txt'
  $rc = Layer-Unit 'file' $so $se
  Measure-File 'layer-file' $i $rc $so $se $false
}
for ($i = 1; $i -le $NSeq; $i++) {
  foreach ($u in $before) { $null = Layer-Unit $u (Join-Path $tmp "pre.txt") (Join-Path $tmp "pre.err") }
  Remove-Item $crash -ErrorAction SilentlyContinue
  $so = Join-Path $tmp 'o.txt'; $se = Join-Path $tmp 'e.txt'
  $rc = Layer-Unit 'file' $so $se
  Measure-File 'layer-seq' $i $rc $so $se $false
}
$env:WIN10XBOX_DEBUG = '-all,+relay'
for ($i = 1; $i -le $NTrace; $i++) {
  Remove-Item $crash -ErrorAction SilentlyContinue
  $so = Join-Path $tmp 'o.txt'; $se = Join-Path $tmp 'e.txt'
  $rc = Layer-Unit 'file' $so $se
  Measure-File 'layer-trace' $i $rc $so $se $true
}
$env:WIN10XBOX_DEBUG = '-all'
for ($i = 1; $i -le $NNative; $i++) {
  $so = Join-Path $tmp 'o.txt'; $se = Join-Path $tmp 'e.txt'
  $rc = Run-Proc $Exe @('file', '--platform=windows') $so $se
  Measure-File 'native' $i $rc $so $se $false
}

$rows | Export-Csv -Path (Join-Path $Out 'results.csv') -NoTypeInformation
$md = @('| mode | runs | completed | crashed/timeout | rename-test failures (file.c:16xx-18xx) |', '|---|---|---|---|---|')
$bad = 0
foreach ($g in ($rows | Group-Object mode)) {
  $ok = @($g.Group | Where-Object { $_.outcome -eq 'ok' }).Count
  $pre = @($g.Group | Where-Object { $_.precursor -gt 0 }).Count
  $md += "| $($g.Name) | $($g.Count) | $ok | $($g.Count - $ok) | $pre |"
  if ($g.Name -like 'layer-*') { $bad += ($g.Count - $ok) }
}
$md | ForEach-Object { Write-Host $_ }
$md | Add-Content $env:GITHUB_STEP_SUMMARY
if ($Gate -and $bad -gt 0) { Write-Host "::error::ntdll:file did not complete in $bad layer iterations"; exit 1 }
exit 0
