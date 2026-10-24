# docs/slices/uwpswap.md round 8: the same guest (guest_d3d11_swap.exe) on machine-native-d3d (passes) and machine-emu-d3d (fails
# in CreateDXGIFactory1), with +modules,+relay: which module is builtin or native on each, and the syscalls (name, status) from the
# moment dxgi.dll is mapped. Informational: the full traces go to build/bin/probe_<machine>.log (uploaded with the logs).
param([string]$Bin = 'build/bin', [int]$Lines = 1600)
$exe = Join-Path $Bin 'win10xbox.exe'
foreach ($m in 'machine-native-d3d', 'machine-emu-d3d') {
  $md = Join-Path $Bin $m
  if (-not (Test-Path $md)) { Write-Host "probe: no $md"; continue }
  $env:WIN10XBOX_DEBUG = '+modules,+relay,+dispatch'
  $out = Join-Path $Bin "probe_$m.out"; $err = Join-Path $Bin "probe_$m.log"
  $p = Start-Process $exe -ArgumentList '--machine', (Resolve-Path $md).Path, 'C:\guest\guest_d3d11_swap.exe' `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
  $null = $p.Handle
  if (-not $p.WaitForExit(150000)) { $p.Kill(); $p.WaitForExit(); Write-Host "probe $m killed after 150 s" }
  Write-Host "=== $m exit $($p.ExitCode)"
  Get-Content $out | Select-Object -First 12 | ForEach-Object { Write-Host "  out: $_" }
  $all = @(Get-Content $err)
  Write-Host "  trace lines: $($all.Count)"
  # module origin: a section of C:\Windows\System32\x.dll whose host path is under Windows\builtin is Wine's, else native
  $mods = foreach ($l in $all) {
    if ($l -match 'Xl_NtCreateSection section \w+: C:\\Windows\\System32\\([^ ]+) \(\d+ bytes, host path ([^)]+)\)') {
      $o = if ($Matches[2] -match '\\Windows\\builtin\\') { 'builtin' } else { 'NATIVE' }
      "{0,-34} {1}" -f $Matches[1], $o
    }
  }
  Write-Host "  modules ($(@($mods).Count)), load order:"
  $mods | ForEach-Object { Write-Host "    $_" }
  Write-Host "  failing syscalls, count by name+status (whole run):"
  $all | ForEach-Object { if ($_ -match 'trace:relay: (Nt\w+)\(.*\) ret=([cC][0-9a-fA-F]{7})') { "$($Matches[1]) $($Matches[2])" } } |
    Group-Object | Sort-Object Count -Descending | Select-Object -First 40 | ForEach-Object { Write-Host ("    {0,5} {1}" -f $_.Count, $_.Name) }
  $i = 0; for ($k = 0; $k -lt $all.Count; $k++) { if ($all[$k] -match 'MapOurs map C:\\Windows\\System32\\dxgi\.dll') { $i = $k; break } }
  $e = $all.Count - 1; for ($k = $i; $k -lt $all.Count; $k++) { if ($all[$k] -match 'W10x_CreateDXGIFactory1') { $e = $k; break } }
  Write-Host "  trace from the dxgi.dll map (lines $i..$e), without the same-file checks:"
  $all[$i..$e] | Where-Object { $_ -notmatch 'AreMappedFilesTheSame|Xl_NtCreateSection image|NtQueryPerformanceCounter|NtYieldExecution' } |
    Select-Object -First $Lines | ForEach-Object { Write-Host ("    " + $_.Substring(0, [Math]::Min(230, $_.Length))) }
}
exit 0
