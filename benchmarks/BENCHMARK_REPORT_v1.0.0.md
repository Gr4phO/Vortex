# Rapport Officiel de Benchmarks — Vortex v1.0.1

> **Version évaluée** : `Vortex v1.0.1` (`--release`, JIT x86-64 AVX2/FMA3)
> **Date de campagne** : `2026-11-14`
> **Protocole** : 5 épreuves algorithmiques identiques ([`suite_10_langs/`](suite_10_langs/)), **100 % mesuré localement** (médiane sur **5 exécutions** ; **3** pour Python 3.12). Aucun résultat estimé ou étalonné dans les classements.

---

## 1. Environnement Matériel & Chaînes Évaluées

| Composant | Spécification Exacte |
| :--- | :--- |
| **Processeur (CPU)** | **AMD Ryzen 5 5500** (Zen 3, 6 cœurs / **12 threads logiques** @ **3,60 GHz**, AVX2 + FMA3) |
| **Mémoire & OS** | `32 Go` RAM DDR4 · **Microsoft Windows 11 x64** |
| **Harnais de mesure** | Chronométrage interne haute résolution par tâche (`QueryPerformanceCounter` / `steady_clock` / `Instant::now` / `time_us`), temps mur externe (`PeakWorkingSet64`) et séparation stricte **Compilation AOT** vs **Démarrage / JIT**. |

### Configurations mesurées localement (10 entrées)

1. **Vortex `v1.0.1`** — JIT x86-64 AVX2/FMA3 (`--release`, binaire CLI autonome `206.5 Ko`) · *Vectorisation flottante réassociative équivalente à `-ffast-math` sur T1*
2. **C (`Clang -O3 -ffast-math`)** — `clang 23.1.4` (`-O3 -march=native -ffast-math`)
3. **C++ (`Clang++ -O3 -ffast-math`)** — `clang++ 23.1.4` (`-O3 -march=native -ffast-math`)
4. **C (`Clang -O3`)** — `clang 23.1.4` (`-O3 -march=native`, IEEE-754 strict)
5. **C++ (`Clang++ -O3`)** — `clang++ 23.1.4` (`-O3 -march=native`, IEEE-754 strict)
6. **Rust** — `rustc 1.99.1 (58cf8f9c3 2026-10-13)` (`-C opt-level=3 -C target-cpu=native`, IEEE-754 strict)
7. **Java** — `OpenJDK HotSpot 17.0.21.0.2 LTS`
8. **C# (.NET)** — `csc v4.8` (`/optimize+ /platform:x64`, JIT RyuJIT)
9. **JavaScript (Node.js)** — `Node.js v26.13.0` (V8 JIT + `worker_threads` pour T5)
10. **Python 3** — `Python 3.12.10` (CPython + `multiprocessing`, arithmétique `f64` sur T1)

> Des implémentations de référence pour **Go** ([`bench_go.go`](suite_10_langs/bench_go.go)), **LuaJIT** ([`bench_luajit.lua`](suite_10_langs/bench_luajit.lua)) et **Zig** ([`bench_zig.zig`](suite_10_langs/bench_zig.zig)) sont présentes dans [`suite_10_langs/`](suite_10_langs/) pour reproduction sur les machines disposant de ces compilateurs ; elles ne figurent pas dans les tableaux faute de mesure locale.

---

## 2. Graphiques Comparatifs

### Temps d'exécution total (T1..T5)

