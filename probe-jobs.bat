@echo off
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%"
del /q harness_shot.jpg gl_log.txt 2>nul
set "MU_TEST_CHARS=100"
set "MU_TEST_SHOT=300"
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_GPUBLENDINST=1"
set "MU_GPUWAVEINST=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
set "MU_JOBS=1"
start "" "%REL%\Main.exe" connect /u127.0.0.1 /p44999
