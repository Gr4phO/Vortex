@echo off
setlocal

echo [BUILD] Compiling Vortex v1.0.0 modular C99 sources (x86-64 AVX2 + ARM64 NEON backends)...
clang -std=c99 -O2 -Wall -Wextra src/arena.c src/lexer_parser.c src/ir_opt.c src/jit_x64.c src/jit_arm64.c src/debug_info.c src/pkg.c src/lsp.c src/runtime.c src/main.c -o vortex.exe -luser32 -lgdi32

if %ERRORLEVEL% NEQ 0 (
    echo [BUILD FAILED] Compilation failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo [BUILD OK] vortex.exe compiled successfully with zero errors.
exit /b 0
