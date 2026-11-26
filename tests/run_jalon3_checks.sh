#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VORTEX_BIN="$ROOT_DIR/vortex"
WORK_DIR="$(mktemp -d /tmp/vortex_j3_test.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASSED=0
FAILED=0

echo "--- [JALON 3 INDUSTRIAL TOOLING, DEBUG, PKG & STDLIB TESTS] ---"

# 1. Native multi-frame --safe stacktrace
cat > "$WORK_DIR/trace_oob.vx" <<'EOF'
fn boom(s: [i64]) -> i64 { return s[5]; }
fn outer(s: [i64]) -> i64 { return boom(s); }
let a = slice_i64(2);
print(outer(a));
EOF
if "$VORTEX_BIN" --safe run "$WORK_DIR/trace_oob.vx" > "$WORK_DIR/trace_out.txt" 2>&1; then
    echo "[FAIL] multi-frame OOB expected failure"
    FAILED=$((FAILED + 1))
else
    if grep -q "\[VORTEX STACKTRACE\]" "$WORK_DIR/trace_out.txt" && \
       grep -q "in boom" "$WORK_DIR/trace_out.txt" && \
       grep -q "in outer" "$WORK_DIR/trace_out.txt" && \
       grep -q "in __main__" "$WORK_DIR/trace_out.txt"; then
        echo "[PASS] multi-frame --safe stacktrace (boom -> outer -> __main__)"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] multi-frame stacktrace missing frames"
        FAILED=$((FAILED + 1))
    fi
fi

# 2. DWARF v4 ELF debug emission (--dump-debug)
if "$VORTEX_BIN" --dump-debug "$WORK_DIR/fib.dbg.elf" run "$SCRIPT_DIR/test_fib.vx" >/dev/null 2>&1; then
    if python3 -c "import sys; b=open('$WORK_DIR/fib.dbg.elf','rb').read(); assert b[:4]==b'\\x7fELF' and b'.debug_info' in b and b'.debug_line' in b" 2>/dev/null; then
        echo "[PASS] DWARF v4 ELF debug info generation (.debug_info, .debug_line)"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] DWARF v4 ELF validation"
        FAILED=$((FAILED + 1))
    fi
else
    echo "[FAIL] --dump-debug execution failed"
    FAILED=$((FAILED + 1))
fi

# 3. Built-in LSP server stdio JSON-RPC + VS Code manifests
if python3 - "$VORTEX_BIN" "$ROOT_DIR" <<'PY'
import json, os, subprocess, sys

vbin, root = sys.argv[1], sys.argv[2]
for rel in ("editors/vscode/package.json", "editors/vscode/language-configuration.json", "editors/vscode/syntaxes/vortex.tmLanguage.json"):
    with open(os.path.join(root, rel), "r", encoding="utf-8") as f:
        json.load(f)

