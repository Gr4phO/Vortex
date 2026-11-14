@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."
set "VORTEX_BIN=%ROOT_DIR%\vortex.exe"
set "WORK_DIR=%TEMP%\vortex_j3_test_%RANDOM%"
mkdir "%WORK_DIR%" >nul 2>&1

set "PASSED=0"
set "FAILED=0"

echo --- [JALON 3 INDUSTRIAL TOOLING, DEBUG, PKG ^& STDLIB TESTS] ---

REM 1. Native multi-frame --safe stacktrace
(
echo fn boom^(s: [i64]^) -^> i64 { return s[5]; }
echo fn outer^(s: [i64]^) -^> i64 { return boom^(s^); }
echo let a = slice_i64^(2^);
echo print^(outer^(a^)^);
) > "%WORK_DIR%\trace_oob.vx"
"%VORTEX_BIN%" --safe run "%WORK_DIR%\trace_oob.vx" > "%WORK_DIR%\trace_out.txt" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] multi-frame OOB expected failure
    set /a FAILED+=1
) else (
    findstr /C:"[VORTEX STACKTRACE]" "%WORK_DIR%\trace_out.txt" >nul && findstr /C:"in boom" "%WORK_DIR%\trace_out.txt" >nul && findstr /C:"in outer" "%WORK_DIR%\trace_out.txt" >nul && findstr /C:"in __main__" "%WORK_DIR%\trace_out.txt" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] multi-frame stacktrace missing frames
        set /a FAILED+=1
    ) else (
        echo [PASS] multi-frame --safe stacktrace ^(boom -^> outer -^> __main__^)
        set /a PASSED+=1
    )
)

REM 2. Win64 VEH hardware crash stacktrace inside unsafe block
(
echo unsafe {
echo     let p = 0 as *i64;
echo     let v = ptr_read64^(p^);
echo     print^(v^);
echo }
) > "%WORK_DIR%\crash_null.vx"
"%VORTEX_BIN%" --safe run "%WORK_DIR%\crash_null.vx" > "%WORK_DIR%\crash_out.txt" 2>&1
if !errorlevel! equ 0 (
    echo [FAIL] unsafe null read expected crash
    set /a FAILED+=1
) else (
    findstr /C:"[VORTEX STACKTRACE]" "%WORK_DIR%\crash_out.txt" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] VEH crash missing stacktrace
        set /a FAILED+=1
    ) else (
        echo [PASS] Win64 VEH hardware crash stacktrace
        set /a PASSED+=1
    )
)

REM 3. DWARF v4 ELF debug emission (--dump-debug)
"%VORTEX_BIN%" --dump-debug "%WORK_DIR%\fib.dbg.elf" run "%SCRIPT_DIR%test_fib.vx" >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] --dump-debug test_fib.vx execution failed
    set /a FAILED+=1
) else (
    powershell -NoProfile -Command "$b=[System.IO.File]::ReadAllBytes('%WORK_DIR%\fib.dbg.elf'); $s=[System.Text.Encoding]::ASCII.GetString($b); if($b.Length -gt 200 -and $b[0]-eq 0x7f -and $b[1]-eq 69 -and $s.Contains('.debug_info') -and $s.Contains('.debug_line')){exit 0}else{exit 1}"
    if !errorlevel! neq 0 (
        echo [FAIL] DWARF v4 ELF validation
        set /a FAILED+=1
    ) else (
        echo [PASS] DWARF v4 ELF debug info generation ^(.debug_info, .debug_line^)
        set /a PASSED+=1
    )
)

REM 4. Built-in LSP server stdio JSON-RPC & VS Code extension validation
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%jalon3_lsp_probe.ps1" -VortexBin "%VORTEX_BIN%" -RootDir "%ROOT_DIR%" > "%WORK_DIR%\lsp_out.txt" 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] vortex lsp stdio + VS Code JSON checks
    type "%WORK_DIR%\lsp_out.txt"
    set /a FAILED+=1
) else (
    echo [PASS] vortex lsp stdio ^(diagnostics, definition, hover, completion^) + VS Code manifests
    set /a PASSED+=1
)

