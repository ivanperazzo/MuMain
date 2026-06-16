@echo off
REM Harness RELEASE con TODOS los flags de producción (GPUINST/BMD/SHADOW/SKIN/BLENDMESH)
REM para medir el per-char setup en condiciones reales. Logs -> gl_log.txt en Release dir.
REM   uso: run-harness-full.bat [N_chars] [shotFrame]
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
set "MU_TEST_CHARS=%~1"
if "%MU_TEST_CHARS%"=="" set "MU_TEST_CHARS=100"
set "MU_TEST_SHOT=%~2"
if "%MU_TEST_SHOT%"=="" set "MU_TEST_SHOT=600"
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [harness-full] chars=%MU_TEST_CHARS% shot=%MU_TEST_SHOT% full-gpu-flags
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
echo [harness-full] terminado.
endlocal
