# Vortex Language Reference & Technical Specification (v1.0.0)

Vortex is a compiled-on-the-fly (`x86-64` JIT) systems and numerical computing language built in pure C99 with zero garbage collection, 2 MB paged region arenas, typed C-layout structures, a 3-address linear SSA-like IR with Linear-Scan register allocation, generic AVX2 auto-vectorization, native OS multi-threading (`parallel_for`), dynamic C FFI (`extern fn`), and standalone single-binary packaging (`vortex build`).

---

## 1. Compiler & Runtime Pipeline Architecture

```
  Source (.vx / REPL)
         │
         ▼
  [1] Lexer, Modules & Recursive-Descent Parser (src/lexer.c, src/modules.c, src/parser_expr.c, src/parser_stmt.c)
         │  Emits 3-address virtual-register instructions (v0..vN)
         ▼
  [2] Loop Auto-Vectorizer Pass (src/ir_opt.c :: ir_loop_vectorize)
         │  Detects vectorizable reduction/element loops -> IR_VEC_LOOP
         ▼
  [3] Liveness Analysis & Linear-Scan Register Allocator (src/ir_opt.c)
         │  Extends intervals across loops, allocates RAX/RBX/R12-R15 + XMM
         ▼
  [4] Dual-ABI x86-64 & AVX2 JIT Code Generator (src/jit_x64.c)
         │  Emits native Windows x64 / Linux System V AMD64 machine code
         ▼
  [5] Native Execution & 2MB Paged-Arena Runtime (src/arena.c, src/runtime.c)
```

---

## 2. Lexical Structure & Primitive Types

### 2.1 Comments & Literals
- Line comments: `// comment until end of line`
- Integer literals: 64-bit signed integers (`i64`), e.g., `42`, `-100`.
- Floating-point literals: 32-bit IEEE-754 floats (`f32`) when written with a decimal point, e.g., `3.14`, `-0.5`.
- String literals: Null-terminated UTF-8 byte strings with escapes (`\n`, `\t`, `\r`, `\\`, `\"`), stored as `i64`/`str` pointers in the literal string pool.

### 2.2 Keywords
`let`, `mut`, `for`, `parallel_for`, `in`, `while`, `if`, `else`, `fn`, `return`, `struct`, `extern`, `i64`, `f32`

Type names recognized in declarations and FFI signatures:
- `i64` (64-bit signed integer)
- `i32` (32-bit signed integer, used in struct fields & C FFI)
- `f32` (32-bit IEEE-754 float)
- `f64` (64-bit IEEE-754 double, promoted automatically on C FFI boundaries)
- `str` (C `const char*` string pointer)
- `ptr` (64-bit raw opaque/typed pointer)
- `void` (valid FFI return type)
- `<StructName>` (reference pointer `*Struct` to a declared `struct`)

### 2.3 Operators & Precedence
- Arithmetic: `+`, `-`, `*`, `/`, `%` (automatically lowered to scalar integer or `f32` SSE instructions depending on operand types; unary `-` supported in primaries)
- Comparisons: `==`, `!=`, `<`, `<=`, `>`, `>=` (integer and `f32` variants)
- Assignment & Member Access: `=`, `.`, `[index]`, `..` (exclusive integer range `start..end`)

---

## 3. Variables, Mutability & Control Flow

### 3.1 Variable Bindings
```vortex
let a = 10;          // Immutable i64 binding
let mut counter = 0; // Mutable binding
counter = counter + 1;
```
Mutability is enforced at compile time by the parser (`is_mut` flag in the symbol table). Reassigning an immutable `let` binding raises a compile error.

### 3.2 Conditionals & Loops
```vortex
if x < 10 {
    print(1);
} else {
    print(2);
}

let mut k = 0;
while k < 5 {
    k = k + 1;
}

for i in 0..100 {
    // Exclusive range: i takes values 0, 1, ..., 99
}
```

### 3.3 Functions & Recursion
Functions take up to 8 arguments, compile directly into the native JIT buffer with ABI-compliant stack frames (`16`-byte aligned), and support arbitrary recursion and forward calls (patched via `g_user_func_relocs`):
```vortex
fn fib(n) {
    if n <= 1 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}
```

---

## 4. Memory Architecture: 2 MB Paged Region Arenas (Zero-GC)

