@echo off
setlocal
rem Uncharted 3 multiplayer menu shell test (frozen menu-shell-v1 image). See Test-Uncharted3-LAN-Lobby.cmd for the variables.
cd /d "%~dp0.."
if not defined UNCHARTED_PC_DIR set "UNCHARTED_PC_DIR=%~dp0..\.."
if not exist "builds\menu-shell-v1\boot-linked.bin" (
 echo ERROR: builds\menu-shell-v1\boot-linked.bin is missing ^(see u3mp\README.md^).
 pause
 exit /b 1
)
"%UNCHARTED_PC_DIR%\python\python.exe" "runner\run_u3_probe.py" --mode baseline --seconds 0 --windowed --prepared-image "builds\menu-shell-v1\boot-linked.bin" --expected-sha256 8979cc0d2e0590d61bdf7935ab8c29e617010aa2aa096584cd868d004058554b
echo Test ended. Logs are saved in work\runtime-tests.
pause
