@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."
set "VORTEX_BIN=%ROOT_DIR%\vortex.exe"
set "WORK_DIR=%TEMP%\vortex_arm64_test_%RANDOM%"
mkdir "%WORK_DIR%" >nul 2>&1

set "PASSED=0"
set "FAILED=0"

echo --- [ARM64 TARGET TESTS] ---

"%VORTEX_BIN%" --target arm64-linux --version | findstr /C:"arm64-linux" >nul
if !errorlevel! neq 0 (
    echo [FAIL] arm64-linux --version
    set /a FAILED+=1
) else (
    echo [PASS] arm64-linux --version
    set /a PASSED+=1
)

"%VORTEX_BIN%" --target arm64-darwin --version | findstr /C:"arm64-darwin" >nul
if !errorlevel! neq 0 (
    echo [FAIL] arm64-darwin --version
    set /a FAILED+=1
) else (
    echo [PASS] arm64-darwin --version
    set /a PASSED+=1
)

"%VORTEX_BIN%" --release --target arm64-linux --dump-jit "%WORK_DIR%\jit_arm64.bin" run "%SCRIPT_DIR%test_arm64_codegen.vx" >nul
if !errorlevel! neq 0 (
    echo [FAIL] arm64-linux --dump-jit compilation
    set /a FAILED+=1
) else (
    if exist "%WORK_DIR%\jit_arm64.bin" (
        echo [PASS] arm64-linux --dump-jit compilation
        set /a PASSED+=1
    ) else (
        echo [FAIL] jit_arm64.bin not created
        set /a FAILED+=1
    )
)

"%VORTEX_BIN%" build --release --target arm64-linux "%SCRIPT_DIR%test_arm64_codegen.vx" -o "%WORK_DIR%\out_elf64" >nul
if !errorlevel! neq 0 (
    echo [FAIL] ELF64 AArch64 build
    set /a FAILED+=1
) else (
    powershell -NoProfile -Command "$b=[System.IO.File]::ReadAllBytes('%WORK_DIR%\out_elf64'); if($b.Length -lt 64 -or $b[0]-ne 0x7f -or $b[1]-ne 0x45 -or $b[2]-ne 0x4c -or $b[3]-ne 0x46 -or $b[18]-ne 0xb7){exit 1}"
    if !errorlevel! neq 0 (
        echo [FAIL] ELF64 AArch64 header validation
        set /a FAILED+=1
    ) else (
        echo [PASS] ELF64 AArch64 binary packaging
        set /a PASSED+=1
    )
)

"%VORTEX_BIN%" build --release --target arm64-darwin "%ROOT_DIR%\benchmarks\benchmark.vx" -o "%WORK_DIR%\bench_macho" >nul
if !errorlevel! neq 0 (
    echo [FAIL] Mach-O 64 ARM64 build
    set /a FAILED+=1
) else (
    powershell -NoProfile -Command "$b=[System.IO.File]::ReadAllBytes('%WORK_DIR%\bench_macho'); if($b.Length -lt 32 -or $b[0]-ne 0xcf -or $b[1]-ne 0xfa -or $b[2]-ne 0xed -or $b[3]-ne 0xfe -or $b[4]-ne 0x0c -or $b[7]-ne 0x01){exit 1}"
    if !errorlevel! neq 0 (
        echo [FAIL] Mach-O 64 ARM64 header validation
        set /a FAILED+=1
    ) else (
        echo [PASS] Mach-O 64 ARM64 NEON packaging
        set /a PASSED+=1
    )
)

where llvm-objdump >nul 2>&1
if !errorlevel! equ 0 (
    powershell -NoProfile -Command "$d = llvm-objdump -d '%WORK_DIR%\bench_macho' | Out-String; if($d -notmatch 'fmul\.4s' -or $d -notmatch 'faddp\.4s' -or $d -match '<unknown>'){exit 1}"
    if !errorlevel! neq 0 (
        echo [FAIL] llvm-objdump NEON disassembly check
        set /a FAILED+=1
    ) else (
        echo [PASS] llvm-objdump NEON disassembly check
        set /a PASSED+=1
    )
)

"%VORTEX_BIN%" build --release "%SCRIPT_DIR%test_fib.vx" -o "%WORK_DIR%\native_fib.exe" >nul
if !errorlevel! neq 0 (
    echo [FAIL] Native host build test_fib.vx
    set /a FAILED+=1
) else (
    "%WORK_DIR%\native_fib.exe" >nul
    if !errorlevel! neq 0 (
        echo [FAIL] Native host standalone execution
        set /a FAILED+=1
    ) else (
        echo [PASS] Native host standalone execution
        set /a PASSED+=1
    )
)

rmdir /s /q "%WORK_DIR%" >nul 2>&1
echo ARM64/BUILD SUMMARY: !PASSED! PASSED, !FAILED! FAILED
if !FAILED! neq 0 exit /b 1
exit /b 0
