# docs/slices/uwpswap.md round 12: which RPC does the native dxgi make in EnumAdapters, and what does machine-native-d3d do?
# (1) the RPCRT4 imports and the RPC_CLIENT_INTERFACE structures (interface UUID + version; the transfer syntax at +0x18 is
# NDR 8A885D04-... or NDR64 71710533-...) and the binding strings found in the native closure of the runner;
# (2) the guest on both machines with +dispatch: 'ipc:' lines (pipe / RPC Control opens with their status, Wine's rpcrt4
# has no trace), the exceptions raised during EnumAdapters with the callers of each. Informational.
param([string]$Bin = 'build/bin', [string]$ObjDump = (Join-Path $env:LLVM_MINGW_WIN 'bin/llvm-objdump.exe'))
$sys = Join-Path $env:WINDIR 'System32'
$ndr = [byte[]](0x04,0x5D,0x88,0x8A,0xEB,0x1C,0xC9,0x11,0x9F,0xE8,0x08,0x00,0x2B,0x10,0x48,0x60)
$ndr64 = [byte[]](0x33,0x05,0x71,0x71,0xBA,0xBE,0x37,0x49,0x83,0x19,0xB5,0xDB,0xEF,0x9C,0xCC,0x36)
function Find-Bytes([byte[]]$h, [byte[]]$n, [int]$from) {
  $r = New-Object System.Collections.Generic.List[int]
  $last = $h.Length - $n.Length
  for ($i = $from; $i -le $last; $i++) {
    if ($h[$i] -ne $n[0]) { continue }
    $ok = $true; for ($k = 1; $k -lt $n.Length; $k++) { if ($h[$i + $k] -ne $n[$k]) { $ok = $false; break } }
    if ($ok) { $r.Add($i) }
  }
  return $r
}
foreach ($d in 'dxgi','d3d11','dxcore','powrprof','umpdc','d3d12','dwmapi','d3d10warp','d3dcompiler_47','msvcp_win','D3DSCache','cfgmgr32','setupapi') {
  $f = Join-Path $sys "$d.dll"
  if (-not (Test-Path $f)) { Write-Host "== ${d}: absent"; continue }
  Write-Host "== $d.dll ($((Get-Item $f).Length) bytes)"
  $imp = & $ObjDump -p $f 2>&1
  $cur = ''; $names = @()
  foreach ($l in $imp) {
    if ($l -match 'DLL Name: (\S+)') { $cur = $Matches[1]; continue }
    if ($cur -match '^(?i)(rpcrt4|api-ms-win-core-rpc\S*|ext-ms-win-rpc\S*|sechost)') { if ($l -match '^\s+[0-9a-fA-F]+\s+(\w+)\s*$') { $names += "$cur!$($Matches[1])" } }
  }
  Write-Host "  rpc-ish imports ($($names.Count)): $($names -join ' ')"
  $b = [IO.File]::ReadAllBytes($f)
  if ($d -eq 'dxgi') {   # the whole import table, compact: who the native dxgi talks to
    $cur2 = ''; $acc = [ordered]@{}
    foreach ($l in $imp) {
      if ($l -match 'DLL Name: (\S+)') { $cur2 = $Matches[1]; $acc[$cur2] = @(); continue }
      if ($cur2 -and $l -match '^\s+[0-9a-fA-F]+\s+(\w+)\s*$') { $acc[$cur2] += $Matches[1] }
    }
    foreach ($k in $acc.Keys) { $v = $acc[$k]; if ($k -match '(?i)^(kernel32|msvcrt|ntdll|vcruntime|api-ms-win-crt)') { Write-Host "  import $k ($($v.Count))" } else { Write-Host "  import ${k}: $($v -join ' ')" } }
  }
  # UTF-16 strings within 160 bytes of each L"ncalrpc" (the binding's endpoint is usually next to it)
  $uni = [Text.Encoding]::Unicode.GetBytes('ncalrpc')
  foreach ($p in (Find-Bytes $b $uni 0 | Select-Object -First 4)) {
    $lo = [Math]::Max(0, $p - 160); $hi = [Math]::Min($b.Length - 1, $p + 200)
    $seg = [Text.Encoding]::Unicode.GetString($b[$lo..$hi])
    $near = [regex]::Matches($seg, '[\x20-\x7e]{4,}') | ForEach-Object { $_.Value }
    Write-Host ("  near L'ncalrpc' @file+0x{0:x}: {1}" -f $p, ($near -join ' | '))
  }
  foreach ($syn in @(@('NDR', $ndr), @('NDR64', $ndr64))) {
    foreach ($p in (Find-Bytes $b $syn[1] 24)) {
      $o = $p - 0x18
      if ([BitConverter]::ToUInt32($b, $o) -ne 0x60) { continue }
      $g = [Guid]::new([byte[]]$b[($o + 4)..($o + 19)])
      $ver = "$([BitConverter]::ToUInt16($b, $o + 20)).$([BitConverter]::ToUInt16($b, $o + 22))"
      Write-Host ("  RPC_CLIENT_INTERFACE at file+0x{0:x}: {1} v{2} ({3})" -f $o, $g, $ver, $syn[0])
    }
  }
  # binding strings: ASCII and UTF-16 runs
  $txt = [Text.Encoding]::ASCII.GetString($b) + "`n" + [Text.Encoding]::Unicode.GetString($b) + "`n" + [Text.Encoding]::Unicode.GetString($b[1..($b.Length - 1)])
  [regex]::Matches($txt, '(?i)(ncalrpc|ncacn_\w+|ncadg_\w+|\\RPC Control\\[\w\-\.\\]*|lrpc[\w\-\.\\]*|epmapper|\\pipe\\[\w\-\.\\]+)') |
    ForEach-Object { $_.Value } | Sort-Object -Unique | Select-Object -First 30 | ForEach-Object { Write-Host "  string: $_" }
}
$exe = Join-Path $Bin 'win10xbox.exe'
foreach ($m in 'machine-native-d3d', 'machine-emu-d3d') {
  $md = Join-Path $Bin $m
  if (-not (Test-Path $md)) { Write-Host "probe: no $md"; continue }
  $env:WIN10XBOX_DEBUG = '+dispatch'
  $out = Join-Path $Bin "rpc_$m.out"; $err = Join-Path $Bin "rpc_$m.log"
  $p = Start-Process $exe -ArgumentList '--machine', (Resolve-Path $md).Path, 'C:\guest\guest_d3d11_swap.exe' `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
  $null = $p.Handle
  if (-not $p.WaitForExit(150000)) { $p.Kill(); $p.WaitForExit(); Write-Host "rpc probe $m killed after 150 s" }
  Write-Host "=== $m exit $($p.ExitCode)"
  $all = @(Get-Content $err)
  Write-Host "  trace lines: $($all.Count); ipc: opens of pipes / RPC Control objects (all):"
  $all | Where-Object { $_ -match 'ipc: ' } | Select-Object -First 60 | ForEach-Object { Write-Host ("    " + $_.Substring(0, [Math]::Min(260, $_.Length))) }
  Write-Host "  EnumAdapters and the exceptions around it:"
  $all | Where-Object { $_ -match 'dxgi: EnumAdapters|dxgi:   exc|dxgi:   debug|dxgi: fail-fast|dxgi:   stack|dxgi: EnumAdapters failed' } |
    Select-Object -First 400 | ForEach-Object { Write-Host ("    " + $_.Substring(0, [Math]::Min(260, $_.Length))) }
}
exit 0
