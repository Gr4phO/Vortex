<p align="center">
  <img src="docs/assets/vortex_logo.png" alt="Vortex Logo" width="140" />
</p>

# Vortex Programming Language

**Langage système et de calcul haute performance compilé à la volée (JIT `x86-64` AVX2 & `ARM64` NEON)** — *zéro garbage collector, arènes paginées à régions statiques, FFI C dynamique et compilation en binaire autonome.*

[![Version](https://img.shields.io/github/v/tag/Gr4phO/Vortex?label=version&color=10B981&style=flat-square)](https://github.com/Gr4phO/Vortex/releases/latest)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=flat-square)](LICENSE)
[![Architecture](https://img.shields.io/badge/arch-x86--64%20AVX2%20%7C%20ARM64%20NEON-334155?style=flat-square)](#architecture-sécurité--limites-connues)
[![Standard](https://img.shields.io/badge/C99-zero%20external%20deps-334155?style=flat-square)](#compilation-depuis-les-sources--tests)
[![CI](https://img.shields.io/github/actions/workflow/status/Gr4phO/Vortex/ci.yml?style=flat-square&label=CI)](.github/workflows/ci.yml)

Vortex compile les scripts `.vx` et sessions REPL directement en code machine natif en mémoire (~23 700 lignes de C99 pur modulaire dans `src/`, sans LLVM ni dépendance externe). Il combine une gestion mémoire sans GC basée sur des arènes paginées (`2 Mio`) avec contrôle statique d'échappement, un allocateur de registres Linear-Scan sur IR 3-adresses, le support natif double précision **`f64`**, une vectorisation SIMD (`AVX2/FMA3` / `NEON`), un moteur silicium CPU/Cache/GPU optionnel (`std/hw` avec buffers VRAM résidents `HwBuf` + routeur adaptatif), des modes de sûreté progressifs (`--safe`, `--ultrasafe 1/2/3`, `--release`), un exécuteur de tests natif (`vortex test`), une spec auto-générée (`vortex spec`), un serveur LSP intégré, un gestionnaire de paquets (`vortex pkg`), une bibliothèque partagée d'intégration native (`vortex.dll` / `libvortex.so` / `libvortex.dylib`) et une bibliothèque standard modulaire (`11` modules dans `std/`).

---

## Points Clés

- **JIT natif `x86-64` (Windows x64 & System V Linux/macOS) & `ARM64` NEON** : émission directe en mémoire (`VirtualAlloc` / `mmap` + `W^X`), `omit_fp`, support natif `f32` et **`f64`** (SSE2 `sd` / AArch64 `D`-registers) et génération de binaires autonomes (`vortex build` : `PE32+`, `ELF64`, `Mach-O 64`), avec binaires précompilés multi-OS sur [GitHub Releases](https://github.com/Gr4phO/Vortex/releases/latest).
- **Mémoire sans GC & sûreté progressive (`--safe` par défaut, `--ultrasafe 1|2|3`, `--release`)** : arènes de portée (`scope_push()` / `scope_pop()`), vérification de régions à la compilation (`ir_region_verify`, incluant la durée de vie VRAM des `HwBuf`), garde-fous `unsafe` sur l'arithmétique de pointeurs et le FFI, contrôle rapide par arène active et déduplication des bornes en bloc de base.
- **Optimiseur IR 3-adresses & Thread Pool Spin-Barrier** : inlining (`<= 64` IR instrs / `256` expansions en `--release`), constant folding `i64`/`f32`/`f64`, identités algébriques, fusion `IR_ADD_SUB_MOD_IMM`, intrinsèques mathématiques matériels inline (`sqrtsd`/`sqrtss`/`fabs` sur x86-64 et `fsqrt`/`fabs` sur ARM64), *range hoisting* affine (`arr[off + i]`) + contrôles de bornes inline avec *cold traps* en `--safe`, vectorisation `AVX2` (dont `IR_VEC_F64_AXPY` `vmulpd + vaddpd` IEEE-754 strict sans FMA, `vpminud` et streaming stores `vmovntdq`), et pool de threads hybride spin-barrier `YieldProcessor` piloté par la topologie CPU physique (sans charger OpenCL) avec découpages alignés sur lignes de cache (`64 octets`).
- **Moteur Silicium `import "std/hw";` (CPU BLIS & GPU VRAM `HwBuf`)** : détection `CPUID` + `GetLogicalProcessorInformationEx` + OpenCL (`hw_print_map()`), **buffers résidents RAM/VRAM `HwBuf`** (`hw_buf_from_f32`/`f64`, `hw_matmul_f32_buf`, `hw_axpy_f32_buf`, `hw_relu_f32_buf`, libérés automatiquement sur `scope_pop()`), **routeur adaptatif « Waze » EMA** (latences lissées, coût PCIe uniquement si `host_dirty`, bascule auto vers CPU en cas de congestion GPU), **cache binaire disque OpenCL**, microkernels BLIS `AVX2+FMA3` (`hw_matmul_f64`) et noyau GPU **`64×64` *register-blocked* `4×4` `float4`**.
- **Écosystème & outillage AI-friendly** : serveur LSP JSON-RPC 2.0 (`vortex lsp` avec `--ultrasafe 3`), formateur `vortex fmt`, gestionnaire de paquets (`vortex pkg` avec verrouillage `SHA-256`), diagnostics `--json`, validation `--check`, exécuteur `vortex test`, `vortex spec`, FFI C dynamique (`extern fn`) et traces d'appels natives / `DWARF v4`.

---

## Benchmarks Réalistes — 9 Langages à Armes Égales (100 % Mesurés Localement)

Banc d'essai [`benchmarks/realistic_suite/`](benchmarks/realistic_suite/) exécuté sur **AMD Ryzen 5 5500 (6C/12T) + RX 6700 XT, Windows 11 x64**, médiane sur **5 exécutions** (`1` pour Python 3.12). **Aucune tricherie méthodologique** : strict IEEE-754 `f64` sans `-ffast-math` ni contraction FMA (`-ffp-contract=off` sous Clang), Rust écrit en itérateurs sûrs idiomatiques (`.iter_mut().zip()`), zéro motifs artificiels ou allocateurs dédiés, **checksums vérifiés à l'identique sur les 9 langages** (`T1=2379.995400`, `T2=3218448098`, `T3=6920530.340000`, `T4=3203324994356`, `T5=59429842`).

![Benchmark Réaliste Total 9 Langages](docs/assets/bench_realistic_total.png)

![Détail par Épreuve et Latence AOT vs Startup](docs/assets/bench_realistic_breakdown.png)

| Langage / Profil | Total (ms) | Ratio | T1 N-Body `f64` | T2 QSort `500k` | T3 MatMul `320³` | T4 Sieve `10M` | T5 Mandel `12T` | AOT (ms) | Startup/JIT (ms) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **C++ (`Clang++ -O3`)** | **189.63** | 1.00× | 34.93 | 30.03 | 3.95 | **80.93** | 32.27 | 1 725.2 | 34.64 |
| C (`Clang -O3`) | 189.72 | 1.00× | 34.66 | 30.20 | 3.95 | 86.29 | 31.18 | 1 059.2 | 32.65 |
| Rust (`rustc 1.99 -O3`, *safe zip*) | 191.99 | 1.01× | 35.05 | **29.60** | **3.94** | 90.01 | 31.24 | 1 207.0 | **30.70** |
| **Vortex (`--release`, sans bornes)** | **202.72** | **1.07×** | 37.38 | 32.08 | 7.95 | 86.86 | 34.33 | **0.0** | 35.24 |
| C# (`.NET Framework 4.8`) | 205.69 | 1.08× | **34.35** | 31.51 | 25.46 | 86.60 | **22.91** | 1 344.1 | 57.02 |
| Go (`1.27`) | 221.27 | 1.17× | 36.98 | 32.74 | 19.58 | 94.88 | 31.53 | 945.5 | 33.01 |
| Java (`OpenJDK 21`, à froid) | 255.78 | 1.35× | 59.88 | 51.23 | 10.31 | 84.14 | 43.51 | 780.5 | 342.33 |
| **Vortex (`--safe`, bornes + arènes)** | **350.57** | **1.85×** | 37.24 | 173.06 | 7.79 | 94.00 | 36.30 | **0.0** | 34.91 |
| Node.js (`v26.13 V8`, à froid) | 364.51 | 1.92× | 52.02 | 50.79 | 41.29 | 128.06 | 84.29 | **0.0** | 142.50 |
| Python `3.12` (`CPython`) | 15 169.25 | 80.00× | 2 826.74 | 1 085.48 | 2 562.22 | 1 160.28 | 7 439.52 | **0.0** | 284.87 |

- **Lecture transparente** :
  - **Vortex `--release` (`202.72 ms`)** termine **4e au classement général** à **`1.07×` de Clang++ `-O3` (`189.63 ms`)**, quasiment au coude-à-coude avec **Rust `-O3` (`191.99 ms`)** et C# 4.8 (`205.69 ms`), devant Go (`221.27 ms`), Java 21 (`255.78 ms`) et Node.js v26 (`364.51 ms`), avec **0 ms de compilation AOT**.
  - **Précisions d'équité** : `--release` désactive les contrôles de bornes comme `C`/`C++`, alors que Rust/Go/Java/C# conservent les leurs. À **sécurité mémoire équivalente**, **`Vortex --safe` (`350.57 ms`, 3.9× plus rapide qu'avant)** bat Node.js v26 (`364.51 ms`) et égale déjà `--release` sur T1 (`37.24 ms`), T3 (`7.79 ms`), T4 (`94.00 ms`) et T5 (`36.30 ms`), seul T2 Quicksort récursif restant ralenti (`173.06 ms`). C# est compilé avec `csc` **.NET Framework 4.8** (pas .NET 8/9), et Java/Node sont mesurés sur processus court à froid (~255–365 ms).

### Accélération Silicium Optionnelle (`import "std/hw";`)

Séparée du benchmark pur ci-dessus, l'API matérielle opt-in [`std/hw`](std/hw.vx) fournit des noyaux BLIS `AVX2+FMA3` multi-cœur, des buffers résidents RAM/VRAM `HwBuf`, un routeur adaptatif Waze EMA, un cache binaire OpenCL disque et un noyau GPU `64×64` `float4` :

![Accélération Silicium std/hw CPU AVX2 BLIS & GPU OpenCL](docs/assets/bench_silicon_hw.png)

- **`hw_matmul_f64` (`512×512×512`)** : **1.22 ms** (**219.49 GFLOP/s** `f64`, route `L3_CACHE`, erreur `0.000000`) — **21.5×** plus rapide que la boucle Vortex auto-vectorisée 1T (`26.18 ms`) et **13.2×** plus rapide que C `Clang -O3` mono-cœur (`16.16 ms`).
- **`hw_matmul_f32` (`1024×1024×1024`, PCIe H2D+D2H inclus)** : **2.69 ms** (**799.81 GFLOP/s** `f32`) vs **5.23 ms** (`410.77 GFLOP/s`) CPU AVX2 6 cœurs (`hw_matmul_f32_cpu`) et **64.56 ms** (`33.27 GFLOP/s`) en C `-O3` 1T.
- **`hw_matmul_f32_buf` résident VRAM (`HwBuf`, `1024³`, zéro transfert PCIe)** : **0.701 ms** (**`3 063.46 GFLOP/s` = `3.06 TFLOP/s`**, erreur `0.000000`) sur `gfx1031` (**92×** plus rapide que C `-O3`), et **1.93 ms** pour le pipeline complet `MatMul 1024³ -> AXPY -> ReLU -> hw_buf_sync_host` avec **cache binaire disque (`2` hits / `0` recompilation)**.
- **Rapports** : [Rapport réaliste v1.3.0](benchmarks/BENCHMARK_REPORT_v1.3.0.md) · [Ancien rapport v1.0.0](benchmarks/BENCHMARK_REPORT_v1.0.0.md) · [Feuille de route](docs/OPTIMIZATION_ROADMAP.md).

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

- **Mémoire sûre (`--safe` par défaut)** : contrôle d'époques sur les arènes (`VORTEX_SAFE_MAGIC`) avec fast-path sur l'arène active du thread, déduplication des bornes par bloc de base, *range hoisting* affine (`arr[off + i]`) et contrôles inline avec *cold traps* dans les boucles sûres sans appel (**3.9× plus rapide** en `v1.3.0`), traces d'appels exactes (`stacktrace`), garde-fous `unsafe` pour les pointeurs bruts/`extern fn` et rejet des pointeurs `NULL` dans `ffi_call0..6`.
- **Mode performance (`--release`)** : élimination des contrôles prouvés sûrs (RCE/LICM), intrinsèques mathématiques inline (`sqrtsd`/`fabs`), allocations compactes/zero-copy sans `memset` redondant, inlining (`<= 64` instructions IR / `256` expansions), `omit_fp` et noyaux vectoriels `AVX2` (incluant `IR_VEC_F64_AXPY` IEEE-754 strict).
- **Limites actuelles du compilateur ([`src/vortex.h`](src/vortex.h))** : types scalaires natifs (`i64`, `i32`, `i16`, `i8`, `u64`, `u32`, `u16`, `u8`, `usize`, `isize`, `bool`, `char`, `f32`, `f64`, `ptr`, `str`, `enum`), `MAX_FIELDS = 64` par `struct`, `MAX_CALL_ARGS = 16` par appel, `MAX_STRUCT_TYPES = 1024`, `MAX_ENUM_TYPES = 256` (`128` variantes max), `MAX_SYMBOLS = 1024` par portée/fonction, `MAX_USER_FUNCS = 8192`, `MAX_DYN_LIBS = 256`, `MAX_FFI_FUNCS = 2048`, littéraux chaînes jusqu'à `4095` octets, `MAX_JIT_CODE_SIZE = 16 MiB`, `MAX_IR_INSTRS = 262 144` instructions par fonction, et réductions `f32` vectorisées réassociatives sur les boucles `IR_VEC_LOOP`.

### Modes `--ultrasafe 1 / 2 / 3` (cumulatifs, au-dessus de `--safe`)

Syntaxes acceptées : `--ultrasafe 1`, `--ultrasafe 2`, `--ultrasafe 3`, `--ultrasafe=1|2|3` ou `--ultrasafe` seul (équivalent à `3`).

| Niveau | Contrôles statiques (avant JIT) | Gardes runtime | Récupération |
| :--- | :--- | :--- | :--- |
| **`--ultrasafe 1`** | Warnings non bloquants : `W101` (code mort après `return`/`break`/`continue`), `W102` (`mut` non réassigné), `W103` (shadowing hors `_`), `W104` (variable locale inutilisée hors `_`) | Garde division/modulo entier par `0` (`IR_DIV_CHECK` hors `unsafe` -> `[VORTEX SAFETY PANIC]`) | Stop à la 1re erreur |
| **`--ultrasafe 2`** | Niveau 1 + erreurs bloquantes : `E201` (div/mod par constante `0`), `E202` (taille slice `< 0` ou indice constant hors bornes), `E203` (déréf/champ sur pointeur nul constant), `E204` (`return` manquant sur fonction `-> T` non-void) + warning global `W105` (fonction inutilisée hors `main` / `test_*`) | Niveau 1 + garde profondeur de récursion/appels (max `2048` sur fonctions non-`unsafe` avec `IR_CALL_USER`, 0 surcoût sur fonctions feuilles) | Stop à la 1re erreur |
| **`--ultrasafe 3`** | Niveau 2 + collecte complète : `E001` (erreurs parsing/types), `E101` (fonctions non résolues), `E301` (toutes violations d'arènes dans `ir_region_verify`), `E302` (appels `unsafe` hors bloc `unsafe`) | Identique au niveau 2 (exécuté uniquement si `0` erreur) | **Continue après chaque erreur** (`setjmp` + `vortex_lexer_sync`, max `256` diagnostics dédupliqués), idéal avec `--check --json` |

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
make shared     # Bibliothèque partagée libvortex.so / libvortex.dylib
```

### Utilisation CLI & Outils AI-Friendly (`v1.3.0`)

```sh
./vortex                                                 # REPL interactif
./vortex run --release benchmarks/realistic_suite/bench_vortex.vx
./vortex --headless run --safe tests/test_safety_pass.vx # Validation mémoire/bornes + frames exactes
./vortex --ultrasafe 1 script.vx                         # + div/0 runtime & warnings (W101..W104)
./vortex --ultrasafe 2 --check script.vx                 # + audits statiques (E201..E204, W105) & garde récursion
./vortex --ultrasafe 3 --check --json script.vx          # Multi-erreurs en un passage + sortie JSON structurée
./vortex test [chemin] [--ultrasafe 1|2|3] [--json]      # Exécuteur natif de tests (assert / assert_eq / test_*)
./vortex spec [--json]                                   # Spécification auto-générée depuis le compilateur
./vortex eval "let mut x = 21; x *= 2; print(x);"
./vortex build examples/app_3d.vx -o app_3d.exe          # Binaire autonome (Windows / Linux / macOS)
./vortex fmt [--check|--stdout] script.vx                # Formateur de code natif
./vortex lsp                                             # Serveur LSP (JSON-RPC)
```

### Ergonomie & Syntaxe (`v1.3.0`)

- **Types & littéraux** : annotations `bool`, `i8`/`u8`, `i16`/`u16`, `i32`/`u32`, `i64`/`u64`, `usize`/`isize`, `char`, `f32`, **`f64`**, `[f64]` (`slice_f64`), `<Enum>` ; séparateurs `1_000_000`, binaire `0b1010`, octal `0o755`, scientifique `1.5e-3`, littéraux caractère `'A'`/`'\n'`, chaînes littérales jusqu'à **4095 octets**
- **Constantes globales & indexation chaînée** : `const MAX = 16;` (expressions constantes `i64`/`f32`/`f64`/`str`/`enum` évaluées à la compilation), accès et écriture chaînés `obj.arr[i]`, `a.b.arr[i] += v`, `mat[i][j]` et `arr[i].champ`
- **Opérateurs & expressions** : court-circuit `&&`/`||`/`!`, bit-à-bit `& | ^ ~ << >>`, ternaire `cond ? a : b`, littéraux de tableau `[10, 20, 30]`, écriture chaînée sur sous-structs `world.player.pos.x += 1.0;` *(attention : `==`/`!=`/`<`/`>` sont plus prioritaires que `&`/`^`/`|`, parenthéser `(x & mask) == 0`)*
- **Contrôle de flux & affectations composées** : `match x { 1 | 2 => ..., _ => ... }`, `break`, `continue`, `enum Nom { A, B = 10, C }` (`Nom::B` / `Nom.B`), `+=`..`>>=`, `/* ... */`
- **Builtins & formatage natif** : `assert(cond)`, `assert_eq(a, b)`, `sinf_rad`..`atan2f_rad`, **`sin_f64`..`atan2_f64`**, `str_from_f64`, **`33` builtins silicium `hw_*`** (`hw_init`, `hw_buf_from_f32`/`f64`, `hw_matmul_f32_buf`/`f64_buf`, `hw_axpy_f32_buf`, `hw_relu_f32_buf`, `hw_buf_sync_host`, `hw_router_stats`...), `fs_*`, `args_*`, `env_*`, `unix_now_ms`, `sleep_ms` (**`137` builtins au total**) et formateur `vortex fmt`

### Transparence Benchmarks & Reproductibilité

- Banc d'essai reproductible multi-OS : exécuter [`benchmarks/realistic_suite/run_realistic_benchmarks.py`](benchmarks/realistic_suite/run_realistic_benchmarks.py) (`--runs 5`, Windows / Linux / macOS, Python 3 stdlib) pour reproduire les benchmarks 9 langages sur votre propre CPU/OS.
- `--safe` (et `--ultrasafe 1/2/3` qui reposent dessus) conserve les contrôles dynamiques d'indices, de pointeurs et d'arènes, tout en bénéficiant du *range hoisting* affine et des *cold traps* inline (`350.57 ms` en `--safe` vs `202.72 ms` en `--release` sur 5 passes : égalité sur T1/T3/T4/T5, contrôles conservés sur T2 Quicksort récursif à `173.06 ms`).
- **Types & plafonds compilateur** : `bool`, entiers (`i8`..`i64`, `u8`..`u64`, `usize`/`isize`, `char`), `f32`, **`f64`**, `str`, `ptr` et `enum` ; jusqu'à `64` champs par `struct`, `1024` structs, `256` enums et `8192` fonctions utilisateur ([`src/vortex.h`](src/vortex.h)).

### Suite de tests automatisés

```bat
tests\run_tests.bat   :: Windows (18 suites : unitaires, ergonomics, opt_regression, stdlib v1.2, lang/fmt v1.2, f64 v1.3, std/hw v1.3, 16 sécurité, 7 ARM64, 9 outillage & stdlib, 10 ultrasafe 1/2/3 + fmt)
```

```sh
./tests/run_tests.sh  # Linux / macOS (dispatch FFI POSIX/Darwin automatique)
```

### Extension VS Code (`Gr4phO.vortex-systems` `1.2.0`)

- **Exécution en 1 clic** : bouton **▶ Run Vortex File in Terminal** dans la barre d'onglet + sous-menu (`--release`, `--ultrasafe 1`, `--ultrasafe 2`, `--ultrasafe 3`, `--ultrasafe 3 --check`, `vortex test`, `vortex build`, `REPL`).
- **LSP & coloration syntaxique** : serveur `vortex lsp` branché sur `--ultrasafe 3` (multi-erreurs + warnings en direct), formatage natif (`vortex fmt` / `textDocument/formatting`), définition, survol, complétion et coloration complète (`const`, `match`, `enum`, `bool`, `f64`, `u8..usize`, `/* */`, `'a'`, `0b`/`0o`/`1e-5`).
- **Installation** : via le [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gr4phO.vortex-systems), les paquets `.vsix` joints aux [GitHub Releases](https://github.com/Gr4phO/Vortex/releases/latest), ou depuis [`editors/vscode/`](editors/vscode/README.md).

---

## Structure du Dépôt

```text
.
├── .github/             # CI (ci.yml), release multi-OS + .vsix (release.yml), SemVer auto & ISSUE_TEMPLATE/
├── src/                 # Compilateur C99 modulaire (~23 700 lignes) : lexer, modules, parser_expr/stmt, ir_core/ir_opt/ir_vec_ra, jit_x64/jit_x64_simd/ffi/jit_arm64, hw, lsp, pkg, arena, gui, packager, runtime
├── std/                 # Bibliothèque standard modulaire (.vx : net, http, json, crypto, db, gpu, fs, math, str, env, hw)
├── editors/vscode/      # Extension VS Code officielle (bouton ▶ Run, LSP client & grammaire .vx)
├── benchmarks/          # Suite réaliste 9 langages (realistic_suite/), historique & rapport BENCHMARK_REPORT_v1.3.0.md
├── docs/                # Spécification (VORTEX_SPEC.md), roadmap & graphiques transparents RGBA (assets/)
├── examples/            # Démonstrations 3D et GUI natives (.vx)
├── tests/               # Tests fonctionnels, ergonomie, sécurité (--safe/--ultrasafe), régression opt, FFI multi-OS, ARM64, LSP/pkg & stdlib
├── CHANGELOG.md         # Journal des modifications (du plus récent au plus ancien)
├── LICENSE              # Licence Apache-2.0
├── VERSION              # Version courante SemVer (lue par build.bat / Makefile)
├── build.bat            # Script de compilation Windows (CLI + vortex.dll)
├── build.sh             # Script de compilation Linux / macOS
└── Makefile             # Build multiplateforme Linux / macOS (GCC & Clang + shared)
```

---

## Code Signing & Privacy Policy

- **Code Signing Policy** : Free code signing on Windows is provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).
- **Team Roles & Build Provenance** : Committers, Reviewers, and Approvers: `@Gr4phO` (project maintainer). All official release binaries are built automatically from public sources via GitHub Actions ([`.github/workflows/release.yml`](.github/workflows/release.yml)), accompanied by `SHA256SUMS.txt` checksums and GitHub Artifact Attestations. On Windows, prefer downloading the portable `vortex-windows-x64.zip` archive from [GitHub Releases](https://github.com/Gr4phO/Vortex/releases).
- **Privacy Policy** : Vortex (`vortex`, `vortex.dll` / `libvortex`, and the VS Code extension) does not collect, store, or transmit any user data, telemetry, or analytics. Network requests only occur when explicitly triggered by user code (`std/net`, `std/http`, `vortex pkg`).

---

## Licence

Distribué sous licence [Apache-2.0](LICENSE).
