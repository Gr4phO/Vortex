# Vortex Programming Language

**Langage système et de calcul haute performance compilé à la volée (JIT `x86-64` AVX2 & `ARM64` NEON)** — *zéro garbage collector, arènes paginées à régions statiques, FFI C dynamique et compilation en binaire autonome.*

[![Version](https://img.shields.io/github/v/tag/corbac10099/Vortex?label=version&color=10B981&style=flat-square)](https://github.com/corbac10099/Vortex/releases/latest)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=flat-square)](LICENSE)
[![Architecture](https://img.shields.io/badge/arch-x86--64%20AVX2%20%7C%20ARM64%20NEON-334155?style=flat-square)](#architecture-sécurité--limites-connues)
[![Standard](https://img.shields.io/badge/C99-zero%20external%20deps-334155?style=flat-square)](#compilation-depuis-les-sources--tests)
[![CI](https://img.shields.io/github/actions/workflow/status/corbac10099/Vortex/ci.yml?style=flat-square&label=CI)](.github/workflows/ci.yml)

Vortex compile les scripts `.vx` et sessions REPL directement en code machine natif en mémoire (~17 200 lignes de C99 pur dans `src/`, sans LLVM ni dépendance externe). Il combine une gestion mémoire sans GC basée sur des arènes paginées (`2 Mo`) avec contrôle statique d'échappement, un allocateur de registres Linear-Scan sur IR 3-adresses, une vectorisation SIMD automatique (`AVX2/FMA3` / `NEON`), un serveur LSP intégré, un gestionnaire de paquets (`vortex pkg`) et une bibliothèque standard modulaire (`std/`).

---

## Points Clés

- **JIT natif `x86-64` (Windows x64 & System V Linux/macOS) & `ARM64` NEON** : émission directe en mémoire (`VirtualAlloc` / `mmap` + `W^X`), `omit_fp` et génération de binaires autonomes (`vortex build` : `PE32+`, `ELF64`, `Mach-O 64`).
- **Mémoire sans GC & sûreté (`--safe` par défaut, `--release` pour la prod)** : arènes de portée (`scope_push()` / `scope_pop()`), vérification de régions à la compilation et garde-fous `unsafe` sur l'arithmétique de pointeurs et le FFI.
- **Optimiseur IR 3-adresses** : inlining de fonctions, déroulement de récursion binaire, élimination des bornes & LICM, auto-vectorisation `AVX2` (`4 × YMM` + `vfmadd231ps`), allocation Linear-Scan.
- **Écosystème intégré** : serveur LSP JSON-RPC 2.0 (`vortex lsp`), gestionnaire de paquets (`vortex pkg` avec verrouillage `SHA-256`), compilation conditionnelle `#[cfg(...)]`, FFI C dynamique (`extern fn`) et traces d'appels natives / `DWARF v4`.

---

## Benchmarks — Protocole Identique (100 % Mesuré Localement)

Banc d'essai [`benchmarks/suite_10_langs/`](benchmarks/suite_10_langs/) exécuté sur **AMD Ryzen 5 5500** (`6C/12T @ 3.60 GHz`, Windows 11 x64, `32 Go RAM`), médiane sur **5 exécutions** (`3` pour Python 3.12). Les entrées C et C++ sont mesurées **avec et sans `-ffast-math`**, et C est également mesuré avec un **allocateur par arène 2 Mio (`-DBENCH_ARENA`)** sur T3 afin de rendre la comparaison T1 et T3 méthodologiquement équitable.

![Temps d'exécution total Vortex](docs/assets/bench_execution_time.svg)

![Compilation AOT, démarrage et taille binaire](docs/assets/bench_binary_size_and_compile.svg)

| Langage / Profil | Total (ms) | T1 (ms) | T2 (ms) | T3 (ms) | T4 (ms) | T5 (ms) | AOT (ms) | Startup / JIT (ms) | Binaire (Ko) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Vortex (`--release`, JIT AVX2 + arène)** | **404.72** | 10.31 | **113.82** | 11.22 | **260.12** | 13.15 | 0.0 | 78.5 | 207.0 |
| C++ (`Clang++ 23 -O3 -ffast-math`) | 476.13 | 9.42 | 143.96 | 41.68 | 264.26 | 8.51 | 1 592.4 | 62.8 | 77.5 |
| C (`Clang 23 -O3 -ffast-math + arène`) | 480.03 | **9.40** | 138.77 | **9.71** | 312.03 | **7.00** | 1 358.9 | 44.7 | 69.0 |
| C (`Clang 23 -O3 -ffast-math`) | 501.93 | 9.83 | 136.24 | 58.23 | 294.25 | 7.26 | 823.9 | 65.1 | 69.0 |
| C (`Clang 23 -O3 + arène`) | 523.43 | 72.45 | 138.36 | 11.32 | 290.67 | 7.57 | 850.2 | 51.8 | **68.5** |
| C++ (`Clang++ 23 -O3`) | 526.85 | 72.92 | 148.38 | 40.20 | 267.34 | 7.95 | 1 574.3 | 75.3 | 76.5 |
| C (`Clang 23 -O3`) | 591.78 | 72.79 | 139.05 | 60.93 | 318.13 | 7.14 | 837.0 | **43.7** | **68.5** |
| Rust (`rustc 1.99 -O3`) | 602.66 | 84.05 | 122.04 | 66.93 | 316.80 | 11.99 | 932.3 | 49.3 | 4 309.5 |
| Java (`HotSpot 17`) | 820.32 | 297.34 | 124.57 | 16.24 | 344.03 | 29.91 | 641.1 | 220.2 | 3.8 (+JVM) |
| C# (`.NET v4.8 / RyuJIT`) | 930.45 | 88.06 | 125.66 | 83.00 | 604.99 | 17.67 | 275.2 | 73.5 | 5.0 (+CLR) |
| Node.js `v26` (`V8 JIT`) | 1 588.62 | 273.31 | 208.90 | 89.81 | 968.55 | 57.26 | 0.0 | 409.5 | 97 751.8 |
| Python `3.12` (`CPython`) | 39 854.55 | 6 533.63 | 7 468.86 | 617.33 | 21 076.67 | 4 562.25 | 0.0 | 2 259.1 | 171.2 (+DLL) |

- **Épreuves** : **T1** Riemann `f32` (`100 × 1M` avec `step = 1e-6f`, zéro saturation IEEE-754) · **T2** Tableau `i64` (`50M`) · **T3** Allocation `1M` structs `Node` · **T4** `fib(40)` + `100M` appels · **T5** Grille `2000×50000` sur `12` threads.
- **Honnêteté T1 (`-ffast-math`)** : Sur T1, Vortex (`10.31 ms`) fait jeu égal avec Clang C/C++ `-O3 -ffast-math` (`9.40–9.83 ms`) grâce à une réduction vectorielle réassociative `4 × YMM` (`vfmadd231ps`). Sans `-ffast-math`, C/C++/Rust respectent l'ordre strict IEEE-754 (`72–84 ms`).
- **Honnêteté T3 (`arène vs malloc`)** : L'arène de portée de Vortex (`11.22 ms`) évite 1M d'appels `malloc/free` (`58–61 ms` en C standard). À méthode d'allocation identique (`C -DBENCH_ARENA`), C par arène paginée 2 Mio obtient **`9.71–11.32 ms`** : les deux approches sont à parité. Sur T5 (multi-thread), C/C++/Rust (`7.00–11.99 ms`) restent devant Vortex (`13.15 ms`).
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
  │ 2. Optimiseur IR & Linear Scan (src/ir_opt.c)                    │
  │    Inlining, déroulement récursion binaire, RCE/LICM,            │
  │    vectorisation AVX2/NEON, vivacité & allocation de registres   │
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 3. Backends Natifs & Runtime (jit_x64.c, jit_arm64.c, arena.c)   │
  │    x86-64 (Win64/SysV64) · ARM64 NEON (ELF64/Mach-O) · FFI · OS  │
  └──────────────────────────────────────────────────────────────────┘
```

- **Mémoire sûre (`--safe` par défaut)** : contrôle d'époques sur les arènes (`VORTEX_SAFE_MAGIC`), vérification de bornes dynamique avec trace d'appels (`stacktrace`), garde-fous `unsafe` pour les pointeurs bruts/appels `extern fn` et rejet des pointeurs de fonction `NULL` dans `ffi_call0..6`.
- **Mode performance (`--release`)** : élimination des contrôles prouvés sûrs (RCE/LICM), allocations OS sans `memset` redondant, inlining, `omit_fp` et noyaux vectoriels AVX2/FMA3.
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
tests\run_tests.bat   :: Windows (unitaires, 16 tests sécurité, 7 ARM64, 9 outillage & stdlib)
```

```sh
./tests/run_tests.sh  # Linux / macOS (dispatch FFI POSIX/Darwin automatique)
```

---

## Structure du Dépôt

```text
.
├── .github/workflows/   # CI (ci.yml), release multi-OS (release.yml) & SemVer auto (auto-version.yml)
├── src/                 # Compilateur C99 modulaire (~17 200 lignes : lexer, modules, parser_expr/stmt, ir_opt, jit_x64/arm64, lsp, pkg, arena, runtime)
├── std/                 # Bibliothèque standard modulaire (.vx : math, str, fs, json, crypto, net, http, db, gpu)
├── benchmarks/          # Banc d'essai suite_10_langs/ & rapport BENCHMARK_REPORT_v1.0.0.md
├── docs/                # Spécification (VORTEX_SPEC.md), roadmap & graphiques SVG (assets/)
├── examples/            # Démonstrations 3D et GUI natives (.vx)
├── tests/               # Tests fonctionnels, sécurité (--safe/neg_*), FFI multi-OS, ARM64, LSP/pkg & stdlib
├── LICENSE              # Licence Apache-2.0
├── VERSION              # Version courante SemVer (lue par build.bat / Makefile)
├── build.bat            # Script de compilation Windows
└── Makefile             # Build multiplateforme Linux / macOS (GCC & Clang)
```

---

## Licence

Distribué sous licence [Apache-2.0](LICENSE).
