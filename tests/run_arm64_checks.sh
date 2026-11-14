#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VORTEX_BIN="$ROOT_DIR/vortex"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASSED=0
FAILED=0

echo "--- [ARM64 TARGET TESTS] ---"

if "$VORTEX_BIN" --target arm64-linux --version | grep -q "arm64-linux"; then
    echo "[PASS] arm64-linux --version"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] arm64-linux --version"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" --target arm64-darwin --version | grep -q "arm64-darwin"; then
    echo "[PASS] arm64-darwin --version"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] arm64-darwin --version"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" --release --target arm64-linux --dump-jit "$WORK_DIR/jit_arm64.bin" run "$SCRIPT_DIR/test_arm64_codegen.vx" >/dev/null && [ -s "$WORK_DIR/jit_arm64.bin" ]; then
    echo "[PASS] arm64-linux --dump-jit compilation"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] arm64-linux --dump-jit compilation"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" build --release --target arm64-linux "$SCRIPT_DIR/test_arm64_codegen.vx" -o "$WORK_DIR/out_elf64" >/dev/null && \
   [ "$(od -An -tx1 -N4 "$WORK_DIR/out_elf64" | tr -d ' \n')" = "7f454c46" ]; then
    echo "[PASS] ELF64 AArch64 binary packaging"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] ELF64 AArch64 binary packaging"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" build --release --target arm64-darwin "$ROOT_DIR/benchmarks/benchmark.vx" -o "$WORK_DIR/bench_macho" >/dev/null && \
   [ "$(od -An -tx1 -N4 "$WORK_DIR/bench_macho" | tr -d ' \n')" = "cffaedfe" ]; then
    echo "[PASS] Mach-O 64 ARM64 NEON packaging"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] Mach-O 64 ARM64 NEON packaging"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" build --release "$SCRIPT_DIR/test_fib.vx" -o "$WORK_DIR/native_fib" >/dev/null && \
   "$WORK_DIR/native_fib" >/dev/null; then
    echo "[PASS] Native host standalone execution"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] Native host standalone execution"
    FAILED=$((FAILED + 1))
fi

echo "ARM64/BUILD SUMMARY: $PASSED PASSED, $FAILED FAILED"
if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
exit 0
