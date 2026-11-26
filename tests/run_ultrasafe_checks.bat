@echo off
setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
set "VTX=%SCRIPT_DIR%..\vortex.exe"
echo [ULTRASAFE CHECKS] Running Vortex --ultrasafe 1 / 2 / 3, --check, --json, test, spec...

set TMP1=%TEMP%\vx_us1.vx
set TMP2=%TEMP%\vx_us2_static.vx
set TMP2REC=%TEMP%\vx_us2_rec.vx
set TMP3=%TEMP%\vx_us3_multi.vx
set TMPWARN=%TEMP%\vx_us1_warn.vx

REM 1) Level 1 runtime div-by-zero panic (using dynamic variable so constant folding does not mask runtime trap)
> "%TMP1%" (
    echo fn dyn_div^(d: i64^) -^> i64 { return 10 / d; }
    echo let z = 0;
    echo print^(dyn_div^(z^)^);
)
"%VTX%" --ultrasafe 1 "%TMP1%" >nul 2>nul
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 1 did NOT trap runtime division by zero!
    del "%TMP1%" 2>nul
    exit /b 1
)
echo   [PASS] --ultrasafe 1 caught runtime div-by-zero.

REM 2) Level 1 static warnings (W101 unreachable, W102 unused mut, W103 shadowing, W104 unused local)
> "%TMPWARN%" (
    echo fn f^(x: i64^) -^> i64 {
    echo     let mut unused_m = 5;
    echo     let x = x + 1;
    echo     return x + unused_m;
    echo     let dead = 9;
    echo }
    echo print^(f^(2^)^);
)
"%VTX%" --ultrasafe 1 --json "%TMPWARN%" >"%TEMP%\vx_warn.json" 2>&1
findstr /C:"\"warnings\":" "%TEMP%\vx_warn.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] --ultrasafe 1 --json did not emit JSON warnings!
    type "%TEMP%\vx_warn.json"
    exit /b 1
)
echo   [PASS] --ultrasafe 1 --json emitted static warnings.

REM 3) Level 2 static div-by-zero + OOB + missing return -> non-zero exit
> "%TMP2%" (
    echo fn bad^(x: i64^) -^> i64 {
    echo     let a = slice_i64^(3^);
    echo     let b = a[5];
    echo     let c = x / 0;
    echo }
)
"%VTX%" --ultrasafe 2 --check "%TMP2%" >"%TEMP%\vx_us2.out" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 2 --check allowed static errors!
    exit /b 1
)
echo   [PASS] --ultrasafe 2 --check caught static OOB/div0/missing-return.

REM 4) Level 2 recursion guard
> "%TMP2REC%" (
    echo fn inf^(n: i64^) -^> i64 {
    echo     if n ^< 0 { return 0; }
    echo     return 1 + inf^(n + 1^);
    echo }
    echo print^(inf^(1^)^);
)
"%VTX%" --ultrasafe 2 "%TMP2REC%" >nul 2>nul
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 2 did NOT stop infinite recursion!
    exit /b 1
)
echo   [PASS] --ultrasafe 2 caught deep/infinite recursion safely.

REM 5) Level 3 multi-error recovery + JSON output
> "%TMP3%" (
    echo fn one^(^) -^> i64 {
    echo     let x = nope1 + 1;
    echo     let y = nope2 + 2;
    echo }
    echo fn two^(^) -^> i64 {
    echo     let a = slice_i64^(2^);
    echo     return a[9];
    echo }
)
"%VTX%" --ultrasafe 3 --check --json "%TMP3%" >"%TEMP%\vx_us3.json" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 3 should have failed on broken script!
    exit /b 1
)
findstr /C:"\"E202\"" "%TEMP%\vx_us3.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] --ultrasafe 3 did NOT recover past function 'one' to report E202 in function 'two'!
    type "%TEMP%\vx_us3.json"
    exit /b 1
)
echo   [PASS] --ultrasafe 3 recovered across multiple errors and reported E202 in later function.

REM 6) Native `vortex test` and `vortex spec --json`
"%VTX%" test "%SCRIPT_DIR%test_ergonomics.vx" --json >"%TEMP%\vx_test.json"
if !errorlevel! neq 0 (
    echo [FAIL] vortex test test_ergonomics.vx failed!
    exit /b 1
)
"%VTX%" spec --json >"%TEMP%\vx_spec.json"
if !errorlevel! neq 0 (
    echo [FAIL] vortex spec --json failed!
    exit /b 1
)
findstr /C:"\"--ultrasafe 3\"" "%TEMP%\vx_spec.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] vortex spec --json missing expected fields!
    exit /b 1
)
echo   [PASS] vortex test and vortex spec --json succeeded!

del "%TMP1%" "%TMPWARN%" "%TMP2%" "%TMP2REC%" "%TMP3%" "%TEMP%\vx_warn.json" "%TEMP%\vx_us2.out" "%TEMP%\vx_us3.json" "%TEMP%\vx_test.json" "%TEMP%\vx_spec.json" 2>nul
echo [ULTRASAFE CHECKS] All checks PASSED!
exit /b 0
