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

echo "[ULTRASAFE CHECKS] All checks PASSED!"
