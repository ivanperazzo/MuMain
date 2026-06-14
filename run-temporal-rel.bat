@echo off
REM Lanza el cliente temporal RELEASE contra el server local, con instancing+GPU skinning
REM activos (MU_GPUINST/MU_GPUBMD) para que el defer de skinning (A6a) este ACTIVO.
REM Para validar in-game: capas/cloth, side-hair, sombras, y FPS reales.
REM Requiere server OpenMU arriba (connect 44406). Cuentas test0-test9 (pass=user).
setlocal
set "REL=%~dp0out\build\windows-x86\src\Release"
cd /d "%REL%" || (echo No se encontro %REL% & exit /b 1)
set "MU_GPUBMD=1"
set "MU_GPUINST=1"
set "MU_GPUSHADOW=1"
set "MU_GPUSKIN=1"
set "MU_NOVSYNC=1"
set "MU_FPS=1"
echo [temporal-rel] GPUINST/BMD/SHADOW/SKIN/NOVSYNC=1 FPS=1 launching client connect /u127.0.0.1 /p44406
"%REL%\Main.exe" connect /u127.0.0.1 /p44406
endlocal
