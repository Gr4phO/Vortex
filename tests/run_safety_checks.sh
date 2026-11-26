#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VORTEX="$SCRIPT_DIR/../vortex"
PASS_COUNT=0
FAIL_COUNT=0

expect_fail() {
    local name="$1"
    local file="$2"
    local match="$3"
    local out
    if out="$("$VORTEX" "$file" 2>&1)"; then
        echo "[FAIL] $name unexpectedly succeeded!"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    elif echo "$out" | grep -iq "$match"; then
        echo "[PASS] $name rejected ($match)"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "[FAIL] $name failed without expected message '$match': $out"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

echo "========================================================"
echo "  VORTEX JALON 1 - MEMORY SAFETY & FFI NEGATIVE TESTS"
echo "========================================================"

expect_fail "raw ptr arith outside unsafe"      "$SCRIPT_DIR/safety/neg_01_ptr_arith.vx"     "unsafe"
expect_fail "ptr/int cast outside unsafe"       "$SCRIPT_DIR/safety/neg_02_ptr_cast.vx"      "unsafe"
expect_fail "raw deref outside unsafe"          "$SCRIPT_DIR/safety/neg_03_ptr_deref.vx"     "unsafe"
expect_fail "extern FFI call outside unsafe"    "$SCRIPT_DIR/safety/posix_neg_01_extern.vx"  "unsafe"
expect_fail "unsafe fn call outside unsafe"     "$SCRIPT_DIR/safety/neg_04_unsafe_fn.vx"     "unsafe"
expect_fail "fwd unsafe fn call outside unsafe" "$SCRIPT_DIR/safety/neg_05_fwd_unsafe_fn.vx" "unsafe"
expect_fail "region static escape return"       "$SCRIPT_DIR/safety/neg_06_region_ret.vx"    "region"
expect_fail "region use-after-pop"              "$SCRIPT_DIR/safety/neg_07_region_uap.vx"    "region"
expect_fail "region outer vec escape"           "$SCRIPT_DIR/safety/neg_08_region_vec.vx"    "region"
expect_fail "runtime bounds OOB upper (--safe)" "$SCRIPT_DIR/safety/neg_09_bounds_upper.vx" "BOUNDS ERROR"
expect_fail "runtime bounds OOB neg (--safe)"   "$SCRIPT_DIR/safety/neg_10_bounds_neg.vx"   "BOUNDS ERROR"
expect_fail "runtime dynamic use-after-pop"     "$SCRIPT_DIR/safety/neg_11_dynamic_uap.vx"  "MEMORY ERROR"
expect_fail "runtime FFI null ptr (--safe)"     "$SCRIPT_DIR/safety/posix_neg_02_ffi_null.vx"  "FFI ERROR"
expect_fail "runtime FFI unaligned (--safe)"    "$SCRIPT_DIR/safety/posix_neg_03_ffi_align.vx" "Unaligned pointer"

if "$VORTEX" --release "$SCRIPT_DIR/test_safety_pass.vx" >/dev/null 2>&1; then
    echo "[PASS] test_safety_pass.vx --release"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "[FAIL] test_safety_pass.vx --release"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo "========================================================"
echo "  SAFETY SUMMARY: $PASS_COUNT PASSED, $FAIL_COUNT FAILED"
echo "========================================================"
[ "$FAIL_COUNT" -eq 0 ]
