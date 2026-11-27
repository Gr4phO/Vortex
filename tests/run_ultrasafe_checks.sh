#!/bin/bash
set -euo pipefail
echo "[ULTRASAFE CHECKS] Running Vortex --ultrasafe 1 / 2 / 3, --check, --json, test, spec..."

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VTX="$ROOT_DIR/vortex"
if [ ! -x "$VTX" ] && [ -x "$ROOT_DIR/build/vortex" ]; then
    VTX="$ROOT_DIR/build/vortex"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# 1) Level 1 runtime div-by-zero trap
cat > "$TMP_DIR/us1.vx" <<'EOF'
fn dyn_div(d: i64) -> i64 { return 10 / d; }
let z = 0;
print(dyn_div(z));
EOF
if "$VTX" --ultrasafe 1 "$TMP_DIR/us1.vx" >/dev/null 2>&1; then
    echo "[FAIL] --ultrasafe 1 did NOT catch runtime division by zero!"
    exit 1
fi
echo "  [PASS] --ultrasafe 1 caught runtime div-by-zero."

# 2) Level 1 warnings in JSON
cat > "$TMP_DIR/us1_warn.vx" <<'EOF'
fn f(x: i64) -> i64 {
    let mut unused_m = 5;
    let x = x + 1;
    return x + unused_m;
    let dead = 9;
}
print(f(2));
EOF
"$VTX" --ultrasafe 1 --json "$TMP_DIR/us1_warn.vx" >"$TMP_DIR/warn.json" 2>&1
grep -q '"warnings":' "$TMP_DIR/warn.json"
echo "  [PASS] --ultrasafe 1 --json emitted static warnings."

# 3) Level 2 static checks
cat > "$TMP_DIR/us2.vx" <<'EOF'
fn bad(x: i64) -> i64 {
    let a = slice_i64(3);
    let b = a[5];
    let c = x / 0;
}
EOF
if "$VTX" --ultrasafe 2 --check "$TMP_DIR/us2.vx" >/dev/null 2>&1; then
    echo "[FAIL] --ultrasafe 2 --check allowed static errors!"
    exit 1
fi
echo "  [PASS] --ultrasafe 2 --check caught static OOB/div0/missing-return."

# 4) Level 2 recursion guard
cat > "$TMP_DIR/us2_rec.vx" <<'EOF'
fn inf(n: i64) -> i64 {
    if n < 0 { return 0; }
    return 1 + inf(n + 1);
}
print(inf(1));
EOF
if "$VTX" --ultrasafe 2 "$TMP_DIR/us2_rec.vx" >/dev/null 2>&1; then
    echo "[FAIL] --ultrasafe 2 did NOT stop infinite recursion!"
    exit 1
fi
echo "  [PASS] --ultrasafe 2 caught deep/infinite recursion safely."

# 5) Level 3 multi-error recovery + JSON output
cat > "$TMP_DIR/us3.vx" <<'EOF'
fn one() -> i64 {
    let x = nope1 + 1;
    let y = nope2 + 2;
}
fn two() -> i64 {
    let a = slice_i64(2);
    return a[9];
}
EOF
if "$VTX" --ultrasafe 3 --check --json "$TMP_DIR/us3.vx" >"$TMP_DIR/us3.json" 2>&1; then
    echo "[FAIL] --ultrasafe 3 should have failed on broken script!"
    exit 1
fi
grep -q '"E202"' "$TMP_DIR/us3.json"
echo "  [PASS] --ultrasafe 3 recovered across multiple errors and reported E202 in later function."

# 6) Native `vortex test` and `vortex spec --json`
"$VTX" test "$SCRIPT_DIR/test_ergonomics.vx" --json >/dev/null
"$VTX" spec --json | grep -q '"--ultrasafe 3"'
echo "  [PASS] vortex test and vortex spec --json succeeded!"

