# deploy_xbox.ps1 - deploy the signed .msix to an Xbox Series S / X in Dev Mode
# via the Device Portal REST API (https://<console-ip>:11443).
#
# The same package works on laptop UWP and Xbox; this script only wraps the
# three Device Portal calls needed to install, launch, and pull the resulting
# JSON report back from the app's LocalState.
#
# Prerequisites (one-time, on the Xbox):
#   1. Enable Dev Mode via the Xbox Dev Mode Activation app.
#   2. In the Dev Home dashboard: set a Device Portal username+password, note
#      the console IP, and enable "Remote access".
#   3. "Install App Package" (bottom of Dev Home) must be allowed - it is by
#      default in Dev Mode.
#
# Then from the laptop:
#   .\tools\deploy_xbox.ps1 -Console 192.168.1.42 -Msix .\uwp-host\AppPackages\xbox-probe-uwp.msix -Cert .\uwp-host\xbox-probe.cer
# You will be prompted once for the Device Portal credentials; they can also be
# passed via -Credential. The script:
#   - uploads the .cer and installs it to the Xbox's trusted roots;
#   - uploads the .msix and polls until install completes;
#   - starts the app (XboxDevModeProbe_...!App);
#   - after -WaitForReport seconds, GETs probe-report.json out of the app's
#     LocalState via the Device Portal /api/filesystem/apps/file endpoint;
#   - saves it to .\results\xbox-uwp.json (overridable with -OutJson).
#
# Everything is HTTPS to a self-signed console cert, so -SkipCertificateCheck
# is forced on. All operations are read-only to the console filesystem apart
# from installing OUR OWN package and reading OUR OWN LocalState.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Console,
    [Parameter(Mandatory=$true)][string]$Msix,
    [Parameter(Mandatory=$true)][string]$Cert,
    [pscredential]$Credential,
    [string]$OutJson = (Join-Path $PSScriptRoot "..\results\xbox-uwp.json"),
    [int]$WaitForReport = 90,
    [string]$PackageFamily = "XboxDevModeProbe_eqad6j9zhtw80",
    [string]$Aumid = "XboxDevModeProbe_1.0.0.0_x64__eqad6j9zhtw80!App"
)
$ErrorActionPreference = "Stop"

if (-not $Credential) {
    $Credential = Get-Credential -Message "Device Portal credentials for ${Console}:11443"
}
$base = "https://${Console}:11443"
$Msix = (Resolve-Path $Msix).Path
$Cert = (Resolve-Path $Cert).Path
$out  = $OutJson   # may not exist yet
New-Item -Force -ItemType Directory (Split-Path $out -Parent) | Out-Null

# PS 5.1 does not support -SkipCertificateCheck on Invoke-RestMethod; override
# the global cert validator for this session instead.
if (-not ("IgnoreCerts" -as [type])) {
    Add-Type -TypeDefinition @"
using System.Net;
using System.Security.Cryptography.X509Certificates;
public class IgnoreCerts {
    public static bool Check(object s, X509Certificate c, X509Chain ch, SslPolicyErrors e) { return true; }
    public static void Install() {
        ServicePointManager.ServerCertificateValidationCallback = Check;
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
    }
}
"@ -ReferencedAssemblies System.Net.Primitives,System.Net.ServicePoint
}
[IgnoreCerts]::Install()

function Dp-Rest {
    param([string]$Path, [string]$Method = "GET", $Form = $null, $Body = $null)
    $u = "$base$Path"
    $args = @{ Uri = $u; Method = $Method; Credential = $Credential; UseBasicParsing = $true }
    if ($Form) { $args['Form'] = $Form }
    if ($Body) { $args['Body'] = $Body; $args['ContentType'] = 'application/json' }
    try {
        return Invoke-RestMethod @args
    } catch {
        $resp = $_.Exception.Response
        if ($resp) {
            $stream = $resp.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $txt = $reader.ReadToEnd()
            throw "DP $Method $Path failed: $($resp.StatusCode) $txt"
        }
        throw
    }
}

Write-Host "[1/5] Device Portal reachable at $base ?"
$v = Dp-Rest "/api/os/version"
Write-Host "    $(($v | ConvertTo-Json -Compress))"

Write-Host "[2/5] Upload + install cert ($(Split-Path $Cert -Leaf))"
$certForm = @{ cert = Get-Item $Cert }
Dp-Rest "/api/app/packagemanager/certificate" "POST" $certForm | Out-Null

Write-Host "[3/5] Upload + install .msix ($(Split-Path $Msix -Leaf))"
$pkgForm = @{ ($(Split-Path $Msix -Leaf)) = Get-Item $Msix }
Dp-Rest "/api/app/packagemanager/package?package=$([uri]::EscapeDataString((Split-Path $Msix -Leaf)))" "POST" $pkgForm | Out-Null

# Poll install progress until Code=0 (success) or Code<0 (error).
Write-Host "    polling install state..."
$deadline = (Get-Date).AddMinutes(5)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $s = Dp-Rest "/api/app/packagemanager/state"
    Write-Host "      Code=$($s.Code) PercentComplete=$($s.PercentComplete) CodeText=$($s.CodeText)"
    if ($s.Code -eq 0) { break }
    if ($s.Code -lt 0) { throw "install failed: $($s.CodeText)" }
}

Write-Host "[4/5] Launch $Aumid"
Dp-Rest "/api/taskmanager/app?appid=$([uri]::EscapeDataString($Aumid))&package=$([uri]::EscapeDataString($PackageFamily))" "POST" | Out-Null

Write-Host "[5/5] Wait up to $WaitForReport s for probe-report.json"
Start-Sleep -Seconds $WaitForReport

# Fetch the report straight out of the app's LocalState.
$fileUri = "$base/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=XboxDevModeProbe_1.0.0.0_x64__eqad6j9zhtw80&path=\LocalState&filename=probe-report.json"
Write-Host "    GET $fileUri"
try {
    $bytes = Invoke-WebRequest -Uri $fileUri -Credential $Credential -UseBasicParsing -OutFile $out
    Write-Host "    saved -> $out ($((Get-Item $out).Length) bytes)"
} catch {
    Write-Warning "could not pull report: $_"
    Write-Warning "Manually: https browser to $fileUri with your Device Portal creds"
}

Write-Host "`nDiff against baseline + laptop-uwp:"
Write-Host "  python -I tools\diff_reports.py results\baseline-win32.json results\laptop-uwp.json $out --only-changed --format md --out results\matrix.md"
