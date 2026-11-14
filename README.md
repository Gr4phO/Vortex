# Vortex

**A lightweight, zero-GC x86-64 AVX2 JIT systems language and native runtime written in modular C99.**

Vortex compiles source scripts and interactive REPL sessions directly into native x86-64 machine code in memory, pairing `2 MiB` paged region arenas and loop-aware linear-scan register allocation with generic 256-bit AVX2 auto-vectorization, native OS multi-threading (`parallel_for`), dynamic C FFI (`extern fn`), and single-binary standalone packaging (`vortex build`).

---

## Télécharger Vortex CLI v1.0.0 (Exécutable Autonome < 200 Ko)

Uniquement le compilateur/CLI autonome `vortex` (zéro dépendance externe, aucune application de démo empaquetée) :

| Plateforme | Architecture | Lien Direct (Dépôt / GitHub Releases) | Taille | Exécution Rapide |
| :--- | :--- | :--- | :---: | :--- |
| **Windows** | `x86-64` | **[`vortex-windows-x64.exe` (local)](releases/v1.0.0/vortex-windows-x64.exe)** · [GitHub Release](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-windows-x64.exe) | **188.5 Ko** (`193 024` o) | `.\vortex-windows-x64.exe run --release script.vx` |
| **Linux** | `x86-64` (headless) | **[`vortex-linux-x64` (GitHub Release)](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-linux-x64)** | `< 200 Ko` (CI `ubuntu-latest`) | `chmod +x vortex-linux-x64 && ./vortex-linux-x64 --headless run --release script.vx` |
| **macOS** | `x86-64` (Darwin) | **[`vortex-macos-x64` (GitHub Release)](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-macos-x64)** | `< 200 Ko` (CI `macos-latest`) | `chmod +x vortex-macos-x64 && ./vortex-macos-x64 --headless run --release script.vx` |

> *Note* : Le binaire Windows est versionné directement dans [`releases/v1.0.0/`](releases/v1.0.0/). Les binaires Linux et macOS (ainsi que la publication GitHub Releases complète) sont compilés et testés automatiquement sur les runners natifs par [`.github/workflows/release.yml`](.github/workflows/release.yml).

```sh
# Lancer un script en mode haute performance (--release désactive les vérifications runtime)
./vortex run --release benchmarks/suite_10_langs/bench_vortex.vx

# Exécuter sans interface graphique (serveurs / CI Linux & macOS)
./vortex --headless run --release mon_script.vx   # ou VORTEX_HEADLESS=1 ./vortex run mon_script.vx
```

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
- **Optimizing Linear IR & Register Allocation**: Automatic small-function IR inlining, constant folding & DCE, generalized LICM invariant hoisting, branchless `cmovge` if-conversion, and loop-depth-weighted linear-scan GPR/XMM allocation with direct register argument passing (`RCX`/`RDI`).
- **Generic 256-bit AVX2 + FMA3 Auto-Vectorization**: Promotes countable `f32` loops to **32-way FMA3** (`vfmadd231ps` across 4 YMM accumulators) and `i64` slice transforms/reductions to **8-wide AVX2** (`vmovdqu`, `vpsllq`, `vpaddq`), plus compact peephole encoding.
- **Native Multi-Core `parallel_for`**: Persistent OS worker thread pool (`CreateThread` / `pthread`) automatically capturing outer variables and giving every core its own lock-free thread-local 2 MiB `VortexRegionArena`.
- **Zero-Wrapper Dynamic C FFI (`extern "lib" fn`)**: Runtime loading via `LoadLibraryA`/`GetProcAddress` (Windows) and `dlopen`/`dlsym` (Linux), supporting up to 8 arguments (register + stack spill), `i32`/`i64`/`f32`/`f64`/`str`/`ptr`/`void`, and variadic C ABI marshalling.
- **Standalone Packaging (`vortex build`)**: Clones the runtime binary and embeds a `"VRTX"` / `"VTXPACK1"` payload trailer to produce single-file `.exe` / ELF executables with zero external dependencies.
- **Cross-Platform Native GUI**: Built-in software-framebuffer windowing and drawing primitives (`Win32 GDI` on Windows, `Xlib` on Linux, optional `-DVORTEX_NO_X11` headless mode).

---

## Benchmarks Officiels (Vortex v1.0.0 vs 10 Langages)

Campagne exécutée sur **AMD Ryzen 5 5500** (`6C/12T @ 3,60 GHz`, Windows 11 x64, `32 Go RAM`), médiane sur **5 exécutions** (`3` pour Python 3.12) sur [`benchmarks/suite_10_langs/`](benchmarks/suite_10_langs/) (8 langages mesurés localement, 2 marqués `*` étalonnés sur référence Zen 3) :

