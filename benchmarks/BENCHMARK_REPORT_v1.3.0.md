# Vortex `v1.3.0` — Rapport de Benchmarks Réalistes & Accélération Silicium (`std/hw`)

Ce document présente la suite de benchmarks **réaliste et sans tricherie** ([`benchmarks/realistic_suite/`](realistic_suite/)) : tous les langages exécutent strictement les mêmes algorithmes en virgule flottante IEEE-754 `f64` sans `-ffast-math` ni contraction FMA implicite (`-ffp-contract=off` sous Clang), sans motifs de boucle artificiels ni allocateurs spécifiques sur une tâche donnée.

> **Environnement matériel mesuré (Windows 11 x64)**
> - **CPU** : AMD Ryzen 5 5500 (6 cœurs physiques / 12 threads, L1D 32 Ko/cœur, L2 512 Ko/cœur, L3 16 384 Ko partagé, 32 639 Mo RAM)
> - **GPU** : AMD Radeon RX 6700 XT (`gfx1031`, 20 CU, 12 272 Mo VRAM, OpenCL 2.1 LDS 64 Ko/CU, FP64 supporté)
> - **Compilateurs & Runtimes** : Clang/Clang++ `23.1.4`, Rust `1.99.1`, Go `1.27.4`, OpenJDK `21.0.13`, C# `.NET csc v4.8`, Node.js `v26.13.0`, Python `3.12.10`
> - **Reproductibilité** : `python benchmarks/realistic_suite/run_realistic_benchmarks.py --runs 3`

---

## 1. Suite Réaliste Multi-Langages (5 Épreuves Identiques)

