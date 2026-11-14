# Vortex Compiler Optimization Roadmap — "Road to #1" (v1.0.0 → v1.2.0+)

> **Objectif** : Surpasser C (`Clang -O3`), C++ (`Clang++ -O3`) et Rust (`rustc -C opt-level=3`) sur l'ensemble des 5 classes d'épreuves (SIMD flottant, bande passante mémoire, allocation massive, appels intensifs / récursion, parallélisme multi-cœur), tout en conservant le binaire autonome `< 250 Ko` et la compilation JIT instantanée (`~68 ms`).

---

## 📊 Diagnostic de Performance Actuel (Vortex v1.0.0 — Ryzen 5 5500 Zen 3)

| Tâche | Vortex v1.0.0 (`--release`) | Meilleur Concurrent (AOT) | Écart Actuel | Goulot Principal Identifié (`src/`) |
| :--- | :---: | :---: | :---: | :--- |
| **T1** : Riemann `f32` (100M) | **8.91 ms** 🏆 | C++ **73.09 ms** | **8.2x plus rapide** | `emit_avx2_vector_loop` limite à 2 YMM acc (`ymm0, ymm1`) et utilise `vmulps+vaddps+vdivps` sans FMA3 |
| **T2** : Bande passante `i64` (50M) | 209.58 ms | Rust **111.37 ms** | 1.88x plus lent | `memset(0)` forcé 400 Mo (`arena.c`), boucle scalaire non vectorisée, branches non fusionnées, rechargement d'adresses |
| **T3** : 1M Struct `Node` | **20.02 ms** 🏆 (n°2) | Java **16.93 ms** / C++ 40.92 ms | **2.0x–3.0x > C/Rust** | Indirection pointeur par `Node` + slice header; pas de stockage AoS contigu à plat |
| **T4** : `fib(40)` + 100M appels | 1573.51 ms | C++ **270.37 ms** | 5.82x plus lent | Aucun inlining (`step_fn`), prologue fixe lourd (7 push + stack home + cadre >= 192 o) sur 100M+ appels |
| **T5** : Parallélisme 12 threads | 16.23 ms | C **7.72 ms** | 2.10x plus lent | Sauts conditionnels non fusionnés (`cmp`+`setge`+`test`+`jz` au lieu de `cmovge`) + constantes rechargées par itération |

---

## 🏆 Classement des 5 Chantiers d'Optimisation

| Priorité | Optimisation Architecturale | Gain de Vitesse Estimé (%) | Impact Cible | Complexité d'Implémentation | Fichiers C Concernés |
| :---: | :--- | :---: | :---: | :---: | :--- |
| **#1** | **Inlining automatique des fonctions courtes & prologue feuille** | **+380% à +550%** sur T4 (`~1573 ms → ~260 ms`), **+15%** global | Tâche 4 (`fib`, `step_fn`) | **Moyenne** | `src/ir_opt.c`, `src/jit_x64.c`, `src/lexer_parser.c` |
| **#2** | **Vectorisation AVX2 flux mémoire + FMA3 (`vfmadd231ps`) + déroulement 32x** | **+80% à +140%** sur T2 (`~209 ms → ~80 ms`), **+35% à +55%** sur T1 (`~5.5 ms`) | Tâche 2 & Tâche 1 | **Élevée** | `src/ir_opt.c`, `src/jit_x64.c` |
| **#3** | **Suppression `memset(0)` paresseuse (`--release`), adressage SIB `disp32` & `LEA` 1-cycle** | **+40% à +65%** sur T2, **+30% à +50%** sur T3 (`~12 ms`) | Tâche 2 & Tâche 3 | **Faible → Moyenne** (AoS plat : Élevée) | `src/arena.c`, `src/jit_x64.c`, `src/lexer_parser.c` |
| **#4** | **Hoisting des invariants de boucle (LICM généralisé) & épinglage registres** | **+25% à +45%** sur T2 et T5, élimine les spills boucle interne | Toutes les boucles | **Moyenne-Élevée** | `src/ir_opt.c`, `src/lexer_parser.c` |
| **#5** | **Constant Folding, DCE, fusion `cmp/jcc` + `cmovcc` & Peephole x86-64** | **+15% à +35%** global, **-30%** empreinte I-cache JIT | Tâche 5, Tâche 4, Tâche 2 | **Faible-Moyenne** | `src/ir_opt.c`, `src/jit_x64.c` |

