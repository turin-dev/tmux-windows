@echo off
REM Build the tmuxw custom install wizard (Inno Setup) into dist\.
REM Usage: scripts\installer.bat [version]   (e.g. scripts\installer.bat 0.1.0)
REM Requires Inno Setup 6 (ISCC.exe). Get it: https://jrsoftware.org/isdl.php

setlocal
set VER=%1
if "%VER%"=="" set VER=0.1.0

set ROOT=%~dp0..

REM Locate ISCC: PATH first, then the default install location.
set ISCC=
for %%I in (ISCC.exe) do if not "%%~$PATH:I"=="" set ISCC=%%~$PATH:I
if "%ISCC%"=="" if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" (
  echo [installer] Inno Setup ISCC.exe not found.
  echo [installer] Install Inno Setup 6 from https://jrsoftware.org/isdl.php
  exit /b 1
)

REM Build the Release binaries first (produces build\tmuxw.exe and build\tmux.exe).
call "%ROOT%\scripts\build.bat" Release || exit /b 1

"%ISCC%" /DAppVersion=%VER% "%ROOT%\installer\tmuxw.iss" || exit /b 1

echo.
echo [installer] wizard built: %ROOT%\dist\tmuxw-%VER%-setup.exe
endlocal