![Comparatif des temps d'exécution Vortex v1.0.1](../docs/assets/bench_execution_time.svg)

### Compilation AOT + Démarrage/JIT & Taille Binaire / Runtime

![Taille du binaire et temps de compilation Vortex v1.0.1](../docs/assets/bench_binary_size_and_compile.svg)

---

## 3. Résultats Complets (Médiane sur 5 runs ; 3 pour Python 3)

### A. Temps d'exécution par tâche (ms, plus bas = meilleur)

| Rang | Langage / Profil | T1 : Riemann `f32` (`100×1M`) | T2 : Tableau `i64` (`50M`) | T3 : Structs `Node` (`1M`) | T4 : `fib(40)` + `100M` | T5 : Grille 12 threads | Total médian |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **#1** | **Vortex v1.0.1 (`--release`)** | `9.87 ms` | `119.44 ms` | **`11.38 ms`** | **`259.89 ms`** | `12.68 ms` | **`407.89 ms`** |
| **#2** | **C++ (`Clang++ -O3 -ffast-math`)** | **`9.40 ms`** | `147.26 ms` | `40.84 ms` | `267.28 ms` | `8.10 ms` | `470.20 ms` |
| **#3** | **C (`Clang -O3 -ffast-math`)** | `9.45 ms` | `143.84 ms` | `61.10 ms` | `300.96 ms` | `7.49 ms` | `525.63 ms` |
| **#4** | **C++ (`Clang++ -O3`)** *(IEEE-754)* | `73.44 ms` | `148.14 ms` | `44.29 ms` | `276.86 ms` | `8.63 ms` | `551.55 ms` |
| **#5** | **Rust (`rustc -O3`)** *(IEEE-754)* | `82.77 ms` | **`117.14 ms`** | `61.97 ms` | `314.92 ms` | `10.96 ms` | `588.56 ms` |
| **#6** | **C (`Clang -O3`)** *(IEEE-754)* | `73.94 ms` | `140.03 ms` | `61.28 ms` | `312.48 ms` | **`7.23 ms`** | `592.92 ms` |
| **#7** | **Java (OpenJDK HotSpot 17)** | `300.09 ms` | `129.99 ms` | `16.83 ms` | `341.14 ms` | `31.01 ms` | `828.39 ms` |
| **#8** | **C# (.NET v4.8 / RyuJIT)** | `86.45 ms` | `122.76 ms` | `79.12 ms` | `597.58 ms` | `17.21 ms` | `902.90 ms` |
| **#9** | **Node.js v26 (V8 JIT)** | `267.16 ms` | `190.13 ms` | `75.58 ms` | `970.38 ms` | `60.33 ms` | `1 566.88 ms` |
| **#10** | **Python 3.12** *(3 runs)* | `6 505.76 ms` | `7 366.69 ms` | `634.56 ms` | `20 929.30 ms` | `5 153.60 ms` | `40 421.43 ms` |

> *Note statistique* : Chaque cellule est la médiane indépendante de la métrique sur les runs (ex. `Total médian` est la médiane de `TOTAL_US` et peut différer de quelques millisecondes de la somme des médianes par colonne). Données brutes : [`results_raw.json`](suite_10_langs/results_raw.json).

### B. Compilation AOT vs Démarrage/JIT, Taille Binaire & RAM

| Langage / Profil | Compilation AOT (ms) | Démarrage / JIT (ms) | Temps Mur Total (ms) | Taille Binaire / Runtime (Ko) | RAM Max (Mo) | Notes sur l'empreinte disque |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **Vortex v1.0.1 (`--release`)** | `0.00 ms` *(JIT)* | `76.04 ms` | `483.92 ms` | `206.50 Ko` | `419.92 Mo` | CLI autonome (compilateur + JIT + runtime) |
| **C (`Clang -O3`)** | `904.37 ms` | `52.61 ms` | `645.53 ms` | `68.50 Ko` | **`386.36 Mo`** | Binaire AOT natif seul |
| **C (`Clang -O3 -ffast-math`)** | `898.15 ms` | `42.86 ms` | `568.49 ms` | `69.00 Ko` | `386.80 Mo` | Binaire AOT natif seul |
| **C++ (`Clang++ -O3`)** | `1 695.58 ms` | `70.35 ms` | `621.90 ms` | `76.50 Ko` | `386.72 Mo` | Binaire AOT natif seul |
| **C++ (`Clang++ -O3 -ffast-math`)** | `1 669.66 ms` | `70.08 ms` | `540.28 ms` | `77.50 Ko` | `387.21 Mo` | Binaire AOT natif seul |
| **Rust (`rustc -O3`)** | `1 047.53 ms` | `48.05 ms` | `636.61 ms` | `4 309.50 Ko` | `386.48 Mo` | Binaire AOT statique (`std` incluse) |
| **Java (HotSpot 17)** | `714.94 ms` (`javac`) | `221.74 ms` | `1 050.13 ms` | `3.83 Ko` | `480.35 Mo` | Bytecode `.class` + JVM HotSpot requise |
| **C# (.NET v4.8)** | `313.51 ms` (`csc`) | `77.71 ms` | `980.61 ms` | `5.00 Ko` | `395.48 Mo` | Assembly IL + runtime .NET CLR requis |
| **Node.js v26** | `0.00 ms` *(JIT)* | `325.58 ms` | `1 892.47 ms` | `97 751.82 Ko` | `456.47 Mo` | Runtime `node.exe` (~95,5 Mo) |
| **Python 3.12** | `0.00 ms` *(interprété)* | `2 252.16 ms` | `42 673.59 ms` | `171.22 Ko` | `1 801.06 Mo` | `python.exe` seul (+ `python312.dll` ~6,8 Mo) |

> **Méthode Démarrage / JIT** : `CompileAotMs` mesure exclusivement l'appel au compilateur AOT (`clang`, `clang++`, `rustc`, `javac`, `csc`) avant exécution. `StartupRuntimeMs` mesure `WallMs - TotalMs` lors de l'exécution du programme (démarrage processus + runtime/VM + parsing/JIT dans le cas de Vortex, Node.js et Python).

---

## 4. Sommes de Contrôle & Protocole Équitable sur T1

| Épreuve | Vortex v1.0.1 | C / C++ `-ffast-math` | C / C++ `-O3` / Rust / Java / C# / Node | Python 3.12 | Conformité |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **T1** (`100×1M` `f32`, `step=1e-6`) | `3.141480` | `3.142597` | `3.133350` | `3.141593` (`f64`) | **Toutes convergentes vers $\pi$** |
| **T2** (`50M` tableau `i64`) | `76774977344` | `76774977344` | `76774977344` | `76774977344` | **Identique au bit près** |
| **T3** (`1M` structs `Node`) | `2000003000000` | `2000003000000` | `2000003000000` | `2000003000000` | **Identique au bit près** |
| **T4** (`fib(40)` + `100M` `step`) | `102958464` | `102958464` | `102958464` | `102958464` | **Identique au bit près** |
| **T5** (Grille `2000×50000`, 12 threads) | `1899629000` | `1899629000` | `1899629000` | `1899629000` | **Identique au bit près** |

> **Correction méthodologique T1 (`100 × 1 000 000`, `step = 1e-6`)** : Sur une seule boucle de `100M` itérations avec `step = 1e-8f`, la précision `f32` (`24` bits de mantisse) saturait à `x = 0.25f` en sémantique IEEE-754 stricte (`0.25f + 1e-8f == 0.25f`, produisant `0.671089` en scalaire contre `3.097748` en SIMD réassociatif). Avec `100` passes de `1 000 000` pas (`step = 1e-6f`, `x0 = 5e-7f`), `1 ULP` reste strictement inférieur à `step` sur tout `[0, 1]`, et **tous** les langages intègrent effectivement $[0, 1]$ vers $\pi$ (~`3.133–3.143`).
>
> **Comparaison équitable `-ffast-math`** : L'auto-vectoriseur `IR_VEC_LOOP` de Vortex réassocie les réductions flottantes sur `4 × ymm` (32 voies FMA3), ce qui correspond exactement au mode `-ffast-math` de Clang. Face à `Clang/Clang++ -O3 -ffast-math`, T1 est à **quasi-parité** (`9.87 ms` contre `9.40–9.45 ms`). L'écart vis-à-vis de `C/C++/Rust -O3` (`73–83 ms`) reflète uniquement la réassociation SIMD autorisée ou non par défaut.
>
> **Garde-fou contre le constant folding** : Toutes les bornes de `bench_vortex.vx` utilisent un zéro opaque calculé à l'exécution (`let dyn_zero: i64 = num_workers() - num_workers();`), symétrique aux variables `volatile` en C/C++ et `black_box` en Rust.

---

## 5. Analyse Technique par Épreuve (`src/`)

- **T1 (`9.87 ms`) — Quasi-parité avec C/C++ `-ffast-math` (`9.40–9.45 ms`)** : Le noyau `emit_avx2_vector_loop()` (`src/jit_x64.c`) traite 32 `f32` par itération via 4 accumulateurs YMM et `vfmadd231ps`. Avec préservation explicite des `xmm6..xmm15` et marquage des vregs entiers/flottants traversant `IR_VEC_*` dans `ir_liveness_analysis()`, les boucles imbriquées restent justes aussi bien en `--safe` qu'en `--release`.
- **T2 (`119.44 ms`) — Niveau Rust (`117.14 ms`), devant C (`140.03 ms`) & C++ (`148.14 ms`)** : Allocation zero-copy de grandes pages OS en `--release` (`src/arena.c`), conversion sans branchement `cmovge` (`IR_SUB_MOD_IMM`) et noyau vectoriel AVX2 `i64` 16 éléments avec `prefetcht0` (`emit_avx2_i64_array_loop()`).
- **T3 (`11.38 ms`) — #1 grâce au modèle d'arène de portée (`scope_push`/`scope_pop`)** : L'arène paginée `2 MiB` par pointeur de pile évite `1 000 000` d'appels `malloc`/`free`, contre `16.83 ms` en Java TLAB, `40.84–44.29 ms` en C++ et `61–62 ms` en C/Rust. Cette approche s'applique aux allocations de durée de vie bornée par portée.
- **T4 (`259.89 ms`) — #1 (3 % devant C++ `-O3 -ffast-math` à `267.28 ms` et `-O3` à `276.86 ms`)** : `ir_inline_small_functions()` élimine les `100M` appels à `step_fn`, tandis que `ir_unroll_binary_recursion()` déroule 1 niveau de récursion binaire sur `fib(n)` avec omission du frame pointer (`omit_fp`) et cadre minimal dans `jit_x64.c`.
- **T5 (`12.68 ms`) — Derrière C (`7.23 ms`), C++ (`8.10 ms`) et Rust (`10.96 ms`)** : Le pool persistant `parallel_for` (`12` threads) vectorise `8` lignes par tour via `vpcmpgtq`/`vpand`/`vpsubq` (`emit_avx2_grid_mod_loop`), mais reste légèrement pénalisé par la coordination de réveil et l'overhead de lancement face aux threads OS statiques + LLVM Clang.
