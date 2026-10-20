# install_laptop_uwp.ps1 - run from an ELEVATED PowerShell (admin). Imports the
# dev cert into LocalMachine\Root (needed for AppX chain trust of our
# self-signed package), then Add-AppxPackage-installs the .msix.
param(
  [string]$Cert = "$PSScriptRoot\..\uwp-host\xbox-probe.cer",
  [string]$Msix = "$PSScriptRoot\..\uwp-host\AppPackages\xbox-probe-uwp.msix"
)
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Error "Must run elevated. Open 'Windows Terminal (Admin)' and re-run."
    exit 1
}
$Cert = (Resolve-Path $Cert).Path
$Msix = (Resolve-Path $Msix).Path
Write-Host "cert: $Cert"
Write-Host "msix: $Msix"
Import-Certificate -FilePath $Cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Write-Host "cert imported to LocalMachine\Root"
Add-AppxPackage -Path $Msix -ForceApplicationShutdown
Write-Host "---"
Get-AppxPackage -Name XboxDevModeProbe | Select-Object Name, PackageFullName, InstallLocation, Status
