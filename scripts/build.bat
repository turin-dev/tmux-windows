@echo off
REM Build tmuxw with the bundled VS 2026 toolchain (CMake + Ninja + MSVC).
REM Usage: scripts\build.bat [Debug|Release]

setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Debug

set VS=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [build] failed to initialize MSVC environment
  exit /b 1
)

set ROOT=%~dp0..
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
cmake --build "%ROOT%\build" || exit /b 1

echo.
echo [build] done: %ROOT%\build\tmuxw.exe
endlocal
