<#
.SYNOPSIS
  Informational CI probe for docs/slices/uwpswap.md (machine-emu-d3d). EnumAdapters(0) of the native dxgi fail-fasts with
  STATUS_PROCEDURE_NOT_FOUND where the native-gui machine succeeds: a GetProcAddress or a delay-load import of a function
  that the real user32/gdi32/win32u/imm32 export and Wine's builtins do not. This lists, for the native dxgi, d3d11, dxcore,
  d3d10warp (and dwmapi):
    1. the normal and the delay-load import tables (parsed from the PE headers, no objdump needed), and every name imported
       from user32/gdi32/win32u/imm32 (or from an API-set DLL: checked against the union of all Wine builtins) that Wine's
       builtins do not export;
    2. every ASCII string of the image that is the name of an export of the runner's real user32/gdi32/win32u/imm32 (a
       GetProcAddress by string constant) and is not exported by Wine's builtin of the same DLL.
  Output lines: "delayprobe: <image> <kind> <dll>!<name> MISSING" (kind = import | delay | string).
#>
param([string]$Builtin = 'wine-pe/out/builtin', [string]$System32 = "$env:SystemRoot\System32")
$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic; using System.Text;
public class PeInfo {
  public byte[] B; public int[] SecVa, SecRaw, SecSize; public int NSec; public long Is64;
  public int DirRva(int i) { int oh = BitConverter.ToInt32(B, 0x3c) + 24; int off = oh + (Is64 != 0 ? 112 : 96) + i * 8; return BitConverter.ToInt32(B, off); }
  public int Off(int rva) { for (int i = 0; i < NSec; i++) if (rva >= SecVa[i] && rva < SecVa[i] + Math.Max(SecSize[i], 1)) return rva - SecVa[i] + SecRaw[i]; return -1; }
  public string Str(int off) { int e = off; while (e < B.Length && B[e] != 0) e++; return Encoding.ASCII.GetString(B, off, e - off); }
  public static PeInfo Load(string path) {
    var p = new PeInfo(); p.B = File.ReadAllBytes(path);
    int pe = BitConverter.ToInt32(p.B, 0x3c); p.NSec = BitConverter.ToUInt16(p.B, pe + 6);
    int opt = BitConverter.ToUInt16(p.B, pe + 20); int oh = pe + 24; p.Is64 = BitConverter.ToUInt16(p.B, oh) == 0x20b ? 1 : 0;
    int sh = oh + opt; p.SecVa = new int[p.NSec]; p.SecRaw = new int[p.NSec]; p.SecSize = new int[p.NSec];
    for (int i = 0; i < p.NSec; i++) { int s = sh + i * 40; p.SecVa[i] = BitConverter.ToInt32(p.B, s + 12); p.SecSize[i] = Math.Max(BitConverter.ToInt32(p.B, s + 8), BitConverter.ToInt32(p.B, s + 16)); p.SecRaw[i] = BitConverter.ToInt32(p.B, s + 20); }
    return p;
  }
  public List<string> Exports() {
    var r = new List<string>(); int rva = DirRva(0); if (rva == 0) return r; int o = Off(rva); if (o < 0) return r;
    int n = BitConverter.ToInt32(B, o + 24), names = Off(BitConverter.ToInt32(B, o + 32));
    for (int i = 0; i < n; i++) r.Add(Str(Off(BitConverter.ToInt32(B, names + 4 * i))));
    return r;
  }
  // kind "import" (directory 1) or "delay" (directory 13): list of "dll!name" ("dll!#ord" by ordinal)
  public List<string> Imports(bool delay) {
    var r = new List<string>(); int rva = DirRva(delay ? 13 : 1); if (rva == 0) return r; int o = Off(rva); if (o < 0) return r;
    for (;; o += delay ? 32 : 20) {
      int nameRva, intRva;
      if (delay) { if (BitConverter.ToInt32(B, o + 4) == 0) break; nameRva = BitConverter.ToInt32(B, o + 4); intRva = BitConverter.ToInt32(B, o + 16); }
      else { if (BitConverter.ToInt32(B, o + 12) == 0) break; nameRva = BitConverter.ToInt32(B, o + 12); intRva = BitConverter.ToInt32(B, o); if (intRva == 0) intRva = BitConverter.ToInt32(B, o + 16); }
      string dll = Str(Off(nameRva)); int t = Off(intRva); if (t < 0) continue;
      for (;; t += 8) {
        long v = BitConverter.ToInt64(B, t); if (v == 0) break;
        if (v < 0) r.Add(dll + "!#" + (v & 0xffff)); else r.Add(dll + "!" + Str(Off((int)v) + 2));
      }
    }
    return r;
  }
  public HashSet<string> AsciiStrings() {
    var h = new HashSet<string>(); int s = -1;
    for (int i = 0; i <= B.Length; i++) {
      bool ok = i < B.Length && (B[i] == '_' || (B[i] >= '0' && B[i] <= '9') || (B[i] >= 'A' && B[i] <= 'Z') || (B[i] >= 'a' && B[i] <= 'z'));
      if (ok) { if (s < 0) s = i; } else { if (s >= 0 && i - s >= 4 && i - s < 80) h.Add(Encoding.ASCII.GetString(B, s, i - s)); s = -1; }
    }
    return h;
  }
}
"@

