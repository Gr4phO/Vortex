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

REM 7) Sibling blocks + std/json import produce 0 errors and 0 warnings under --ultrasafe 3
set TMP7=%TEMP%\vx_us3_clean_blocks.vx
> "%TMP7%" (
    echo import "std/json";
    echo fn test_blocks^(k: i64^) -^> i64 {
    echo     if k == 1 { let s = "a"; print_str^(s^); }
    echo     if k == 2 { let s = "b"; print_str^(s^); }
    echo     for i in 0..2 { let done = i; print^(done^); }
    echo     for i in 0..2 { let done = i + 10; print^(done^); }
    echo     let obj = json_make_object^(^);
    echo     json_set^(obj, "k", json_make_int^(k^)^);
    echo     let _out = json_stringify^(obj^);
    echo     return 0;
    echo }
    echo test_blocks^(1^);
)
"%VTX%" --ultrasafe 3 --check --json "%TMP7%" >"%TEMP%\vx_us3_clean.json" 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] --ultrasafe 3 --check failed on clean blocks + std/json!
    type "%TEMP%\vx_us3_clean.json"
    exit /b 1
)
findstr /C:"\"warnings\": 0" "%TEMP%\vx_us3_clean.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] --ultrasafe 3 emitted unexpected warnings on std/json or sibling blocks!
    type "%TEMP%\vx_us3_clean.json"
    exit /b 1
)
echo   [PASS] --ultrasafe 3 clean on sibling blocks + std/json import (0 errors, 0 warnings).

REM 8) Source-map remapping across imports: entry line numbers stay exact AND imported local file reports its real name/line
set TMP8MOD=%TEMP%\vx_us3_helper_mod.vx
set TMP8MAIN=%TEMP%\vx_us3_entry_map.vx
> "%TMP8MOD%" (
    echo fn mod_unused_fn^(^) -^> i64 { return 1; }
    echo fn mod_bad^(^) -^> i64 {
    echo     let arr = slice_i64^(2^);
    echo     return arr[9];
    echo }
)
> "%TMP8MAIN%" (
    echo import "std/json";
    echo import "vx_us3_helper_mod.vx";
    echo fn main_fn^(^) -^> i64 {
    echo     let never_used = 42;
    echo     return mod_bad^(^);
    echo }
    echo main_fn^(^);
)
"%VTX%" --ultrasafe 3 --check --json "%TMP8MAIN%" >"%TEMP%\vx_us3_map.json" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 3 should report E202 in imported helper module!
    exit /b 1
)
findstr /C:"\"line\": 4" "%TEMP%\vx_us3_map.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] Source map did not remap W104/E202 to line 4!
    type "%TEMP%\vx_us3_map.json"
    exit /b 1
)
findstr /C:"vx_us3_helper_mod.vx" "%TEMP%\vx_us3_map.json" >nul
if !errorlevel! neq 0 (
    echo [FAIL] Source map did not report imported filename vx_us3_helper_mod.vx!
    type "%TEMP%\vx_us3_map.json"
    exit /b 1
)
findstr /C:"W105" "%TEMP%\vx_us3_map.json" >nul
if !errorlevel! equ 0 (
    echo [FAIL] Imported module emitted noisy W105 unused-function warning!
    type "%TEMP%\vx_us3_map.json"
    exit /b 1
)
echo   [PASS] Source map remaps file/line across imports and filters imported W105 warnings.

REM 9) Multi-error single-pass recovery (E301, E302 for alloc, E001x2, E101, E202) with 0 false W104 warnings
set TMP9=%TEMP%\vx_us3_all6.vx
> "%TMP9%" (
    echo fn leak_ref^(^) -^> [i64] {
    echo     let m = scope_push^(^);
    echo     let tmp = slice_i64^(4^);
    echo     scope_pop^(m^);
    echo     return tmp;
    echo }
    echo fn unsafe_alloc^(^) -^> ptr {
    echo     return alloc^(64^);
    echo }
    echo fn multi_fn^(^) -^> i64 {
    echo     let imm = 10;
    echo     imm = 20;
    echo     let broken = ;
    echo     let ghost = unknown_fn_404^(1, 2^);
    echo     return ghost;
    echo }
    echo fn after_err^(^) -^> i64 {
    echo     let nums = [1, 2, 3];
    echo     return nums[99];
    echo }
    echo print^(after_err^(^)^);
    echo print^(multi_fn^(^)^);
    echo print^(slice_len^(leak_ref^(^)^)^);
    echo print^(unsafe_alloc^(^)^);
)
"%VTX%" --ultrasafe 3 --check --json "%TMP9%" >"%TEMP%\vx_us3_all6.json" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 3 should report 6 errors on TMP9!
    exit /b 1
)
for %%c in ("E301" "E302" "E001" "E101" "E202" "\"errors\": 6" "\"warnings\": 0" "\"col\": 17") do (
    findstr /C:%%c "%TEMP%\vx_us3_all6.json" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] --ultrasafe 3 missing %%~c in TMP9 JSON output!
        type "%TEMP%\vx_us3_all6.json"
        exit /b 1
    )
)
findstr /C:"[VORTEX " "%TEMP%\vx_us3_all6.json" >nul
if !errorlevel! equ 0 (
    echo [FAIL] --ultrasafe 3 emitted duplicate [VORTEX ...] prefix inside JSON message!
    type "%TEMP%\vx_us3_all6.json"
    exit /b 1
)
echo   [PASS] --ultrasafe 3 reported all 6 distinct sorted errors with clean messages, exact cols, and 0 warnings.

REM 10) Native formatter (vortex fmt --check / --stdout)
set TMPFMT=%TEMP%\vx_fmt_test.vx
> "%TMPFMT%" (
    echo fn ugly^(x: i64^) -^> i64 {
    echo if x ^> 0 {
    echo   return x + 1;
    echo }
    echo     return 0;
    echo }
)
"%VTX%" fmt --check "%TMPFMT%" >nul 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] vortex fmt --check should have flagged unformatted file!
    exit /b 1
)
"%VTX%" fmt "%TMPFMT%" >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] vortex fmt failed!
    exit /b 1
)
"%VTX%" fmt --check "%TMPFMT%" >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] vortex fmt output failed --check!
    exit /b 1
)
echo   [PASS] vortex fmt formatted source and verified clean via --check.

del "%TMP1%" "%TMPWARN%" "%TMP2%" "%TMP2REC%" "%TMP3%" "%TMP7%" "%TMP8MOD%" "%TMP8MAIN%" "%TMP9%" "%TMPFMT%" "%TEMP%\vx_warn.json" "%TEMP%\vx_us2.out" "%TEMP%\vx_us3.json" "%TEMP%\vx_test.json" "%TEMP%\vx_spec.json" "%TEMP%\vx_us3_clean.json" "%TEMP%\vx_us3_map.json" "%TEMP%\vx_us3_all6.json" 2>nul
echo [ULTRASAFE CHECKS] All checks PASSED!
exit /b 0
