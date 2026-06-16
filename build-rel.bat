@echo off
REM One-shot Release build wrapper: VS2026 Insiders env + VS Installer on PATH (AOT link).
setlocal
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x86 || exit /b 1
cd /d "%~dp0" || exit /b 1
cmake --build --preset windows-x86-release
exit /b %ERRORLEVEL%
