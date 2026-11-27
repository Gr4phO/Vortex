# Vortex `v1.3.0` — Rapport de Benchmarks Réalistes & Accélération Silicium (`std/hw`)

Ce document présente la suite de benchmarks **réaliste et sans tricherie** ([`benchmarks/realistic_suite/`](realistic_suite/)) : tous les langages exécutent strictement les mêmes algorithmes en virgule flottante IEEE-754 `f64` sans `-ffast-math` ni contraction FMA implicite (`-ffp-contract=off` sous Clang), sans motifs de boucle artificiels ni allocateurs spécifiques sur une tâche donnée.

> **Environnement matériel mesuré (Windows 11 x64)**
> - **CPU** : AMD Ryzen 5 5500 (6 cœurs physiques / 12 threads, L1D 32 Ko/cœur, L2 512 Ko/cœur, L3 16 384 Ko partagé, 32 639 Mo RAM)
> - **GPU** : AMD Radeon RX 6700 XT (`gfx1031`, 20 CU, 12 272 Mo VRAM, OpenCL 2.1 LDS 64 Ko/CU, FP64 supporté)
> - **Compilateurs & Runtimes** : Clang/Clang++ `23.1.4`, Rust `1.99.1`, Go `1.27.4`, OpenJDK `21.0.13`, C# `.NET csc v4.8`, Node.js `v26.13.0`, Python `3.12.10`
> - **Reproductibilité** : `python benchmarks/realistic_suite/run_realistic_benchmarks.py --runs 5`

---

## 1. Suite Réaliste Multi-Langages (5 Épreuves Identiques)

