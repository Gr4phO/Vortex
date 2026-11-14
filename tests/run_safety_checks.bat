@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "VORTEX=%SCRIPT_DIR%..\vortex.exe"
if not exist "%VORTEX%" set "VORTEX=.\vortex.exe"
set PASS_COUNT=0
set FAIL_COUNT=0
set "TMP_LOG=%TEMP%\vortex_safety_check.log"

echo ========================================================
echo   VORTEX JALON 1 - MEMORY SAFETY ^& FFI NEGATIVE TESTS
echo ========================================================

call :expect_fail "raw ptr arith outside unsafe"     "%SCRIPT_DIR%safety\neg_01_ptr_arith.vx"     "unsafe"
call :expect_fail "ptr/int cast outside unsafe"      "%SCRIPT_DIR%safety\neg_02_ptr_cast.vx"      "unsafe"
call :expect_fail "raw deref outside unsafe"         "%SCRIPT_DIR%safety\neg_03_ptr_deref.vx"     "unsafe"
call :expect_fail "extern FFI call outside unsafe"   "%SCRIPT_DIR%safety\win_neg_01_extern.vx"    "unsafe"
call :expect_fail "unsafe fn call outside unsafe"    "%SCRIPT_DIR%safety\neg_04_unsafe_fn.vx"     "unsafe"
call :expect_fail "fwd unsafe fn call outside unsafe" "%SCRIPT_DIR%safety\neg_05_fwd_unsafe_fn.vx" "unsafe"
call :expect_fail "region static escape return"      "%SCRIPT_DIR%safety\neg_06_region_ret.vx"    "region"
call :expect_fail "region use-after-pop"             "%SCRIPT_DIR%safety\neg_07_region_uap.vx"    "region"
call :expect_fail "region outer vec escape"          "%SCRIPT_DIR%safety\neg_08_region_vec.vx"    "region"
call :expect_fail "runtime bounds OOB upper (--safe)" "%SCRIPT_DIR%safety\neg_09_bounds_upper.vx" "BOUNDS ERROR"
call :expect_fail "runtime bounds OOB neg (--safe)"  "%SCRIPT_DIR%safety\neg_10_bounds_neg.vx"   "BOUNDS ERROR"
call :expect_fail "runtime dynamic use-after-pop"    "%SCRIPT_DIR%safety\neg_11_dynamic_uap.vx"  "MEMORY ERROR"
call :expect_fail "runtime FFI null ptr (--safe)"    "%SCRIPT_DIR%safety\win_neg_02_ffi_null.vx"  "FFI ERROR"
call :expect_fail "runtime FFI unaligned (--safe)"   "%SCRIPT_DIR%safety\win_neg_03_ffi_align.vx" "Unaligned pointer"

echo --- [RUN] test_safety_pass.vx (--release) ---
"%VORTEX%" --release "%SCRIPT_DIR%test_safety_pass.vx" >nul 2>&1
if !errorlevel! EQU 0 (
    echo [PASS] test_safety_pass.vx --release
    set /a PASS_COUNT+=1
) else (
    echo [FAIL] test_safety_pass.vx --release exited with error
    set /a FAIL_COUNT+=1
)

echo --- [RUN] vortex build --release packaging check ---
"%VORTEX%" build --release "%SCRIPT_DIR%test_safety_pass.vx" -o "%TEMP%\vortex_safe_pkg_test.exe" >nul 2>&1
if !errorlevel! NEQ 0 (
    echo [FAIL] vortex build --release failed
    set /a FAIL_COUNT+=1
) else (
    "%TEMP%\vortex_safe_pkg_test.exe" >nul 2>&1
    if !errorlevel! EQU 0 (
        echo [PASS] standalone --release packaged executable ran cleanly
        set /a PASS_COUNT+=1
    ) else (
        echo [FAIL] standalone --release packaged executable failed
        set /a FAIL_COUNT+=1
    )
    del /q "%TEMP%\vortex_safe_pkg_test.exe" >nul 2>&1
)

echo ========================================================
echo   SAFETY SUMMARY: !PASS_COUNT! PASSED, !FAIL_COUNT! FAILED
echo ========================================================
if exist "%TMP_LOG%" del /q "%TMP_LOG%" >nul 2>&1
if !FAIL_COUNT! GTR 0 exit /b 1
exit /b 0

:expect_fail
set "NAME=%~1"
set "FILE=%~2"
set "MATCH=%~3"
"%VORTEX%" "%FILE%" > "%TMP_LOG%" 2>&1
if !errorlevel! EQU 0 (
    echo [FAIL] !NAME! unexpectedly succeeded!
    set /a FAIL_COUNT+=1
    exit /b 0
)
findstr /I /C:"!MATCH!" "%TMP_LOG%" >nul 2>&1
if !errorlevel! EQU 0 (
    echo [PASS] !NAME! rejected ^(matched "!MATCH!"^)
    set /a PASS_COUNT+=1
) else (
    echo [FAIL] !NAME! failed without expected message "!MATCH!":
    type "%TMP_LOG%"
    set /a FAIL_COUNT+=1
)
exit /b 0
