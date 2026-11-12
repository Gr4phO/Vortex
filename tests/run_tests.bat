@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."
set "VORTEX_BIN=%ROOT_DIR%\vortex.exe"

if not exist "%VORTEX_BIN%" (
    if exist ".\vortex.exe" (
        set "VORTEX_BIN=.\vortex.exe"
    ) else (
        echo [ERROR] vortex.exe not found! Run build.bat first.
        exit /b 1
    )
)

echo ========================================================
echo   VORTEX AUTOMATED TEST SUITE
echo ========================================================

set "FAILED=0"
set "PASSED=0"

for %%t in (test_struct.vx test_vec_struct.vx test_fib.vx test_ffi.vx test_memory_1m.vx) do (
    echo.
    echo --- [RUN] %%t ---
    "%VORTEX_BIN%" "%SCRIPT_DIR%%%t"
    if !errorlevel! neq 0 (
        echo [FAIL] %%t exited with code !errorlevel!
        set /a FAILED+=1
    ) else (
        echo [PASS] %%t
        set /a PASSED+=1
    )
)

echo.
echo ========================================================
echo   SUMMARY: !PASSED! PASSED, !FAILED! FAILED
echo ========================================================

if !FAILED! neq 0 exit /b 1
exit /b 0
