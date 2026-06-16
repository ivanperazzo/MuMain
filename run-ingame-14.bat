@echo off
REM In-game launch del cliente temporal RELEASE con TODOS los flags de producción +
REM Etapa 1.4 (MU_GPUBLENDINST + MU_GPUWAVEINST) para validar alas/scroll de fuego en motion.
REM Uso: run-ingame-14.bat <blendinst 0|1> <waveinst 0|1>   (default 1 1)
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_GPUBLENDINST=%~1"
if "%~1"=="" set "MU_GPUBLENDINST=1"
set "MU_GPUWAVEINST=%~2"
if "%~2"=="" set "MU_GPUWAVEINST=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [ingame-1.4] blendinst=%MU_GPUBLENDINST% waveinst=%MU_GPUWAVEINST% -> connect 127.0.0.1:44406
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
endlocal
