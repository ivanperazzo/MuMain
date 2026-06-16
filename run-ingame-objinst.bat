@echo off
REM In-game (server real 44406) con MU_GPUINSTOBJ=1 + todos los flags GPU prod, para
REM medir si props instancian en Lorencia. Logs en gl_log.txt del dir Release.
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
del /q gl_log.txt 2>nul
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUINSTOBJ=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_GPUBLENDINST=1"
set "MU_GPUWAVEINST=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [ingame-objinst] connect 127.0.0.1:44406  (login test0/test0 -> Lorencia)
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
endlocal