![Comparatif des temps d'exécution Vortex v1.0.0 vs 10 langages](docs/assets/bench_execution_time.svg)

![Taille du binaire et temps de compilation Vortex v1.0.0 vs 10 langages](docs/assets/bench_binary_size_and_compile.svg)

| Langage | Compilation / Démarrage | T1 : Riemann `f32` (100M) | T2 : Tableau `i64` (50M) | T3 : Structs `Node` (1M) | T4 : `fib(40)` + 100M calls | T5 : Grille 12 threads | Binaire / Runtime |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Vortex v1.0.0 (`--release`)** | **`80.63 ms`** | 🏆 **`10.10 ms`** | 🥉 **`109.90 ms`** | 🏆 **`11.57 ms`** | `525.52 ms` | `11.10 ms` | **`188.5 Ko`** *(autonome)* |
| **C (`Clang 23 -O3`)** | `872.31 ms` | `73.82 ms` | `136.48 ms` | `58.42 ms` | `327.58 ms` | `8.19 ms` | `68.5 Ko` |
| **C++ (`Clang++ 23 -O3`)** | `1 688.53 ms` | `72.88 ms` | `141.52 ms` | `38.31 ms` | **`266.72 ms`** | **`7.65 ms`** | `76.5 Ko` |
| **Rust (`rustc 1.99 -O3`)** | `912.27 ms` | `82.05 ms` | `108.94 ms` | `57.27 ms` | `305.69 ms` | `8.61 ms` | `4 309.5 Ko` |
| **Go 1.23\*** *(étalonné Zen 3)* | `420.00 ms`\* | `88.00 ms`\* | `135.00 ms`\* | `62.00 ms`\* | `360.00 ms`\* | `10.50 ms`\* | `2 100.0 Ko`\* |
| **Java (HotSpot 17)** | `643.13 ms` | `244.02 ms` | `113.70 ms` | 🥈 `14.78 ms` | `320.84 ms` | `30.06 ms` | `3.8 Ko` *(+ 12,5 Mo JVM)* |
| **C# (.NET v4.8)** | `292.20 ms` | `81.81 ms` | **`108.77 ms`** | `68.94 ms` | `623.28 ms` | `15.50 ms` | `5.0 Ko` *(+ .NET CLR)* |
| **JavaScript (Node v26)** | `319.73 ms` | `262.60 ms` | `204.98 ms` | `83.35 ms` | `1 018.56 ms` | `55.58 ms` | `97 751.8 Ko` (`node.exe`) |
| **LuaJIT 2.1\*** *(étalonné Zen 3)* | `12.00 ms`\* | `115.00 ms`\* | `165.00 ms`\* | `28.00 ms`\* | `1 930.00 ms`\* | `180.00 ms`\* | `520.0 Ko`\* |
| **Python 3.12 (CPython)** | `1 883.24 ms` | `6 454.61 ms` | `7 578.33 ms` | `604.72 ms` | `20 626.96 ms` | `4 897.81 ms` | `6 885.0 Ko` |

- 📄 **Rapport complet & protocole** : [`benchmarks/BENCHMARK_REPORT_v1.0.0.md`](benchmarks/BENCHMARK_REPORT_v1.0.0.md)
- 🚀 **Feuille de route "Road to #1" (Inlining, LICM, SIB/LEA, FMA3, Peephole)** : [`docs/OPTIMIZATION_ROADMAP.md`](docs/OPTIMIZATION_ROADMAP.md)

---

## Quick Start

### Build from Source

**Windows (Clang):**
```bat
build.bat
```

**Linux / macOS (Clang or GCC):**
```sh
make          # Auto-detects Linux/Darwin (X11 on Linux if available, headless on macOS)
make headless # Without X11 dependency (-DVORTEX_NO_X11)
make release  # Stripped LTO CLI executable (< 200 KB)
```

### Run Scripts, Interactive REPL & Standalone Builds

```sh
# Launch the interactive Vortex REPL terminal (also default when double-clicking vortex.exe)
./vortex

# Execute a script (or add --release / --headless)
./vortex run benchmarks/benchmark.vx
./vortex --headless run --release benchmarks/suite_10_langs/bench_vortex.vx

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

Full syntax, type rules, IR & AVX2 pipeline mechanics, builtin runtime APIs, C FFI marshalling, benchmarks, and optimization plans are documented in:

- **[Vortex Language Reference & Technical Specification](docs/VORTEX_SPEC.md)**
- **[Rapport Officiel de Benchmarks v1.0.0 (10 Langages)](benchmarks/BENCHMARK_REPORT_v1.0.0.md)**
- **[Feuille de Route d'Optimisation "Road to #1"](docs/OPTIMIZATION_ROADMAP.md)**

---

## Repository Structure

```text
.
├── .github/workflows/   # GitHub Actions CI & multi-OS CLI release workflow (Win/Linux/macOS)
├── src/
│   ├── vortex.h         # Shared types, IR opcodes, ABI & module interfaces
│   ├── arena.c          # 2MB paged region arena, scopes, Vec, Map, UTF-8 strings
│   ├── lexer_parser.c   # Lexer, C struct layouts, recursive-descent IR frontend
│   ├── ir_opt.c         # Generic AVX2 loop vectorizer, liveness, Linear-Scan RegAlloc
│   ├── jit_x64.c        # Dual-ABI x86-64/AVX2 & ARM64 emitter + dynamic C FFI codegen
│   ├── runtime.c        # Win32/X11/Headless GUI, thread pool, builtins, standalone packager
│   └── main.c           # CLI driver & interactive REPL shell
├── releases/v1.0.0/     # Official standalone CLI release binary (vortex-windows-x64.exe < 200 Ko)
├── examples/            # Real-time 3D & native GUI showcase demos (.vx)
├── benchmarks/          # Benchmark report, AVX2 benchmarks & 10-language suite (suite_10_langs/)
├── tests/               # Regression tests & cross-platform test runners
├── docs/                # VORTEX_SPEC.md, OPTIMIZATION_ROADMAP.md & SVG benchmark charts (assets/)
├── build.bat            # Windows C99 build script (clang -O2 -Wall -Wextra)
└── Makefile             # Cross-platform Linux/macOS Makefile (default, headless & release targets)
```
