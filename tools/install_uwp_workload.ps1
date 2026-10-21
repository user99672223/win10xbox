# install_uwp_workload.ps1 - run from an ELEVATED PowerShell (admin).
# Adds the UWP workload + the C++ (v143) UWP tools component to VS 2022, so we
# can build a C++/WinRT UWP that Xbox's launcher accepts (real WinRT activation
# metadata) and target Windows.Xbox for the Game memory bucket.
$ErrorActionPreference = "Stop"
$setup = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
$instPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Error "Run this from an elevated (Admin) terminal."
    exit 1
}
$a = @(
  "modify",
  # Quoted explicitly: Windows PowerShell 5.1's Start-Process joins
  # -ArgumentList with spaces and doesn't quote, so the path would split at
  # "C:\Program".
  "--installPath", "`"$instPath`"",
  "--add", "Microsoft.VisualStudio.Workload.Universal",
  "--add", "Microsoft.VisualStudio.ComponentGroup.UWP.VC",
  "--includeRecommended",
  "--passive", "--norestart"
)
Write-Host "Starting VS Installer modify (a progress window will open)..."
# --passive shows a progress window but needs no clicks; the installer also
# rejects --norestart unless --passive or --quiet is given.
$p = Start-Process -FilePath $setup -ArgumentList $a -PassThru -Wait
Write-Host "VS Installer exited with code $($p.ExitCode)"
Write-Host "Done. The next build of uwp-host will have C++ UWP tooling available."
