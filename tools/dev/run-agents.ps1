# Runs one-shot Claude Code agents one after another, each on a brief file (docs/briefs/*.md), saving every report to
# reports\<brief>.txt. Run in a PowerShell window that has ANTHROPIC_API_KEY set, from the clone root:
#   powershell -ExecutionPolicy Bypass -File .\tools\dev\run-agents.ps1 -Briefs docs\briefs\swapgpu-3.md[, more]
param([Parameter(Mandatory = $true)][string[]] $Briefs, [string] $Model = 'claude-sonnet-5-5')
New-Item -ItemType Directory -Force -Path .\reports | Out-Null
foreach ($b in $Briefs) {
  if (-not (Test-Path $b)) { Write-Host "[fail] no such brief: $b"; continue }
  $name = [IO.Path]::GetFileNameWithoutExtension($b)
  $out = ".\reports\$name.txt"
  "=== $name  started $(Get-Date -Format s)" | Tee-Object -FilePath $out
  & claude -p --model $Model (Get-Content -Raw $b) 2>&1 | Tee-Object -FilePath $out -Append
  "=== $name  finished $(Get-Date -Format s)  exit $LASTEXITCODE" | Tee-Object -FilePath $out -Append
}
