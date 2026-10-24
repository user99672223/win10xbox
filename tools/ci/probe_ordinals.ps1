# Probe: what is the runner's gdi32.dll export at ordinal 1004 (Wine exports 1000..1016 as unnamed stubs), and who imports
# it. Prints (1) the export-table entry of ordinals 1000..1016 of gdi32/gdi32full/win32u (name, forwarder string, or the
# first code bytes), (2) every import BY ORDINAL of the native d3d closure, (3) the import each given caller address
# (dxgi RVA of a return address in the crash dump) goes through (FF 15 call [rip+disp] -> IAT slot -> descriptor).
param([string]$System32 = "$env:SystemRoot\System32")
$ErrorActionPreference = 'Continue'

function Open-Pe($path) {
  $b = [IO.File]::ReadAllBytes($path)
  $nt = [BitConverter]::ToInt32($b, 0x3C)
  $nsec = [BitConverter]::ToUInt16($b, $nt + 6)
  $optsz = [BitConverter]::ToUInt16($b, $nt + 20)
  $opt = $nt + 24
  $secs = @(); $s = $opt + $optsz
  for ($i = 0; $i -lt $nsec; $i++) {
    $o = $s + 40 * $i
    $secs += [pscustomobject]@{ va = [BitConverter]::ToUInt32($b, $o + 12); vs = [BitConverter]::ToUInt32($b, $o + 8); raw = [BitConverter]::ToUInt32($b, $o + 20); rs = [BitConverter]::ToUInt32($b, $o + 16) }
  }
  [pscustomobject]@{ b = $b; opt = $opt; secs = $secs; path = $path }
}
function Off($pe, [uint32]$rva) {
  foreach ($s in $pe.secs) { if ($rva -ge $s.va -and $rva -lt $s.va + [Math]::Max($s.vs, $s.rs)) { return [int]($rva - $s.va + $s.raw) } }
  -1
}
function DataDir($pe, $i) { $o = $pe.opt + 112 + 8 * $i; @([BitConverter]::ToUInt32($pe.b, $o), [BitConverter]::ToUInt32($pe.b, $o + 4)) }
function Cstr($pe, [uint32]$rva) {
  $o = Off $pe $rva; if ($o -lt 0) { return '?' }
  $e = $o; while ($pe.b[$e] -ne 0) { $e++ }
  [Text.Encoding]::ASCII.GetString($pe.b, $o, $e - $o)
}

# (1) export entries
foreach ($m in 'gdi32.dll', 'gdi32full.dll', 'win32u.dll') {
  $f = Join-Path $System32 $m; if (-not (Test-Path $f)) { Write-Host "ord: $m missing"; continue }
  $pe = Open-Pe $f
  $d = DataDir $pe 0; $eo = Off $pe $d[0]
  $base = [BitConverter]::ToUInt32($pe.b, $eo + 16); $nf = [BitConverter]::ToUInt32($pe.b, $eo + 20); $nn = [BitConverter]::ToUInt32($pe.b, $eo + 24)
  $af = Off $pe ([BitConverter]::ToUInt32($pe.b, $eo + 28)); $an = Off $pe ([BitConverter]::ToUInt32($pe.b, $eo + 32)); $ao = Off $pe ([BitConverter]::ToUInt32($pe.b, $eo + 36))
  $names = @{}
  for ($i = 0; $i -lt $nn; $i++) { $names[[int][BitConverter]::ToUInt16($pe.b, $ao + 2 * $i)] = Cstr $pe ([BitConverter]::ToUInt32($pe.b, $an + 4 * $i)) }
  Write-Host "ord: $m base=$base functions=$nf names=$nn"
  for ($i = 0; $i -lt $nf; $i++) {
    $ord = $base + $i
    if ($ord -lt 1000 -or $ord -gt 1020) { continue }
    $rva = [BitConverter]::ToUInt32($pe.b, $af + 4 * $i)
    $what = ''
    if ($rva -ge $d[0] -and $rva -lt $d[0] + $d[1]) { $what = 'forwarder -> ' + (Cstr $pe $rva) }
    elseif ($rva) { $co = Off $pe $rva; if ($co -ge 0) { $what = 'code ' + (($pe.b[$co..($co + 31)] | ForEach-Object { '{0:x2}' -f $_ }) -join ' ') } }
    Write-Host ("ord: {0} #{1} rva 0x{2:x} name '{3}' {4}" -f $m, $ord, $rva, $names[$i], $what)
  }
}

# (2) + (3) imports of the closure
foreach ($n in 'dxgi', 'd3d11', 'd3d10warp', 'dxcore', 'd3dcompiler_47', 'dwmapi', 'D3DSCache', 'directxdatabasehelper', 'msvcp_win') {
  $f = Join-Path $System32 "$n.dll"; if (-not (Test-Path $f)) { continue }
  $pe = Open-Pe $f
  $d = DataDir $pe 1; if (-not $d[0]) { continue }
  $io = Off $pe $d[0]
  $slots = @{}   # IAT rva -> 'dll!name'
  for (; ; $io += 20) {
    $nameRva = [BitConverter]::ToUInt32($pe.b, $io + 12); if (-not $nameRva) { break }
    $dll = Cstr $pe $nameRva
    $oft = [BitConverter]::ToUInt32($pe.b, $io); $ft = [BitConverter]::ToUInt32($pe.b, $io + 16); if (-not $oft) { $oft = $ft }
    $to = Off $pe $oft
    for ($k = 0; ; $k++) {
      $v = [BitConverter]::ToUInt64($pe.b, $to + 8 * $k); if ($v -eq 0) { break }
      if (($v -shr 63) -eq 1) { $lbl = "$dll!#" + ($v -band 0xFFFF); Write-Host "ord-import: $n.dll imports $lbl" }
      else { $lbl = "$dll!" + (Cstr $pe ([uint32]$v + 2)) }
      $slots[[uint32]($ft + 8 * $k)] = $lbl
    }
  }
  if ($n -eq 'dxgi') {
    foreach ($ra in 0x53ae2, 0x724b6, 0x1504c) {
      for ($back = 5; $back -le 8; $back++) {
        $o = Off $pe ([uint32]($ra - $back)); if ($o -lt 0) { continue }
        if ($pe.b[$o] -eq 0xFF -and $pe.b[$o + 1] -eq 0x15 -and $back -eq 6) {
          $disp = [BitConverter]::ToInt32($pe.b, $o + 2); $slot = [uint32]($ra + $disp)
          Write-Host ("ord-caller: dxgi+0x{0:x}: call [iat 0x{1:x}] = {2}" -f $ra, $slot, $slots[$slot])
        }
      }
      $o = Off $pe ([uint32]($ra - 16)); if ($o -ge 0) { Write-Host ("ord-caller: dxgi+0x{0:x} bytes before: {1}" -f $ra, (($pe.b[$o..($o + 15)] | ForEach-Object { '{0:x2}' -f $_ }) -join ' ')) }
    }
  }
}
exit 0