Vortex eliminates garbage collection pauses and per-object heap fragmentation via `VortexRegionArena` (`src/arena.c`):
- **Huge-Page Regions**: Each arena page allocates `2 MiB` (`2,097,152` bytes) via OS-aligned pages (`_aligned_malloc` on Windows, `posix_memalign` on POSIX).
- **64-Byte Cache-Line / AVX-512 Alignment**: Every bump-pointer allocation rounds up to a `64`-byte boundary (`VORTEX_ALIGNMENT = 64`). Oversized allocations (`> 2 MiB`) transparently allocate a dedicated aligned page.
- **Scoped Lifetimes (`scope_push` / `scope_pop`)**: Captures the exact `(current_page_index, cursor)` bookmark in $O(1)$ time and rewinds the bump pointer in $O(1)$ at scope exit without walking objects or invoking destructors:
```vortex
let mark = scope_push();
for i in 0..1000000 {
    let inner = scope_push();
    let v = Vertex3D { x: 1.0, y: 2.0, z: 3.0 };
    scope_pop(inner);
}
scope_pop(mark);
```
- **Thread-Local Worker Arenas**: Each native OS worker thread owns a dedicated `VortexRegionArena` (`__declspec(thread)` / `__thread`), enabling zero-lock, zero-contention allocations inside `parallel_for`.

---

## 5. Typed Data Structures (`struct`)

Vortex structures follow standard C alignment and padding rules:
```vortex
struct Vertex3D {
    x: f32,
    y: f32,
    z: f32,
    id: i64
}

let mut p = Vertex3D { x: 1.5, y: 2.5, z: 3.5, id: 7 };
p.x = p.x + 1.0;
print(p.x);
print(struct_size("Vertex3D"));   // 24
print(struct_align("Vertex3D"));  // 8
```
- Both named field initialization (`Vertex3D { x: 1.0, y: 2.0, z: 3.0, id: 1 }`) and positional initialization (`Vertex3D { 1.0, 2.0, 3.0, 1 }`) are supported.
- Struct instances are allocated inside the active region arena and manipulated through compile-time computed field offsets (`IR_LOAD_OFFSET` / `IR_STORE_OFFSET`).
- Nested struct types and typed pointers inside structs are supported up to `MAX_FIELDS` (16 fields per struct, `MAX_STRUCT_TYPES` = 32).

---

## 6. IR, Register Allocation & Generic AVX2 Auto-Vectorization

### 6.1 3-Address Virtual-Register IR (`src/ir_opt.c`)
All statements and expressions lower into a flat `IRInstruction` stream using unbounded virtual registers (`v0..vN`) typed as `i64`, `f32`, or pointer types.

### 6.2 Loop-Aware Liveness & Linear-Scan Allocation
`ir_liveness_analysis` scans loop label intervals (`IR_LABEL` -> backward `IR_JMP`) and extends `[start_ip, end_ip]` lifetimes for any virtual register referenced inside a loop until the loop end, preventing premature register reuse across loop back-edges.

`ir_linear_scan_allocate` assigns active virtual registers across physical GPRs (`RBX`, `R12`–`R15` callee-saved; `RAX`, `RCX`, `RDX`, `RSI`, `RDI`, `R8`–`R11` caller-saved, respecting `crosses_call` analysis and Windows x64 / System V ABI differences) and SSE/XMM scalar registers, spilling excess live values to 16-byte aligned frame slots.

### 6.3 Generic Loop Auto-Vectorizer (`ir_loop_vectorize`)
Before register allocation, `ir_loop_vectorize` inspects countable `for` loops containing independent arithmetic reductions (`sum += expr` or alternating signs with `i` induction steps) with zero function calls (`IR_CALL`/`IR_CALL_USER`/`IR_CALL_FFI`) or nested control flow.

Eligible loops are lowered to `IR_VEC_LOOP` and emitted by `emit_avx2_vector_loop` (`src/jit_x64.c`) using:
- **16-way unrolled 256-bit AVX2 YMM lanes** (2 independent 8-float accumulators `YMM0` + `YMM1` per iteration),
- **VEX-encoded `vaddps`, `vsubps`, `vmulps`, `vdivps`** (`0xC5`/`0xC4` prefixes),
- Horizontal YMM reduction (`vextractf128` + `vhaddps`) plus scalar epilogue for non-multiple-of-16 tails,
- Automatic cleanup (`vzeroupper`) prior to exiting back to scalar SSE code.

---

## 7. Multi-Core Parallelism (`parallel_for`)

Vortex includes a persistent OS thread pool initialized to the host's logical core count (`GetSystemInfo` on Windows, `sysconf(_SC_NPROCESSORS_ONLN)` on POSIX, capped at `VORTEX_MAX_THREADS = 64`).

