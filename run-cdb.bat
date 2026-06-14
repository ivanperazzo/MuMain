@echo off
REM Lanza el cliente temporal bajo cdb para capturar el stack de un crash.
REM Log -> Debug\cdb-out.txt ; comandos -> Debug\cdb-cmds.txt
setlocal
set "DBG=%~dp0out\build\windows-x86\src\Debug"
cd /d "%DBG%" || (echo No se encontro %DBG% & exit /b 1)
set "CDB=C:\Users\ipera\AppData\Local\Microsoft\WindowsApps\cdbX86.exe"
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
if not "%~1"=="" (
    set "MU_TEMPORAL_CSV=%~1"
    echo [cdb] CSV -^> %~1
)
echo [cdb] lanzando bajo debugger...
"%CDB%" -g -G -logo cdb-out.txt -cf cdb-cmds.txt "%DBG%\Main.exe" connect /u127.0.0.1 /p44406
echo [cdb] terminado.
endlocal
