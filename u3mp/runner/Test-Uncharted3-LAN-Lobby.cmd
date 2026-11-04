@echo off
setlocal
rem Uncharted 3 LAN lobby test. Run from u3mp\runner. Needs:
rem   UNCHARTED_GAME_DIR = your dumped PS4 game folder (CUSA02320 was used for all offsets)
rem   UNCHARTED_PC_DIR   = the PC port install (defaults to the repository root)
rem   builds\lan-lobby-vX\boot-linked.bin built with helper\build_lan_lobby.py (not committed: it is derived from the game)
cd /d "%~dp0.."
if not defined UNCHARTED_PC_DIR set "UNCHARTED_PC_DIR=%~dp0..\.."
if "%U3MP_BUILD%"=="" set "U3MP_BUILD=lan-lobby-v4.63"
title Uncharted 3 - LAN Lobby Test (%U3MP_BUILD%)
echo Controller test: Multiplayer - LAN Party - select your profile.
echo Hosting, joining and matches are not wired yet. Close the game normally when finished.
"%UNCHARTED_PC_DIR%\python\python.exe" "runner\run_u3_probe.py" --mode baseline --seconds 0 --windowed --prepared-image "builds\%U3MP_BUILD%\boot-linked.bin" --trace-state 0x2064100 --inspect-pages --net-trace --file-trace pak23 --overlay "work\overlay-mp" --profile-store "user\lan-profiles.v1"
echo Logs: work\runtime-tests. Saved profiles: user\lan-profiles.v1.
pause