### Syntax & Semantics
```vortex
parallel_for i in 0..10000000 {
    // Compiled as a standalone JIT worker kernel taking (ctx*, start, end)
    let p = Particle { x: 1.0, y: 2.0, z: 3.0 };
}
```
- The compiler automatically detects free variables referenced inside the `parallel_for` block, packs them into a stack capture context (`ctx`), and compiles the loop body into an isolated JIT kernel function respecting the host ABI (`(RCX, RDX, R8)` on Windows x64; `(RDI, RSI, RDX)` on System V AMD64).
- Work `start..end` is partitioned evenly across all `num_workers()` cores with synchronization barriers (`WaitForMultipleObjects` / `pthread_cond_wait`).
- Each worker binds its thread-local 2 MB `VortexRegionArena` (`arena_set_thread_local`), guaranteeing lock-free parallel allocations.

---

## 8. Dynamic C Native FFI (`extern fn`)

Vortex calls arbitrary C shared libraries (`.dll` on Windows, `.so` on Linux) directly from JIT-compiled machine code without C wrappers:

### Declaration Syntax
```vortex
extern "kernel32" fn GetTickCount64() -> i64;
extern "msvcrt" fn puts(s: str) -> i32;
extern "msvcrt" fn pow(x: f64, y: f64) -> f64;
extern "msvcrt" fn _snprintf(buf: ptr, max_len: i64, fmt: str, a1: i64, a2: i64, a3: i64) -> i32;
```
### Supported ABI Features (`src/jit_x64.c`)
- **Library & Symbol Caching**: Libraries loaded once via `LoadLibraryA` / `dlopen` (with `.dll` / `.so` fallback and `"msvcrt"` -> `"libc.so.6"` mapping on Linux), symbols cached in `FFIFunc`.
- **Up to 8 Arguments**: First 4 (Windows x64: `RCX`/`XMM0`, `RDX`/`XMM1`, `R8`/`XMM2`, `R9`/`XMM3` + 32-byte shadow space) or 6 GPRs + 8 XMMs (System V AMD64), with additional arguments spilled dynamically onto the caller stack at `[RSP + 32 + ...]`.
- **Automatic Type Marshalling**:
  - `f64` parameters automatically promote Vortex `f32` (`XMM`) values via `cvtss2sd`; `f64` return values demote back via `cvtsd2ss`.
  - Variadic C ABI compliance on x64 (`AL = num_xmm_args` under System V; mirrored float/int register passing on Windows x64).

---

## 9. Built-in Standard Runtime Functions

| Category | Builtin Signature | Description |
| :--- | :--- | :--- |
| **I/O** | `print(val)` | Prints `i64` or `f32` followed by newline |
| | `print_str(s)` | Prints string without implicit formatting overhead |
| **Memory** | `alloc(bytes)` | Allocates `bytes` (64B aligned) in active 2MB arena |
| | `scope_push() -> i64` | Captures arena watermark |
| | `scope_pop(mark)` | Rewinds active arena to `mark` |
| | `struct_size(name) -> i64` | Returns compiled byte size of struct `name` |
| | `struct_align(name) -> i64` | Returns alignment of struct `name` |
| **Vectors** | `vec_new() -> ptr` | Creates arena-backed dynamic `VortexVec` |
| | `vec_push(v, val)` | Appends 64-bit integer/pointer to `v` |
| | `vec_get(v, idx) -> i64` | Reads element at `idx` (0 on out-of-bounds) |
| | `vec_set(v, idx, val)` | Writes `val` at `idx` |
| | `vec_len(v) -> i64` | Returns element count |
| **Maps** | `map_new() -> ptr` | Creates open-addressed FNV-1a hash map |
| | `map_set(m, key, val)` | Inserts/updates string key `key` |
| | `map_get(m, key) -> i64` | Looks up `key` (returns `0` if absent) |
| | `map_has(m, key) -> i64` | Returns `1` if `key` exists, `0` otherwise |
| | `map_len(m) -> i64` | Returns entry count |
| **Strings** | `str_concat(s1, s2) -> str` | Arena-allocated string concatenation |
| | `str_len(s) -> i64` | Byte length of string |
| | `str_sub(s, start, len) -> str` | Substring slice |
| | `str_from_i64(n) -> str` | Formats integer to arena string |
| | `str_from_f32(bits) -> str` | Formats float bits to arena string |
| **Timing & Parallel** | `time_us() -> i64` | Monotonic microsecond clock (`QPC` / `CLOCK_MONOTONIC`) |
| | `num_workers() -> i64` | Active parallel worker count |
| **Math** | `math_sin(deg_x10) -> i64` | Q12 fixed-point `sin * 4096` (degrees $\times 10$) |
| | `math_cos(deg_x10) -> i64` | Q12 fixed-point `cos * 4096` (degrees $\times 10$) |
| | `math_sqrt(x) -> i64` | Integer square root |
| **Dynamic FFI** | `ffi_load(lib) -> ptr` | Loads shared library handle |
| | `ffi_symbol(h, sym) -> ptr` | Resolves symbol pointer |
| | `ffi_call0` .. `ffi_call4` | Raw dynamic C function calls (0–4 args) |
| **Native GUI** | `gui_window(title, w, h) -> ptr` | Creates native Win32 / X11 software-framebuffer window |
| | `gui_is_open(w) -> i64` | Pumps OS events; returns `1` while open |
| | `gui_rgb(r, g, b) -> i64` | Packs `0x00RRGGBB` color |
| | `gui_clear(w, rgb)` | Fills framebuffer with solid color |
| | `gui_draw_pixel(w, x, y, rgb)` | Sets clipped pixel |
| | `gui_draw_line(w, x0, y0, x1, y1, rgb)` | Clipped Bresenham line |
| | `gui_draw_rect(w, x, y, width, height, rgb)` | Solid rectangle |
| | `gui_draw_text(w, txt, x, y, rgb)` | Native text draw |
| | `gui_draw_number(w, num, x, y, rgb)` | Renders integer value at `(x, y)` |
| | `gui_button(w, id, lbl, x, y, wb, hb) -> i64` | Immediate-mode clickable button (`1` on click) |
| | `gui_set_pos(w, x, y)` | Moves window position |
| | `gui_present(w)` | Blits software framebuffer to OS window |

