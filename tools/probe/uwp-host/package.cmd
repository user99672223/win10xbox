@echo off
rem package.cmd - assemble the UWP build + manifest + payload DLL + assets into
rem a signed .msix. All command-line SDK tools; no UWP VS workload needed.
rem
rem Deliberately no `enabledelayedexpansion` (the final echo prints the AUMID
rem which contains !App), and we avoid using any variable that contains "(x86)"
rem inside an `if (...)` block (cmd parses parens before expansion so an
rem expanded ) inside the block closes it prematurely, producing the dreaded
rem `\Windows was unexpected at this time.`).
setlocal
cd /d "%~dp0"

set "SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
set "MAKEAPPX=%SDKBIN%\makeappx.exe"
set "MAKEPRI=%SDKBIN%\makepri.exe"
set "SIGNTOOL=%SDKBIN%\signtool.exe"

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "BUILD=..\build-uwp\%CFG%"
set "STAGE=.\AppX"
set "OUT=.\AppPackages"

if not exist "%BUILD%\xbox-probe-uwp.exe" goto :missing_build
if not exist "%MAKEAPPX%" goto :missing_sdk

echo [1/5] staging to %STAGE%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 1>nul 2>nul
mkdir "%STAGE%\Assets" 1>nul 2>nul
mkdir "%OUT%" 1>nul 2>nul
copy /y "%BUILD%\xbox-probe-uwp.exe" "%STAGE%\" >nul
copy /y "%BUILD%\probe_payload.dll"  "%STAGE%\" >nul
rem makeappx pack requires the manifest to be named AppxManifest.xml inside
rem the content directory - Package.appxmanifest is the Visual-Studio-project
rem source name, which makeappx will not recognize at package time.
copy /y "Package.appxmanifest"       "%STAGE%\AppxManifest.xml" >nul
copy /y "Assets\*.png"               "%STAGE%\Assets\" >nul

echo [2/5] makepri priconfig + build
"%MAKEPRI%" createconfig /cf "%STAGE%\priconfig.xml" /dq en-US /o >nul
if errorlevel 1 goto :pri_fail
"%MAKEPRI%" new /pr "%STAGE%" /cf "%STAGE%\priconfig.xml" /mn "%STAGE%\AppxManifest.xml" /of "%STAGE%\resources.pri" /o >nul
if errorlevel 1 goto :pri_fail
del "%STAGE%\priconfig.xml" >nul 2>nul

echo [3/5] makeappx pack
set "MSIX=%OUT%\xbox-probe-uwp.msix"
if exist "%MSIX%" del "%MSIX%"
"%MAKEAPPX%" pack /h SHA256 /d "%STAGE%" /p "%MSIX%" /o
if errorlevel 1 goto :pack_fail

echo [4/5] cert / pfx
set "PFX=xbox-probe.pfx"
set "CER=xbox-probe.cer"
if not exist "%PFX%" call :make_cert
if errorlevel 1 goto :cert_fail

echo [5/5] signtool sign
"%SIGNTOOL%" sign /fd SHA256 /a /f "%PFX%" /p probe "%MSIX%"
if errorlevel 1 goto :sign_fail

echo.
echo === built: %MSIX% ===
echo cert:    %CER% (install as Trusted Root on laptop; already trusted on Xbox Dev Mode)
echo.
echo Install on laptop (PowerShell as admin for cert import; current user for Add-AppxPackage):
echo   Import-Certificate -FilePath '%~dp0%CER%' -CertStoreLocation Cert:\LocalMachine\Root
echo   Add-AppxPackage -Path '%~dp0%MSIX%'
echo Launch:
echo   explorer.exe shell:AppsFolder\XboxDevModeProbe_1.0.0.0_x64__!!App
echo.
echo Deploy to Xbox (Device Portal must be on at https://^<console-ip^>:11443):
echo   ..\tools\deploy_xbox.ps1 -Console ^<ip^> -Msix '%~dp0%MSIX%' -Cert '%~dp0%CER%'
exit /b 0

:missing_build
echo missing %BUILD%\xbox-probe-uwp.exe - run: cmake --build ..\build-uwp --config %CFG%
exit /b 2
:missing_sdk
echo makeappx.exe not at expected SDK path; edit SDKBIN at the top of package.cmd
exit /b 2
:pri_fail
echo makepri failed
exit /b 1
:pack_fail
echo makeappx failed
exit /b 1
:cert_fail
echo cert generation failed
exit /b 1
:sign_fail
echo signtool failed
exit /b 1

:make_cert
echo generating self-signed cert CN=Dev
powershell -NoProfile -ExecutionPolicy Bypass -Command "$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Dev' -KeyUsage DigitalSignature -FriendlyName 'xbox-probe dev cert' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5); $pw = ConvertTo-SecureString -String 'probe' -Force -AsPlainText; Export-PfxCertificate -Cert $c -FilePath '%~dp0%PFX%' -Password $pw | Out-Null; Export-Certificate -Cert $c -FilePath '%~dp0%CER%' | Out-Null; Write-Host ('cert thumbprint ' + $c.Thumbprint)"
exit /b %errorlevel%
