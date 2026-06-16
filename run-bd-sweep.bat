@echo off
REM Frame-breakdown sweep: empty/25/100/200 (uncapped), 100 capped, 100 jobs-on.
call "%~dp0run-harness-bd.bat" bd_empty   0   1 0
call "%~dp0run-harness-bd.bat" bd_25      25  1 0
call "%~dp0run-harness-bd.bat" bd_100     100 1 0
call "%~dp0run-harness-bd.bat" bd_200     200 1 0
call "%~dp0run-harness-bd.bat" bd_100cap  100 0 0
call "%~dp0run-harness-bd.bat" bd_100jobs 100 1 1
echo [sweep] ALL DONE