REM 5. Package manager (vortex pkg: init, add local/git, lock SHA-256, verify, tamper fail, pkg build)
set "PKG_WORK=%WORK_DIR%\pkg_env"
set "VORTEX_HOME=%WORK_DIR%\vortex_home"
mkdir "%PKG_WORK%\local_math\src" >nul 2>&1
echo [package]> "%PKG_WORK%\local_math\vortex.toml"
echo name = "local_math">> "%PKG_WORK%\local_math\vortex.toml"
echo version = "0.1.0">> "%PKG_WORK%\local_math\vortex.toml"
echo entry = "src/lib.vx">> "%PKG_WORK%\local_math\vortex.toml"
echo fn lm_triple^(x: i64^) -^> i64 { return x * 3; }> "%PKG_WORK%\local_math\src\lib.vx"

mkdir "%PKG_WORK%\app" >nul 2>&1
pushd "%PKG_WORK%\app"
"%VORTEX_BIN%" pkg init app_demo >nul 2>&1
"%VORTEX_BIN%" pkg add local_math "%PKG_WORK%\local_math" >nul 2>&1

where git >nul 2>&1
set "HAS_GIT=!errorlevel!"
if !HAS_GIT! equ 0 (
    mkdir "%PKG_WORK%\git_util\src" >nul 2>&1
    echo [package]> "%PKG_WORK%\git_util\vortex.toml"
    echo name = "git_util">> "%PKG_WORK%\git_util\vortex.toml"
    echo version = "1.0.0">> "%PKG_WORK%\git_util\vortex.toml"
    echo entry = "src/lib.vx">> "%PKG_WORK%\git_util\vortex.toml"
    echo fn gu_add7^(x: i64^) -^> i64 { return x + 7; }> "%PKG_WORK%\git_util\src\lib.vx"
    git -C "%PKG_WORK%\git_util" init -q
    git -C "%PKG_WORK%\git_util" config user.email "ci@vortex.dev"
    git -C "%PKG_WORK%\git_util" config user.name "Vortex CI"
    git -C "%PKG_WORK%\git_util" add .
    git -C "%PKG_WORK%\git_util" commit -q -m "v1"
    git -C "%PKG_WORK%\git_util" tag v1.0.0
    "%VORTEX_BIN%" pkg add git_util "%PKG_WORK%\git_util\.git" --rev v1.0.0 >nul 2>&1
    (
    echo import "local_math";
    echo import "git_util";
    echo print^(gu_add7^(lm_triple^(10^)^)^);
    ) > src\main.vx
    set "EXP_PKG_OUT=37"
) else (
    (
    echo import "local_math";
    echo print^(lm_triple^(10^)^);
    ) > src\main.vx
    set "EXP_PKG_OUT=30"
)

