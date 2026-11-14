# Vortex

**Compilateur JIT natif x86-64 AVX2/FMA3 & ARM64 NEON et runtime autonome à arènes en C99 sans dépendance externe.**

[![Version](https://img.shields.io/github/v/tag/corbac10099/Vortex?label=version&color=10B981&style=flat-square)](https://github.com/corbac10099/Vortex/releases/latest)
[![Architecture](https://img.shields.io/badge/arch-x86--64%20AVX2%20%7C%20ARM64%20NEON-334155?style=flat-square)](#architecture-du-compilateur)
[![Standard](https://img.shields.io/badge/C99-zero%20external%20deps-334155?style=flat-square)](#compilation-depuis-les-sources)
[![CI](https://img.shields.io/github/actions/workflow/status/corbac10099/Vortex/release.yml?style=flat-square&label=release%20CI)](.github/workflows/release.yml)

Vortex compile les scripts `.vx` et sessions REPL directement en code machine natif en mémoire (~24 000 lignes de C99 pur). Il associe une gestion mémoire par arènes paginées `2 MiB` avec vérification par époques (`scope_push` / `scope_pop`), un optimiseur IR à 3 adresses (inlining, LICM, RCE, auto-vectorisation AVX2/FMA3 & NEON, déroulement récursif), une allocation de registres Linear Scan, du parallélisme natif OS (`parallel_for`), une FFI C dynamique (`extern fn`), un serveur LSP et le packaging en exécutable autonome (`vortex build`).

---

## Télécharger Vortex CLI

Binaires CLI autonomes publiés via [GitHub Releases](https://github.com/corbac10099/Vortex/releases/latest) et construits sur runners natifs par [`.github/workflows/release.yml`](.github/workflows/release.yml) :

| Plateforme | Architecture | Téléchargement | Commande type |
| :--- | :--- | :--- | :--- |
| **Windows** | `x86-64` | [vortex-windows-x64.exe](https://github.com/corbac10099/Vortex/releases/latest/download/vortex-windows-x64.exe) | `.\vortex-windows-x64.exe run --release script.vx` |
| **Linux** | `x86-64` (headless) | [vortex-linux-x64](https://github.com/corbac10099/Vortex/releases/latest/download/vortex-linux-x64) | `./vortex-linux-x64 --headless run --release script.vx` |
| **macOS** | `x86-64` (Darwin) | [vortex-macos-x64](https://github.com/corbac10099/Vortex/releases/latest/download/vortex-macos-x64) | `./vortex-macos-x64 --headless run --release script.vx` |

---

## Benchmarks — Protocole Identique (100 % Mesuré Localement)

Banc d'essai [`benchmarks/suite_10_langs/`](benchmarks/suite_10_langs/) exécuté sur **AMD Ryzen 5 5500** (`6C/12T @ 3.60 GHz`, Windows 11 x64, `32 Go RAM`), médiane sur **5 exécutions** (`3` pour Python 3.12). Les entrées C et C++ sont mesurées **avec et sans `-ffast-math`** afin de comparer équitablement la vectorisation réassociative `f32` de Vortex.

![Temps d'exécution total Vortex](docs/assets/bench_execution_time.svg)

![Compilation AOT, démarrage et taille binaire](docs/assets/bench_binary_size_and_compile.svg)

| Rang | Langage / Profil | Total (ms) | T1 (ms) | T2 (ms) | T3 (ms) | T4 (ms) | T5 (ms) | AOT (ms) | Startup / JIT (ms) | Binaire (Ko) |
| :---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **#1** | **Vortex v1.0.1 (`--release`)** | **407.89** | 9.87 | 119.44 | **11.38** | **259.89** | 12.68 | 0.0 | 76.0 | 206.5 |
| #2 | C++ (`Clang++ 23 -O3 -ffast-math`) | 470.20 | **9.40** | 147.26 | 40.84 | 267.28 | 8.10 | 1 669.7 | 70.1 | 77.5 |
| #3 | C (`Clang 23 -O3 -ffast-math`) | 525.63 | 9.45 | 143.84 | 61.10 | 300.96 | 7.49 | 898.2 | **42.9** | 69.0 |
| #4 | C++ (`Clang++ 23 -O3`) | 551.55 | 73.44 | 148.14 | 44.29 | 276.86 | 8.63 | 1 695.6 | 70.4 | 76.5 |
| #5 | Rust (`rustc 1.99 -O3`) | 588.56 | 82.77 | **117.14** | 61.97 | 314.92 | 10.96 | 1 047.5 | 48.1 | 4 309.5 |
| #6 | C (`Clang 23 -O3`) | 592.92 | 73.94 | 140.03 | 61.28 | 312.48 | **7.23** | 904.4 | 52.6 | 68.5 |
| #7 | Java (`HotSpot 17`) | 828.39 | 300.09 | 129.99 | 16.83 | 341.14 | 31.01 | 714.9 | 221.7 | 3.8 (+JVM) |
| #8 | C# (`.NET v4.8 / RyuJIT`) | 902.90 | 86.45 | 122.76 | 79.12 | 597.58 | 17.21 | 313.5 | 77.7 | 5.0 (+CLR) |
| #9 | Node.js `v26` (`V8 JIT`) | 1 566.88 | 267.16 | 190.13 | 75.58 | 970.38 | 60.33 | 0.0 | 325.6 | 97 751.8 |
| #10 | Python `3.12` (`CPython`) | 40 421.43 | 6 505.76 | 7 366.69 | 634.56 | 20 929.30 | 5 153.60 | 0.0 | 2 252.2 | 171.2 (+DLL) |

- **Épreuves** : **T1** Riemann `f32` (`100 × 1M` avec `step = 1e-6f`, zéro saturation IEEE-754) · **T2** Tableau `i64` (`50M`) · **T3** Allocation `1M` structs `Node` par arène · **T4** `fib(40)` + `100M` appels · **T5** Grille `2000×50000` sur `12` threads.
- **Honnêteté T1** : Sur T1, Vortex (`9.87 ms`) fait jeu égal avec Clang C/C++ `-O3 -ffast-math` (`9.40–9.45 ms`) grâce à une réduction vectorielle associative `4 × YMM` (`vfmadd231ps`). Sans `-ffast-math`, C/C++/Rust respectent l'ordre strict IEEE-754 (`73–83 ms`).
- **Rapports** : [Rapport complet](benchmarks/BENCHMARK_REPORT_v1.0.0.md) · [Feuille de route](docs/OPTIMIZATION_ROADMAP.md).

---

## Architecture, Sécurité & Limites Connues

```text
  Script .vx / REPL Interactif / LSP (stdio)
         │
         ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 1. Lexer & Parser récursif (src/lexer_parser.c)                  │
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

- **Mémoire sûre (`--safe` par défaut)** : contrôle d'époques sur les arènes (`VORTEX_SAFE_MAGIC`), vérification de bornes dynamique avec trace d'appels (`stacktrace`) et garde-fous `unsafe` pour les pointeurs bruts et appels `extern fn`.
- **Mode performance (`--release`)** : élimination des contrôles prouvés sûrs (RCE/LICM), allocations OS sans `memset` redondant, inlining, `omit_fp` et noyaux vectoriels AVX2/FMA3.
- **Limites actuelles du compilateur ([`src/vortex.h`](src/vortex.h))** : `MAX_FIELDS = 16` par struct, `MAX_CALL_ARGS = 16` par appel, `MAX_STRUCT_TYPES = 128`, `MAX_USER_FUNCS = 512`, `MAX_JIT_CODE_SIZE = 1 MiB`, `MAX_IR_INSTRS = 65 536` instructions par fonction, et réductions `f32` vectorisées réassociatives sur les boucles `IR_VEC_LOOP`.

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
tests\run_tests.bat   :: Windows (unitaires, 16 tests sécurité, 7 ARM64, 9 outillage)
```

```sh
./tests/run_tests.sh  # Linux / macOS (dispatch FFI POSIX/Darwin automatique)
```

---

## Structure du Dépôt

```text
.
├── .github/workflows/   # CI multi-OS (release.yml) & SemVer auto (auto-version.yml)
├── src/                 # Compilateur C99 modulaire (parser, IR/opt, JIT x64/ARM64, LSP, pkg)
├── benchmarks/          # Banc d'essai suite_10_langs/ & rapport BENCHMARK_REPORT_v1.0.0.md
├── docs/                # Spécification (VORTEX_SPEC.md), roadmap & graphiques SVG (assets/)
├── examples/            # Démonstrations 3D et GUI natives (.vx)
├── tests/               # Tests fonctionnels, sécurité (--safe/neg_*), FFI multi-OS, ARM64 & LSP/pkg
├── VERSION              # Version courante SemVer (lue par build.bat / Makefile)
├── build.bat            # Script de compilation Windows
└── Makefile             # Build multiplateforme Linux / macOS (GCC & Clang)
```