$gui = 'user32', 'gdi32', 'win32u', 'imm32'
$wine = @{}; $wineAll = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($f in Get-ChildItem $Builtin -Filter *.dll -ErrorAction SilentlyContinue) {
  try { $ex = [PeInfo]::Load($f.FullName).Exports() } catch { continue }
  $wine[$f.BaseName.ToLower()] = New-Object 'System.Collections.Generic.HashSet[string]' (, [string[]]$ex)
  foreach ($e in $ex) { [void]$wineAll.Add($e) }
}
Write-Host "delayprobe: wine builtins $($wine.Count) dlls; gui exports: $(($gui | ForEach-Object { "$_=$($wine[$_].Count)" }) -join ' ')"
$real = @{}
foreach ($g in $gui) { $p = Join-Path $System32 "$g.dll"; if (Test-Path $p) { $real[$g] = [PeInfo]::Load($p).Exports(); Write-Host "delayprobe: real $g exports $($real[$g].Count)" } }
$realUnion = @{}; foreach ($g in $real.Keys) { foreach ($e in $real[$g]) { if (-not $realUnion.ContainsKey($e)) { $realUnion[$e] = $g } } }

foreach ($img in 'dxgi', 'd3d11', 'dxcore', 'd3d10warp', 'dwmapi') {
  $p = Join-Path $System32 "$img.dll"; if (-not (Test-Path $p)) { Write-Host "delayprobe: $img.dll absent"; continue }
  $pe = [PeInfo]::Load($p)
  foreach ($kind in 'import', 'delay') {
    $all = $pe.Imports($kind -eq 'delay')
    Write-Host "delayprobe: $img.dll $kind table: $($all.Count) imports from $((($all | ForEach-Object { $_.Split('!')[0].ToLower() }) | Sort-Object -Unique) -join ' ')"
    foreach ($i in $all) {
      $dll, $name = $i.Split('!', 2); $b = $dll.ToLower() -replace '\.dll$', ''
      if ($gui -contains $b) {
        if ($name.StartsWith('#')) { continue }
        if (-not $wine[$b].Contains($name)) { Write-Host "delayprobe: $img.dll $kind $dll!$name MISSING" }
        elseif ($kind -eq 'delay') { Write-Host "delayprobe: $img.dll delay $dll!$name present" }
      } elseif ($b -like 'api-ms-*' -or $b -like 'ext-ms-*') {
        if (-not $name.StartsWith('#') -and -not $wineAll.Contains($name)) { Write-Host "delayprobe: $img.dll $kind $dll!$name MISSING (apiset, in no Wine builtin)" }
      }
    }
  }
  $strs = $pe.AsciiStrings()
  foreach ($s in $realUnion.Keys) {
    if ($strs.Contains($s)) { $g = $realUnion[$s]; if (-not $wine[$g].Contains($s)) { Write-Host "delayprobe: $img.dll string $g.dll!$s MISSING" } }
  }
}
