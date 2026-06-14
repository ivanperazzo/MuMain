@echo off
REM Harness de render autonomo: spawnea N chars de prueba en la login scene y corre
REM bajo cdb para medir cobertura/perf sin server.
REM   uso: run-harness.bat [N_chars] [gpuinst 0/1] [gpubmd 0/1]
setlocal
set "DBG=%~dp0out\build\windows-x86\src\Debug"
cd /d "%DBG%" || (echo No se encontro %DBG% & exit /b 1)
set "CDB=C:\Users\ipera\AppData\Local\Microsoft\WindowsApps\cdbX86.exe"

set "MU_TEST_CHARS=%~1"
if "%MU_TEST_CHARS%"=="" set "MU_TEST_CHARS=80"
set "MU_GPUINST=%~2"
if "%MU_GPUINST%"=="" set "MU_GPUINST=1"
set "MU_GPUBMD=%~3"
if "%MU_GPUBMD%"=="" set "MU_GPUBMD=1"

echo [harness] chars=%MU_TEST_CHARS% gpuinst=%MU_GPUINST% gpubmd=%MU_GPUBMD%
"%CDB%" -g -G -logo cdb-out.txt -cf cdb-cmds.txt "%DBG%\Main.exe" connect /u127.0.0.1 /p44406
echo [harness] terminado.
endlocal
