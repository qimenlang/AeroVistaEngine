@echo off
REM ============================================================
REM Windows launcher for the multichannel startup script.
REM
REM The single source of truth is the git-bash script
REM   scripts\run_multichannel.sh
REM (all exe paths, IG list, log layout live there). This .bat
REM only forwards to it, so the two never drift apart.
REM
REM Usage:
REM   scripts\run_multichannel.bat            start
REM   scripts\run_multichannel.bat stop       stop all
REM ============================================================
setlocal

set "ROOT=%~dp0.."

set "GIT_BASH=C:\Program Files\Git\bin\bash.exe"
if not exist "%GIT_BASH%" (
    echo [ERROR] git-bash not found at: %GIT_BASH%
    echo         This launcher forwards to run_multichannel.sh via git-bash.
    echo         Install Git for Windows, or run scripts\run_multichannel.sh directly.
    exit /b 1
)

set "SH=%ROOT%\scripts\run_multichannel.sh"
if not exist "%SH%" (
    echo [ERROR] missing: %SH%
    exit /b 1
)

"%GIT_BASH%" "%SH%" %*
exit /b %ERRORLEVEL%
