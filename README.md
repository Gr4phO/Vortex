# Vortex Programming Language

**Langage système et de calcul haute performance compilé à la volée (JIT `x86-64` AVX2 & `ARM64` NEON)** — *zéro garbage collector, arènes paginées à régions statiques, FFI C dynamique et compilation en binaire autonome.*

[![Version](https://img.shields.io/github/v/tag/corbac10099/Vortex?label=version&color=10B981&style=flat-square)](https://github.com/corbac10099/Vortex/releases/latest)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=flat-square)](LICENSE)
[![Architecture](https://img.shields.io/badge/arch-x86--64%20AVX2%20%7C%20ARM64%20NEON-334155?style=flat-square)](#architecture-sécurité--limites-connues)
[![Standard](https://img.shields.io/badge/C99-zero%20external%20deps-334155?style=flat-square)](#compilation-depuis-les-sources--tests)
[![CI](https://img.shields.io/github/actions/workflow/status/corbac10099/Vortex/ci.yml?style=flat-square&label=CI)](.github/workflows/ci.yml)

Vortex compile les scripts `.vx` et sessions REPL directement en code machine natif en mémoire (~16 450 lignes de C99 pur modulaire dans `src/`, sans LLVM ni dépendance externe). Il combine une gestion mémoire sans GC basée sur des arènes paginées (`2 Mo`) avec contrôle statique d'échappement, un allocateur de registres Linear-Scan sur IR 3-adresses, une vectorisation SIMD automatique (`AVX2/FMA3` / `NEON`), un serveur LSP intégré, un gestionnaire de paquets (`vortex pkg`) et une bibliothèque standard modulaire (`std/`).

---

## Points Clés

- **JIT natif `x86-64` (Windows x64 & System V Linux/macOS) & `ARM64` NEON** : émission directe en mémoire (`VirtualAlloc` / `mmap` + `W^X`), `omit_fp` et génération de binaires autonomes (`vortex build` : `PE32+`, `ELF64`, `Mach-O 64`).
- **Mémoire sans GC & sûreté (`--safe` par défaut, `--release` pour la prod)** : arènes de portée (`scope_push()` / `scope_pop()`), vérification de régions à la compilation, garde-fous `unsafe` sur l'arithmétique de pointeurs et le FFI, contrôle rapide par arène active et déduplication des bornes en bloc de base.
- **Optimiseur IR 3-adresses** : inlining de fonctions, compression algébrique de récursion binaire (`fib`), RCE/LICM, pliage puissance de 2 (`div`/`mod` sans branchement), contrôles de bornes `--release` inline avec *cold traps* hors boucle, auto-vectorisation `AVX2` (`4 × YMM` + `vfmadd231ps`, `IR_VEC_MOD_FILL`, réductions/grilles `32-bit`), allocation Linear-Scan.
- **Écosystème intégré** : serveur LSP JSON-RPC 2.0 (`vortex lsp`), gestionnaire de paquets (`vortex pkg` avec verrouillage `SHA-256`), compilation conditionnelle `#[cfg(...)]`, FFI C dynamique (`extern fn`), traces d'appels natives / `DWARF v4` et primitives rapides (`str_join`, `str_find_any2`, `std/json` linéaire).

---

## Benchmarks — Protocole Identique (100 % Mesuré Localement)

Banc d'essai [`benchmarks/suite_10_langs/`](benchmarks/suite_10_langs/) exécuté sur **Windows 11 x64 (`x86-64` AVX2)**, médiane sur **3 exécutions** (`1` exécution exacte non mise à l'échelle pour Python 3). Les entrées C et C++ sont mesurées **avec et sans `-ffast-math`**, et C est également mesuré avec un **allocateur par arène 2 Mio (`-DBENCH_ARENA`)** sur T3 afin de rendre la comparaison T1 et T3 méthodologiquement équitable.

![Temps d'exécution total Vortex](docs/assets/bench_execution_time.svg)

![Compilation AOT, démarrage et taille binaire](docs/assets/bench_binary_size_and_compile.svg)

| Langage / Profil | Total (ms) | T1 (ms) | T2 (ms) | T3 (ms) | T4 (ms) | T5 (ms) | AOT (ms) | Startup / JIT (ms) | Binaire (Ko) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Vortex (`--release`, JIT AVX2 + arène)** | **211.18** | 10.05 | **99.28** | 11.62 | **78.60** | 16.45 | 0.0 | 67.3 | 220.0 |
| C (`Clang -O3 -ffast-math + arène`) | 445.15 | **9.35** | 127.12 | **7.39** | 293.29 | 6.05 | 716.6 | 45.7 | 69.0 |
| C++ (`Clang++ -O3 -ffast-math`) | 452.47 | 9.36 | 135.73 | 35.94 | 263.57 | 7.54 | 1 447.7 | 69.6 | 77.5 |
| C (`Clang -O3 -ffast-math`) | 473.92 | 9.42 | 124.38 | 53.06 | 277.43 | 6.84 | 726.0 | **41.0** | 69.0 |
| C (`Clang -O3 + arène`) | 501.55 | 71.91 | 129.33 | 7.51 | 281.63 | 7.42 | 706.2 | 50.5 | **68.5** |
| C++ (`Clang++ -O3`) | 532.31 | 71.53 | 136.06 | 39.39 | 269.79 | 7.87 | 1 460.7 | 64.8 | 76.5 |
| C (`Clang -O3`) | 556.32 | 72.24 | 124.88 | 53.15 | 295.68 | **6.04** | 701.5 | 43.5 | **68.5** |
| Rust (`rustc -O3`) | 560.33 | 82.83 | 109.39 | 55.97 | 300.64 | 10.17 | 851.3 | 43.2 | 4 309.5 |
| Java (`HotSpot`) | 780.50 | 296.20 | 123.57 | 15.29 | 316.76 | 27.72 | 580.1 | 186.2 | 3.8 (+JVM) |
| C# (`.NET / RyuJIT`) | 836.33 | 82.67 | 105.28 | 73.26 | 564.15 | 15.69 | 288.1 | 70.2 | 5.0 (+CLR) |
| Node.js (`V8 JIT`) | 1 488.84 | 275.83 | 183.41 | 72.55 | 912.55 | 47.18 | 0.0 | 154.6 | 97 751.8 |
| Python `3` (`CPython`) | 41 328.32 | 6 408.91 | 7 532.48 | 634.63 | 21 776.22 | 4 976.07 | 0.0 | 218.9 | 171.2 (+DLL) |

- **Épreuves** : **T1** Riemann `f32` (`100 × 1M` avec `step = 1e-6f`, zéro saturation IEEE-754) · **T2** Tableau `i64` (`50M`, Pass 1+2 vectorisées AVX2 `IR_VEC_MOD_FILL` + `IR_VEC_ARR_OP`) · **T3** Allocation `1M` structs `Node` · **T4** `fib(40)` + `100M` appels · **T5** Grille `2000×50000` sur `12` threads (`worker_dispatch = count - 1`, noyau 32-bit AVX2).
- **Honnêteté T1 (`-ffast-math`)** : Sur T1, Vortex (`10.05 ms`) fait jeu égal avec Clang C/C++ `-O3 -ffast-math` (`9.35–9.42 ms`) grâce à une réduction vectorielle réassociative `4 × YMM` (`vfmadd231ps`). Sans `-ffast-math`, C/C++/Rust respectent l'ordre strict IEEE-754 (`71.5–82.8 ms`).
- **Honnêteté T3 (`arène vs malloc`)** : L'arène de portée de Vortex (`11.62 ms`) évite 1M d'appels `malloc/free` (`53–56 ms` en C/Rust). À méthode d'allocation identique (`C -DBENCH_ARENA`), C par arène paginée 2 Mio obtient **`7.39–7.51 ms`**. Sur T5 (multi-thread), C/C++/Rust (`6.04–10.17 ms`) restent devant Vortex (`16.45 ms`).
- **Algorithmes classiques (exécution immédiate 0 ms AOT)** :
  - **Fibonacci récursif `fib(38)`** : Vortex `--release` **0.059 ms** (`ir_unroll_binary_recursion`), Vortex `--safe` **177.66 ms**, C `-O3` **79.69 ms**, Node.js **354.43 ms**, Python 3 **4 894.12 ms**.
  - **Tri Quicksort (200 000 `i64`)** : C `-O3` **10.09 ms**, Vortex `--release` **11.04 ms** (`-87 %` après déduplication & contrôles de bornes inline avec *cold traps*), Node.js (`Int32Array`) **15.63 ms**, Vortex `--safe` **64.13 ms**, Python 3 **259.14 ms**.
  - **Parsing + Sérialisation JSON (`std/json`, 200 × 50 objets)** : Node.js (C++ V8) **3.81 ms**, Python 3 (`_json` C) **15.03 ms**, Vortex `--release` (pur Vortex via `str_find_any2` & `str_join`) **19.32 ms**, Vortex `--safe` **50.17 ms**.
  - **Manipulation de chaînes (`str_join` / `str_find` / `str_sub`, 150 × 1 500 jetons)** : Node.js **7.31 ms**, Vortex `--release` **8.31 ms**, Vortex `--safe` **12.99 ms**, C `-O3` (`snprintf`) **16.09 ms**, Python 3 **27.64 ms**.
- **Rapports** : [Rapport complet](benchmarks/BENCHMARK_REPORT_v1.0.0.md) · [Feuille de route](docs/OPTIMIZATION_ROADMAP.md).

---

## Architecture, Sécurité & Limites Connues

```text
  Script .vx / REPL Interactif / LSP (stdio)
         │
         ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 1. Frontend Modulaire (lexer.c, modules.c, parser_expr/stmt.c)   │
  │    Mutabilité stricte, régions, layout C des structs, IR 3-adr.  │
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 2. Optimiseur IR & Linear Scan (ir_core.c, ir_opt.c, ir_vec_ra.c)│
  │    Inlining, déroulement récursion binaire, dédup/RCE/LICM,      │
  │    vectorisation AVX2/NEON, vivacité & allocation de registres   │
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 3. Backends Natifs & Runtime (jit_x64*.c, jit_arm64.c, ffi.c,    │
  │    arena.c, runtime.c, gui.c, packager.c)                        │
  │    x86-64 (Win64/SysV64) · ARM64 NEON · FFI · GUI/Packaging OS   │
  └──────────────────────────────────────────────────────────────────┘
```

- **Mémoire sûre (`--safe` par défaut)** : contrôle d'époques sur les arènes (`VORTEX_SAFE_MAGIC`) avec fast-path sur l'arène active du thread, déduplication des bornes par bloc de base, vérification de bornes dynamique avec trace d'appels (`stacktrace`), garde-fous `unsafe` pour les pointeurs bruts/appels `extern fn` et rejet des pointeurs de fonction `NULL` dans `ffi_call0..6`.
- **Mode performance (`--release`)** : élimination des contrôles prouvés sûrs (RCE/LICM), contrôle de bornes rapide inline avec *cold traps*, allocations compactes/zero-copy sans `memset` redondant, inlining, `omit_fp` et noyaux vectoriels AVX2/FMA3.
- **Limites actuelles du compilateur ([`src/vortex.h`](src/vortex.h))** : types scalaires de base principalement `i64`/`f32` (avec `i32`/`ptr`/`str`), `MAX_FIELDS = 16` par struct, `MAX_CALL_ARGS = 16` par appel, `MAX_STRUCT_TYPES = 128`, `MAX_USER_FUNCS = 512`, `MAX_JIT_CODE_SIZE = 1 MiB`, `MAX_IR_INSTRS = 65 536` instructions par fonction, et réductions `f32` vectorisées réassociatives sur les boucles `IR_VEC_LOOP`.

---

## Compilation depuis les Sources & Tests

### Compiler `vortex`

```bat
:: Windows (Clang)
build.bat
```

```sh
# Linux / macOS (Clang ou GCC)
make            # Détection automatique OS / X11
make headless   # Sans dépendance X11 (-DVORTEX_NO_X11)
make release    # Binaire LTO strippé
```

### Utilisation CLI & REPL

```sh
./vortex                                                 # REPL interactif
./vortex run --release benchmarks/suite_10_langs/bench_vortex.vx
./vortex --headless run --safe tests/test_safety_pass.vx # Validation mémoire/bornes
./vortex eval "let mut x = 21; x = x * 2; print(x);"
./vortex build examples/app_3d.vx -o app_3d.exe          # Binaire autonome
./vortex lsp                                             # Serveur LSP (JSON-RPC)
```

### Suite de tests automatisés

```bat
tests\run_tests.bat   :: Windows (unitaires, test_opt_regression.vx, 16 sécurité, 7 ARM64, 9 outillage & stdlib)
```

```sh
./tests/run_tests.sh  # Linux / macOS (dispatch FFI POSIX/Darwin automatique)
```

---

## Structure du Dépôt

```text
.
├── .github/workflows/   # CI (ci.yml), release multi-OS (release.yml) & SemVer auto (auto-version.yml)
├── src/                 # Compilateur C99 modulaire (~16 450 lignes : lexer, modules, parser_expr/stmt, ir_core/ir_opt/ir_vec_ra, jit_x64/jit_x64_simd/ffi/jit_arm64, lsp, pkg, arena, gui, packager, runtime)
├── std/                 # Bibliothèque standard modulaire (.vx : math, str, fs, json, crypto, net, http, db, gpu)
├── benchmarks/          # Banc d'essai suite_10_langs/ & rapport BENCHMARK_REPORT_v1.0.0.md
├── docs/                # Spécification (VORTEX_SPEC.md), roadmap & graphiques SVG (assets/)
├── examples/            # Démonstrations 3D et GUI natives (.vx)
├── tests/               # Tests fonctionnels, sécurité (--safe/neg_*), régression opt, FFI multi-OS, ARM64, LSP/pkg & stdlib
├── LICENSE              # Licence Apache-2.0
├── VERSION              # Version courante SemVer (lue par build.bat / Makefile)
├── build.bat            # Script de compilation Windows
└── Makefile             # Build multiplateforme Linux / macOS (GCC & Clang)
```

---

## Licence

Distribué sous licence [Apache-2.0](LICENSE).
