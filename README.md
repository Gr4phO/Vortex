# Vortex

**A lightweight, zero-GC x86-64 AVX2 JIT systems language and native runtime written in modular C99.**

Vortex compiles source scripts and interactive REPL sessions directly into native x86-64 machine code in memory, pairing `2 MiB` paged region arenas and loop-aware linear-scan register allocation with generic 256-bit AVX2 auto-vectorization, native OS multi-threading (`parallel_for`), dynamic C FFI (`extern fn`), and single-binary standalone packaging (`vortex build`).

---

## Architecture Pipeline

```text
  .vx Script / Interactive REPL
         │
         ▼
  ┌──────────────────────────────────────────────────────────┐
  │ 1. Lexer & Recursive-Descent Parser (src/lexer_parser.c) │
  │    Struct layouts, mutability checks, 3-address IR emit  │
  └──────────────────────────┬───────────────────────────────┘
                             ▼
  ┌──────────────────────────────────────────────────────────┐
  │ 2. IR Optimizations & RegAlloc (src/ir_opt.c)            │
  │    Generic Loop Vectorizer -> Liveness -> Linear Scan    │
  └──────────────────────────┬───────────────────────────────┘
                             ▼
  ┌──────────────────────────────────────────────────────────┐
  │ 3. Dual-ABI x86-64 + AVX2 JIT Codegen (src/jit_x64.c)    │
  │    Windows x64 & Linux System V AMD64, VEX AVX2, C FFI   │
  └──────────────────────────┬───────────────────────────────┘
                             ▼
  ┌──────────────────────────────────────────────────────────┐
  │ 4. Native Paged-Arena Runtime (src/arena.c, runtime.c)   │
  │    2MB huge pages, 64B align, thread pool, Win32/X11 GUI │
  └──────────────────────────────────────────────────────────┘
```

---

## Core Capabilities

- **2 MiB Paged Region Arenas (Zero-GC)**: Cache-line / AVX-512 aligned (`64`-byte) bump allocation across chained 2 MiB OS pages with $O(1)$ scoped rewind (`scope_push` / `scope_pop`) and zero per-object destructor overhead.
- **Typed C-Layout Structures (`struct`)**: Natural field alignment and offsets, named/positional struct literals, nested field reads/writes, and `struct_size` / `struct_align` introspection.
- **Linear IR & Loop-Aware Register Allocation**: 3-address virtual-register IR (`v0..vN`), loop back-edge lifetime extension, caller/callee-saved GPR/XMM allocation, and automatic frame spilling.
- **Generic 256-bit AVX2 Auto-Vectorization**: Automatically promotes eligible countable reduction loops to 16-way unrolled VEX AVX2 `YMM` pipelines (`vaddps`, `vsubps`, `vmulps`, `vdivps`, `vhaddps`) with scalar tail handling.
- **Native Multi-Core `parallel_for`**: Persistent OS worker thread pool (`CreateThread` / `pthread`) automatically capturing outer variables and giving every core its own lock-free thread-local 2 MiB `VortexRegionArena`.
- **Zero-Wrapper Dynamic C FFI (`extern "lib" fn`)**: Runtime loading via `LoadLibraryA`/`GetProcAddress` (Windows) and `dlopen`/`dlsym` (Linux), supporting up to 8 arguments (register + stack spill), `i32`/`i64`/`f32`/`f64`/`str`/`ptr`/`void`, and variadic C ABI marshalling.
- **Standalone Packaging (`vortex build`)**: Clones the runtime binary and embeds a `"VRTX"` / `"VTXPACK1"` payload trailer to produce single-file `.exe` / ELF executables with zero external dependencies.
- **Cross-Platform Native GUI**: Built-in software-framebuffer windowing and drawing primitives (`Win32 GDI` on Windows, `Xlib` on Linux, optional `-DVORTEX_NO_X11` headless mode).

---

## Verified Benchmark Results

Measured on Windows x64 (12 logical CPU cores) using `.\vortex.exe run <benchmark>`:

| Benchmark | Workload | Observed Time | Highlight |
| :--- | :--- | :--- | :--- |
| `benchmarks/benchmark.vx` | Riemann $\pi$ approximation (`100,000,000` terms) | **8.88 ms** (`8,879 µs`) | 16-way unrolled AVX2 YMM reduction |
| `benchmarks/test_avx2_generic.vx` | Generic AVX2 reduction loop (`50,000,000` steps) | **4.56 ms** (`4,559 µs`) | Auto-vectorized `IR_VEC_LOOP` pass |
| `benchmarks/test_parallel.vx` | `parallel_for` compute (`10,000,000` iterations, 12 threads) | **1.11 ms** (`1,107 µs`) | Persistent OS thread pool + JIT worker kernels |
| `benchmarks/test_parallel_alloc.vx` | Parallel allocations (`1,000,000` `Particle` structs, 12 threads) | **3.82 ms** (`3,817 µs`) | Zero-lock thread-local 2 MiB arenas |
| `tests/test_memory_1m.vx` | Scoped single-thread `1,000,000` `Vertex3D` allocations | **8–9 ms** | `scope_push` / `scope_pop` instant region reuse |

---

## Quick Start

### Build from Source

**Windows (Clang):**
```bat
build.bat
```

**Linux / macOS (Clang or GCC):**
```sh
make          # With X11 GUI support (-pthread -lX11 -ldl -lm)
make headless # Without X11 dependency (-DVORTEX_NO_X11)
```

### Run Scripts, Interactive REPL & Standalone Builds

```sh
# Launch the interactive Vortex REPL terminal (also default when double-clicking vortex.exe)
./vortex

# Execute a script
./vortex run benchmarks/benchmark.vx
./vortex run examples/app_3d.vx

# Evaluate inline Vortex code
./vortex eval "let mut x = 21; x = x * 2; print(x);"

# Package a self-contained single-file executable
./vortex build examples/app_3d.vx -o app_3d.exe
```

Inside the REPL terminal, use `:help`, `:vars`, `:structs`, `:time <expr>`, `:run <file.vx>`, `:reset`, `:cls`, and `:exit`.

### Run Automated Tests

```bat
tests\run_tests.bat   # Windows
./tests/run_tests.sh  # Linux / macOS
```

---

## Documentation

Full syntax, type rules, IR & AVX2 pipeline mechanics, builtin runtime APIs, C FFI marshalling, and packaging binary layout are documented in the official specification:

- **[Vortex Language Reference & Technical Specification](docs/VORTEX_SPEC.md)**

---

## Repository Structure

```text
.
├── src/
│   ├── vortex.h         # Shared types, IR opcodes, ABI & module interfaces
│   ├── arena.c          # 2MB paged region arena, scopes, Vec, Map, UTF-8 strings
│   ├── lexer_parser.c   # Lexer, C struct layouts, recursive-descent IR frontend
│   ├── ir_opt.c         # Generic AVX2 loop vectorizer, liveness, Linear-Scan RegAlloc
│   ├── jit_x64.c        # Dual-ABI x86-64/AVX2 emitter & dynamic C FFI codegen
│   ├── runtime.c        # Win32/X11 GUI, thread pool, builtins, standalone packager
│   └── main.c           # CLI driver & interactive REPL shell
├── examples/            # Real-time 3D & native GUI showcase demos (.vx)
├── benchmarks/          # Official AVX2 & parallel_for throughput benchmarks (.vx)
├── tests/               # Regression tests & cross-platform test runners
├── docs/                # VORTEX_SPEC.md language & internals specification
├── build.bat            # Windows C99 build script (clang -O2 -Wall -Wextra)
└── Makefile             # Linux/macOS Makefile (full X11 & headless targets)
```