---

## 1. Inlining Automatique des Fonctions Courtes & Élimination du Prologue Feuille

- **Gain de vitesse estimé** : **+380% à +550%** sur les appels intensifs (Tâche 4 : passage attendu de **1 573 ms** à **~260–310 ms**, devant C# et Java, au contact de C/C++/Rust).
- **Complexité** : **Moyenne**.
- **Fichiers C concernés** : [`src/ir_opt.c`](file:///d:/code/vortex/src/ir_opt.c), [`src/jit_x64.c`](file:///d:/code/vortex/src/jit_x64.c), [`src/lexer_parser.c`](file:///d:/code/vortex/src/lexer_parser.c).

### Constat d'audit actuel
1. Dans [`src/jit_x64.c`](file:///d:/code/vortex/src/jit_x64.c#L184-L260), **toute** fonction utilisateur — même une fonction feuille triviale de 4 instructions comme `step_fn(a, b)` — exécute systématiquement :
   - `emit_func_prologue` : sauvegarde de **tous les 7 registres callee-saved** (`rbx, rsi, rdi, r12..r15`), écriture mémoire des paramètres vers la pile (`[rbp+16..40]` ou `[rbp-64..-104]`), et réservation minimale de **192+ octets** via `compute_frame_size()`.
   - Côté appelant (`IR_CALL_USER`) : `push` de chaque argument sur la pile, `sub rsp`, relecture mémoire vers `rcx/rdx/r8/r9` (ou `rdi/rsi/...`), `call`, puis `add rsp`.
   - Côté retour (`IR_RET`) : `vzeroupper` systématique + dépilement complet des 7 registres.
2. Sur 100 000 000 d'appels dans la Tâche 4, cet overhead représente environ **~2,8 milliards d'instructions** purement administratives avant même la récursion `fib(40)`.

### Modifications concrètes à implémenter
1. **Pass d'inlining IR (`ir_inline_small_functions` dans `ir_opt.c`)** :
   - Conserver une table `UserFuncIR` contenant l'IR pré-alloué des fonctions utilisateur (avant `ir_linear_scan_allocate`).
   - Critères d'éligibilité : `!is_unsafe_boundary_change && !is_recursive && ir_count <= 24 && loop_count == 0`.
   - Au niveau de chaque `IR_CALL_USER` : remapper les `VReg` de la fonction appelée vers de nouveaux `VReg` de l'appelant, remplacer `IR_LOAD_PARAM idx` par `IR_MOV arg_vreg`, remplacer `IR_RET src` par `IR_MOV call_dest, src + IR_JMP inline_exit_lbl`.
   - *Effet immédiat sur `step_fn`* : `step_fn` disparaît totalement de la boucle de Tâche 4, exposant `acc = acc + 7919; if acc >= 1000003 { acc -= 1000003; }` aux passes LICM et `cmovge`.
2. **Prologue adaptatif & convention registres directe (`jit_x64.c`)** :
   - Analyser par fonction `used_callee_mask` et `has_calls`.
   - Si `!has_calls && total_spill_bytes == 0 && !needs_stack_home` (fonction feuille pure) : omettre `push rbp` (ou garder frame minimal sans `push r12..r15` non utilisés) et ne sauvegarder que les registres réellement modifiés (`__builtin_popcount(used_callee_mask)`).
   - Passer et lire les 4/6 premiers paramètres directement dans les registres ABI (`IR_LOAD_PARAM k` -> lire le registre d'entrée tant qu'il n'est pas écrasé ou utiliser `caller-saved` en priorité dans les feuilles).

---

## 2. Instructions FMA3 (`vfmadd231ps`), Vectorisation AVX2 Mémoire & Déroulement Multi-Accumulateurs

- **Gain de vitesse estimé** : **+80% à +140%** sur les réductions et transformations de tableaux (Tâche 2 : **209.58 ms → ~80–95 ms**, dépassant Rust et C/C++ sur le chemin mémoire), et **+35% à +55%** sur le calcul pur (Tâche 1 : **8.91 ms → ~5.5 ms**).
- **Complexité** : **Élevée**.
- **Fichiers C concernés** : [`src/ir_opt.c`](file:///d:/code/vortex/src/ir_opt.c), [`src/jit_x64.c`](file:///d:/code/vortex/src/jit_x64.c).

### Constat d'audit actuel
1. **Tâche 2 entièrement scalaire** : dans [`ir_loop_vectorize()`](file:///d:/code/vortex/src/ir_opt.c#L868-L960), le vectoriseur n'accepte aucune boucle contenant `IR_LOAD_ARRAY`, `IR_STORE_ARRAY` ou de l'arithmétique entière `i64`. Ainsi, `arr[i] = cur` et `v = arr[i] * 3 + 1; sum += v` traitent **1 seul `int64_t` par itération**.
2. **Sous-utilisation des ports AVX2/FMA sur Tâche 1** : [`emit_avx2_vector_loop()`](file:///d:/code/vortex/src/jit_x64.c#L917-L1031) déroule sur 16 flottants avec **seulement 2 accumulateurs** (`ymm0`, `ymm1`) et émet une chaîne séquentielle `vmulps -> vaddps -> vdivps -> vaddps`. Sur AMD Zen 3 (latence `vdivps` 10–13 cycles, `vaddps` 3 cycles, double port FMA 256-bit), 2 accumulateurs ne suffisent pas à saturer les unités vectorielles ni la latence de division.

### Modifications concrètes à implémenter
1. **Auto-vectoriseur de flux mémoire AVX2 (`ir_vectorize_array_loops` dans `ir_opt.c` + `jit_x64.c`)** :
   - Après RCE (`ir_optimize_bounds_and_licm` ayant prouvé les bornes sûres en `--release`), reconnaître les boucles `for i in start..end` affines sans dépendances arrière (`arr[i]` lu/écrit linéairement + réduction associative `sum += expr`).
   - Émettre pour `*i64` (4 éléments / YMM) ou `*f32` (8 éléments / YMM) déroulé 4x (`16 i64` ou `32 f32` par itération) :
     - `vmovdqu ymm, [base + idx*8]` + `prefetcht0 [base + idx*8 + 512]`.
     - Pour `* 3 + 1` en `i64` : `vpsllq ymm1, ymm0, 1` (`*2`), `vpaddq ymm0, ymm0, ymm1` (`*3`), `vpaddq ymm0, ymm0, ymm_one` (`+1`) — zéro multiplication lente nécessaire !
     - Réduction vectorielle horizontale finale en fin de boucle.
2. **FMA3 matériel & 4 accumulateurs (`emit_avx2_vector_loop`)** :
   - Vérifier CPUID (`EBX bit 12 AVX2` + `ECX bit 12 FMA` au boot runtime).
   - Remplacer `vmulps + vaddps` par `vfmadd231ps` (préfixe VEX 3-octets `0xC4 0xE2`, opcode `0xB8`).
   - Passer à **4 paquets parallèles** (`ymm0..ymm3`, **32 `f32` / itération**) afin de masquer totalement la latence du pipeline `vdivps`, voire proposer un mode `--fast-math` utilisant `vrcpps` + 1 itération Newton-Raphson `x * (2 - a*x)` (11x -> 23 bits de mantisse en 3 cycles au lieu de 13 cycles).

---

## 3. Adressage x86-64 SIB Natif (`[base + index*scale + disp32]`), Strength Reduction & Tableaux AoS Contigus

- **Gain de vitesse estimé** : **+40% à +65%** sur Tâche 2 (`-60 ms` minimum dès l'élimination du double passage DRAM), **+30% à +50%** sur Tâche 3 (`~20 ms → ~10–12 ms`, **#1 absolu devant Java TLAB**).
- **Complexité** : **Faible** (`arena.c` + `LEA`/SIB `disp32`) à **Élevée** (tableaux AoS inline `[Node; N]`).
- **Fichiers C concernés** : [`src/arena.c`](file:///d:/code/vortex/src/arena.c), [`src/jit_x64.c`](file:///d:/code/vortex/src/jit_x64.c), [`src/lexer_parser.c`](file:///d:/code/vortex/src/lexer_parser.c).

### Constat d'audit actuel
1. **Double balayage DRAM 400 Mo caché en Tâche 2 (`arena.c:193-196`)** : `vortex_alloc_slice_common()` alloue via `arena_alloc_with_header()` puis exécute systématiquement `memset(ptr, 0, bytes)` **même en mode `--release`** ! Pour `slice_i64(50_000_000)`, Vortex force l'OS à commettre physiquement et écrire **400 Mo de zéros** avant même la Pass 1 qui réécrit intégralement ces 400 Mo — alors que `VirtualAlloc` (Windows) et `mmap(MAP_ANONYMOUS)` (Linux/macOS) livrent déjà des pages garanties à zéro à la demande !
2. **SIB rigide (`jit_x64.c:589-605`)** : `emit_load_mem_array` / `emit_store_mem_array` encodent toujours `Mod=01, disp8=0` (`0x00` fixe). Impossible d'accéder directement à un champ de structure contiguë `[base + idx*8 + field_offset]` sans calcul d'adresse intermédiaire.
3. **Indirection pointeur en Tâche 3 (`lexer_parser.c:665-671`)** : `[Node]` alloue chaque `Node` séparément via `vortex_alloc_struct(24)` et stocke un pointeur dans `nodes3[i]`, doublant l'empreinte cache et ajoutant une déréférencement aléatoire à la relecture.

### Modifications concrètes à implémenter
1. **Allocation Slice Zero-Copy en `--release` (`arena.c:187-198`)** :
   - Sur les grandes allocations `bytes >= VORTEX_PAGE_SIZE` issues directement d'un nouveau bloc OS (`VirtualAlloc` / `mmap`) en mode `VORTEX_MODE_RELEASE` (et sans recyclage sale), **supprimer le `memset(ptr, 0, bytes)` redondant** (l'OS zéro-initialise au premier fault matériel en parallèle de Pass 1). Sur une arène réutilisée en `--safe`, conserver le zérotage de sécurité.
2. **Généraliser les émetteurs SIB (`disp32`) & `LEA` (`jit_x64.c`)** :
   - Ajouter paramètre `int32_t disp` à `emit_load_mem_array_disp` / `emit_store_mem_array_disp` (omettre `disp8` quand `disp == 0 && (base&7) != 5`, `Mod=01` si `-128..127`, `Mod=10` sinon).
   - Strength Reduction : reconnaître `x * 3 + k`, `x * 5 + k`, `x * 9 + k` et émettre une **unique instruction** `lea r64, [reg + reg*{2,4,8} + k]`, ou `shl` pour les puissances de 2.
3. **Tableaux Contigus Value/AoS (`slice_struct(type, len)` / `[Node; N]`)** :
   - Allouer `N * sizeof(Node)` d'un seul tenant dans l'arène et accéder à `nodes[i].y` en 1 cycle via pointeur glissant (`ptr += sizeof(Node)` par Strength Reduction) ou `imul idx, sizeof(Node)` + `mov rax, [base + offset + 8]`.

---

## 4. Hoisting des Invariants de Boucle (LICM) & Épinglage des Pointeurs de Tableaux en Registres

- **Gain de vitesse estimé** : **+25% à +45%** sur les boucles imbriquées (Tâche 2 & Tâche 5) et réduction drastique des accès pile `[rbp - disp]`.
- **Complexité** : **Moyenne-Élevée**.
- **Fichiers C concernés** : [`src/ir_opt.c`](file:///d:/code/vortex/src/ir_opt.c), [`src/lexer_parser.c`](file:///d:/code/vortex/src/lexer_parser.c).

### Constat d'audit actuel
1. Dans [`src/lexer_parser.c`](file:///d:/code/vortex/src/lexer_parser.c#L2441-L2493), chaque itération de `for i in 0..n` réémet `v_one = IR_CONST_I64 1` à l'intérieur du corps de boucle (`movabs r10, 1` — **10 octets** par itération !) faute d'un vrai pré-en-tête LICM.
2. [`ir_optimize_bounds_and_licm()`](file:///d:/code/vortex/src/ir_opt.c#L584-L770) ne hisse **que** les `IR_BOUNDS_CHECK`. Toutes les constantes (`7`, `1024`, `7919`, `1000003`) et calculs invariants restent coincés dans le corps de boucle.
3. [`ir_linear_scan_allocate()`](file:///d:/code/vortex/src/ir_opt.c#L995-L1110) traite tous les intervalles `[start_ip, end_ip]` à égalité, sans pondération par profondeur de boucle ni fractionnement d'intervalle (*live-range splitting*). Sous Windows (3 int scratch + 7 int callee = 10 GPR) — et pire sous Linux/macOS SysV64 où **`float_callee_count == 0`** (`ir_opt.c:29`) — un pointeur de tableau (`arr2`, `row_sums`) ou une variable flottante qui traverse un appel ou subit un pic de pression local est expulsé (`spill`) vers `[rbp - offset]` pour toute sa durée de vie, forçant un `mov reg, [rbp - disp]` à chaque tour de boucle !

### Modifications concrètes à implémenter
1. **Pré-en-tête LICM généralisé (`ir_opt.c`)** :
   - Détecter chaque boucle `(lbl_start .. backedge_jmp)` et identifier les registres invariants (définis hors boucle et jamais réécrits dans `[ip_start..ip_jmp]`).
   - Déplacer vers un bloc *preheader* juste avant `lbl_start` toute instruction `IR_CONST_I64`, `IR_CONST_F32` ou calcul pur (`IR_ADD/SUB/MUL/...`) dont tous les opérandes sont invariants.
2. **Linear Scan pondéré par boucle & Coalescing (`ir_linear_scan_allocate`)** :
   - Calculer `loop_depth[ip]` et un coût d'expulsion `spill_weight[v] = sum(10^loop_depth[ip] for each use/def)`.
   - Autoriser l'**éviction intelligente** : lorsqu'un `VReg` de boucle interne a besoin d'un registre et qu'un `VReg` froid (hors boucle) en occupe un, évincer (spiller) le `VReg` froid plutôt que le pointeur de tableau ou l'accumulateur chaud !
   - Coalescer `IR_MOV dst, src` lorsque `src` meurt à cette instruction pour éviter des `mov reg, reg` en sortie de chaque boucle `if`/`for`.

---

## 5. Constant Folding, Dead Code Elimination (DCE) & Peephole Optimizer x86-64

- **Gain de vitesse estimé** : **+15% à +35%** global (impact direct sur Tâche 5 et Tâche 2 via conversion sans branchement `cmovge`), et **-25% à -35%** de taille de code machine JIT généré.
- **Complexité** : **Faible-Moyenne**.
- **Fichiers C concernés** : [`src/ir_opt.c`](file:///d:/code/vortex/src/ir_opt.c), [`src/jit_x64.c`](file:///d:/code/vortex/src/jit_x64.c).

### Constat d'audit actuel
1. **Chaînes conditionnelles verbeuses** : dans les boucles à modulo conditionnel (`if cur >= 1024 { cur = cur - 1024; }` en Tâche 2 ou `if acc >= 1000003 { acc = acc - 1000003; }` en Tâches 4 & 5), le backend génère séquentiellement `cmp -> setge r8b -> movzx -> test r8, r8 -> jz skip -> sub -> mov`, ajoutant ~5 instructions inutiles et exposant le prédicteur de branchements à des pénalités de 15+ cycles lorsque la condition alterne.
2. **Encodage d'immédiats systématiquement long (`jit_x64.c:371-376`)** : `emit_mov_ri64()` émet **toujours 10 octets** (`REX.W + B8+reg + imm64`), même pour charger `0`, `1` ou une petite constante 32 bits.
3. **Absence de pliage de constantes & DCE** : expressions constantes comme `rows5 * cols5` ou variables temporaires réassignées ne sont ni pliées avant génération ni purgées avant l'allocation de registres (ce qui gonfle artificiellement les intervalles de vie).

### Modifications concrètes à implémenter
1. **Pass `ir_fold_and_dce()` avant Linear Scan (`ir_opt.c`)** :
   - Propagation et pliage arithmétique de constantes (`IR_ADD/SUB/MUL` sur 2 constantes -> `IR_CONST_I64`), élimination d'instructions mortes (résultat `dest` jamais lu et sans effet de bord).
   - Fusion `IR_CMP_*` + `IR_JZ/JNZ` : éviter de matérialiser le booléen `0/1` dans un registre GP avec `setcc`/`movzx`/`test` lorsqu'il n'est consommé que par le saut suivant -> émettre directement `cmp` + `jl`/`jge`/`jne`.
   - **If-Conversion (`IR_SELECT` → `cmovcc`)** : transformer les petits diamants `if cond { dst = alt }` sans appel ni accès mémoire en `lea tmp, [val - M]; cmp val, M; cmovge val, tmp` — zéro branchement, **100 % immunisé contre les mauvaises prédictions** sur Zen 3 ! (Explique directement pourquoi Rust gagne la Tâche 2 en `111 ms`).
2. **Encodeur compact & Peephole pass (`jit_x64.c`)** :
   - Dans `emit_mov_ri64(j, rd, imm)`:
     - `imm == 0` → `xor r32, r32` (2–3 octets, zéro dépendance et débit maximal sur Zen 3).
     - `0 < imm <= 0xFFFFFFFFULL` → `mov r32, imm32` (5–6 octets au lieu de 10).
     - `-2147483648LL <= imm < 0` → `mov r64, sign_ext_imm32` (`C7 /0`, 7 octets).
   - Peephole immédiat sur `add/sub reg, imm32` et suppression des `mov reg, reg` redondants.

---

## 📈 Projection Cible (Vortex v1.2.0 après Chantiers #1 à #5 sur Ryzen 5 5500)

| Épreuve | Vortex v1.0.0 (Mesuré) | Vortex v1.2.0 (Projeté) | C++ (`Clang++ -O3`, Mesuré) | Statut Projeté |
| :--- | :---: | :---: | :---: | :--- |
| **T1** (SIMD Riemann `f32`) | 8.91 ms | **~5.5 ms** | 73.09 ms | 🏆 **#1 (+13x devant C++)** |
| **T2** (50M Tableau `i64`) | 209.58 ms | **~80–95 ms** | 144.70 ms | 🏆 **#1 (devant Rust/C++/C)** |
| **T3** (1M Structs) | 20.02 ms | **~10–12 ms** | 40.92 ms | 🏆 **#1 (devant Java TLAB & C++)** |
| **T4** (`fib(40)` + 100M calls) | 1573.51 ms | **~260–310 ms** | 270.37 ms | 🥇 **Parité C / C++ / Rust** |
| **T5** (Grille 12 Threads) | 16.23 ms | **~7.0–8.5 ms** | 8.32 ms | 🏆 **Parité C / #1 ex æquo** |
