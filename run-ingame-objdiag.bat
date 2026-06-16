@echo off
REM In-game Lorencia con MU_GPUINSTOBJ=1 + MU_JOBSDIAG=1: mide si props instancian
REM ([obj_inst] draws/instances) y por que se rechazan ([objdiag] notranslate/blendExcl).
REM Logs en gl_log.txt del dir Release. Login test0/test0 -> entrar a Lorencia + caminar.
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
set "MU_JOBSDIAG=1"
set "MU_GPULOG=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [ingame-objdiag] connect 127.0.0.1:44406  (login test0/test0 -> Lorencia, caminar unos pasos)
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
endlocal