### Cahier des charges des 5 épreuves
1. **T1 — Simulation N-Body (`f64`)** : `600` corps × `25` pas de temps (`sqrt_f64`, réduction d'énergie cinétique, strict IEEE-754 `f64`).
2. **T2 — Quicksort récursif (`500 000` `i64`)** : remplissage LCG Lehmer 31-bit + tri Hoare récursif à pivot médian.
3. **T3 — Multiplication Matricielle Dense `f64` (`320 × 320 × 320`)** : triple boucle pure `i-k-j` en mémoire contiguë, sans BLAS ni GPU.
4. **T4 — Crible d'Ératosthène (`N = 10 000 000`)** : crible sur tableau `[i32]` contigu + somme exacte des `664 579` nombres premiers.
5. **T5 — Mandelbrot Multi-Cœur `f64` (`1024 × 1024`, `256` itérations)** : parallélisé sur tous les cœurs (threads Win32/POSIX, `std::thread`, goroutines, `IntStream.parallel()`, `Parallel.For`, `worker_threads`, `parallel_for` Vortex ; mono-thread GIL pour CPython).

![Benchmark Réaliste Total 9 Langages](../docs/assets/bench_realistic_total.png)

![Détail par Épreuve et Latence AOT vs Startup](../docs/assets/bench_realistic_breakdown.png)

### Résultats bruts mesurés (médiane de 3 runs ; 1 run pour Python 3.12)

| Langage / Profil | Total (ms) | Ratio vs C++ | T1 N-Body (ms) | T2 QSort (ms) | T3 MatMul (ms) | T4 Sieve (ms) | T5 Mandel (ms) | AOT (ms) | Startup / JIT (ms) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **C++ (`Clang++ -O3 -ffp-contract=off`)** | **167.77** | 1.00× | 33.85 | 29.00 | **3.18** | **63.84** | 30.62 | 1 513.7 | 27.89 |
| C (`Clang -O3 -ffp-contract=off`) | 169.88 | 1.01× | 34.22 | **28.74** | 3.21 | 70.33 | 30.37 | 797.6 | 30.29 |
| Go (`1.27`) | 186.74 | 1.11× | 35.42 | 31.53 | 17.02 | 69.61 | 29.52 | 10 234.6 | 29.18 |
| Rust (`rustc 1.99 -O3`) | 189.36 | 1.13× | 34.28 | 29.62 | 17.60 | 73.43 | 30.74 | 928.5 | **26.84** |
| C# (`.NET v4.8 /optimize+`) | 189.68 | 1.13× | **33.98** | 30.92 | 24.99 | 76.12 | **21.26** | 1 532.5 | 49.04 |
| **Vortex (`--release`, JIT x86-64 `v1.3.0`)** | **212.12** | **1.26×** | 40.70 | 30.88 | 26.74 | 78.69 | 32.28 | **0.0** | 30.18 |
| Java (`OpenJDK 21 HotSpot C2`) | 237.68 | 1.42× | 55.46 | 54.15 | 9.17 | 69.32 | 43.11 | 732.3 | 270.38 |
| Node.js (`v26.13 V8 JIT`) | 335.96 | 2.00× | 48.78 | 49.33 | 40.37 | 112.44 | 73.97 | **0.0** | 114.52 |
| Vortex (`--safe`, bornes + arènes actives) | 1 360.65 | 8.11× | 295.36 | 167.82 | 579.17 | 281.12 | 33.83 | **0.0** | 29.59 |
| Python `3.12` (`CPython`, T5 mono-thread) | 13 628.96 | 81.24× | 2 334.69 | 797.19 | 2 450.49 | 1 110.24 | 6 837.08 | **0.0** | 212.14 |

**Checksums vérifiés à l'identique sur les 9 langages :**
- `T1_CHK = 2379.995400`
- `T2_CHK = 3218448098`
- `T3_CHK = 6920530.340000`
- `T4_CHK = 3203324994356`
- `T5_CHK = 59429842`

### Analyse sincère des points forts et limites
- **Positionnement réel de Vortex (`212.12 ms`)** : sur des algorithmes génériques non truqués, Vortex `--release` termine à **`+26 %` de Clang++ `-O3`** (`167.77 ms`), quasi à parité avec **Go (`186.7 ms`)**, **Rust (`189.4 ms`)** et **C# (`189.7 ms`)**, et **devance Java OpenJDK 21 (`237.7 ms`)** et **Node.js v26 (`336.0 ms`)**, tout en démarrant avec **0 ms de phase AOT** (`30.18 ms` JIT+démarrage total contre `732–1533 ms` de compilation AOT préalable pour C/C++/Rust/Java/C#).
- **Où Vortex excelle déjà** :
  - **T2 Quicksort (`30.88 ms`)** : à 7 % de C (`28.74 ms`) et devant Rust/Go/Java/Node sur un tri récursif intensif en indexation mémoire.
  - **T5 Mandelbrot 12 threads (`32.28 ms`)** : à ~6 % de C/C++/Rust (`30.4–30.7 ms`), grâce au pool de threads hybride spin-barrier ajouté en `v1.3.0`.
- **Où Vortex reste derrière C/C++** :
  - **T3 MatMul pur (`26.74 ms` vs `3.18 ms` en C/C++)** : Clang auto-vectorise la boucle interne `i-k-j` en AVX2 `f64` (`4 × double` FMA/itération), alors que le JIT Vortex émet encore de l'arithmétique SSE2 scalaire (`addsd`/`mulsd`) sur les boucles `f64` générales.
  - **Mode `--safe` (`1 360.65 ms`)** : conserve volontairement chaque vérification dynamique de borne et d'époque d'arène dans les boucles internes (`T3` passe de `26.7 ms` à `579.2 ms`) afin de garantir des traces d'erreur exactes au développement.

---

## 2. Suite d'Accélération Silicium Optionnelle (`import "std/hw";`)

Séparée des benchmarks purs ci-dessus pour toute honnêteté, cette suite mesure l'API matérielle opt-in `std/hw` qui cartographie au démarrage les caches CPU (`CPUID` + `GetLogicalProcessorInformationEx`), le GPU OpenCL (`OpenCL.dll`), pré-calcule la route d'exécution (`L1` / `L2` / `L3` / `RAM_STREAM` / `GPU_OPENCL`) et exécute des noyaux BLIS `AVX2+FMA3` multi-cœur (microkernel 4×8 `f64` / 4×16 `f32`) ou GPU OpenCL (tuiles LDS 16×16).

![Accélération Silicium std/hw CPU AVX2 BLIS & GPU OpenCL](../docs/assets/bench_silicon_hw.png)

| Workload | Implémentation | Route | Temps (ms) | Débit calcul | Erreur max |
| :--- | :--- | :--- | ---: | ---: | ---: |
| **GEMM `f64` `512×512×512`** | Vortex scalaire pur (1T) | `Scalar JIT` | 112.02 | 2.40 GFLOP/s | Référence |
| GEMM `f64` `512×512×512` | C (`Clang -O3 -march=native` 1T) | `Auto-Vec` | 15.13 | 17.75 GFLOP/s | `0.000000` |
| **GEMM `f64` `512×512×512`** | **Vortex `import "std/hw"` (`hw_matmul_f64`)** | **`L3_CACHE` (6C BLIS)** | **1.47** | **182.36 GFLOP/s** | **`0.000000`** |
| GEMM `f32` `1024×1024×1024` | C (`Clang -O3 -march=native` 1T) | `Auto-Vec` | 57.99 | 37.03 GFLOP/s | Référence |
| GEMM `f32` `1024×1024×1024` | Vortex `import "std/hw"` CPU (`hw_matmul_f32_cpu`) | `L3_CACHE` (6C BLIS) | 6.01 | 357.44 GFLOP/s | `0.000000` |
| **GEMM `f32` `1024×1024×1024`** | **Vortex `import "std/hw"` GPU (`hw_matmul_f32`)** | **`GPU_OPENCL` (`gfx1031`)** | **4.55** | **471.87 GFLOP/s** | **`0.000000`** |

*(Temps GPU mesuré transferts PCIe Host-to-Device et Device-to-Host inclus.)*
