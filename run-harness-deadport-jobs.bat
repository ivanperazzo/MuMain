@echo off
REM Like run-harness-deadport but with MU_JOBS=1 (parallel Phase B char build).
REM Args: <blendinst> <waveinst> <tag> [chars]   (chars defaults to 100)
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
del /q gl_log.txt 2>nul
del /q harness_shot.jpg 2>nul
set "CHARS=%~4"
if "%CHARS%"=="" set "CHARS=100"
set "MU_TEST_CHARS=%CHARS%"
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
set "MU_JOBS=1"
echo [dead] JOBS blendinst=%~1 waveinst=%~2 tag=%~3 chars=%CHARS%
start "" "%REL%\Main.exe" connect /u127.0.0.1 /p44999
set /a tries=0
:wait
if exist harness_shot.jpg goto done
set /a tries+=1
if %tries% GTR 180 ( echo [dead] TIMEOUT no shot & taskkill /F /IM Main.exe >nul 2>nul & copy /y gl_log.txt "gl_log_%~3.txt" >nul & exit /b 2 )
ping -n 2 127.0.0.1 >nul
goto wait
:done
ping -n 5 127.0.0.1 >nul
taskkill /F /IM Main.exe >nul 2>nul
copy /y gl_log.txt "gl_log_%~3.txt" >nul
copy /y harness_shot.jpg "shot_%~3.jpg" >nul
echo [dead] done tag=%~3
endlocal
