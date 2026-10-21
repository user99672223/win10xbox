@echo off
rem ccheck.cmd <source.cpp> - syntax/semantic compile check of ONE probe module
rem against the frozen probe.h, without touching the shared CMake build tree (so
rem parallel authors never race on the build dir). Sets up the x64 MSVC/SDK
rem environment, compiles to a throwaway obj, prints OK or the compiler errors.
setlocal
if "%~1"=="" (echo usage: ccheck.cmd ^<source.cpp^> & exit /b 2)
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CORE=%~dp0..\probe-core"
set "OUT=%TEMP%\ccheck_%RANDOM%"
mkdir "%OUT%" 2>nul
call "%VCVARS%" >nul
cl /c /nologo /EHa /std:c++20 /permissive- /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I"%CORE%" /Fo"%OUT%\\" "%~1"
set ERR=%ERRORLEVEL%
rmdir /s /q "%OUT%" 2>nul
if "%ERR%"=="0" (echo CCHECK-OK) else (echo CCHECK-FAIL %ERR%)
exit /b %ERR%
