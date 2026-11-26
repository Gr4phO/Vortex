# VORTEX_BIBLE.md — Référence Technique Exhaustive du Langage Vortex

> **Public cible** : Document de référence unique destiné aux agents IA chargés d'écrire un moteur de jeu 3D voxel et un serveur multijoueur haute performance en Vortex (`v1.1.5`).  
> **Périmètre vérifié** : Toutes les règles, limites, syntaxes, fonctions intégrées et comportements ci-dessous ont été vérifiés directement dans le code source C99 du compilateur/runtime (`src/*.c`, `src/vortex.h`, ~20 500 lignes), de la bibliothèque standard (`std/*.vx`) et du binaire compilateur (`vortex.exe`, `vortex spec`). Toute fonctionnalité absente de ces sources est explicitement marquée **NON SUPPORTÉ**.

---

## 1. VUE D'ENSEMBLE & PHILOSOPHIE DU LANGAGE

### 1.1 Paradigme, typage et modèle d'exécution

- **Paradigme** : Langage impératif et procédural orienté performance système, sans classes ni programmation orientée objet, sans ramasse-miettes (*Zero-GC*), conçu autour d'arènes mémoire paginées déterministes.
- **Modèle de compilation et d'exécution** :
  - **Compilé nativement en mémoire (JIT)** vers du code machine natif sans machine virtuelle bytecode au runtime.
  - Pipeline complet du compilateur C99 :
    1. **Préprocesseur d'imports** (`vortex_resolve_imports` dans `src/runtime.c` et `src/modules.c`) : résout récursivement les directives `import` et évalue la compilation conditionnelle `#[cfg(...)]`.
    2. **Analyseur lexical** (`src/lexer.c`) : produit le flux de jetons (*tokens*).
    3. **Analyseur syntaxique à descente récursive directe vers IR** (`src/parser_stmt.c`, `src/parser_expr.c`) : émet directement une **représentation intermédiaire linéaire à 3 adresses (IR)** sans conserver d'arbre syntaxique abstrait (AST) en mémoire.
    4. **Vérificateur statique de régions mémoire** (`ir_region_verify` dans `src/ir_core.c`) : interdit statiquement les fuites de références d'arène éphémères, le retour de pointeurs locaux après `scope_pop()`, et le *use-after-pop* (actif dans tous les modes : `--safe`, `--ultrasafe 1/2/3` et `--release` ; en `--ultrasafe 3`, collecte toutes les violations `E301` sans s'arrêter à la première).
    5. **Passes d'audit et d'optimisation IR** (`src/ir_opt.c`, `src/ir_vec_ra.c`, `src/runtime.c`) : en `--safe` / `--ultrasafe`, constant folding, propagation de copies, LICM constante, DCE et déduplication des vérifications ; en `--ultrasafe 2/3`, audits statiques (`E201`–`E204`, `W101`–`W105`) et garde de récursion ; en `--release`, inlining des petites fonctions utilisateur (`<= 40` instructions IR), déroulement de récursion binaire, élimination des vérifications de bornes (RCE), hissage d'invariants de boucle (LICM), auto-vectorisation SIMD AVX2 (`8 x f32`), pliage des puissances de 2 (divisions/modulos sur `x86_64`), et élimination de code mort (DCE).
    6. **Analyse de vivacité et allocation de registres** (`ir_liveness_analysis`, `Linear Scan` dans `src/ir_vec_ra.c`) : `10 GPR / 14 FPR` sur `x86_64`.
    7. **Émission machine native** (`jit_x64.c`, `jit_arm64.c`) dans des pages mémoire W^X (taille maximale du tampon JIT par unité compilée : `MAX_JIT_CODE_SIZE = 16 MiB`, soit `16 777 216` octets ; capacité maximale de `262 144` instructions IR et `65 536` registres virtuels par fonction).
  - **Cibles supportées** (`--target`) : `x86_64-windows` (hôte Windows par défaut), `x86_64-linux`, `x86_64-darwin`, `arm64-linux` (cross-compilation ELF64 possible via `vortex build`), `arm64-darwin` (cross-compilation Mach-O 64 possible via `vortex build`).
- **Typage** :
  - **Statique strict sans conversion implicite** : les variables `let` infèrent leur type depuis l'expression d'initialisation (ou utilisent une annotation explicite `: Type`) ; les paramètres et types de retour des fonctions (`fn`) ainsi que les champs de `struct` doivent être annotés explicitement.
  - Polymorphisme paramétrique / génériques (`<T>`), traits/interfaces, surcharge de fonctions/opérateurs, héritage : **NON SUPPORTÉ**.

### 1.2 Structure minimale d'un programme valide

Il n'existe **PAS de fonction `main()` implicite**. Toutes les instructions exécutables placées au niveau racine (*top-level*, en dehors des déclarations `import`, `struct`, `extern`, `fn` et directives `#[cfg(...)]`) sont compilées dans une fonction synthétique `__main__` et exécutées séquentiellement de haut en bas.

Programme minimal valide à une seule ligne :

```vortex
print_str("Vortex pret\n");
```

Si vous définissez une fonction `fn main() -> i64`, le runtime ne l'appellera **jamais automatiquement** : vous devez l'appeler explicitement au niveau racine :

```vortex
fn main() -> i64 {
    print(42);
    return 0;
}

main();
```

### 1.3 Modules, `import`, visibilité et compilation conditionnelle

#### Syntaxe et placement de `import`

L'instruction `import` n'est autorisée **qu'au niveau racine (top-level)** d'un fichier `.vx`. L'utiliser dans une fonction ou un bloc provoque une erreur de syntaxe.

```vortex
import "std/net";
import std/json;
import "engine/chunk.vx";
```

- L'extension `.vx` est ajoutée automatiquement si elle est absente.
- Les guillemets sont optionnels si le spécificateur ne contient que des caractères alphanumériques, `/`, `.` ou `-`.
- **Inclusion textuelle unique** : le préprocesseur normalise chaque chemin et n'inclut chaque fichier qu'une seule fois (protection intégrée contre les inclusions doubles et circulaires).

#### Ordre de résolution des chemins (`vortex_resolve_import`)

1. **Modules commençant par `std/`** :
   1. Répertoire désigné par la variable d'environnement `VORTEX_STD` (`$VORTEX_STD/<nom>.vx`)
   2. `<repertoire_executable>/std/<nom>.vx`
   3. `<repertoire_executable>/../std/<nom>.vx`
   4. `<repertoire_fichier_importeur>/std/<nom>.vx`
   5. `std/<nom>.vx` (relatif au répertoire de travail courant)
2. **Autres chemins** :
   1. Relatif au répertoire du fichier qui contient l'`import`
   2. Relatif au répertoire de travail courant
   3. Résolution via le gestionnaire de paquets (`vortex.toml` / cache `.vortex/packages`)

#### Visibilité publique / privée