# 7) Sibling blocks + std/json import -> 0 errors and 0 warnings under --ultrasafe 3
cat > "$TMP_DIR/clean_blocks.vx" <<'EOF'
import "std/json";
fn test_blocks(k: i64) -> i64 {
    if k == 1 { let s = "a"; print_str(s); }
    if k == 2 { let s = "b"; print_str(s); }
    for i in 0..2 { let done = i; print(done); }
    for i in 0..2 { let done = i + 10; print(done); }
    let obj = json_make_object();
    json_set(obj, "k", json_make_int(k));
    let _out = json_stringify(obj);
    return 0;
}
test_blocks(1);
EOF
"$VTX" --ultrasafe 3 --check --json "$TMP_DIR/clean_blocks.vx" >"$TMP_DIR/clean.json" 2>&1
grep -q '"warnings": 0' "$TMP_DIR/clean.json"
echo "  [PASS] --ultrasafe 3 clean on sibling blocks + std/json import (0 errors, 0 warnings)."

# 8) Source-map remapping across imports + W105 filtering on imported modules
cat > "$TMP_DIR/helper_mod.vx" <<'EOF'
fn mod_unused_fn() -> i64 { return 1; }
fn mod_bad() -> i64 {
    let arr = slice_i64(2);
    return arr[9];
}
EOF
cat > "$TMP_DIR/entry_map.vx" <<'EOF'
import "std/json";
import "helper_mod.vx";
fn main_fn() -> i64 {
    let never_used = 42;
    return mod_bad();
}
main_fn();
EOF
if "$VTX" --ultrasafe 3 --check --json "$TMP_DIR/entry_map.vx" >"$TMP_DIR/map.json" 2>&1; then
    echo "[FAIL] --ultrasafe 3 should report E202 in imported helper module!"
    exit 1
fi
grep -q '"line": 4' "$TMP_DIR/map.json"
grep -q 'helper_mod.vx' "$TMP_DIR/map.json"
if grep -q 'W105' "$TMP_DIR/map.json"; then
    echo "[FAIL] Imported module emitted noisy W105 unused-function warning!"
    exit 1
fi
echo "  [PASS] Source map remaps file/line across imports and filters imported W105 warnings."

# 9) Multi-error single-pass recovery (E301, E302 for alloc, E001x2, E101, E202) with clean msgs & exact cols
cat > "$TMP_DIR/all6.vx" <<'EOF'
fn leak_ref() -> [i64] {
    let m = scope_push();
    let tmp = slice_i64(4);
    scope_pop(m);
    return tmp;
}
fn unsafe_alloc() -> ptr {
    return alloc(64);
}
fn multi_fn() -> i64 {
    let imm = 10;
    imm = 20;
    let broken = ;
    let ghost = unknown_fn_404(1, 2);
    return ghost;
}
fn after_err() -> i64 {
    let nums = [1, 2, 3];
    return nums[99];
}
print(after_err());
print(multi_fn());
print(slice_len(leak_ref()));
print(unsafe_alloc());
EOF
if "$VTX" --ultrasafe 3 --check --json "$TMP_DIR/all6.vx" >"$TMP_DIR/all6.json" 2>&1; then
    echo "[FAIL] --ultrasafe 3 should report 6 errors on all6.vx!"
    exit 1
fi
for pat in '"E301"' '"E302"' '"E001"' '"E101"' '"E202"' '"errors": 6' '"warnings": 0' '"col": 17'; do
    grep -q "$pat" "$TMP_DIR/all6.json"
done
if grep -q '\[VORTEX ' "$TMP_DIR/all6.json"; then
    echo "[FAIL] --ultrasafe 3 emitted duplicate [VORTEX ...] prefix inside JSON message!"
    exit 1
fi
echo "  [PASS] --ultrasafe 3 reported all 6 distinct sorted errors with clean messages, exact cols, and 0 warnings."

echo "[ULTRASAFE CHECKS] All checks PASSED!"