---

## 10. Standalone Packaging Format (`vortex build`)

Running `vortex build script.vx -o app.exe` clones the active Vortex runtime binary and appends an embedded self-contained payload trailer:

```
+-----------------------------+---------------------------+-----------------------+
|  Vortex Runtime Executable  |  VortexPackageHeader      |  Source UTF-8 Bytes   |  VortexPackageFooter
|  (Win32 PE / Linux ELF)     |  Magic: "VRTX", Ver: 1    |  (script.vx payload)  |  offset + "VTXPACK1"
+-----------------------------+---------------------------+-----------------------+
```

At startup, `check_and_run_embedded_payload` reads the last 16 bytes of the binary; if `VTXPACK1` matches, it seeks directly to `payload_offset`, verifies `"VRTX"`, extracts the embedded program (including `extern fn` declarations, structs, and functions), and JIT-executes it immediately with zero external `.vx` files or runtime DLL dependencies.

---

## 11. CLI & REPL Meta-Commands

- **CLI Flags**:
  - `vortex` / `vortex repl` / `vortex -i`: Launch interactive REPL
  - `vortex <file.vx>` / `vortex run <file.vx>`: Execute script file
  - `vortex eval "<code>"`: Evaluate inline Vortex source
  - `vortex build <in.vx> [-o out]`: Package standalone executable
  - `vortex --version` / `-v`, `vortex --help` / `-h`
- **REPL Meta-Commands**:
  - `:help` — Command reference and examples
  - `:vars` — Inspect active variables, inferred types, and mutability
  - `:structs` — Inspect registered `struct` sizes, alignment, and field byte offsets
  - `:time <expr/stmt>` — Measure compilation + execution latency in microseconds and milliseconds
  - `:run <file.vx>` — Load and run an external `.vx` file
  - `:cls` / `:clear` — Clear screen
  - `:reset` — Wipe session declarations, structs, FFI handles, and reset arenas
  - `:exit` / `:quit` — Exit terminal

---

## 12. Multi-File Modules (`import`)

Vortex supports native multi-file projects via the preprocessor `import` directive:

```vortex
import "lib/math.vx";
import "models/vec2.vx";
```

- **Recursive Preprocessor Resolution (`vortex_resolve_imports`)**: Before parsing and JIT compilation, `vortex run`, `vortex <file.vx>`, and `vortex build` recursively resolve top-level `import "rel/path.vx";` directives (outside `//` comments) and inline the target module source with traceability markers.
- **Relative Paths**: Each `import` path is resolved relative to the directory of the file that declares it (normalizing both `/` and `\`), with fallback to standard library (`std/...`) and `vortex.toml` package dependencies.
- **Deduplication & Cycle Protection (`#pragma once`)**: Canonical file paths are tracked across the compilation unit. Subsequent or circular imports of an already-included canonical path are silently skipped, preventing duplicate definitions and infinite recursion.
- **Error Diagnostics**: Missing or unreadable imported files abort compilation immediately with `[VORTEX IMPORT ERROR] Cannot open imported file '<path>' (at <file>:<line>)`.
- **Standalone Packaging**: `vortex build main.vx -o app.exe` embeds the entire resolved module graph into the `VTXPACK1` payload so the generated executable remains 100% self-contained with no external `.vx` files required.
