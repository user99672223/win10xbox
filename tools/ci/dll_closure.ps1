# Prints the DLL dependency closure of a guest program against a machine folder: every DLL the loader would have to
# find (the layer's rule: Windows\System32 first, Windows\builtin when there is no native copy; ntdll, kernel32,
# kernelbase, win32u, user32, gdi32 and imm32 are always builtin), and which imports have no file in either place. A
# missing DLL is tagged "on runner" when the runner's own System32 has it (add it to machine/guest_dlls.txt) or
# "not on runner" (a Wine builtin has to supply it: wine-pe/dlls.txt). Names starting api-ms-/ext-ms- are API sets,
# resolved by the schema, not by file; they are listed separately. Report only: always exits 0.
#
#   tools/ci/dll_closure.ps1 -Program build/bin/machine/guest/fastfetch.exe -Machine build/bin/machine   (needs dumpbin)
param(
  [Parameter(Mandatory)] [string] $Program,
  [Parameter(Mandatory)] [string] $Machine
)
$ErrorActionPreference = 'Continue'
$sys = Join-Path $Machine 'Windows\System32'
$blt = Join-Path $Machine 'Windows\builtin'
$runner = Join-Path $env:SystemRoot 'System32'
$alwaysBuiltin = 'ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'win32u.dll', 'user32.dll', 'gdi32.dll', 'imm32.dll'

function Get-Deps([string] $file) {
  $hard = @(); $delay = @(); $mode = 'hard'
  foreach ($l in (& dumpbin /nologo /dependents $file 2>&1)) {
    $t = "$l".Trim()
    if ($t -match 'delay load dependencies') { $mode = 'delay'; continue }
    if ($t -match '^Summary') { break }
    if ($t -match '^[A-Za-z0-9_.+-]+\.(dll|DLL)$') { if ($mode -eq 'hard') { $hard += $t.ToLowerInvariant() } else { $delay += $t.ToLowerInvariant() } }
  }
  ,@($hard, $delay)
}

$seen = @{}; $apis = @{}; $missing = @{}
$queue = New-Object System.Collections.Queue
$queue.Enqueue(@((Resolve-Path $Program).Path, (Split-Path -Leaf $Program).ToLowerInvariant(), 'hard'))
while ($queue.Count) {
  $item = $queue.Dequeue(); $path = $item[0]; $name = $item[1]
  $hd = Get-Deps $path
  foreach ($kind in 'hard', 'delay') {
    foreach ($d in $hd[$(if ($kind -eq 'hard') { 0 } else { 1 })]) {
      if ($d -like 'api-ms-*' -or $d -like 'ext-ms-*') { $apis[$d] = "$name ($kind)"; continue }
      if ($seen.ContainsKey($d) -or $missing.ContainsKey($d)) { continue }
      $n = Join-Path $sys $d; $b = Join-Path $blt $d
      $found = if ($alwaysBuiltin -contains $d) { $b } elseif (Test-Path $n) { $n } elseif (Test-Path $b) { $b } else { $null }
      if ($found -and (Test-Path $found)) {
        $seen[$d] = if ($found -eq $b) { 'builtin' } else { 'native' }
        if ($kind -eq 'hard') { $queue.Enqueue(@($found, $d, $kind)) }
      } else {
        $missing[$d] = "needed by $name ($kind); " + $(if (Test-Path (Join-Path $runner $d)) { 'on runner' } else { 'not on runner' })
      }
    }
  }
}
Write-Host "dll closure of ${Program}: $($seen.Count) DLLs found, $($missing.Count) missing, $($apis.Count) API sets"
foreach ($k in ($seen.Keys | Sort-Object)) { Write-Host ("  found   {0,-34} {1}" -f $k, $seen[$k]) }
foreach ($k in ($missing.Keys | Sort-Object)) { Write-Host ("  MISSING {0,-34} {1}" -f $k, $missing[$k]) }
foreach ($k in ($apis.Keys | Sort-Object)) { Write-Host ("  apiset  {0,-50} {1}" -f $k, $apis[$k]) }
exit 0
