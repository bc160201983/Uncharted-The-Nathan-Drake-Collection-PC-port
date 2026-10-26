@echo off
rem Unattended test queue: drop a job.cmd into u3mp\work\queue and this window runs it
rem (output in work\queue\last-run.log, completion in work\queue\done.flag).
title Uncharted 3 test runner - leave this window open
cd /d "%~dp0.."
if not exist "work\queue" mkdir "work\queue"
echo Waiting for test jobs in work\queue\job.cmd. Close this window to stop.
:loop
if exist "work\queue\job.cmd" (
  move /y "work\queue\job.cmd" "work\queue\running.cmd" >nul
  echo [%date% %time%] running test job
  call "work\queue\running.cmd" > "work\queue\last-run.log" 2>&1
  del "work\queue\running.cmd"
  echo %date% %time%> "work\queue\done.flag"
  echo [%date% %time%] job finished
)
timeout /t 2 /nobreak >nul
goto loop