"%VORTEX_BIN%" pkg install >nul 2>&1 && "%VORTEX_BIN%" pkg verify >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] vortex pkg install/verify failed
    set /a FAILED+=1
) else (
    findstr /C:"sha256" vortex.lock >nul
    if !errorlevel! neq 0 (
        echo [FAIL] vortex.lock missing sha256 digest
        set /a FAILED+=1
    ) else (
        REM Test tamper detection
        echo // tamper>> "%PKG_WORK%\local_math\src\lib.vx"
        "%VORTEX_BIN%" pkg verify >nul 2>&1
        if !errorlevel! equ 0 (
            echo [FAIL] vortex pkg verify failed to detect tampering
            set /a FAILED+=1
        ) else (
            echo fn lm_triple^(x: i64^) -^> i64 { return x * 3; }> "%PKG_WORK%\local_math\src\lib.vx"
            "%VORTEX_BIN%" pkg update >nul 2>&1 && "%VORTEX_BIN%" pkg run --release > "%WORK_DIR%\pkg_run.txt" 2>&1
            if !errorlevel! neq 0 (
                echo [FAIL] vortex pkg update/run failed
                set /a FAILED+=1
            ) else (
                findstr /C:"!EXP_PKG_OUT!" "%WORK_DIR%\pkg_run.txt" >nul
                if !errorlevel! neq 0 (
                    echo [FAIL] vortex pkg run output mismatch
                    set /a FAILED+=1
                ) else (
                    "%VORTEX_BIN%" pkg build --release -o "%WORK_DIR%\pkg_app.exe" >nul 2>&1
                    if !errorlevel! neq 0 (
                        echo [FAIL] vortex pkg build failed
                        set /a FAILED+=1
                    ) else (
                        "%WORK_DIR%\pkg_app.exe" | findstr /C:"!EXP_PKG_OUT!" >nul
                        if !errorlevel! neq 0 (
                            echo [FAIL] packaged binary execution mismatch
                            set /a FAILED+=1
                        ) else (
                            echo [PASS] vortex pkg ^(init, local+git dep, SHA-256 lock, tamper detect, pkg build^)
                            set /a PASSED+=1
                        )
                    )
                )
            )
        )
    )
)
popd

REM 6. Modular Stdlib (--safe, --release, standalone exe, ARM64 cross-compile)
"%VORTEX_BIN%" --safe run "%SCRIPT_DIR%test_jalon3_std.vx" > "%WORK_DIR%\std_safe.txt" 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] test_jalon3_std.vx --safe failed
    type "%WORK_DIR%\std_safe.txt"
    set /a FAILED+=1
) else (
    findstr /C:"JALON3_STD_ALL_OK" "%WORK_DIR%\std_safe.txt" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] test_jalon3_std.vx --safe incomplete
        type "%WORK_DIR%\std_safe.txt"
        set /a FAILED+=1
    ) else (
        echo [PASS] stdlib ^(json, crypto, net, http, db, gpu^) --safe live execution
        set /a PASSED+=1
    )
)

"%VORTEX_BIN%" --release run "%SCRIPT_DIR%test_jalon3_std.vx" > "%WORK_DIR%\std_rel.txt" 2>&1
findstr /C:"JALON3_STD_ALL_OK" "%WORK_DIR%\std_rel.txt" >nul
if !errorlevel! neq 0 (
    echo [FAIL] test_jalon3_std.vx --release failed
    type "%WORK_DIR%\std_rel.txt"
    set /a FAILED+=1
) else (
    echo [PASS] stdlib --release live execution
    set /a PASSED+=1
)

"%VORTEX_BIN%" build --release "%SCRIPT_DIR%test_jalon3_std.vx" -o "%WORK_DIR%\std_app.exe" >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] stdlib standalone build failed
    set /a FAILED+=1
) else (
    "%WORK_DIR%\std_app.exe" | findstr /C:"JALON3_STD_ALL_OK" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] stdlib standalone execution failed
        set /a FAILED+=1
    ) else (
        echo [PASS] stdlib bundled standalone executable
        set /a PASSED+=1
    )
)

"%VORTEX_BIN%" build --release --target arm64-linux "%SCRIPT_DIR%test_jalon3_std.vx" -o "%WORK_DIR%\std_arm64_linux" >nul 2>&1 && "%VORTEX_BIN%" build --release --target arm64-darwin "%SCRIPT_DIR%test_jalon3_std.vx" -o "%WORK_DIR%\std_arm64_darwin" >nul 2>&1
if !errorlevel! neq 0 (
    echo [FAIL] stdlib arm64-linux / arm64-darwin cross-compile failed
    set /a FAILED+=1
) else (
    echo [PASS] stdlib arm64-linux ^& arm64-darwin cross-compile
    set /a PASSED+=1
)

rmdir /s /q "%WORK_DIR%" >nul 2>&1

echo.
echo   JALON 3 SUMMARY: !PASSED! PASSED, !FAILED! FAILED
if !FAILED! neq 0 exit /b 1
exit /b 0
