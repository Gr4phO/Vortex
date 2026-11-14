#!/usr/bin/env bash
set -euo pipefail

CC="${CC:-}"
if [ -z "$CC" ]; then
    if command -v clang >/dev/null 2>&1; then
        CC="clang"
    elif command -v gcc >/dev/null 2>&1; then
        CC="gcc"
    else
        CC="cc"
    fi
fi

HEADLESS=0
OUT=""
EXTRA_CFLAGS=""

for arg in "$@"; do
    case "$arg" in
        --headless)
            HEADLESS=1
            ;;
        -o=*)
            OUT="${arg#-o=}"
            ;;
        --werror)
            EXTRA_CFLAGS="$EXTRA_CFLAGS -Werror"
            ;;
        --help|-h)
            echo "Usage: ./build.sh [--headless] [--werror] [-o=<output>]"
            exit 0
            ;;
        *)
            echo "[BUILD ERROR] Unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

OS="$(uname -s)"
ARCH="$(uname -m)"
SRCS="src/arena.c src/lexer.c src/modules.c src/parser_expr.c src/parser_stmt.c src/ir_core.c src/ir_opt.c src/ir_vec_ra.c src/ffi.c src/jit_x64_simd.c src/jit_x64.c src/jit_arm64.c src/debug_info.c src/pkg.c src/lsp.c src/gui.c src/packager.c src/runtime.c src/main.c"
CFLAGS="-std=c99 -O2 -Wall -Wextra $EXTRA_CFLAGS"
LDFLAGS=""

case "$OS" in
    Darwin)
        OUT="${OUT:-vortex}"
        CFLAGS="$CFLAGS -DVORTEX_NO_X11"
        LDFLAGS="-pthread -lm"
        ;;
    Linux)
        OUT="${OUT:-vortex}"
        if [ "$HEADLESS" -eq 1 ]; then
            CFLAGS="$CFLAGS -DVORTEX_NO_X11"
            LDFLAGS="-pthread -ldl -lm"
        else
            LDFLAGS="-pthread -lX11 -ldl -lm"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        OUT="${OUT:-vortex.exe}"
        LDFLAGS="-luser32 -lgdi32"
        ;;
    *)
        OUT="${OUT:-vortex}"
        CFLAGS="$CFLAGS -DVORTEX_NO_X11"
        LDFLAGS="-pthread -ldl -lm"
        ;;
esac

echo "[BUILD] Compiler: $CC | Host: $OS/$ARCH | Output: $OUT"
# shellcheck disable=SC2086
"$CC" $CFLAGS $SRCS -o "$OUT" $LDFLAGS
echo "[BUILD OK] $OUT built successfully with pure C99."
