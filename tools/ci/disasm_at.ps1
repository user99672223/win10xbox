# Disassembles a System32 DLL around the RVAs that tools/ci/dumpinfo.cpp attributed to it (docs/slices/uwpswap.md round 11).
# Disassembly only, of the runner's own file. Usage: disasm_at.ps1 -ObjDump llvm-objdump.exe -Dll dxgi.dll -InfoFile dumps/dumpinfo.txt
param([string]$ObjDump, [string]$Dll = 'dxgi.dll', [string]$InfoFile, [int]$Back = 192, [int]$Fwd = 48, [int]$Max = 6)
$stem = [IO.Path]::GetFileName($Dll)
$rvas = @()
foreach ($l in Get-Content $InfoFile) {
  if ($l -match ("(?i)stack\[|rip=|exception address") -and $l -match ("(?i)" + [regex]::Escape($stem) + "\+0x([0-9a-f]+)")) {
    $r = [Convert]::ToInt64($Matches[1], 16); if ($rvas -notcontains $r) { $rvas += $r }
  }
}
if (-not $rvas) { Write-Host "disasm_at: no $stem address in $InfoFile"; exit 0 }
$path = Join-Path $env:SystemRoot "System32\$Dll"
$ib = 0
foreach ($l in (& $ObjDump -p $path 2>$null)) { if ($l -match '^ImageBase\s+(?:0x)?([0-9a-fA-F]+)') { $ib = [Convert]::ToInt64($Matches[1], 16); break } }
Write-Host ("disasm_at: {0} ImageBase 0x{1:x}, RVAs: {2}" -f $Dll, $ib, (($rvas | Select-Object -First $Max | ForEach-Object { '0x{0:x}' -f $_ }) -join ' '))
foreach ($r in ($rvas | Select-Object -First $Max)) {
  $s = $ib + $r - $Back; $e = $ib + $r + $Fwd
  Write-Host ("=== {0}+0x{1:x} (rva-{2} .. rva+{3}; the call/ud2/int 29h nearest the end is the site)" -f $Dll, $r, $Back, $Fwd)
  & $ObjDump -d --no-show-raw-insn ("--start-address=0x{0:x}" -f $s) ("--stop-address=0x{0:x}" -f $e) $path 2>&1 | ForEach-Object { Write-Host "  $_" }
}
exit 0