- Mots-clés `pub`, `priv`, `export`, `module`, `namespace`, résolution qualifiée `module::symbole` ou `module.symbole` pour les fonctions : **NON SUPPORTÉ** *(le séparateur `::` est réservé à l'accès aux variantes d'`enum`, ex. `Etat::Actif`)*.
- Toutes les déclarations `struct`, `enum`, `extern fn` et `fn` issues des fichiers importés sont injectées dans un **espace de noms global unique** (préfixer vos symboles par le nom du module, par exemple `net_tcp_listen`, `chunk_create`, afin d'éviter toute collision).
- **Important** : les variables `let` déclarées au niveau racine d'un fichier importé s'exécutent au début de `__main__`, mais elles sont **invisibles** à l'intérieur des fonctions `fn`.

#### Compilation conditionnelle `#[cfg(...)]`

Placée immédiatement avant un élément de niveau racine (`fn`, `struct`, `extern`, `import`, ou instruction/bloc racine), la directive `#[cfg(predicat)]` conserve l'élément si le prédicat est vrai pour la cible active, et l'ignore totalement sinon.

Prédicats simples supportés dans `src/modules.c` :

| Prédicat | Condition vraie |
| :--- | :--- |
| `windows` | Cible `x86_64-windows` |
| `linux` | Cible `x86_64-linux` ou `arm64-linux` |
| `macos` / `darwin` | Cible `x86_64-darwin` ou `arm64-darwin` |
| `unix` | Toute cible Linux ou macOS/Darwin |
| `x86_64` / `x64` | Architecture x86-64 |
| `arm64` / `aarch64` | Architecture ARM64 |

Combinaisons booléennes `#[cfg(all(...))]`, `#[cfg(any(...))]`, `#[cfg(not(...))]` : **NON SUPPORTÉ**.

Exemple :

```vortex
#[cfg(windows)]
fn os_tag() -> str {
    return "windows";
}

#[cfg(unix)]
fn os_tag() -> str {
    return "unix";
}
```

---

## 2. SYNTAXE EXACTE & GRAMMAIRE

### 2.1 Règles lexicales et limites du lexer (`src/lexer.c`)

- **Commentaires** :
  - Ligne : `// commentaire jusqu'à la fin de ligne`.
  - Multi-ligne : `/* commentaire sur une ou plusieurs lignes */` (non imbriqués).
- **BOM UTF-8** : automatiquement ignoré s'il est présent au début du fichier (ainsi que dans le serveur LSP).
- **Identifiants** : `[A-Za-z_][A-Za-z0-9_]*`. Les noms de symboles, champs, fonctions, `struct` et `enum` sont stockés dans des tampons de **64 octets** (`char name[64]`, soit **63 caractères** significatifs max).
- **Mots-clés réservés exacts du lexer** (`22` jetons dédiés dans `src/lexer.c`) :  
  `let`, `mut`, `for`, `parallel_for`, `in`, `while`, `if`, `else`, `fn`, `return`, `struct`, `enum`, `extern`, `unsafe`, `as`, `import`, `break`, `continue`, `true`, `false`, `i64`, `f32`.  
  *(Les noms de types `i32`, `f64`, `ptr`, `str`, `void`, `str?`, `ptr?` sont analysés contextuellement comme spécificateurs de type.)*
- **Littéraux booléens** : `true` (vaut `1` de type `i64`) et `false` (vaut `0` de type `i64`).
- **Littéraux entiers (`i64`)** :
  - Décimaux (`0`, `42`, `4096`) ou hexadécimaux (`0xFF`, `0X1A2B`).
  - Séparateurs numériques `1_000`, notation binaire `0b1010`, notation octale `0o755`, suffixes de type (`42u64`, `42i32`, `42L`) : **NON SUPPORTÉ**.
- **Littéraux flottants (`f32`)** :
  - Doivent comporter des chiffres avant et après le point décimal (`0.0`, `3.14159`). Le lexer s'arrête avant `..` afin de ne pas casser les intervalles `0..10`.
  - Forme sans zéro initial (`.5`), sans décimale (`1.`), notation scientifique (`1e-5`, `2.5E3`), suffixe (`1.0f`) : **NON SUPPORTÉ**.
- **Littéraux chaînes (`str`)** :
  - Délimités par guillemets doubles `"..."`.
  - Séquences d'échappement supportées : `\n`, `\t`, `\r`, `\0`, `\\`, `\"`.
  - **LIMITE (`Token.text[4096]`)** : chaque littéral chaîne individuel peut contenir jusqu'à **4095 octets** (`MAX_LITERAL_STRINGS = 16384` littéraux par programme). Pour assembler des chaînes dynamiques encore plus longues, utiliser `str_concat(a, b)` ou `str_join(vec, sep)`.
  - Interpolation (`"${x}"`), raw strings (`r"..."`), littéraux caractère (`'A'`) : **NON SUPPORTÉ**.
- **Points-virgules** : obligatoires après `import`, `extern`, `let`, `return`, `break`, `continue` (tolérés optionnellement après `break`/`continue`/`}` d'`enum`), appels utilisés comme instructions, affectations et `scope_pop(...)`. Interdits après les blocs `{ ... }` de `fn`, `struct`, `if`, `else`, `for`, `while`, `unsafe`.

### 2.2 Variables, constantes, mutabilité et portée (`scoping`)

#### Déclarations de variables

Une déclaration `let` exige toujours un initialiseur `= expr;`. Déclarer sans initialiser (`let x: i64;`) est **NON SUPPORTÉ**.

```vortex
let largeur = 16;                 // i64 immuable (type inféré)
let mut compteur = 0;             // i64 mutable
let gravite: f32 = 9.81;          // f32 immuable (annotation explicite)
let mut dt: f32 = 0.0;            // f32 mutable
let buf = slice_i32(4096);        // [i32] immuable (contenu du tableau modifiable)
compteur = compteur + 1;
dt = 0.016;
```

- Réassigner une variable non déclarée `mut` déclenche `[VORTEX SAFETY ERROR] Cannot reassign immutable binding`.
- **Important** : l'immuabilité d'un `let buf = slice_i32(4096);` ou `let c = Chunk { ... };` concerne la **liaison** (réassigner `buf = ...;`), pas les éléments ou champs pointés : `buf[0] = 1 as i32;` et `c.dirty = 1;` sont autorisés même si la liaison n'est pas `mut`.

#### Constantes et visibilité globale

- Mots-clés `const`, `static`, `constexpr`, `#define` : **NON SUPPORTÉ**.
- **PORTÉE ISOLÉE DES FONCTIONS** : `parse_function_def` crée une table des symboles vierge (`SymbolTable`) ne contenant que les paramètres de la fonction. **Une fonction `fn` ne peut PAS lire les variables `let` déclarées au niveau racine.**
- **Pattern officiel pour les constantes globales** : déclarer des fonctions sans paramètre retournant la constante (automatiquement inlinées en `--release` si le corps comporte 40 instructions IR ou moins) :

```vortex
fn CHUNK_W() -> i64 { return 16; }
fn CHUNK_VOL() -> i64 { return 4096; }

fn voxel_offset(x: i64, y: i64, z: i64) -> i64 {
    let w = CHUNK_W();
    return y * 256 + z * w + x;
}
```

#### Portée (`scoping`) et limites de table de symboles

- Les blocs `{ ... }` (`if`, `else`, `for`, `while`, `unsafe`) créent des portées lexicales : les variables locales déclarées à l'intérieur ne sont plus accessibles après `}`.
- La boucle `for i in a..b` crée une variable de boucle locale `i` de type `i64`.
- Capacité maximale par portée/fonction : `MAX_SYMBOLS = 1024` symboles.

### 2.3 Types primitifs exacts et règles de conversion

#### Tableau complet des types reconnus par `vortex_parse_type_spec_full`

| Type Vortex | Représentation interne | Taille | Alignement `struct` | Usage et remarques |
| :--- | :--- | :--- | :--- | :--- |
| `i64` | Entier signé 64 bits | 8 octets | 8 octets | Type entier principal (boucles, indexation, booléens `0`/`1` ou `false`/`true`, valeurs d'`enum`, tailles) |
| `i32` | Entier signé 32 bits | 4 octets | 4 octets | Champs compacts, voxels `[i32]`, FFI `int` / `int32_t` |
| `f32` | Flottant IEEE 754 32 bits | 4 octets | 4 octets | Type flottant principal (physique, vecteurs 3D, SIMD AVX2 8 lanes) |
| `f64` | Flottant 64 bits (`TYPE_F64`) | 8 octets | — | Reconnu dans les types FFI ; les expressions arithmétiques utilisent `f32` |
| `str` | Pointeur chaîne C (`\0`) | 8 octets | 8 octets | Chaîne de caractères ; comparer avec `str_eq(a, b) == 1` |
| `ptr` | Pointeur brut non typé | 8 octets | 8 octets | Adresse mémoire (`0` = NULL) |
| `str?` / `ptr?` | Pointeur / chaîne nullable | 8 octets | 8 octets | Spécification FFI acceptant `0` sans déclencher `IR_FFI_CHECK` |
| `void` | Absence de valeur | — | — | Type de retour `extern ... -> void` ou `fn ... -> void` |
| `*T` | Pointeur typé (`*i64`, `*i32`, `*f32`, `*Struct`) | 8 octets | — | Déréférencement / arithmétique FFI ; non reconnu directement en champ `struct` |
| `[T]` | Slice d'arène (`[i64]`, `[i32]`, `[f32]`, `[Struct]`) | 8 octets | 8 octets | Tableau contigu alloué en arène avec longueur tracée par en-tête |
| `NomStruct` | Référence vers `struct` en arène | 8 octets | 8 octets | Pointeur 64 bits vers la structure allouée en arène |
| `NomEnum` | Constantes nommées `i64` | 8 octets | — | Utiliser `i64` dans les annotations de paramètres/champs/retours ; constantes accessibles via `NomEnum::Var` / `NomEnum.Var` |

Types **NON SUPPORTÉS** : `bool` (en tant qu'annotation de type — utiliser `i64` avec `true`/`false`), `i8`, `i16`, `u8`, `u16`, `u32`, `u64`, `usize`, `isize`, `char`, `string`, tuples `(A, B)`, pointeurs de fonction `fn(...) -> ...` en tant que variable.

#### Règles de conversion (`as`) — CRITIQUES

**IL N'Y A AUCUNE CONVERSION IMPLICITE EN VORTEX** (ni `i64 -> f32`, ni `i32 -> i64`, ni dans les expressions mixtes, ni dans les initialisations).

1. **Piège arithmétique mixte (`i64` + `f32`)** : si l'un des deux opérandes est `f32`, le parser émet directement une instruction flottante (`IR_FADD`, `IR_FMUL`, ...) sans convertir l'opérande entier.  
   - **FAUX** : `let r = i + 0.5;` ou `let r = dt_us / 1000000.0;` (corrompt les bits de l'entier !)  
   - **CORRECT** : `let r = (i as f32) + 0.5;` et `let r = (dt_us as f32) / 1000000.0;`
2. **Piège d'initialisation `f32` depuis un entier** : dans `let x: f32 = 0;` ou `Vec3 { x: 0, y: 1, z: 0 }`, le compilateur copie les bits bruts de l'entier sans conversion flottante.  
   - **CORRECT** : toujours écrire `0.0`, `1.0`, ou `(n as f32)`.
3. **Sûreté des conversions `as`** :
   - Conversions numériques entre `i64`, `i32`, `f32` : **autorisées en code sûr** (hors `unsafe`).
   - Conversions impliquant `ptr`, `str`, `*T`, `[T]` ou `Struct` (par exemple `p as i64`, `addr as ptr`, `buf as *i32`) : **requièrent un bloc `unsafe { ... }` ou `unsafe fn`**.
   - **Avertissement sur les slices** : effectuer `s as *i32` efface les métadonnées de longueur/slice (`is_slice_ptr = false`, `known_len = -1`). Ne jamais caster un slice si vous souhaitez conserver l'indexation sûre.

```vortex
let idx: i64 = 12;
let fy: f32 = (idx as f32) * 0.25;     // OK : conversion explicite i64 -> f32
let block_id: i32 = 7 as i32;          // OK : i64 -> i32 pour stockage [i32]
let back: i64 = block_id as i64;       // OK : i32 -> i64
let trunc: i64 = 3.9 as i64;           // OK : conversion f32 -> i64
```

### 2.4 Structures (`struct`), énumérations, unions, tableaux et maps

#### Structures (`struct`)

- **Limites du compilateur** (`src/vortex.h`) : maximum **`64` champs** par structure (`MAX_FIELDS = 64`), et **`1024` structures** au total par programme (`MAX_STRUCT_TYPES = 1024`).
- **Types de champs reconnus dans `struct`** (`parse_struct_stmt`) : `i64`, `i32`, `f32`, `ptr`, `str`, `<NomStruct>`, `[i64]`, `[i32]`, `[f32]`, `[NomStruct]`. *(Ne pas écrire `*i32` dans un champ de `struct`, utiliser `ptr` ou `[i32]`.)*
- **Pas de point-virgule** après `}`. Virgules entre champs (dernière virgule optionnelle).

```vortex
struct Vec3 {
    x: f32,
    y: f32,
    z: f32
}

struct Entity {
    id: i64,
    pos: Vec3,
    hp: i32,
    flags: i32
}

// Instanciation par noms de champs (obligatoirement complets, pas de valeurs par défaut)
let p = Vec3 { x: 1.0, y: 64.0, z: -2.0 };
// Instanciation positionnelle
let e = Entity(101, p, 20 as i32, 1 as i32);
```

- **Règle d'écriture sur sous-structure** : la lecture chaînée `e.pos.x` fonctionne, mais **l'écriture chaînée `e.pos.x = 2.0;` est rejetée par le parser** (`try_parse_ident_stmt` ne supporte que `ident.champ = ...;` et `arr[i].champ = ...;`, y compris avec les affectations composées `+=`, `-=`, etc.). Extraire d'abord le sous-pointeur :

```vortex
let pos_ref: Vec3 = e.pos;
pos_ref.x = 2.0;    // Modifie directement la Vec3 pointée en arène
```

- **Règle d'accès à un champ slice `[T]` d'une `struct`** : copier un champ slice dans un `let` local (`let a: [i32] = chunk.blocks; a[0]`) perd le suivi de slice et déclenche `[VORTEX SAFETY ERROR] Indexing untracked raw pointer requires an 'unsafe' block or 'unsafe fn'`. **Solution idiomatique sans `unsafe`** : passer `chunk.blocks` directement en argument à une fonction dont le paramètre est typé `[i32]` (voir Sections 3 et 8).
- **Méthodes `impl` / `self` / `this`** : **NON SUPPORTÉ**. Utiliser des fonctions libres prenant la structure en premier argument : `fn entity_move(e: Entity, dx: f32) -> i64`.

#### Énumérations (`enum`) et unions (`union`)

- **`enum` entier C-like** (`parse_enum_stmt` dans `src/parser_stmt.c`) : **SUPPORTÉ**.
  - Chaque variante reçoit une constante `i64` auto-incrémentée à partir de `0`, avec valeur initiale ou explicite optionnelle (entiers positifs ou négatifs autorisés : `= -1`, `= 10`).
  - Accès aux variantes via `NomEnum::Variante` **ou** `NomEnum.Variante` (évalué à la compilation comme constante `i64`).
  - Limites (`src/vortex.h`) : `MAX_ENUM_TYPES = 256` énumérations par programme, `MAX_ENUM_VARIANTS = 128` variantes par `enum`.
```vortex
enum NetState {
    Disconnected,       // 0
    Connecting = 10,    // 10
    Connected,          // 11
    Failed = -1         // -1
}

let st = NetState::Connected;
if st == NetState.Connected {
    print(st);          // 11
}
```
- **`enum` à somme algébrique (avec données associées) et `union`** : **NON SUPPORTÉ**. Remplacer par une `struct` avec champ `kind: i64` (exemple : `JsonNode` dans `std/json.vx`).

#### Tableaux fixes, littéraux de tableau, slices, vecteurs et maps

- **Tableaux fixes `[T; N]` et littéraux `[1, 2, 3]`** : **NON SUPPORTÉ**.
- **Slices contigus d'arène (`[i64]`, `[i32]`, `[f32]`, `[Struct]`)** :
  - Allocation initialisée à zéro en arène :
    - `let a = slice_i64(n);` (éléments 8 octets)
    - `let b = slice_i32(n);` (éléments 4 octets)
    - `let c = slice_f32(n);` (éléments 4 octets)
  - Longueur maximale : `1000000000`.
  - Nombre d'éléments : `slice_len(s)` (retourne `i64`).
  - Lecture / écriture : `let v = s[i];` et `s[i] = v;`.
  - **Indexation chaînée `mat[i][j]` ou `obj.arr[i]`** : **NON SUPPORTÉ**.
- **Vecteurs dynamiques (`VortexVec`, type `ptr`/`i64`)** :
  - `let v = vec_new();` (capacité initiale `8`, doublement automatique dans l'arène ; stocke des valeurs 64 bits : `i64`, `str`, `ptr`, `Struct`).
  - `vec_push(v, val);`
  - `let item = vec_get(v, idx);`
  - `vec_set(v, idx, val);`
  - `let n = vec_len(v);`
- **Maps (`VortexMap`, table de hachage FNV-1a à adressage ouvert, facteur de charge `0.7`)** :
  - `let m = map_new();` (clés de type `str` copiées dans l'arène, valeurs 64 bits `i64` / `ptr` / `Struct`).
  - `map_set(m, "clé", valeur);`
  - `let v = map_get(m, "clé");` *(retourne `0` si absente — vérifier d'abord avec `map_has` si `0` est une valeur valide)*
  - `let exists = map_has(m, "clé");` *(retourne `1` ou `0`)*
  - `let count = map_len(m);`

### 2.5 Fonctions, passage de paramètres et closures

```vortex
fn clamp_i64(val: i64, min_v: i64, max_v: i64) -> i64 {
    if val < min_v {
        return min_v;
    }
    if val > max_v {
        return max_v;
    }
    return val;
}

unsafe fn poke32(addr: ptr, val: i64) -> i64 {
    ptr_write32(addr, val);
    return 0;
}
```

- **Limites** : maximum **`16` paramètres** par fonction (`MAX_CALL_ARGS = 16`), maximum **`8192` fonctions utilisateur** (`MAX_USER_FUNCS = 8192`).
- **Règle de `return`** :
  - L'instruction `return expr;` exige **toujours** une expression. Écrire `return;` nu déclenche `[VORTEX SYNTAX ERROR]`, même dans une fonction `-> void`.
  - Si vous déclarez `fn foo() -> void { ... }`, omettez simplement `return;` à la fin du corps ou retournez `return 0;`. Si vous déclarez explicitement un type de retour non-void (`fn foo() -> i64`), `--ultrasafe 2`/`3` vérifie statiquement (`E204`) qu'un `return` est bien présent. Par cohérence, privilégiez `-> i64 { ... return 0; }` dès qu'un retour anticipé est nécessaire.
- **Mode de passage des arguments** :
  - **Scalaires (`i64`, `i32`, `f32`, `ptr`, `str`)** : passés **par valeur**. Modifier un paramètre scalaire dans la fonction ne modifie pas la variable de l'appelant.
  - **Structures (`NomStruct`), slices (`[T]`), `VortexVec`, `VortexMap`** : passés **par référence implicite (copie du pointeur 64 bits vers l'arène, zéro copie de données)**. Les modifications sur les champs (`s.x = ...;`) ou éléments (`buf[i] = ...;`) sont visibles par l'appelant.
- **Closures, lambdas (`|x| ...`), fonctions imbriquées, pointeurs de fonction Vortex** : **NON SUPPORTÉ**.

### 2.6 Contrôle de flux

#### Conditions (`if` / `else` / `else if`)

Les accolades `{ ... }` sont toujours obligatoires. Toute valeur entière non nulle (`!= 0`) est évaluée comme vraie.

```vortex
if err < 0 {
    print_str("Erreur critique\n");
} else if err == 0 {
    print_str("Succes\n");
} else {
    print(err);
}
```

#### Boucle `for` séquentielle

Intervalle semi-ouvert `[debut, fin)` par pas de `+1`, variable d'itération de type `i64` locale au corps :

```vortex
for i in 0..4096 {
    blocks[i] = 0 as i32;
}
```

- Intervalle inclusif `0..=10`, itération inverse, pas `step`, boucle for-each `for item in collection` : **NON SUPPORTÉ**. Pour itérer à l'envers ou par pas différent de 1, utiliser `while`.

#### Boucle parallèle `parallel_for`

Répartit l'intervalle `[debut, fin)` sur le pool de threads natifs (`num_workers()`, maximum `VORTEX_MAX_THREADS = 64`, le thread principal exécute la dernière tranche) :

```vortex
parallel_for idx in 0..4096 {
    data[idx] = (idx as f32) * 0.5;
}
```

- **Règles critiques de `parallel_for`** :
  1. Le compilateur capture les variables locales utilisées dans un contexte lecture seule (`ctx`) passé aux threads : **les modifications d'une variable scalaire (`let mut sum = 0;`) à l'intérieur d'un `parallel_for` ne sont PAS propagées à l'appelant.**
  2. Chaque worker dispose de sa propre arène locale (2 Mio), mais écrire sur des indices **disjoints** d'un slice ou d'une structure allouée avant le `parallel_for` permet de retourner des résultats sans course critique (*data race*). Ne jamais pousser dans un même `VortexVec` ou modifier un même compteur sans partition par index.

#### Boucle `while`, `break` et `continue`

- `break;` et `continue;` sont supportés dans les boucles `for` et `while` (profondeur d'imbrication maximale : `MAX_LOOP_DEPTH = 64`). Utiliser `break`/`continue` hors boucle déclenche une erreur à la compilation.

```vortex
let mut i = 0;
let mut sum = 0;
while true {
    i += 1;
    if i > 10 {
        break;
    }
    if (i % 2) == 0 {
        continue;
    }
    sum += i;
}
```

#### Instructions absentes

- `match` / `switch` / `case` / `default` : **NON SUPPORTÉ**. Utiliser des chaînes `if ... { ... } else if ... { ... }`.
- Opérateur ternaire `cond ? a : b` : **NON SUPPORTÉ**.
- `loop { ... }`, `do { ... } while (...)` : **NON SUPPORTÉ**.

### 2.7 Opérateurs et précédence exacte (`src/parser_expr.c`, `src/parser_stmt.c`)

Table de précédence de la priorité la plus haute (`1`) à la plus basse (`12`) vérifiée dans `src/parser_expr.c` :

| Niveau | Catégorie | Opérateurs exacts | Associativité / Remarques |
| :--- | :--- | :--- | :--- |
| `1` | Primaire / Postfixe | `()`, `fn(...)`, `Struct { ... }`, `Enum::Var`/`Enum.Var`, `arr[i]` (1 niveau), `s.champ` (chaîné), `true`/`false` | Gauche à droite |
| `2` | Unaire | `-x`, `!x`, `~x`, `*ptr` | Négation (`i64`/`f32`), NON logique (`!x` -> `i64` `0`/`1`), NON bit-à-bit (`~x`), déréf `*ptr` en `unsafe` |
| `3` | Conversion de type | `as` | `expr as Type` (prioritaire sur `*` : écrire `(a * b) as f32`) |
| `4` | Multiplicatif | `*`, `/`, `%` | Gauche à droite ; `/` entier tronque vers zéro ; `%` sur `i64` (piégé si div par 0 en `--ultrasafe`) |
| `5` | Additif | `+`, `-` | Gauche à droite |
| `6` | Décalages binaires | `<<`, `>>` | Gauche à droite sur `i64` |
| `7` | Comparaison | `==`, `!=`, `<`, `<=`, `>`, `>=` | Retourne `i64` (`1` ou `0`) ; **ATTENTION : plus prioritaire que `&`, `^`, `\|` !** |
| `8` | ET bit-à-bit | `&` | Gauche à droite sur `i64` |
| `9` | XOR bit-à-bit | `^` | Gauche à droite sur `i64` |
| `10` | OU bit-à-bit | `\|` | Gauche à droite sur `i64` |
| `11` | ET logique | `&&` | Court-circuit (*short-circuit*), retourne `i64` (`1` ou `0`) |
| `12` | OU logique | `\|\|` | Court-circuit (*short-circuit*), retourne `i64` (`1` ou `0`) |

> **PIÈGE DE PRÉCÉDENCE IMPORTANT** : les opérateurs de comparaison (`==`, `!=`, `<`, `>`, etc.) sont au niveau `7`, donc **plus prioritaires** que `&` (`8`), `^` (`9`) et `|` (`10`). Toujours parenthéser les masques binaires : écrire `if (flags & 4) != 0`, et **jamais** `if flags & 4 != 0` (qui s'analyse comme `flags & (4 != 0)`).

#### Affectations composées supportées (`src/parser_stmt.c`)

- Disponibles sur les variables `mut`, les champs `s.champ` et les slices `arr[i]` :  
  `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`.
- En mode `--ultrasafe 1/2/3`, `/=` et `%=` sur entiers hors bloc `unsafe` émettent également la garde runtime division par zéro `IR_DIV_CHECK`.
- **Incrémentation/décrémentation `++` et `--`** : **NON SUPPORTÉ**. Écrire `i += 1;` ou `i = i + 1;`.

---

## 3. GESTION MÉMOIRE & PERFORMANCE (MOTEUR 3D & SERVEUR)

### 3.1 Modèle d'arènes paginées (`src/arena.c`)

- **Zéro Garbage Collector, zéro `malloc`/`free` utilisateur par objet, zéro destructeur RAII, zéro borrow checker syntaxique**.
- Toute allocation dynamique du langage (`struct`, `slice_*`, `vec_new`, `map_new`, `str_concat`, `str_from_*`, `alloc`) avance un curseur linéaire (*bump pointer*) dans une **arène paginée** composée de pages natives de **2 Mio** (`VORTEX_PAGE_SIZE = 2 * 1024 * 1024` octets alloués via `VirtualAlloc` sous Windows / `mmap` sous POSIX).
- Un programme possède une arène principale et chaque worker de `parallel_for` possède une arène locale dédiée (2 Mio initialement).
- Il n'existe **aucune fonction `free(ptr)`** individuelle : la mémoire est récupérée en bloc O(1) grâce aux bornes de portée (`scope_push` / `scope_pop`).

### 3.2 Discipline `scope_push()` / `scope_pop(mark)` — Anti-fuites et anti-dangling

- `let mark = scope_push();` sauvegarde l'état courant de l'arène dans un jeton `i64` compacté (`depth` sur 12 bits | `page_idx` sur 20 bits | `cursor` sur 32 bits). Profondeur d'imbrication maximale : `VORTEX_MAX_SCOPE_DEPTH = 256`.
- `scope_pop(mark);` ramène instantanément le curseur d'arène au marqueur `mark` et incrémente le compteur d'époque (`g_arena.epoch++`).
- Les nouveaux slices réutilisant une région précédemment libérée sont remis à zéro jusqu'au `high_water` de la page.

#### Vérificateur statique de régions (`ir_region_verify` dans `src/ir_core.c`)

Actif à la compilation en mode **`--safe` ET `--release`**. Il rejette avant même l'exécution les 3 classes de bugs mémoire suivants (`[VORTEX REGION ERROR]`) :
1. **Retour d'une référence locale libérée** : vous ne pouvez pas faire `let m = scope_push(); let s = slice_i32(16); scope_pop(m); return s;` ni retourner une allocation créée après un `scope_push()` ouvert dans la fonction.  
   **Règle d'or pour les constructeurs** : une fonction qui alloue et retourne une `struct` ou un `slice` ne doit **pas** encadrer cette allocation retournée entre `scope_push()` et `scope_pop()` ; elle alloue directement dans la région de l'appelant.
2. **Fuite de référence éphémère vers une variable ou un conteneur parent** : il est interdit de stocker un objet/slice/chaîne créé dans un scope interne dans une variable, une `struct` ou un `slice`/`vec`/`map` créé dans un scope englobant.
3. **Use-after-pop** : toute utilisation d'une variable ou d'un pointeur après le `scope_pop(mark)` de son scope d'allocation est rejetée statiquement (et piégée au runtime par comparaison d'`epoch` en mode `--safe`).

#### Architecture mémoire recommandée pour un moteur 3D ou serveur persistant

```text
[Démarrage]  Allocations persistantes (tableau de chunks, entités, socket d'écoute)
     |
     v
[Boucle principale : while running == 1]
     |--> let frame_mark = scope_push();
     |        // Allocations éphémères du tick uniquement :
     |        // chaînes de logs, paquets JSON temporaires, buffers scratch de meshing
     |--> scope_pop(frame_mark);   // Remise à zéro O(1), 0 octet fui sur 24h/24
```

### 3.3 Modes d'exécution (`--safe`, `--ultrasafe 1/2/3`, `--release`) et optimisations

Tous les niveaux `--ultrasafe` sont **cumulatifs** (`1 ⊂ 2 ⊂ 3`) et s'exécutent **au-dessus de `--safe`**.

| Aspect | `--safe` (défaut) | `--ultrasafe 1` | `--ultrasafe 2` | `--ultrasafe 3` | `--release` |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Vérificateur statique `ir_region_verify` | Actif | Actif | Actif | Actif (**collecte toutes les erreurs `E301`**) | Actif |
| En-tête de garde `64` octets (`VortexSafeHeader`, magie, `len`, `epoch`) | Sur `struct`, `slice_*`, `alloc` | Comme `--safe` | Comme `--safe` | Comme `--safe` | Omis sur `struct` (`8` o) ; **conservé sur `slice_*`** |
| `IR_PTR_CHECK` et `IR_FFI_CHECK` | Vérifiés à chaque accès | Comme `--safe` | Comme `--safe` | Comme `--safe` | Supprimés (`IR_NOP`) après dédup |
| `IR_BOUNDS_CHECK` sur `arr[i]` | Vérifié à chaque indexation (`0 <= i < len`) | Comme `--safe` | Comme `--safe` | Comme `--safe` | Éliminé en boucle `for i in 0..N` (RCE), sinon `IR_RANGE_CHECK` |
| `IR_DIV_CHECK` (`/` `%` `/=` `%=` entiers hors `unsafe`) | Non | **Actif** (`[VORTEX SAFETY PANIC]`) | **Actif** | **Actif** | Non |
| Garde profondeur de récursion/appels (max `2048`, fonctions non-`unsafe` avec `IR_CALL_USER`) | Non | Non | **Actif** (0 surcoût sur feuilles) | **Actif** | Non |
| Audits statiques (avant JIT) | — | Warnings `W101`–`W104` | Niveau 1 + erreurs `E201`–`E204` + `W105` | Niveau 2 + `E001`, `E101`, `E301`, `E302` | — |
| Récupération multi-erreurs (`setjmp` + `vortex_lexer_sync`) | Stop 1re erreur | Stop 1re erreur | Stop 1re erreur | **Continue** (max `256` diags) | Stop 1re erreur |
| Optimisations IR | Constant folding, copy prop, LICM const, DCE | Comme `--safe` | Comme `--safe` | Comme `--safe` | + Inlining (`<= 40`), déroulage, RCE, AVX2/NEON |

Détail exact des codes diagnostiques `--ultrasafe` ([src/runtime.c](src/runtime.c), [src/parser_stmt.c](src/parser_stmt.c), [src/ir_core.c](src/ir_core.c)) :
- **Warnings (non bloquants)** : `W101` code inaccessible après `return`/`break`/`continue`, `W102` `mut` jamais réassigné, `W103` shadowing de variable (ignoré si nom commence par `_`), `W104` variable locale inutilisée (ignoré si `_`), `W105` fonction définie mais jamais appelée (hors `main` et `test_*`).
- **Erreurs statiques `--ultrasafe 2/3` (bloquent le JIT)** : `E201` division/modulo entier par constante `0`, `E202` taille de slice `< 0` ou indice constant hors bornes, `E203` déréf/champ sur pointeur nul constant `0`, `E204` `return` manquant dans une fonction déclarant explicitement `-> T` non-void.
- **Collecte complète `--ultrasafe 3`** : `E001` erreurs de syntaxe/typage/réaffectation immuable récupérées via `vortex_lexer_sync`, `E101` appels vers fonctions inconnues, `E301` toutes violations de durée de vie/échappement d'arène dans `ir_region_verify`, `E302` appels `unsafe` hors bloc `unsafe`.
- Syntaxes CLI équivalentes : `--ultrasafe 1|2|3`, `--ultrasafe=1|2|3`, `--ultrasafe` (vaut `3` par défaut). Combiner avec `--check` pour valider sans exécuter le JIT, et `--json` / `--error-format=json` pour une sortie structurée.

### 3.4 Éviter les copies et garder les accès rapides sur chunks 4096 voxels

1. **Toujours utiliser un slice 1D plat** (`slice_i32(4096)` = 16 Kio, tient dans le cache L1) plutôt qu'un tableau de tableaux ou des structs par voxel.
2. **Ne jamais réallouer un slice dans la boucle chaude** : muter le slice existant en place.
3. **Passer le slice directement en argument de fonction typé `[i32]`** (passage du pointeur 64 bits sans aucune copie).
4. **RCE et vectorisation en `--release`** : écrire les boucles de scan sous la forme canonique `for i in 0..4096` avec une variable de limite constante ou `let n = slice_len(blocks); for i in 0..n` sans modifier la variable `blocks` dans la boucle, afin que la passe RCE élimine 100 % des vérifications de bornes.
5. **Ne pas copier un champ `[T]` d'une `struct` dans un `let` local avant d'indexer en code sûr** :
   - Éviter : `let tmp: [i32] = chunk.blocks; tmp[i] = ...;` (perd le marqueur interne `is_slice_ptr` -> erreur `[VORTEX SAFETY ERROR]`).
   - Préférer : `fn blocks_set(b: [i32], i: i64, v: i32) -> i64 { b[i] = v; return 0; }` appelée via `blocks_set(chunk.blocks, i, v);` (le paramètre `b: [i32]` réactive `is_slice_ptr = true`, est sûr en `--safe`, et est inliné en `--release` s'il fait 40 instructions IR ou moins).

### 3.5 Alignement mémoire et disposition des champs (`struct layout`)

Dans `parse_struct_stmt` (`src/parser_stmt.c`), le compilateur applique une disposition C séquentielle **sans réordonnancement automatique des champs** :

- L'alignement de départ d'une `struct` est initialisé à **4 octets** (`max_align = 4`), et passe à **8 octets** dès qu'un champ de 8 octets (`i64`, `ptr`, `str`, `SubStruct`, `[T]`) est déclaré.
- Tailles et alignements exacts :
  - `i32` : taille `4` octets, alignement `4` octets.
  - `f32` : taille `4` octets, alignement `4` octets.
  - `i64`, `ptr`, `str`, `SubStruct`, `[T]` : taille `8` octets, alignement `8` octets.
- La taille totale de la `struct` est arrondie au multiple supérieur de `max_align`.
- **Règle d'or anti-padding** : regrouper tous les champs 8 octets en tête (ou tous les `i32`/`f32` par paires) pour éviter des trous d'alignement inutiles :

```vortex
// MAUVAIS LAYOUT : 4 + (4 pad) + 8 + 4 + (4 pad) = 24 octets
struct BadPacket {
    kind: i32,
    timestamp: i64,
    seq: i32
}

// BON LAYOUT : 8 + 4 + 4 = 16 octets (zéro padding)
struct GoodPacket {
    timestamp: i64,
    kind: i32,
    seq: i32
}
```

- **Inspection à l'exécution** : `struct_size("NomStruct")` et `struct_align("NomStruct")` retournent la taille et l'alignement exacts en octets (essentiel avant d'allouer des tampons bruts `alloc(...)` pour Vulkan/OpenGL/WinSock).
- L'allocateur brut `alloc(bytes)` garantit un alignement sur **64 octets** (`VORTEX_ALIGNMENT = 64`, aligné ligne de cache / AVX-512).

---

## 4. GESTION DES ERREURS & DÉBOGAGE

### 4.1 Mécanisme officiel de gestion des erreurs

- `Result<T, E>`, `Option<T>`, opérateur `?`, exceptions `try`/`catch`/`throw`, `panic!()` : **NON SUPPORTÉS**.
- **Modèles officiels utilisés dans le runtime et `std/*.vx`** :
  1. **Code de retour entier `i64`** : `0` (ou `>= 0` pour un handle/socket/longueur) signifie succès ; `< 0` (ex. `-1`, `-2`) signifie erreur.
  2. **Pointeur/handle nul (`0`)** : `ptr` ou identifiant égal à `0` pour signaler un échec d'ouverture/résolution.
  3. **Structure résultat dédiée** : inclure un champ `valid: i64` ou `ok: i64` + `err_code: i64` (exemple vérifié dans `std/http.vx` avec `HttpRequest.valid` et `HttpResponse.valid`, et `std/db.vx` avec `DbConn.last_err`).
- **Arrêt volontaire en cas d'erreur fatale** : `exit(code);` termine immédiatement le processus avec le code entier `code`.

### 4.2 Assertions, tests et débogage natif

- **Assertions natives (`src/runtime.c`)** :
  - `assert(cond: i64) -> i64` : échoue immédiatement si `cond == 0` (intégrée au runner `vortex test`).
  - `assert_eq(a: i64, b: i64) -> i64` : échoue immédiatement si `a != b` en affichant les deux valeurs.
- **Validation statique et diagnostics JSON** :
  - `--check` : exécute le préprocesseur, le parser, `ir_region_verify` et les audits `--ultrasafe` sans générer ni lancer le JIT.
  - `--json` / `--error-format=json` : émet un objet JSON structuré (`version`, `ultrasafe`, `ok`, `errors`, `warnings`, `diagnostics[]` avec `severity`, `code`, `file`, `line`, `col`, `message`, `suggestion`).

```vortex
fn test_math_basics() -> i64 {
    assert(1 == 1 && true);
    assert_eq(21 * 2, 42);
    return 0;
}
```

- **Affichage console** :
  - `print(x)` : accepte un `i64` (`%lld\n`) ou un `f32` (`%.6f\n`) et ajoute automatiquement un saut de ligne. **NE JAMAIS passer une `str` à `print()`** (utiliser `print_str`).
  - `print_str(s)` : affiche la chaîne brute **sans saut de ligne** (toujours inclure `"\n"` ou appeler `print_str("\n")` séparément).
- **Traps d'exécution et stacktraces** :
  - En cas de violation mémoire en `--safe`/`--ultrasafe` (ou `IR_RANGE_CHECK` / exception matérielle `SIGSEGV`), le runtime affiche un préfixe explicite (`[VORTEX BOUNDS ERROR]`, `[VORTEX MEMORY ERROR]`, `[VORTEX SAFETY PANIC]`, `[VORTEX CRASH]` + `[VORTEX STACKTRACE]`) puis quitte.
- **Débogage machine** :
  - `--dump-jit <fichier>` : écrit les octets bruts du code machine généré.
  - `--dump-debug <fichier>` : génère un fichier ELF contenant les symboles de débogage **DWARF v4** (`MAX_DEBUG_LINES = 8192`, `MAX_DEBUG_FUNCS = 512`), compatible avec GDB/LLDB (qui s'accrochent aussi au descripteur JIT natif en mémoire via `debug_info.c`).

---

## 5. FFI (INTEROPÉRABILITÉ C / SYSTÈME) & BIBLIOTHÈQUE STANDARD

### 5.1 FFI Statique (`extern`) et blocs `unsafe`

#### Déclaration au niveau racine

```vortex
#[cfg(windows)]
extern "msvcrt" fn strlen(s: str) -> i64;
#[cfg(windows)]
extern "kernel32.dll" fn Sleep(ms: i64) -> void;

#[cfg(unix)]
extern "libc" fn strlen(s: str) -> i64;
#[cfg(unix)]
extern "libc" fn usleep(usec: i64) -> i64;
```

- Syntaxe des fonctions C variadiques : `..` ou `...` en dernier paramètre, exemple : `extern "msvcrt" fn printf(fmt: str, ...) -> i64;`.
- **Résolution automatique des bibliothèques** (`src/ffi.c`) :
  - Windows : `msvcrt` -> `msvcrt.dll`, ou ajout automatique de `.dll` si absent ; fallback automatique de `snprintf` vers `_snprintf`.
  - Linux : `libc` -> `libc.so.6`, `libm` -> `libm.so.6`, sinon `lib<nom>.so`.
  - macOS : `libc` / `libSystem` -> `libSystem.B.dylib`, sinon `lib<nom>.dylib`.
- **Limites FFI** (`src/vortex.h`) : maximum `256` bibliothèques dynamiques chargées (`MAX_DYN_LIBS = 256`), `2048` symboles externes (`MAX_FFI_FUNCS = 2048`), `16` arguments par appel (`MAX_CALL_ARGS = 16`).

#### Obligation `unsafe` et vérifications FFI (`--safe`)

Tout appel à une fonction `extern fn`, à `ffi_load` / `ffi_symbol` / `ffi_call0..6`, aux lectures/écritures pointeur `ptr_read*` / `ptr_write*`, à `alloc(bytes)`, ou tout cast impliquant des pointeurs doit se trouver :
- dans un bloc `unsafe { ... }`, ou
- dans une fonction déclarée `unsafe fn ...`.

En mode `--safe`, les arguments `str`, `ptr`, `*T` passés à une `extern fn` sont vérifiés non nuls (`IR_FFI_CHECK`), sauf si le paramètre est annoté `str?` ou `ptr?`.

### 5.2 FFI Dynamique et accès mémoire bas niveau (`unsafe`)

Pour charger dynamiquement OpenGL (`opengl32.dll` / `libGL.so`), Vulkan (`vulkan-1.dll` / `libvulkan.so.1`), SDL2, GLFW ou les sockets OS :

- `ffi_load(lib_name: str) -> ptr` (ou utilisé en `i64` : retourne `0` en cas d'échec)
- `ffi_symbol(lib_handle: ptr, sym_name: str) -> ptr` (retourne `0` si introuvable)
- `ffi_call0(fn_ptr)` jusqu'à `ffi_call6(fn_ptr, a1, a2, a3, a4, a5, a6)` -> `i64` *(appelle une adresse de fonction C avec 0 à 6 arguments 64 bits ; piège immédiatement si `fn_ptr == 0`)*
- Tampon C aligné 64 octets : `alloc(bytes: i64) -> ptr`
- Lecture/écriture mémoire brute (`ptr` ou adresse `i64`) :
  - `ptr_read8(p)` / `ptr_write8(p, byte_val)`
  - `ptr_read32(p)` / `ptr_write32(p, i32_val)`
  - `ptr_read64(p)` / `ptr_write64(p, i64_val)`
  - `ptr_to_str(p) -> str` *(interprète une adresse mémoire C terminée par `\0` comme `str`)*

### 5.3 Catalogue complet des 72 fonctions intégrées (builtins, `src/runtime.c`)

Toutes ces fonctions sont disponibles sans `import` :

1. **Vecteurs dynamiques (`5`)** :
   - `vec_new() -> ptr`
   - `vec_push(v, val)`
   - `vec_get(v, idx) -> i64`
   - `vec_set(v, idx, val)`
   - `vec_len(v) -> i64`
2. **Tables de hachage (`5` — total collections `10`)** :
   - `map_new() -> ptr`
   - `map_set(m, key: str, val)`
   - `map_get(m, key: str) -> i64`
   - `map_has(m, key: str) -> i64`
   - `map_len(m) -> i64`
3. **Chaînes de caractères (`13`)** :
   - `str_concat(s1: str, s2: str) -> str`
   - `str_join(vec_of_str: ptr, sep: str) -> str`
   - `str_len(s: str) -> i64`
   - `str_sub(s: str, start: i64, len: i64) -> str`
   - `str_from_i64(n: i64) -> str`
   - `str_from_f32(f: f32) -> str` *(formate avec 4 décimales)*
   - `str_byte(s: str, idx: i64) -> i64` *(retourne l'octet ASCII `0..255` à l'indice `idx`)*
   - `str_from_byte(ch: i64) -> str`
   - `str_eq(s1: str, s2: str) -> i64` *(`1` si égales, `0` sinon)*
   - `str_find(haystack: str, needle: str, start: i64) -> i64` *(`-1` si non trouvé)*
   - `str_find_any2(s: str, ch1: i64, ch2: i64, start: i64) -> i64` *(cherche l'un des 2 octets ASCII dès `start`)*
   - `str_to_i64(s: str) -> i64`
   - `ptr_to_str(p: ptr) -> str`
4. **Arènes et slices (`7`)** :
   - `scope_push() -> i64`
   - `scope_pop(mark: i64)`
   - `slice_i64(len: i64) -> [i64]`
   - `slice_f32(len: i64) -> [f32]`
   - `slice_i32(len: i64) -> [i32]`
   - `slice_len(s) -> i64`
   - `alloc(bytes: i64) -> ptr` *(requiert `unsafe`)*
5. **Pointeurs bas niveau (`6`, requièrent `unsafe`)** :
   - `ptr_read8(p)`, `ptr_write8(p, val)`
   - `ptr_read32(p)`, `ptr_write32(p, val)`
   - `ptr_read64(p)`, `ptr_write64(p, val)`
6. **FFI dynamique (`9`, requièrent `unsafe`)** :
   - `ffi_load(lib: str)`, `ffi_symbol(handle, sym: str)`
   - `ffi_call0(fn_ptr)` à `ffi_call6(fn_ptr, a1, a2, a3, a4, a5, a6)`
7. **Interface graphique 2D intégrée Win32 GDI / X11 (`12`)** :
   - `gui_window(title: str, w: i64, h: i64) -> ptr` *(désactivé si `--headless` ou `VORTEX_HEADLESS=1`)*
   - `gui_is_open(win) -> i64`
   - `gui_rgb(r: i64, g: i64, b: i64) -> i64`
   - `gui_clear(win, rgb: i64)`
   - `gui_draw_rect(win, x, y, w, h, rgb)`
   - `gui_draw_text(win, txt: str, x, y, rgb)`
   - `gui_draw_number(win, num: i64, x, y, rgb)`
   - `gui_button(win, id, lbl: str, x, y, wb, hb) -> i64`
   - `gui_present(win)`
   - `gui_set_pos(win, x, y)`
   - `gui_draw_line(win, x0, y0, x1, y1, rgb)`
   - `gui_draw_pixel(win, x, y, rgb)`
8. **Maths, réflexion de struct, assertions, horloge, threads et système (`15`)** :
   - `math_sin(deg_x10: i64) -> i64` : **ATTENTION**, l'angle est en **dixièmes de degré** (`900` = `90.0°`) et la valeur retournée est en **virgule fixe Q12** (`sin * 4096`, soit `4096` pour `1.0`).
   - `math_cos(deg_x10: i64) -> i64` : même convention (`cos(0°) -> math_cos(0) == 4096`).
   - `math_sqrt(x: i64) -> i64` : racine carrée entière sur `i64` (`math_sqrt(16) == 4`).
   - `sinf_rad(rad: f32) -> f32` : sinus flottant 32 bits en **radians**.
   - `cosf_rad(rad: f32) -> f32` : cosinus flottant 32 bits en **radians**.
   - `sqrtf_val(x: f32) -> f32` : racine carrée flottante 32 bits.
   - `struct_size(name: str) -> i64`
   - `struct_align(name: str) -> i64`
   - `assert(cond: i64) -> i64` : échoue si `cond == 0` (utilisé par `vortex test`).
   - `assert_eq(a: i64, b: i64) -> i64` : échoue si `a != b`.
   - `time_us() -> i64` : horloge monotone haute résolution en **microsecondes**.
   - `num_workers() -> i64` : nombre de threads du pool `parallel_for`.
   - `print(val)` : affiche `i64` ou `f32` avec saut de ligne.
   - `print_str(s: str)` : affiche `str` sans saut de ligne.
   - `exit(code: i64)` : quitte immédiatement le processus.

### 5.4 Bibliothèque standard (`std/` — 6 modules disponibles)

Il n'existe actuellement **QUE 6 modules** dans `std/` : `net`, `http`, `json`, `crypto`, `db` et `gpu`.  
Modules absents (**NON SUPPORTÉS** dans `std/`) : il n'existe pas de `std/fs` (accès fichiers), `std/math`, `std/str`, `std/collections`, `std/sync`, `std/thread`, ni `std/time`. Pour la trigonométrie flottante de base (`sinf_rad`, `cosf_rad`, `sqrtf_val`), utiliser les builtins intégrés ; pour l'E/S fichiers, d'autres fonctions `libm` (`atan2f`, `powf`) ou les threads/mutex OS, passer par `extern` C (`fopen`/`fread`/`fwrite`/`fclose`, etc.).

#### 1. `std/net` (`import "std/net";`) — Sockets TCP non bloquants multi-OS (`ws2_32.dll` / `libc`)
- `net_init() -> i64` *(initialise Winsock sous Windows ; retourne `0` ou `< 0`)*
- `net_socket_create() -> i64`
- `net_close(sock: i64) -> i64`
- `net_set_nonblocking(sock: i64, nonblocking: i64) -> i64`
- `net_tcp_listen(port: i64, nonblocking: i64) -> i64` *(retourne le descripteur d'écoute ou `< 0`)*
- `net_bound_port(sock: i64) -> i64` *(utile quand `port == 0`)*
- `net_poll_event(sock: i64, want_write: i64, timeout_ms: i64) -> i64`
- `net_poll_read(sock: i64, timeout_ms: i64) -> i64`
- `net_poll_write(sock: i64, timeout_ms: i64) -> i64`
- `net_tcp_accept(listen_sock: i64, nonblocking: i64) -> i64`
- `net_tcp_connect(host: str, port: i64, nonblocking: i64) -> i64`
- `net_send(sock: i64, data: str) -> i64`
- `net_recv(sock: i64, max_bytes: i64) -> str`
- `net_recv_wait(sock: i64, max_bytes: i64, timeout_ms: i64) -> str`
- Utilitaires internes exposés : `net_i32_sext`, `net_parse_ipv4_octets`, `net_ws_lib`, `net_libc`, `net_fill_sockaddr`, `net_sock_lib`, `net_sol_socket`, `net_so_reuseaddr`. *(Note : UDP n'est pas encapsulé dans `std/net.vx` ; pour UDP haute fréquence de jeu multijoueur, déclarer `socket`, `sendto`, `recvfrom`, `bind` via `extern` sur `ws2_32.dll` / `libc`.)*

#### 2. `std/http` (`import "std/http";`) — Client et parseur HTTP/1.1 (`std/net` + `std/crypto`)
- Structures :
  - `HttpRequest { method: str, path: str, version: str, headers: str, body: str, valid: i64 }`
  - `HttpResponse { status: i64, reason: str, headers: str, body: str, raw: str, valid: i64 }`
- Fonctions : `http_crlf()`, `http_find_char_from(s, ch, start)`, `http_trim_cr(s)`, `http_format_request(method, host, path, body)`, `http_format_response(status, reason, content_type, body)`, `http_find_header_end(raw)`, `http_header_sep_len(raw, pos)`, `http_parse_request(raw) -> HttpRequest`, `http_parse_response(raw) -> HttpResponse`, `http_exchange_tcp(host, port, wire_req) -> HttpResponse`, `http_get(host, port, path) -> HttpResponse`, `http_post(host, port, path, body) -> HttpResponse`, `http_tls_supported() -> i64`.

#### 3. `std/json` (`import "std/json";`) — Parseur et sérialiseur JSON en arène
- Structures :
  - `JsonNode { kind: i64, bool_val: i64, int_val: i64, str_val: str, arr: i64, keys: i64, obj: i64 }`  
    *(Valeurs de `kind` : `0` = null, `1` = bool, `2` = int, `3` = float_str, `4` = str, `5` = array, `6` = object)*
  - `JsonCursor { src: str, len: i64, pos: i64 }`
- Fonctions : `json_make_null()`, `json_make_bool(b)`, `json_make_int(n)`, `json_make_float_str(raw)`, `json_make_str(s)`, `json_make_array()`, `json_make_object()`, `json_push(arr_node, child)`, `json_set(obj_node, key, child)`, `json_len(node)`, `json_at(arr_node, idx) -> JsonNode`, `json_has(obj_node, key) -> i64`, `json_get(obj_node, key) -> JsonNode`, `json_parse(src: str) -> JsonNode`, `json_stringify(node: JsonNode) -> str`.

#### 4. `std/crypto` (`import "std/crypto";`) — Cryptographie OS et probe TLS (`bcrypt.dll`/`secur32.dll` ou OpenSSL)
- Fonctions : `crypto_sha256_hex(msg: str) -> str`, `crypto_random_hex(byte_count: i64) -> str`, `crypto_tls_available() -> i64`, `crypto_tls_probe_client_hello(server_name: str) -> i64` (plus utilitaires `crypto_hex_nibble`, `crypto_bytes_to_hex`, `crypto_find_openssl_libcrypto`, `crypto_find_openssl_libssl`).

#### 5. `std/db` (`import "std/db";`) — Connecteur SQLite (`sqlite3`/`winsqlite3`) et PostgreSQL (`libpq`)
- Structure : `DbConn { kind: i64, lib: i64, handle: i64, last_err: str, open: i64 }` (`kind` : `0` = none, `1` = sqlite, `2` = postgres).
- Fonctions : `db_sqlite_available() -> i64`, `db_postgres_available() -> i64`, `db_sqlite_version() -> str`, `db_open_sqlite(path: str) -> DbConn`, `db_open_postgres(conninfo: str) -> DbConn`, `db_exec(conn: DbConn, sql: str) -> i64`, `db_query_scalar_str(conn: DbConn, sql: str) -> str`, `db_close(conn: DbConn) -> i64`.

#### 6. `std/gpu` (`import "std/gpu";`) — Détection GPU (Vulkan / DX12 / Metal) & calcul SIMD CPU
- Structure : `GpuInfo { backend: str, driver_loaded: i64, device_count: i64, device_name: str, status: i64 }` (`backend` : `"vulkan"`, `"dx12"`, `"metal"`, ou `"cpu_simd"`).
- Fonctions : `gpu_vulkan_available() -> i64`, `gpu_dx12_available() -> i64`, `gpu_metal_available() -> i64`, `gpu_probe() -> GpuInfo`, `gpu_compute_saxpy(y: [f32], a: f32, x: [f32], n: i64) -> i64` *(implémente `y[i] = a * x[i] + y[i]` via `parallel_for` CPU SIMD)*.

---

## 6. OUTILS & COMPILATION

### 6.1 Commandes CLI exactes (`vortex --help`, `Vortex v1.1.5`)

| Commande | Rôle |
| :--- | :--- |
| `vortex` / `vortex repl` / `vortex -i` | Lance le terminal interactif REPL |
| `vortex [drapeaux] <script.vx>` | Compile en JIT et exécute le fichier `.vx` |
| `vortex run [drapeaux] <script.vx>` | Compile en JIT et exécute le fichier `.vx` |
| `vortex eval [drapeaux] "<code>"` | Compile et exécute du code Vortex inline |
| `vortex build [drapeaux] <in.vx> [-o out]` | Empaquette un exécutable autonome (*standalone*) |
| `vortex test [chemin] [--ultrasafe 1\|2\|3] [--json]` | Exécuteur natif de tests (`assert` / `assert_eq` / fonctions `test_*`) |
| `vortex spec [--json]` | Émet la spécification live extraite directement du compilateur |
| `vortex pkg <sous-commande>` | Gestionnaire de paquets (`vortex.toml` / `vortex.lock`) |
| `vortex lsp` | Démarre le serveur Language Server Protocol (JSON-RPC sur stdio) |
| `vortex --version` / `-v` | Affiche la version et l'architecture |
| `vortex --help` / `-h` | Affiche l'aide CLI |

#### Drapeaux d'exécution, sûreté et diagnostic

- `--safe` *(défaut)* : Active toutes les vérifications de bornes, d'époque d'arène, de pointeur non nul et des frontières FFI (+ constant folding, copy prop, LICM constante, DCE).
- `--ultrasafe <1|2|3>` *(ou `--ultrasafe=1|2|3`, ou `--ultrasafe` = `3`)* : Active les modes de sûreté stricts cumulatifs par-dessus `--safe` (voir tableau §3.3).
- `--check` : Exécute l'analyse frontend, `ir_region_verify` et les audits `--ultrasafe` sans générer ni exécuter le code JIT.
- `--json` / `--error-format=json` : Sortie JSON structurée pour les diagnostics, `vortex test` et `vortex spec`.
- `--release` : Active l'inlining, RCE (élimination des vérifications de bornes en boucle), LICM, auto-vectorisation AVX2/NEON maximale, suppression des `IR_PTR_CHECK`/`IR_FFI_CHECK`, et enlève les en-têtes 64 octets des `struct`.
- `--headless` : Désactive la création de fenêtre GUI Win32/X11 (équivalent à `VORTEX_HEADLESS=1`).
- `--target <triple>` : Cible de compilation (`x86_64-windows`, `x86_64-linux`, `x86_64-darwin`, `arm64-linux`, `arm64-darwin`).
- `--dump-jit <fichier>` : Écrit le code machine JIT brut dans `<fichier>`.
- `--dump-debug <fichier>` : Écrit les symboles de débogage DWARF v4 ELF dans `<fichier>`.
- **Bibliothèque partagée hôte (`vortex.dll` / `libvortex.so` / `libvortex.dylib`)** : expose l'API C d'intégration (`vortex_host_init`, `vortex_host_compile`, `vortex_host_get_fn`, `vortex_host_call0..4`, `vortex_host_scope_push/pop`, `vortex_host_arena_alloc/reset`) pour appels natifs ou JVM FFM/JNI.

#### Gestionnaire de paquets (`vortex pkg`) et REPL

- **Sous-commandes `vortex pkg`** (`src/pkg.c`) :
  - `vortex pkg init` : crée `vortex.toml`.
  - `vortex pkg add <nom> <chemin_ou_url_git> [--rev <rev>]` : ajoute une dépendance locale ou Git.
  - `vortex pkg install` : résout, copie en cache et génère `vortex.lock` avec hachage SHA-256 complet de l'arborescence.
  - `vortex pkg update` : met à jour le verrou `vortex.lock`.
  - `vortex pkg verify` : vérifie l'intégrité SHA-256 des paquets en cache et locaux.
  - `vortex pkg list` : liste les dépendances.
  - `vortex pkg cache-dir` : affiche le répertoire cache.
  - `vortex pkg run` / `vortex pkg build` : exécute ou compile le point d'entrée du projet défini dans `vortex.toml`.
- **Méta-commandes du REPL (`src/main.c`)** : `:help`, `:vars`, `:structs`, `:time <code|expr>`, `:run <fichier.vx>`, `:cls` / `:clear`, `:reset`, `:exit` / `:quit`.

### 6.2 Guide de lecture des erreurs et codes diagnostics Vortex

Chaque erreur affiche la catégorie suivie du message, du fichier et de la position `fichier:ligne:colonne` (ou `fichier:ligne`). Avec `--json`, chaque entrée fournit `{severity, code, file, line, col, message, suggestion}`.

| Préfixe / Code | Phase | Cause typique et correction immédiate |
| :--- | :--- | :--- |
| `[VORTEX IMPORT ERROR]` | Préprocesseur | Fichier introuvable via `import` : vérifier le chemin ou `VORTEX_STD`. |
| `[VORTEX ERROR]` / `E001` / `E101` | Lexer / Parser / Target / JIT | Caractère inconnu (`'c'`, `@`), dépassement des limites (`MAX_FIELDS = 64`, `MAX_SYMBOLS = 1024`), fonction non résolue (`E101`) ou tampon JIT plein (`16 MiB`). |
| `[VORTEX SYNTAX ERROR]` / `E001` | Parser | Jeton inattendu (`return;` sans expression, `,` ou `;` manquant, `for` sans `in`, `=` manquant sur `let`, float mal formé). |
| `[VORTEX SAFETY ERROR]` / `E302` | Analyseur sémantique / Runtime `--safe` | Assignation à un `let` sans `mut`, appel de fonction `extern`/`unsafe fn`/opérations pointeur hors `unsafe` (`E302`), indexation de slice extrait d'un champ `struct` dans un `let`, pointeur nul déréférencé. |
| `[VORTEX SAFETY PANIC]` | Runtime `--ultrasafe 1/2/3` | Division ou modulo entier par zéro (`IR_DIV_CHECK` dès niveau 1), ou profondeur d'appels/récursion > `2048` (dès niveau 2). |
| `E201`–`E204` | Audit statique `--ultrasafe 2/3` | `E201` div/mod par constante `0`, `E202` taille slice `< 0` ou indice constant hors bornes, `E203` déréf/champ sur pointeur nul constant, `E204` `return` manquant dans fonction `-> T` non-void. |
| `W101`–`W105` | Warnings `--ultrasafe 1/2/3` | `W101` code mort après `return`/`break`/`continue`, `W102` `mut` jamais réassigné, `W103` shadowing (hors `_`), `W104` locale inutilisée (hors `_`), `W105` fonction non appelée (niveau 2/3, hors `main`/`test_*`). |
| `[VORTEX REGION ERROR]` / `E301` | Vérificateur statique `ir_region_verify` | Retour d'une référence allouée après `scope_push()`, évasion d'une référence interne vers une variable/struct/slice/vec/map parente, utilisation après `scope_pop()`. |
| `[VORTEX BOUNDS ERROR]` | Runtime `--safe` / `IR_RANGE_CHECK` | Index `i` hors de `[0..len)`, `vec_get`/`vec_set` hors bornes, ou pointeur slice nul. |
| `[VORTEX MEMORY ERROR]` | Runtime arène | Pointeur invalide/non aligné, époque périmée (*use-after-pop* dynamique), ou en-tête `VortexSafeHeader` corrompu. |
| `[VORTEX FFI ERROR]` | Runtime `--safe` / `ffi.c` | Argument pointeur/`str` nul ou mal aligné passé à une `extern fn`, ou appel `ffi_callN` avec pointeur fonction nul. |
| `[VORTEX BUILD ERROR]` / `[VORTEX PKG ERROR]` | `build` / `pkg` | Runtime compagnon absent pour la cible croisée, échec d'écriture binaire, `vortex.toml` absent ou divergence de checksum SHA-256. |
| `[VORTEX PANIC]` / `[VORTEX CRASH]` | Runtime / OS | Échec d'allocation d'une page OS de 2 Mio ou exception matérielle (`SIGSEGV` / `0xC0000005`), suivi de `[VORTEX STACKTRACE]`. |

---

## 7. PIÈGES À ÉVITER (ANTI-HALLUCINATIONS POUR IA)

| Piège courant (Rust / C++ / Java / Python) | **NE PAS FAIRE EN VORTEX** (invalide ou piégeux) | **FAIRE EN VORTEX** (valide et idiomatique) |
| :--- | :--- | :--- |
| Point d'entrée automatique | `fn main() -> i64 { ... }` sans appel racine | Appeler `main();` au niveau racine, ou écrire le flux racine directement |
| Constantes globales lues en fonction | `let N = 4096; fn f() -> i64 { return N; }` | `fn N() -> i64 { return 4096; }` ou `enum` *(les `fn` ont une portée isolée)* |
| Retour sans valeur | `return;` dans une `fn` | `fn f() -> i64 { return 0; }` *(toute instruction `return` exige une expression)* |
| Précédence `&` / `^` / `\|` vs `==` | `if flags & 4 != 0 { ... }` *(évalué `flags & (4 != 0)`)* | `if (flags & 4) != 0 { ... }` *(toujours parenthéser autour des comparaisons)* |
| Incrément/décrément `++`/`--` | `i++; count--;` | `i += 1; count -= 1;` *(affectations composées `+=`..`>>=` supportées, pas `++`/`--`)* |
| Arithmétique mixte `i64` / `f32` | `let t = us / 1000000.0; let x: f32 = 0;` | `let t = (us as f32) / 1000000.0; let x: f32 = 0.0;` *(zéro cast implicite)* |
| Annotation de type `bool` et `match`/`switch` | `let ok: bool = true; match state { 0 => ... }` | `let ok: i64 = true; if state == 0 { ... } else if state == 1 { ... }` |
| Tableaux fixes / littéraux | `let a: [i32; 4] = [1, 2, 3, 4];` | `let a = slice_i32(4); a[0] = 1 as i32; ...` |
| `enum` à payload / `union` | `enum Msg { Move(i64, i64) }` | `enum MsgKind { Move = 1 }` (`i64`) + `struct Msg { kind: i64, ... }` |
| Méthodes `impl` / OOP | `impl Chunk { fn get(&self) ... }` / `chunk.get()` | Fonctions libres : `fn chunk_get(c: Chunk, ...) -> i32` |
| Mutation à 2 niveaux sur `struct` | `world.player.x = 10.0;` | `let pl: Player = world.player; pl.x = 10.0;` |
| Indexer directement un slice extrait d'une `struct` | `let b: [i32] = chunk.blocks; b[i] = 1 as i32;` | Passer `chunk.blocks` à une `fn set_b(b: [i32], i: i64)` (ou bloc `unsafe`) |
| Comparaison / affichage de `str` | `if a == "ping" { print(a); }` | `if str_eq(a, "ping") == 1 { print_str(a); print_str("\n"); }` |
| Littéral chaîne > 4095 octets | Chaîne littérale géante > 4095 octets | Découper et assembler avec `str_concat` / `str_join` |
| Closures et capture mutable en `parallel_for` | `parallel_for i in 0..n { total += 1; }` | Écrire dans des indices disjoints d'un `[i64]` / `[f32]` puis réduire séquentiellement |
| FFI ou pointeurs hors `unsafe` | Appeler `Sleep(10);` ou `alloc(64)` directement | Encapsuler dans `unsafe { Sleep(10); }` ou `unsafe fn ...` |
| Retourner une allocation d'un bloc `scope_push()` | `let m = scope_push(); let s = slice_i32(8); scope_pop(m); return s;` | Ne placer `scope_push`/`scope_pop` qu'autour des temporaires, jamais autour d'une valeur retournée |
| Trigo `math_sin` vs `sinf_rad` | Passer des radians `f32` à `math_sin` | `math_sin(deg_x10)` / `math_cos(deg_x10)` sont en dixièmes de degré `i64` (Q12) ; utiliser `sinf_rad(rad)` / `cosf_rad(rad)` / `sqrtf_val(x)` pour `f32` |
| Visibilité et nombres | `pub fn foo()`, `1_000_000`, `1e-5`, `'a'` | `fn foo()`, `1000000`, `0.00001`, codes ASCII entiers ou `str_byte("a", 0)` |

---

## 8. EXEMPLES DE CODE IDIOMATIQUES COMPLETS

Les trois programmes complets ci-dessous respectent intégralement la grammaire Vortex et ont été conçus pour compiler sans avertissement ni fuite mémoire en modes `--safe` et `--release`.

### Exemple 1 : Manipulation d'un tableau plat 1D de 4096 entiers (Chunk Voxel 16x16x16)

```vortex
// Exemple 1 : Chunk voxel 16x16x16 sur tableau plat 1D de 4096 i32

fn CHUNK_DIM() -> i64 { return 16; }
fn CHUNK_AREA() -> i64 { return 256; }
fn CHUNK_VOL() -> i64 { return 4096; }

fn VOXEL_AIR() -> i64 { return 0; }
fn VOXEL_BEDROCK() -> i64 { return 1; }
fn VOXEL_STONE() -> i64 { return 2; }
fn VOXEL_GRASS() -> i64 { return 3; }

fn voxel_index(x: i64, y: i64, z: i64) -> i64 {
    return y * CHUNK_AREA() + z * CHUNK_DIM() + x;
}

fn voxel_in_bounds(x: i64, y: i64, z: i64) -> i64 {
    if x < 0 { return 0; }
    if x >= CHUNK_DIM() { return 0; }
    if y < 0 { return 0; }
    if y >= CHUNK_DIM() { return 0; }
    if z < 0 { return 0; }
    if z >= CHUNK_DIM() { return 0; }
    return 1;
}

fn chunk_generate_terrain(blocks: [i32]) -> i64 {
    let dim = CHUNK_DIM();
    for y in 0..dim {
        for z in 0..dim {
            for x in 0..dim {
                let idx = voxel_index(x, y, z);
                if y == 0 {
                    blocks[idx] = VOXEL_BEDROCK() as i32;
                } else if y < 6 {
                    blocks[idx] = VOXEL_STONE() as i32;
                } else if y == 6 {
                    blocks[idx] = VOXEL_GRASS() as i32;
                } else {
                    blocks[idx] = VOXEL_AIR() as i32;
                }
            }
        }
    }
    return 0;
}

fn chunk_write_safe(blocks: [i32], x: i64, y: i64, z: i64, id: i64) -> i64 {
    if voxel_in_bounds(x, y, z) == 0 {
        return -1;
    }
    let idx = voxel_index(x, y, z);
    blocks[idx] = id as i32;
    return 0;
}

fn chunk_read_safe(blocks: [i32], x: i64, y: i64, z: i64) -> i64 {
    if voxel_in_bounds(x, y, z) == 0 {
        return -1;
    }
    let idx = voxel_index(x, y, z);
    return blocks[idx] as i64;
}

fn chunk_count_solid(blocks: [i32]) -> i64 {
    let total = slice_len(blocks);
    let mut solid = 0;
    for i in 0..total {
        if (blocks[i] as i64) != 0 {
            solid = solid + 1;
        }
    }
    return solid;
}

fn run_example1() -> i64 {
    let blocks = slice_i32(CHUNK_VOL());
    chunk_generate_terrain(blocks);

    // Creuse un puits vertical de 2x2 au centre du chunk (y = 4..6)
    for y in 4..7 {
        chunk_write_safe(blocks, 8, y, 8, VOXEL_AIR());
        chunk_write_safe(blocks, 9, y, 8, VOXEL_AIR());
        chunk_write_safe(blocks, 8, y, 9, VOXEL_AIR());
        chunk_write_safe(blocks, 9, y, 9, VOXEL_AIR());
    }

    let solid = chunk_count_solid(blocks);
    let center_surface = chunk_read_safe(blocks, 8, 6, 8);
    let edge_surface = chunk_read_safe(blocks, 1, 6, 1);

    // Attendu : 7 couches * 256 - 12 blocs creuses = 1792 - 12 = 1780
    print_str("Solides restants: ");
    print(solid);
    print_str("Voxel (8,6,8) apres creusement: ");
    print(center_surface);
    print_str("Voxel (1,6,1) intact: ");
    print(edge_surface);
    return 0;
}

run_example1();
```

---

### Exemple 2 : Structures complexes, fonctions associées et gestion d'erreurs

```vortex
// Exemple 2 : Architecture par structures, codes d'erreur et arène zéro-fuite

fn ERR_OK() -> i64 { return 0; }
fn ERR_OUT_OF_BOUNDS() -> i64 { return -1; }
fn ERR_INVALID_VOXEL_ID() -> i64 { return -2; }

struct VoxelChunk {
    cx: i64,
    cy: i64,
    cz: i64,
    solid_count: i64,
    dirty: i64,
    blocks: [i32]
}

struct ChunkInitResult {
    ok: i64,
    err_code: i64,
    chunk: VoxelChunk
}

// Constructeur : alloue dans l'arène de l'appelant (jamais de scope_push ici)
fn chunk_new(cx: i64, cy: i64, cz: i64) -> ChunkInitResult {
    if cy < -16 {
        let empty = VoxelChunk {
            cx: 0, cy: 0, cz: 0, solid_count: 0, dirty: 0, blocks: slice_i32(1)
        };
        return ChunkInitResult { ok: 0, err_code: ERR_OUT_OF_BOUNDS(), chunk: empty };
    }
    let blocks = slice_i32(4096);
    let chk = VoxelChunk {
        cx: cx,
        cy: cy,
        cz: cz,
        solid_count: 0,
        dirty: 1,
        blocks: blocks
    };
    return ChunkInitResult { ok: 1, err_code: ERR_OK(), chunk: chk };
}

// Helper typé [i32] : conserve is_slice_ptr = true en --safe sans unsafe
fn blocks_put(buf: [i32], idx: i64, new_val: i64) -> i64 {
    let prev = buf[idx] as i64;
    buf[idx] = new_val as i32;
    return prev;
}

fn chunk_set_block(chk: VoxelChunk, x: i64, y: i64, z: i64, id: i64) -> i64 {
    if x < 0 { return ERR_OUT_OF_BOUNDS(); }
    if x >= 16 { return ERR_OUT_OF_BOUNDS(); }
    if y < 0 { return ERR_OUT_OF_BOUNDS(); }
    if y >= 16 { return ERR_OUT_OF_BOUNDS(); }
    if z < 0 { return ERR_OUT_OF_BOUNDS(); }
    if z >= 16 { return ERR_OUT_OF_BOUNDS(); }
    if id < 0 { return ERR_INVALID_VOXEL_ID(); }
    if id > 255 { return ERR_INVALID_VOXEL_ID(); }

    let idx = y * 256 + z * 16 + x;
    let prev = blocks_put(chk.blocks, idx, id);
    if prev == 0 {
        if id != 0 {
            chk.solid_count = chk.solid_count + 1;
        }
    } else if id == 0 {
        chk.solid_count = chk.solid_count - 1;
    }
    chk.dirty = 1;
    return ERR_OK();
}

// Portée éphémère locale pour le formatage de log sans aucune fuite mémoire
fn chunk_log_status(chk: VoxelChunk, err: i64) -> i64 {
    let tmp = scope_push();
    let s1 = str_concat("Chunk(", str_from_i64(chk.cx));
    let s2 = str_concat(s1, ",");
    let s3 = str_concat(s2, str_from_i64(chk.cz));
    let s4 = str_concat(s3, ") solid=");
    let s5 = str_concat(s4, str_from_i64(chk.solid_count));
    let s6 = str_concat(s5, " err=");
    let line = str_concat(s6, str_from_i64(err));
    print_str(line);
    print_str("\n");
    scope_pop(tmp);
    return 0;
}

fn run_example2() -> i64 {
    let res = chunk_new(2, 0, -3);
    if res.ok == 0 {
        print_str("Echec allocation chunk, code=");
        print(res.err_code);
        exit(1);
    }

    // Extraction obligatoire du pointeur sous-struct (pas d'écriture res.chunk.dirty =)
    let chk: VoxelChunk = res.chunk;
    let e1 = chunk_set_block(chk, 4, 5, 6, 3);
    let e2 = chunk_set_block(chk, 4, 5, 7, 2);
    let e_bad = chunk_set_block(chk, 99, 0, 0, 1);

    chunk_log_status(chk, e1);
    chunk_log_status(chk, e_bad);

    if e2 != ERR_OK() {
        exit(1);
    }
    return 0;
}

run_example2();
```

---

### Exemple 3 : FFI multi-OS et boucle principale avec mesure du delta time (`time_us`)

```vortex
// Exemple 3 : FFI C statique multi-plateforme + boucle principale à delta time fixe/variable

#[cfg(windows)]
extern "msvcrt" fn strlen(s: str) -> i64;
#[cfg(windows)]
extern "kernel32.dll" fn Sleep(ms: i64) -> void;

#[cfg(unix)]
extern "libc" fn strlen(s: str) -> i64;
#[cfg(unix)]
extern "libc" fn usleep(usec: i64) -> i64;

#[cfg(windows)]
fn os_sleep_ms(ms: i64) -> i64 {
    unsafe {
        Sleep(ms);
    }
    return 0;
}

#[cfg(unix)]
fn os_sleep_ms(ms: i64) -> i64 {
    unsafe {
        usleep(ms * 1000);
    }
    return 0;
}

fn ffi_c_strlen(s: str) -> i64 {
    unsafe {
        return strlen(s);
    }
}

struct SimBody {
    x: f32,
    y: f32,
    vx: f32,
    vy: f32,
    ticks: i64
}

fn step_physics(body: SimBody, dt: f32) -> i64 {
    // Aucun mélange implicite : toutes les littérales et opérations sont en f32
    let gravity: f32 = -9.81;
    body.vy = body.vy + gravity * dt;
    body.x = body.x + body.vx * dt;
    body.y = body.y + body.vy * dt;
    if body.y < 0.0 {
        body.y = 0.0;
        body.vy = 0.0;
    }
    body.ticks = body.ticks + 1;
    return 0;
}

fn run_example3() -> i64 {
    let banner = "Vortex Engine Loop";
    let c_len = ffi_c_strlen(banner);
    print_str("FFI strlen = ");
    print(c_len);

    // Etat persistant alloué AVANT la boucle principale
    let player = SimBody {
        x: 0.0,
        y: 10.0,
        vx: 4.5,
        vy: 0.0,
        ticks: 0
    };

    let mut prev_us = time_us();
    let mut running = 1;

    while running == 1 {
        os_sleep_ms(5);

        let now_us = time_us();
        let mut dt_us = now_us - prev_us;
        if dt_us <= 0 {
            dt_us = 1;
        }
        prev_us = now_us;

        // Conversion explicite i64 -> f32 avant division flottante
        let mut dt_sec: f32 = (dt_us as f32) / 1000000.0;
        if dt_sec > 0.05 {
            dt_sec = 0.05;
        }

        step_physics(player, dt_sec);

        // Arène de frame : libère 100% des chaînes de télémétrie à chaque itération
        let frame_mark = scope_push();
        let t1 = str_concat("tick=", str_from_i64(player.ticks));
        let t2 = str_concat(t1, " dt_us=");
        let t3 = str_concat(t2, str_from_i64(dt_us));
        let t4 = str_concat(t3, " y=");
        let line = str_concat(t4, str_from_f32(player.y));
        print_str(line);
        print_str("\n");
        scope_pop(frame_mark);

        if player.ticks >= 5 {
            running = 0;
        }
    }

    print_str("Position X finale: ");
    print(player.x);
    return 0;
}

run_example3();
```