p = subprocess.Popen([vbin, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def send(obj):
    b = json.dumps(obj).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(b)}\r\n\r\n".encode("ascii") + b)
    p.stdin.flush()
def recv():
    cl = 0
    while True:
        line = p.stdout.readline().decode("ascii")
        if line in ("\r\n", "\n", ""):
            break
        if line.lower().startswith("content-length:"):
            cl = int(line.split(":", 1)[1].strip())
    return json.loads(p.stdout.read(cl).decode("utf-8"))

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":os.getpid(),"rootUri":None,"capabilities":{}}})
r1 = recv()
assert r1["result"]["serverInfo"]["name"] == "vortex-lsp"
send({"jsonrpc":"2.0","method":"initialized","params":{}})
uri = "file:///tmp/lsp_probe.vx"
src = "struct Vec2 { x: f32, y: f32 }\nfn add_x(v: Vec2, dx: f32) -> f32 { return v.x + dx; }\nlet p = Vec2 { x: 1.0, y: 2.0 };\nlet s = add_x(p, 3.0);\n"
send({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"vortex","version":1,"text":src}}})
d1 = recv()
assert len(d1["params"]["diagnostics"]) == 0
send({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":uri},"position":{"line":3,"character":9}}})
assert recv()["result"]["range"]["start"]["line"] == 1
send({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":uri},"position":{"line":3,"character":9}}})
assert "add_x" in recv()["result"]["contents"]["value"]
send({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":uri,"version":2},"contentChanges":[{"text":"let k = 1;\nlet bad = 1 as *i64;\n"}]}})
d2 = recv()
assert len(d2["params"]["diagnostics"]) >= 1
send({"jsonrpc":"2.0","id":4,"method":"shutdown","params":None})
recv()
send({"jsonrpc":"2.0","method":"exit","params":None})
assert p.wait(timeout=5) == 0
PY
then
    echo "[PASS] vortex lsp stdio (diagnostics, definition, hover) + VS Code manifests"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] vortex lsp stdio / VS Code checks"
    FAILED=$((FAILED + 1))
fi

# 4. Package manager (vortex pkg)
mkdir -p "$WORK_DIR/pkg_env/local_math/src" "$WORK_DIR/pkg_env/app"
export VORTEX_HOME="$WORK_DIR/vortex_home"
cat > "$WORK_DIR/pkg_env/local_math/vortex.toml" <<'EOF'
[package]
name = "local_math"
version = "0.1.0"
entry = "src/lib.vx"
EOF
echo 'fn lm_triple(x: i64) -> i64 { return x * 3; }' > "$WORK_DIR/pkg_env/local_math/src/lib.vx"

pushd "$WORK_DIR/pkg_env/app" >/dev/null
"$VORTEX_BIN" pkg init app_demo >/dev/null 2>&1
"$VORTEX_BIN" pkg add local_math --path "$WORK_DIR/pkg_env/local_math" >/dev/null 2>&1
printf 'import "local_math";\nprint(lm_triple(10));\n' > src/main.vx
if "$VORTEX_BIN" pkg install >/dev/null 2>&1 && "$VORTEX_BIN" pkg verify >/dev/null 2>&1 && grep -q "sha256" vortex.lock; then
    echo '// tamper' >> "$WORK_DIR/pkg_env/local_math/src/lib.vx"
    if "$VORTEX_BIN" pkg verify >/dev/null 2>&1; then
        echo "[FAIL] vortex pkg verify missed tamper"
        FAILED=$((FAILED + 1))
    else
        echo 'fn lm_triple(x: i64) -> i64 { return x * 3; }' > "$WORK_DIR/pkg_env/local_math/src/lib.vx"
        if "$VORTEX_BIN" pkg update >/dev/null 2>&1 && "$VORTEX_BIN" pkg run --release | grep -q "30"; then
            echo "[PASS] vortex pkg (init, add, SHA-256 lock, tamper detect, update, run)"
            PASSED=$((PASSED + 1))
        else
            echo "[FAIL] vortex pkg update/run"
            FAILED=$((FAILED + 1))
        fi
    fi
else
    echo "[FAIL] vortex pkg install/verify"
    FAILED=$((FAILED + 1))
fi
popd >/dev/null

# 5. Modular stdlib (--safe, --release, arm64 cross-compile)
if "$VORTEX_BIN" --safe run "$SCRIPT_DIR/test_jalon3_std.vx" | grep -q "JALON3_STD_ALL_OK"; then
    echo "[PASS] stdlib (--safe) live execution"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] stdlib (--safe)"
    FAILED=$((FAILED + 1))
fi

if "$VORTEX_BIN" --release run "$SCRIPT_DIR/test_jalon3_std.vx" | grep -q "JALON3_STD_ALL_OK" && \
   "$VORTEX_BIN" build --release --target arm64-linux "$SCRIPT_DIR/test_jalon3_std.vx" -o "$WORK_DIR/std_a64_l" >/dev/null 2>&1 && \
   "$VORTEX_BIN" build --release --target arm64-darwin "$SCRIPT_DIR/test_jalon3_std.vx" -o "$WORK_DIR/std_a64_d" >/dev/null 2>&1; then
    echo "[PASS] stdlib (--release + arm64-linux/arm64-darwin cross-compile)"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] stdlib (--release / arm64 cross-compile)"
    FAILED=$((FAILED + 1))
fi

echo ""
echo "  JALON 3 SUMMARY: $PASSED PASSED, $FAILED FAILED"
if [ "$FAILED" -ne 0 ]; then exit 1; fi
exit 0
