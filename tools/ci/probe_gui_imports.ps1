<#
.SYNOPSIS
  Informational CI probe for docs/slices/uwpswap.md (machine-emu-d3d): which functions the NATIVE Direct3D closure
  (dxgi, d3d11, ... from the runner's System32) imports from DLLs that Wine's builtins provide in the guest, where
  Wine has only a spec `stub` for them (a call raises EXCEPTION_WINE_STUB) or no export at all; and what the host's
  own implementations return for the two functions the first runs named.

.EXAMPLE
  tools/ci/probe_gui_imports.ps1 -ObjDump $env:LLVM_MINGW_WIN\bin\llvm-objdump.exe -WineBuiltin wine-pe/out/builtin -WineSrc wine-pe/wine-src
  tools/ci/probe_gui_imports.ps1 -Control uictx     # native control run of user32!GetProcessUIContextInformation
#>
param(
  [string]$ObjDump, [string]$WineBuiltin, [string]$WineSrc,
  [string]$System32 = "$env:SystemRoot\System32",
  [ValidateSet('', 'uictx', 'devcaps')][string]$Control = ''
)
$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class Probe {
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryExW(string n, IntPtr h, uint f);
  [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern uint GetModuleFileNameW(IntPtr m, StringBuilder b, uint n);
  [DllImport("kernel32", CharSet=CharSet.Ansi, SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
  [DllImport("kernel32")] public static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32")] public static extern void SetLastError(uint e);
  [DllImport("kernel32")] public static extern uint GetLastError();
  [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate long Fn2(IntPtr a, IntPtr b);
  [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate long Fn5(IntPtr a, IntPtr b, ushort c, IntPtr d, IntPtr e);
}
"@

if ($Control -eq 'uictx') {
  foreach ($m in 'user32.dll', 'win32u.dll') {
    $h = [Probe]::LoadLibraryExW($m, [IntPtr]::Zero, 0)
    $name = if ($m -eq 'user32.dll') { 'GetProcessUIContextInformation' } else { 'NtUserGetProcessUIContextInformation' }
    $p = [Probe]::GetProcAddress($h, $name)
    Write-Host "control: $m!$name at $p"
    if ($p -eq [IntPtr]::Zero) { continue }
    $f = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($p, [Probe+Fn2])
    foreach ($proc in @([Probe]::GetCurrentProcess(), [IntPtr]::Zero)) {
      $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(64)
      for ($i = 0; $i -lt 64; $i++) { [Runtime.InteropServices.Marshal]::WriteByte($buf, $i, 0xCC) }
      [Probe]::SetLastError(0x1234)
      $r = $f.Invoke($proc, $buf)
      $le = [Probe]::GetLastError()
      $w = 0..3 | ForEach-Object { '{0:x8}' -f [Runtime.InteropServices.Marshal]::ReadInt32($buf, 4 * $_) }
      Write-Host ("control: {0}!{1}(process {2}, buf) -> {3:x} (rax), GetLastError {4:x}, buffer dwords {5}" -f $m, $name, $proc, $r, $le, ($w -join ' '))
    }
  }
  exit 0
}
if ($Control -eq 'devcaps') {
  $h = [Probe]::LoadLibraryExW('gdi32.dll', [IntPtr]::Zero, 0)
  $p = [Probe]::GetProcAddress($h, 'DeviceCapabilitiesExW')
  Write-Host "control: gdi32!DeviceCapabilitiesExW at $p"
  if ($p -ne [IntPtr]::Zero) {
    $f = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($p, [Probe+Fn5])
    foreach ($cap in 2, 3, 18) {   # DC_PAPERS, DC_PAPERSIZE, DC_FIELDS... (DeviceCapabilitiesW's argument layout is assumed)
      [Probe]::SetLastError(0x1234)
      $r = $f.Invoke([IntPtr]::Zero, [IntPtr]::Zero, [uint16]$cap, [IntPtr]::Zero, [IntPtr]::Zero)
      Write-Host ("control: DeviceCapabilitiesExW(NULL, NULL, {0}, NULL, NULL) -> {1} (rax {1:x}), GetLastError {2:x}" -f $cap, $r, [Probe]::GetLastError())
    }
  }
  exit 0
}

# ---- the import survey ----
function Get-Imports($file) {   # @{ 'USER32.dll' = @('Name', ...) }
  $res = [ordered]@{}; $cur = $null
  foreach ($l in (& $ObjDump -p $file 2>$null)) {
    if ($l -match 'DLL Name:\s*(\S+)') { $cur = $Matches[1]; if (-not $res.Contains($cur)) { $res[$cur] = @() }; continue }
    if ($cur -and $l -match '^\s+\d+\s+(\S+)\s*$') { $res[$cur] += $Matches[1] }
  }
  $res
}
function Get-Exports($file) {
  $s = New-Object 'System.Collections.Generic.HashSet[string]'
  foreach ($l in (& $ObjDump -p $file 2>$null)) { if ($l -match '^\s*\d+\s+0x[0-9a-fA-F]+\s+(\S+)') { [void]$s.Add($Matches[1]) } }
  , $s
}
function Get-SpecStubs($dll) {   # names the Wine spec declares `stub` (they ARE exported; calling one raises EXCEPTION_WINE_STUB)
  $s = New-Object 'System.Collections.Generic.HashSet[string]'
  foreach ($f in (Get-ChildItem (Join-Path $WineSrc "dlls/$dll") -Filter *.spec -ErrorAction SilentlyContinue)) {
    foreach ($l in (Get-Content $f)) {
      if ($l -match '^\s*(@|\d+)\s+stub\s+(?:-\S+\s+)*([A-Za-z0-9_@?$.]+)') { [void]$s.Add($Matches[2]) }
    }
  }
  , $s
}
$natives = 'dxgi', 'd3d11', 'd3d10warp', 'dxcore', 'd3dcompiler_47', 'dwmapi', 'd3d12', 'D3D12Core', 'D3DSCache', 'powrprof', 'umpdc',
           'msvcp_win', 'directxdatabasehelper', 'kernel.appcore'
$wineExp = @{}; $wineStub = @{}
$seen = @{}
foreach ($n in $natives) {
  $f = Join-Path $System32 "$n.dll"
  if (-not (Test-Path $f)) { Write-Host "survey: $n.dll not in $System32"; continue }
  $imps = Get-Imports $f
  Write-Host ("survey: {0}.dll imports from {1} DLL(s)" -f $n, $imps.Count)
  foreach ($d in $imps.Keys) {
    $h = [Probe]::LoadLibraryExW($d, [IntPtr]::Zero, 1)   # DONT_RESOLVE_DLL_REFERENCES: the host loader resolves API sets
    $target = $d
    if ($h -ne [IntPtr]::Zero) { $sb = New-Object Text.StringBuilder 520; [void][Probe]::GetModuleFileNameW($h, $sb, 520); $target = Split-Path -Leaf $sb.ToString() }
    elseif ($d -match '^(api|ext)-ms-') { $target = '?unresolved-apiset' }
    $key = $target.ToLower()
    $wine = Join-Path $WineBuiltin $key
    if (-not (Test-Path $wine)) {
      if ($d -match '^(api|ext)-ms-' -or $key -in 'user32.dll', 'gdi32.dll', 'gdi32full.dll', 'win32u.dll') {
        Write-Host ("survey: {0}.dll: {1} -> {2}: no Wine builtin of that name ({3} import(s))" -f $n, $d, $target, $imps[$d].Count)
        if ($key -in 'gdi32full.dll', 'win32u.dll') { foreach ($i in $imps[$d]) { Write-Host "survey-import: $n.dll $d($target)!$i" } }
      }
      continue
    }
    if (-not $wineExp.ContainsKey($key)) { $wineExp[$key] = Get-Exports $wine; $wineStub[$key] = Get-SpecStubs ([IO.Path]::GetFileNameWithoutExtension($key)) }
    foreach ($i in $imps[$d]) {
      $tag = if (-not $wineExp[$key].Contains($i)) { 'MISSING-IN-WINE' } elseif ($wineStub[$key].Contains($i)) { 'WINE-STUB' } else { $null }
      if ($tag) { $k = "$key!$i"; Write-Host ("survey-import: {0}.dll {1}({2})!{3} {4}" -f $n, $d, $target, $i, $tag); $seen[$k] = $tag }
    }
  }
}
Write-Host ("survey: {0} distinct unimplemented (stub or missing) Wine imports" -f $seen.Count)
$seen.Keys | Sort-Object | ForEach-Object { Write-Host ("survey-list: {0} {1}" -f $_, $seen[$_]) }
exit 0
