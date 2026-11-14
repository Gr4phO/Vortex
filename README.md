# Vortex

**Compilateur JIT natif x86-64 AVX2/FMA3, langage système sans garbage collector et runtime autonome en C99 modulaire.**

[![Version](https://img.shields.io/badge/version-v1.0.0-10B981?style=flat-square)](releases/v1.0.0/)
[![Architecture](https://img.shields.io/badge/arch-x86--64%20AVX2%20%7C%20ARM64-334155?style=flat-square)](#architecture-du-compilateur)
[![Standard](https://img.shields.io/badge/C99-zero%20external%20deps-334155?style=flat-square)](#compilation-depuis-les-sources)
[![CI](https://img.shields.io/github/actions/workflow/status/corbac10099/Vortex/release.yml?style=flat-square&label=release%20CI)](.github/workflows/release.yml)

Vortex compile les scripts `.vx` et sessions REPL directement en code machine natif x86-64 en mémoire. Il combine des arènes paginées de `2 MiB` sans GC, une allocation de registres Linear Scan pondérée par la profondeur de boucle, l'auto-vectorisation AVX2/FMA3, le parallélisme natif OS (`parallel_for`), une FFI C dynamique sans wrapper (`extern fn`) et le packaging en exécutable autonome (`vortex build`).

---

## Télécharger Vortex CLI (`v1.0.0`)

Exécutable CLI autonome uniquement (compilateur + JIT + runtime, aucune dépendance externe) :

| Plateforme | Architecture | Téléchargement direct | Taille | Commande type |
| :--- | :--- | :--- | :---: | :--- |
| **Windows** | `x86-64` | [Local (`releases/v1.0.0`)](releases/v1.0.0/vortex-windows-x64.exe) · [GitHub Release](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-windows-x64.exe) | **205.5 Ko** | `.\vortex-windows-x64.exe run --release script.vx` |
| **Linux** | `x86-64` (headless) | [GitHub Release](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-linux-x64) | `< 210 Ko` | `./vortex-linux-x64 --headless run --release script.vx` |
| **macOS** | `x86-64` (Darwin) | [GitHub Release](https://github.com/corbac10099/Vortex/releases/download/v1.0.0/vortex-macos-x64) | `< 210 Ko` | `./vortex-macos-x64 --headless run --release script.vx` |

> Le binaire Windows (`210 432` octets) est versionné dans [`releases/v1.0.0/`](releases/v1.0.0/). Les binaires Linux et macOS sont compilés et validés sur runners natifs via [`.github/workflows/release.yml`](.github/workflows/release.yml) à chaque tag SemVer généré automatiquement par [`.github/workflows/auto-version.yml`](.github/workflows/auto-version.yml).

---

## Benchmarks Officiels — Vortex v1.0.0 vs 10 Langages

Protocole strictement identique ([`benchmarks/suite_10_langs/`](benchmarks/suite_10_langs/)) exécuté sur **AMD Ryzen 5 5500** (`6C/12T @ 3.60 GHz`, Windows 11 x64, `32 Go RAM`), médiane sur **5 exécutions** (`3` pour Python 3.12 ; `--release` / optimisation maximale sur chaque compilateur). Les entrées marquées `*` sont étalonnées sur la même architecture Zen 3.

![Temps d'exécution total Vortex v1.0.0 vs 10 langages](docs/assets/bench_execution_time.svg)

![Temps de compilation et taille binaire Vortex v1.0.0 vs 10 langages](docs/assets/bench_binary_size_and_compile.svg)

| Rang | Langage | Total (ms) | T1 (ms) | T2 (ms) | T3 (ms) | T4 (ms) | T5 (ms) | Démarrage (ms) | Binaire (Ko) |
| :---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **#1** | **Vortex v1.0.0 (`--release`)** | **410.30** | **10.15** | 111.16 | **10.75** | **257.19** | 12.56 | **72.30** | 205.5 |
| #2 | C++ (`Clang++ 23 -O3`) | 547.28 | 74.91 | 149.83 | 41.12 | 269.35 | **7.90** | 2 508.83 | 76.5 |
| #3 | Rust (`rustc 1.99 -O3`) | 587.08 | 84.12 | **110.19** | 60.39 | 314.14 | 10.07 | 2 393.49 | 4 309.5 |
| #4 | C (`Clang 23 -O3`) | 621.72 | 74.98 | 143.16 | 66.41 | 322.40 | 9.82 | 1 436.69 | 68.5 |
| #5 | Go 1.23\* *(étalonné Zen 3)* | 655.50\* | 88.00\* | 135.00\* | 62.00\* | 360.00\* | 10.50\* | 420.00\* | 2 100.0\* |
| #6 | Java (OpenJDK HotSpot 17) | 745.02 | 246.42 | 125.88 | 15.73 | 326.25 | 29.66 | 900.22 | 3.8 (+JVM) |
| #7 | C# (.NET v4.8 / RyuJIT) | 910.82 | 82.89 | 110.59 | 77.05 | 636.31 | 16.87 | 654.86 | 5.0 (+CLR) |
| #8 | JavaScript (Node.js v26) | 1 580.34 | 264.85 | 199.86 | 78.11 | 983.10 | 62.97 | 344.55 | 97 751.8 |
| #9 | LuaJIT 2.1\* *(étalonné Zen 3)* | 2 418.00\* | 115.00\* | 165.00\* | 28.00\* | 1 930.00\* | 180.00\* | **12.00\*** | 520.0\* |
| #10 | Python 3.12 (CPython) | 42 891.01 | 6 599.29 | 7 379.21 | 627.99 | 22 069.13 | 5 292.36 | 403.10 | 6 885.0 |

> **Épreuves** : **T1** Intégrale de Riemann `f32` (`100M`) · **T2** Transformation & réduction tableau `i64` (`50M`) · **T3** Allocation & réduction `1M` structures `Node` · **T4** `fib(40)` + `100M` appels · **T5** Grille multi-cœur `12` threads.  
> **Rapports détaillés** : [Rapport complet v1.0.0](benchmarks/BENCHMARK_REPORT_v1.0.0.md) · [Feuille de route d'optimisation](docs/OPTIMIZATION_ROADMAP.md).

---

## Capacités & Architecture du Compilateur

```text
  Script .vx / REPL Interactif
         │
         ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 1. Lexer & Parser récursif (src/lexer_parser.c)                  │
  │    Layout C des structs, contrôle de mutabilité, émission IR 3-ad│
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 2. Optimiseur IR & Allocation de Registres (src/ir_opt.c)        │
  │    Inlining, déroulement récursif, LICM, AVX2 Loop Vectorizer,   │
  │    analyse de vivacité & Linear Scan pondéré par boucle          │
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 3. Backends Natifs Dual-ABI (src/jit_x64.c, src/jit_arm64.c)     │
  │    x86-64 (Win64 / SysV64, AVX2 + FMA3, omit-fp) & ARM64 NEON    │
  └──────────────────────────────┬───────────────────────────────────┘
                                 ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ 4. Runtime Natif (src/arena.c, src/runtime.c)                    │
  │    Arènes 2 MiB alignées 64B, pool de threads OS, FFI C, GUI/CLI │
  └──────────────────────────────────────────────────────────────────┘
```

- **Gestion mémoire sans GC** : arènes chaînées par pages de `2 MiB` (`VirtualAlloc` / `mmap`) alignées sur `64` octets, rembobinage $O(1)$ par portée (`scope_push` / `scope_pop`) et allocation OS zero-copy en `--release`.
- **Optimisations IR & JIT** : inlining automatique des fonctions courtes, déroulement 1 niveau de récursion binaire avec spécialisation des cas de base, omission du pointeur de trame (`omit_fp`), passage direct du premier paramètre en registre (`RCX`/`RDI`), if-conversion sans branchement (`cmovge`) et réduction d'adresse `LEA`/`SIB`.
- **Auto-vectorisation AVX2 + FMA3** : boucles `f32` déroulées sur **32 voies FMA3** (`vfmadd231ps`), boucles sur tableaux `i64` sur **16 éléments par itération** avec `prefetcht0`, et boucles de grille modulaires vectorisées via `vpcmpgtq` / `vpand` / `vpsubq`.
- **Parallélisme multi-cœur (`parallel_for`)** : pool persistant de threads natifs OS dotés chacun de sa propre arène locale sans verrou.
- **FFI C dynamique & packaging** : appels directs vers bibliothèques partagées (`LoadLibraryA` / `dlopen`) sans code glue, et génération d'exécutables autonomes (`vortex build`).

---

## Compilation depuis les Sources & Démarrage Rapide

### Compiler le compilateur

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
# Lancer le REPL interactif (:help, :vars, :structs, :time, :run, :exit)
./vortex

# Exécuter un script en mode haute performance ou headless
./vortex run --release benchmarks/suite_10_langs/bench_vortex.vx
./vortex --headless run --release script.vx

# Évaluer une expression en ligne ou packager un binaire autonome
./vortex eval "let mut x = 21; x = x * 2; print(x);"
./vortex build examples/app_3d.vx -o app_3d.exe
```

### Suite de tests automatisés

```bat
tests\run_tests.bat   :: Windows (tests unitaires, sécurité, ARM64, Jalon 3)
```

```sh
./tests/run_tests.sh  # Linux / macOS
```

---

## Documentation & Structure du Dépôt

- **[Spécification Technique du Langage](docs/VORTEX_SPEC.md)** — syntaxe, système de types, pipeline IR/AVX2, FFI et API standard.
- **[Rapport Officiel de Benchmarks v1.0.0](benchmarks/BENCHMARK_REPORT_v1.0.0.md)** — protocole complet, sommes de contrôle et analyse par épreuve.
- **[Feuille de Route d'Optimisation](docs/OPTIMIZATION_ROADMAP.md)** — historique et chantiers d'optimisation bas niveau.

```text
.
├── .github/workflows/   # CI multi-OS (release.yml) & versionnage SemVer auto (auto-version.yml)
├── src/                 # Compilateur C99 modulaire (lexer/parser, IR/opt, JIT x64/ARM64, runtime)
├── releases/v1.0.0/     # Binaire CLI autonome Windows (vortex-windows-x64.exe)
├── benchmarks/          # Banc d'essai 10 langages (suite_10_langs/) & rapport officiel
├── docs/                # Spécification, feuille de route & graphiques SVG (assets/)
├── examples/            # Démonstrations 3D et GUI natives (.vx)
├── tests/               # Tests de régression, sécurité mémoire, ARM64 & outillage
├── VERSION              # Version courante SemVer (gérée automatiquement)
├── build.bat            # Script de compilation Windows
└── Makefile             # Build multiplateforme Linux / macOS
```
