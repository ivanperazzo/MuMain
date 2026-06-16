@echo off
REM Objects-instancing harness: same as run-harness-bd but with MU_GPUINSTOBJ=1.
REM run-harness-objinst.bat <tag> <charcount> <novsync 0|1> <jobs 0|1>
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
del /q gl_log.txt 2>nul
del /q harness_shot.jpg 2>nul
set "MU_TEST_CHARS=%~2"
set "MU_TEST_SHOT=600"
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUINSTOBJ=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_GPUBLENDMESH=1"
set "MU_GPUBLENDINST=1"
set "MU_GPUWAVEINST=1"
set "MU_NOVSYNC=%~3"
set "MU_JOBS=%~4"
set "MU_FPS=1"
set "MU_JOBSLOG=1"
echo [objinst] tag=%~1 chars=%~2 novsync=%~3 jobs=%~4
start "" "%REL%\Main.exe" connect /u127.0.0.1 /p44999
set /a tries=0
:wait
if exist harness_shot.jpg goto done
set /a tries+=1
if %tries% GTR 90 ( echo [objinst] TIMEOUT & taskkill /F /IM Main.exe >nul 2>nul & copy /y gl_log.txt "gl_log_%~1.txt" >nul & exit /b 2 )
ping -n 2 127.0.0.1 >nul
goto wait
:done
ping -n 4 127.0.0.1 >nul
taskkill /F /IM Main.exe >nul 2>nul
copy /y gl_log.txt "gl_log_%~1.txt" >nul
copy /y harness_shot.jpg "shot_%~1.jpg" >nul
echo [objinst] done tag=%~1
endlocal
