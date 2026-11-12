#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VORTEX_BIN="$ROOT_DIR/vortex"

if [ ! -x "$VORTEX_BIN" ]; then
    echo "[ERROR] $VORTEX_BIN not found or not executable! Run 'make' first."
    exit 1
fi

echo "========================================================"
echo "  VORTEX AUTOMATED TEST SUITE"
echo "========================================================"

PASSED=0
FAILED=0

for t in test_struct.vx test_vec_struct.vx test_fib.vx test_ffi.vx test_memory_1m.vx; do
    echo ""
    echo "--- [RUN] $t ---"
    if "$VORTEX_BIN" "$SCRIPT_DIR/$t"; then
        echo "[PASS] $t"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] $t"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "========================================================"
echo "  SUMMARY: $PASSED PASSED, $FAILED FAILED"
echo "========================================================"

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
exit 0
