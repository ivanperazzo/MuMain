@echo off
REM Harness de render RELEASE (sin cdb): spawnea N chars de prueba en la login scene
REM y mide FPS real sin server. Logs a gl_log.txt / harness_shot.jpg en el dir Release.
REM   uso: run-harness-rel.bat [N_chars] [gpuinst 0/1] [gpubmd 0/1] [shotFrame]
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)

set "MU_TEST_CHARS=%~1"
if "%MU_TEST_CHARS%"=="" set "MU_TEST_CHARS=100"
set "MU_GPUINST=%~2"
if "%MU_GPUINST%"=="" set "MU_GPUINST=1"
set "MU_GPUBMD=%~3"
if "%MU_GPUBMD%"=="" set "MU_GPUBMD=1"
set "MU_TEST_SHOT=%~4"
if "%MU_TEST_SHOT%"=="" set "MU_TEST_SHOT=300"
set "MU_SKINSKIP=%~5"
if "%MU_SKINSKIP%"=="" set "MU_SKINSKIP=0"

echo [harness-rel] chars=%MU_TEST_CHARS% gpuinst=%MU_GPUINST% gpubmd=%MU_GPUBMD% shot=%MU_TEST_SHOT% skinskip=%MU_SKINSKIP%
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
echo [harness-rel] terminado.
endlocal
