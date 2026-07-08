@echo off
REM Build a Release tmuxw and bundle a portable zip for distribution.
REM Usage: scripts\package.bat [version]   (e.g. scripts\package.bat 0.1.0)

setlocal
set VER=%1
if "%VER%"=="" set VER=dev

set ROOT=%~dp0..
call "%ROOT%\scripts\build.bat" Release || exit /b 1

set OUT=%ROOT%\dist
set STAGE=%OUT%\tmuxw
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%STAGE%" || exit /b 1

copy /y "%ROOT%\build\tmuxw.exe"            "%STAGE%\" >nul
copy /y "%ROOT%\build\tmux.exe"             "%STAGE%\" >nul
copy /y "%ROOT%\README.md"                  "%STAGE%\" >nul
copy /y "%ROOT%\LICENSE"                     "%STAGE%\" >nul
copy /y "%ROOT%\THIRD-PARTY-NOTICES.md"      "%STAGE%\" >nul
copy /y "%ROOT%\tmuxw.conf.example"          "%STAGE%\" >nul

set ZIP=%OUT%\tmuxw-%VER%-win-x64.zip
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force" || exit /b 1

REM Standalone exe copy + checksums for the release / winget manifest.
copy /y "%ROOT%\build\tmuxw.exe" "%OUT%\tmuxw-%VER%-win-x64.exe" >nul
powershell -NoProfile -Command "Get-FileHash '%ZIP%' -Algorithm SHA256 | Format-List; Get-FileHash '%OUT%\tmuxw-%VER%-win-x64.exe' -Algorithm SHA256 | Format-List"

echo.
echo [package] artifacts in %OUT%
endlocal
