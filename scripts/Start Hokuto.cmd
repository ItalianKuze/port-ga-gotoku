@echo off
rem PortGaGotoku: starts Fist of the North Star: Lost Paradise (CUSA12781) from a Windows build.
rem Double-click it. It does not need MSYS2 or Python: it runs out-win\run\bb-probe.exe directly
rem (scripts/build-win.sh builds it). The game log goes to %PGG_DATA%\logs\hokuto-<date-time>.log;
rem if the game ends with an error the window stays open with the end of the log.
rem
rem Folders (all optional):
rem   PGG_ROOT  the project folder (default: the folder above this script)
rem   PGG_DATA  the data folder (default: PortGaGotoku-data next to the project folder, as the launcher)
rem   PGG_GAME  the game folder with eboot.bin (default: game_dir in %PGG_DATA%\launcher.json, which
rem             the launcher writes when you choose the folder)
setlocal

if not defined PGG_ROOT for %%I in ("%~dp0..") do set "PGG_ROOT=%%~fI"
if not defined PGG_DATA for %%I in ("%PGG_ROOT%\..\PortGaGotoku-data") do set "PGG_DATA=%%~fI"
if not defined PGG_GAME if exist "%PGG_DATA%\launcher.json" (
    for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "(Get-Content -Raw -LiteralPath $env:PGG_DATA\launcher.json | ConvertFrom-Json).game_dir"`) do set "PGG_GAME=%%i"
)

set "RUN=%PGG_ROOT%\out-win\run"
rem Only Windows on PATH: every DLL the game needs is in %RUN%.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0"
rem No watchdog while playing; frame statistics in the log.
set "PGG_TIMEOUT=0"
set "PGG_FRAME_STATS=1"
set "PGG_DATA_DIR=%PGG_DATA%"
set "XDG_DATA_HOME=%PGG_DATA%"
rem The GPU library's folders and the port's settings in the data folder, never %APPDATA%\shadPS4.
set "PGG_GPU_USER_DIR=%PGG_DATA%\gpu"
set "PGG_GPU_CACHE_DIR=%PGG_DATA%\cache"
rem pgg.ini (bbport.ini until 0.1.2, copied the first time; the old file stays).
if not exist "%PGG_DATA%\pgg.ini" if exist "%PGG_DATA%\bbport.ini" copy /y "%PGG_DATA%\bbport.ini" "%PGG_DATA%\pgg.ini" >nul
set "PGG_CONFIG=%PGG_DATA%\pgg.ini"

if not exist "%RUN%\bb-probe.exe" ( echo Missing "%RUN%\bb-probe.exe": run scripts\build-win.sh first. & goto :fail )
if not exist "%PGG_DATA%\out\boot-linked.bin" ( echo Missing "%PGG_DATA%\out\boot-linked.bin": start the game once from the launcher, which prepares it. & goto :fail )
if not defined PGG_GAME ( echo No game folder: choose it in the launcher or set PGG_GAME. & goto :fail )
if not exist "%PGG_GAME%\eboot.bin" ( echo Missing "%PGG_GAME%\eboot.bin": choose the CUSA12781 folder in the launcher or set PGG_GAME. & goto :fail )
if not exist "%PGG_DATA%\logs" mkdir "%PGG_DATA%\logs"
if not exist "%PGG_DATA%\user" mkdir "%PGG_DATA%\user"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%i"
set "LOG=%PGG_DATA%\logs\hokuto-%STAMP%.log"

echo Starting Hokuto. Log: %LOG%
echo (this window closes by itself when the game ends normally)
cd /d "%RUN%"
"%RUN%\bb-probe.exe" "%PGG_DATA%\out\boot-linked.bin" --content-profile "%PGG_DATA%\out\content.bin" ^
    --app0 "%PGG_GAME%" --user "%PGG_DATA%\user" --timeout 0 > "%LOG%" 2>&1
set "CODE=%ERRORLEVEL%"
if "%CODE%"=="0" exit /b 0

echo.
echo The game ended with code %CODE%. Last lines of the log:
echo ------------------------------------------------------------------
powershell -NoProfile -Command "Get-Content -LiteralPath $env:LOG -Tail 25"
echo ------------------------------------------------------------------
echo Full log: %LOG%
:fail
echo.
pause
exit /b 1
