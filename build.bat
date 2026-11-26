@echo off
setlocal

set VORTEX_VER=1.0.1
if exist VERSION set /p VORTEX_VER=<VERSION
for /f "tokens=* delims= " %%a in ("%VORTEX_VER%") do set VORTEX_VER=%%a

echo [BUILD] Compiling Vortex v%VORTEX_VER% modular C99 sources (x86-64 AVX2 + ARM64 NEON backends)...
set VORTEX_SRCS=src/arena.c src/lexer.c src/modules.c src/parser_expr.c src/parser_stmt.c src/ir_core.c src/ir_opt.c src/ir_vec_ra.c src/ffi.c src/jit_x64_simd.c src/jit_x64.c src/jit_arm64.c src/debug_info.c src/pkg.c src/lsp.c src/gui.c src/packager.c src/runtime.c src/main.c

clang -std=c99 -O2 -Wall -Wextra -DVORTEX_VERSION=\"%VORTEX_VER%\" %VORTEX_SRCS% -o vortex.exe -luser32 -lgdi32

if %ERRORLEVEL% NEQ 0 (
    echo [BUILD FAILED] vortex.exe compilation failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo [BUILD] Compiling vortex.dll (Java FFM / JNI Host Bridge + Shared Package Runtime)...
clang -std=c99 -O2 -Wall -Wextra -shared -DVORTEX_SHARED_LIB -DVORTEX_HEADLESS -DVORTEX_VERSION=\"%VORTEX_VER%\" %VORTEX_SRCS% -o vortex.dll -luser32 -lgdi32

if %ERRORLEVEL% NEQ 0 (
    echo [BUILD FAILED] vortex.dll compilation failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo [BUILD OK] vortex.exe and vortex.dll compiled successfully with zero errors.
exit /b 0
