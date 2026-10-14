@echo off
REM ============================================================
REM Windows launcher for scripts\config_build.sh.
REM The git-bash script is the source of truth.
REM
REM Usage:
REM   scripts\config_build.bat
REM   scripts\config_build.bat clang-Ninja-Debug
REM   scripts\config_build.bat vs2019
REM ============================================================
setlocal

set "ROOT=%~dp0.."

set "GIT_BASH=C:\Program Files\Git\bin\bash.exe"
if not exist "%GIT_BASH%" (
    echo [ERROR] git-bash not found at: %GIT_BASH%
    echo         This launcher forwards to config_build.sh via git-bash.
    echo         Install Git for Windows, or run scripts\config_build.sh directly.
    exit /b 1
)

set "SH=%ROOT%\scripts\config_build.sh"
if not exist "%SH%" (
    echo [ERROR] missing: %SH%
    exit /b 1
)

"%GIT_BASH%" "%SH%" %*
exit /b %ERRORLEVEL%
