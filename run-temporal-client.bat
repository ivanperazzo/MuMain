@echo off
REM Lanza el cliente temporal (worktree MuMain-temporal) contra el server local.
REM Por cmd: el host /u127.0.0.1 pasa limpio (sin el mangle de Git Bash/MSYS).
REM
REM Uso:
REM   run-temporal-client.bat                 -> conecta a 127.0.0.1:44406
REM   run-temporal-client.bat <csvPath>       -> ademas captura baseline CSV (Stage 0)
REM
REM Requiere el server OpenMU arriba: connect-server 44406 (-resolveIP:loopback),
REM game server 55901/55902, Postgres 55433. Cuentas test0-test9 (pass=user).

setlocal
set "DBG=%~dp0out\build\windows-x86\src\Debug"
cd /d "%DBG%" || (echo No se encontro %DBG% & exit /b 1)

if not "%~1"=="" (
    set "MU_TEMPORAL_CSV=%~1"
    echo [temporal] CSV baseline -^> %~1
)

echo [temporal] lanzando Main.exe connect /u127.0.0.1 /p44406
REM Path explicito: el sistema deshabilita ejecucion desde cwd (NoDefaultCurrentDirectoryInExePath).
"%DBG%\Main.exe" connect /u127.0.0.1 /p44406
endlocal