### Cahier des charges des 5 épreuves
1. **T1 — Simulation N-Body (`f64`)** : `600` corps × `25` pas de temps (`sqrt_f64`, réduction d'énergie cinétique, strict IEEE-754 `f64`).
2. **T2 — Quicksort récursif (`500 000` `i64`)** : remplissage LCG Lehmer 31-bit + tri Hoare récursif à pivot médian.
3. **T3 — Multiplication Matricielle Dense `f64` (`320 × 320 × 320`)** : triple boucle pure `i-k-j` en mémoire contiguë, sans BLAS ni GPU (en itérateurs sûrs `.iter_mut().zip()` côté Rust pour permettre la vectorisation idiomatique sans `unsafe`).
4. **T4 — Crible d'Ératosthène (`N = 10 000 000`)** : crible sur tableau `[i32]` contigu + somme exacte des `664 579` nombres premiers.
5. **T5 — Mandelbrot Multi-Cœur `f64` (`1024 × 1024`, `256` itérations)** : parallélisé sur tous les cœurs (threads Win32/POSIX, `std::thread`, goroutines, `IntStream.parallel()`, `Parallel.For`, `worker_threads`, `parallel_for` Vortex ; mono-thread GIL pour CPython).

![Benchmark Réaliste Total 9 Langages](../docs/assets/bench_realistic_total.png)

![Détail par Épreuve et Latence AOT vs Startup](../docs/assets/bench_realistic_breakdown.png)

### Résultats bruts mesurés (médiane de 5 runs ; 1 run pour Python 3.12)

| Langage / Profil | Total (ms) | Ratio vs C++ | T1 N-Body (ms) | T2 QSort (ms) | T3 MatMul (ms) | T4 Sieve (ms) | T5 Mandel (ms) | AOT (ms) | Startup / JIT (ms) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **C++ (`Clang++ -O3 -ffp-contract=off`)** | **189.63** | 1.00× | 34.93 | 30.03 | 3.95 | **80.93** | 32.27 | 1 725.2 | 34.64 |
| C (`Clang -O3 -ffp-contract=off`) | 189.72 | 1.00× | 34.66 | 30.20 | 3.95 | 86.29 | 31.18 | 1 059.2 | 32.65 |
| Rust (`rustc 1.99 -O3`, *safe zip*) | 191.99 | 1.01× | 35.05 | **29.60** | **3.94** | 90.01 | 31.24 | 1 207.0 | **30.70** |
| **Vortex (`--release`, sans bornes)** | **202.72** | **1.07×** | 37.38 | 32.08 | 7.95 | 86.86 | 34.33 | **0.0** | 35.24 |
| C# (`.NET Framework 4.8 /optimize+`) | 205.69 | 1.08× | **34.35** | 31.51 | 25.46 | 86.60 | **22.91** | 1 344.1 | 57.02 |
| Go (`1.27`) | 221.27 | 1.17× | 36.98 | 32.74 | 19.58 | 94.88 | 31.53 | 945.5 | 33.01 |
| Java (`OpenJDK 21 HotSpot C2`, à froid) | 255.78 | 1.35× | 59.88 | 51.23 | 10.31 | 84.14 | 43.51 | 780.5 | 342.33 |
| **Vortex (`--safe`, bornes + arènes actives)** | **350.57** | **1.85×** | 37.24 | 173.06 | 7.79 | 94.00 | 36.30 | **0.0** | 34.91 |
| Node.js (`v26.13 V8 JIT`, à froid) | 364.51 | 1.92× | 52.02 | 50.79 | 41.29 | 128.06 | 84.29 | **0.0** | 142.50 |
| Python `3.12` (`CPython`, T5 mono-thread) | 15 169.25 | 80.00× | 2 826.74 | 1 085.48 | 2 562.22 | 1 160.28 | 7 439.52 | **0.0** | 284.87 |

**Checksums vérifiés à l'identique sur les 9 langages :**
- `T1_CHK = 2379.995400`
- `T2_CHK = 3218448098`
- `T3_CHK = 6920530.340000`
- `T4_CHK = 3203324994356`
- `T5_CHK = 59429842`

### Transparence méthodologique & analyse honnête
- **Bornes `--release` vs `--safe`** : comparer `Vortex --release` (`202.72 ms`, sans contrôles dynamiques) à `C`/`C++` (`189.6–189.7 ms`) est directement comparable. Face aux langages gardant leurs vérifications de bornes (Rust `191.99 ms`, C# `205.69 ms`, Go `221.27 ms`, Java `255.78 ms`), l'équivalent sûr de Vortex est **`Vortex --safe` (`350.57 ms`)**, accéléré de **3.9×** (`1 360.65 ms -> 350.57 ms`) grâce au *range hoisting* affine et aux *cold traps* inline (seul T2 Quicksort récursif conserve des contrôles par accès à `173.06 ms`).
- **Rust idiomatique sûr** : avec `.iter_mut().zip()` sur T3 (`3.94 ms`), Rust devance logiquement Vortex `--release` (`191.99 ms` vs `202.72 ms`, écart ~5.6 % / `1.07×` de C++).
- **C# `.NET Framework 4.8` & JVM/V8 à froid** : le compilateur `csc` mesuré ici cible `.NET Framework 4.8` (pas `.NET 8/9`, plus rapide), et Java/Node sont mesurés sans warmup serveur préalable (avantage légitime du JIT léger de Vortex sur scripts/CLI courts, mais Java C2 serait plus proche de C++/Rust sur un serveur long).
- **Où Vortex excelle déjà** :
  - **T1 N-Body (`37.38 ms` `--release` / `37.24 ms` `--safe`)** : intrinsèques matériels inline `sqrtsd`/`fabs` sans pénalité de registre.
  - **T2 Quicksort (`32.08 ms` `--release`) & T5 Mandelbrot 12T (`34.33 ms`)** : très proches de C/C++/Rust.
  - **T3 MatMul `f64` (`7.95 ms` `--release` / `7.79 ms` `--safe`)** : auto-vectoriseur `AVX2` `IR_VEC_F64_AXPY` (`vmulpd + vaddpd` sans FMA) devant Java (`10.31 ms`), Go (`19.58 ms`) et C# 4.8 (`25.46 ms`).
- **Où C/C++/Rust gardent l'avantage** : sur **T3 MatMul (`3.94–3.95 ms` vs `7.95 ms`)** grâce au tuilage/dépliage de registres `k`-unrolling, et sur **T4 Crible (`80.93 ms` vs `86.86 ms`)**.

---

## 2. Suite d'Accélération Silicium Optionnelle (`import "std/hw";`)

Séparée des benchmarks purs ci-dessus pour toute honnêteté, cette suite mesure l'API matérielle opt-in `std/hw` : buffers résidents RAM/VRAM **`HwBuf`** liés aux portées d'arène, routeur adaptatif **« Waze » EMA** (coût PCIe compté uniquement si `host_dirty`, déviation auto CPU en congestion GPU), cache binaire OpenCL disque, microkernels BLIS `AVX2+FMA3` et noyau GPU **`64×64` *register-blocked* `4×4` `float4`**.

![Accélération Silicium std/hw CPU AVX2 BLIS & GPU OpenCL](../docs/assets/bench_silicon_hw.png)

| Workload | Implémentation | Route | Temps (ms) | Débit calcul | Erreur max |
| :--- | :--- | :--- | ---: | ---: | ---: |
| **GEMM `f64` `512×512×512`** | Vortex auto-vec `AVX2` pur (1T) | `AXPY JIT` | 26.18 | 10.25 GFLOP/s | Référence |
| GEMM `f64` `512×512×512` | C (`Clang -O3 -march=native` 1T) | `Auto-Vec` | 16.16 | 16.61 GFLOP/s | `0.000000` |
| **GEMM `f64` `512×512×512`** | **Vortex `import "std/hw"` (`hw_matmul_f64`)** | **`L3_CACHE` (6C BLIS)** | **1.22** | **219.49 GFLOP/s** | **`0.000000`** |
| GEMM `f32` `1024×1024×1024` | C (`Clang -O3 -march=native` 1T) | `Auto-Vec` | 64.56 | 33.27 GFLOP/s | Référence |
| GEMM `f32` `1024×1024×1024` | Vortex `import "std/hw"` CPU (`hw_matmul_f32_cpu`) | `L3_CACHE` (6C BLIS) | 5.23 | 410.77 GFLOP/s | `0.000000` |
| GEMM `f32` `1024×1024×1024` | Vortex `import "std/hw"` GPU (PCIe inclus) | `GPU_OPENCL` (`64×64 float4`) | 2.69 | 799.81 GFLOP/s | `0.000000` |
| **GEMM `f32` `1024×1024×1024`** | **Vortex `std/hw` GPU VRAM (`hw_matmul_f32_buf`)** | **`HwBuf` VRAM (`4×4 float4`)** | **0.701** | **3 063.46 GFLOP/s (`3.06 TFLOP/s`)** | **`0.000000`** |

*(Pipeline complet chaîné en VRAM `hw_matmul_f32_buf + hw_axpy_f32_buf + hw_relu_f32_buf + hw_buf_sync_host` mesuré à **`1.93 ms`** avec **`2` cache hits OpenCL disque**.)*

*(Temps GPU mesuré transferts PCIe Host-to-Device et Device-to-Host inclus.)*
