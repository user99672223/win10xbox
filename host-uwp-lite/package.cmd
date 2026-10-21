@echo off
rem package.cmd - turn the staged AppX layout into a signed .msix.
rem Adapted from tools/probe/uwp-host/package.cmd: all command-line SDK tools
rem (makepri + makeappx + signtool), no UWP VS workload needed.
rem
rem   package.cmd [BUILDDIR]        BUILDDIR defaults to ..\build-uwp-lite
rem
rem Prerequisite: the CMake build of host-uwp-lite has run, so that
rem BUILDDIR\AppX holds the layout (the stage_appx target, part of ALL):
rem   cmake --build BUILDDIR
rem Output: BUILDDIR\AppPackages\win10xbox-uwp-lite.msix and .cer
rem
rem Tool locations come from BUILDDIR\sdk_tools.cmd (written by CMake from
rem the SDK it configured with) and fall back to PATH, which an x64 Native Tools
rem prompt or ilammy/msvc-dev-cmd sets up.
rem
rem Like the probe script: no enabledelayedexpansion, and no variable that can
rem contain a parenthesised "(x86)" is expanded inside a parenthesised block
rem (cmd parses the parentheses before expanding, which ends the block early).
rem Control flow is goto and labels only. Keep this file CRLF (.gitattributes).
setlocal
set "BUILD=%~f1"
if "%~1"=="" set "BUILD=%~dp0..\build-uwp-lite"
cd /d "%~dp0"
for %%I in ("%BUILD%") do set "BUILD=%%~fI"

set "STAGE=%BUILD%\AppX"
set "OUT=%BUILD%\AppPackages"
set "SIGN=%BUILD%\sign"
set "PRIROOT=%BUILD%\pri-root"
set "PKGNAME=win10xbox-uwp-lite"
set "MSIX=%OUT%\%PKGNAME%.msix"
set "PFX=%SIGN%\%PKGNAME%.pfx"
set "CER=%SIGN%\%PKGNAME%.cer"

if not exist "%STAGE%\%PKGNAME%.exe" goto :missing_stage
if not exist "%STAGE%\AppxManifest.xml" goto :missing_stage

if exist "%BUILD%\sdk_tools.cmd" call "%BUILD%\sdk_tools.cmd"
if not defined MAKEAPPX for %%I in (makeappx.exe) do set "MAKEAPPX=%%~$PATH:I"
if not defined MAKEPRI for %%I in (makepri.exe) do set "MAKEPRI=%%~$PATH:I"
if not defined SIGNTOOL for %%I in (signtool.exe) do set "SIGNTOOL=%%~$PATH:I"
if not defined MAKEAPPX goto :missing_sdk
if not defined MAKEPRI goto :missing_sdk
if not defined SIGNTOOL goto :missing_sdk
echo makeappx: %MAKEAPPX%
echo makepri:  %MAKEPRI%
echo signtool: %SIGNTOOL%
echo staged:   %STAGE%

echo [1/4] makepri: resources.pri
rem Index only the manifest + tile assets, not the (large) payload\ folder.
if exist "%PRIROOT%" rmdir /s /q "%PRIROOT%"
mkdir "%PRIROOT%\Assets"
xcopy /y /q "%STAGE%\Assets\*.png" "%PRIROOT%\Assets\" >nul
if errorlevel 1 goto :pri_fail
copy /y "%STAGE%\AppxManifest.xml" "%PRIROOT%\AppxManifest.xml" >nul
"%MAKEPRI%" createconfig /cf "%PRIROOT%\priconfig.xml" /dq en-US /o
if errorlevel 1 goto :pri_fail
"%MAKEPRI%" new /pr "%PRIROOT%" /cf "%PRIROOT%\priconfig.xml" /mn "%PRIROOT%\AppxManifest.xml" /of "%STAGE%\resources.pri" /o
if errorlevel 1 goto :pri_fail
if not exist "%STAGE%\resources.pri" goto :pri_fail

echo [2/4] makeappx pack
if not exist "%OUT%" mkdir "%OUT%"
if exist "%MSIX%" del "%MSIX%"
"%MAKEAPPX%" pack /h SHA256 /d "%STAGE%" /p "%MSIX%" /o
if errorlevel 1 goto :pack_fail

echo [3/4] signing certificate
if not exist "%SIGN%" mkdir "%SIGN%"
if exist "%PFX%" goto :have_cert
call :make_cert
if errorlevel 1 goto :cert_fail
:have_cert
if not exist "%PFX%" goto :cert_fail
if not exist "%CER%" goto :cert_fail

echo [4/4] signtool sign
"%SIGNTOOL%" sign /fd SHA256 /a /f "%PFX%" /p probe "%MSIX%"
if errorlevel 1 goto :sign_fail
copy /y "%CER%" "%OUT%\%PKGNAME%.cer" >nul

echo.
echo === built: %MSIX% ===
echo cert:    %OUT%\%PKGNAME%.cer  (CN=Dev, self-signed; the Device Portal installs it, see README.md)
echo.
echo Package family name: win10xbox.uwp.lite_eqad6j9zhtw80   AUMID: win10xbox.uwp.lite_eqad6j9zhtw80!App
echo Install on a laptop (PowerShell as admin for the cert import; current user for Add-AppxPackage):
echo   Import-Certificate -FilePath '%OUT%\%PKGNAME%.cer' -CertStoreLocation Cert:\LocalMachine\Root
echo   Add-AppxPackage -Path '%MSIX%'
echo Install on the console: Device Portal ^(https://^<console-ip^>:11443^), Apps, Add: upload the .cer, then the .msix.
exit /b 0

:missing_stage
echo missing %STAGE%\%PKGNAME%.exe or AppxManifest.xml - run: cmake --build %BUILD%
exit /b 2
:missing_sdk
echo makeappx.exe / makepri.exe / signtool.exe not found: run from an x64 Native Tools prompt, or run cmake configure there so that sdk_tools.cmd gets written
exit /b 2
:pri_fail
echo makepri failed
exit /b 1
:pack_fail
echo makeappx failed
exit /b 1
:cert_fail
echo certificate generation failed
exit /b 1
:sign_fail
echo signtool failed
exit /b 1

:make_cert
echo generating self-signed code-signing certificate CN=Dev
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Dev' -KeyUsage DigitalSignature -FriendlyName 'win10xbox-uwp-lite dev cert' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5); $pw = ConvertTo-SecureString -String 'probe' -Force -AsPlainText; Export-PfxCertificate -Cert $c -FilePath '%PFX%' -Password $pw | Out-Null; Export-Certificate -Cert $c -FilePath '%CER%' | Out-Null; Write-Host ('cert thumbprint ' + $c.Thumbprint)"
exit /b %errorlevel%
