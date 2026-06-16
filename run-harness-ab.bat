@echo off
REM A/B harness: run-harness-ab.bat <blendinst 0|1> <waveinst 0|1> <tag>
REM Full production GPU flags + MU_GPUBLENDINST=%1 + MU_GPUWAVEINST=%2. Waits, screenshots, kills.
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
del /q gl_log.txt 2>nul
del /q harness_shot.jpg 2>nul
set "MU_TEST_CHARS=100"
set "MU_TEST_SHOT=600"
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_GPUBLENDINST=%~1"
set "MU_GPUWAVEINST=%~2"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [ab] blendinst=%~1 waveinst=%~2 tag=%~3
start "" "%REL%\Main.exe" connect /u127.0.0.1 /p44406
:wait
if not exist harness_shot.jpg ( ping -n 2 127.0.0.1 >nul & goto wait )
ping -n 5 127.0.0.1 >nul
taskkill /F /IM Main.exe >nul 2>nul
copy /y gl_log.txt "gl_log_%~3.txt" >nul
copy /y harness_shot.jpg "shot_%~3.jpg" >nul
echo [ab] done tag=%~3
endlocal
