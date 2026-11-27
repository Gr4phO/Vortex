# Changelog Vortex

Toutes les modifications notables du projet sont listées ci-dessous **dans l'ordre antéchronologique** (les plus récentes en haut).

---

## [En cours / branche `dev`]

- **Logo officiel & icône Windows** : ajout de `docs/assets/vortex_logo.png`, `docs/assets/vortex.ico` et `src/vortex.rc` (intégré à `vortex.exe` via `llvm-rc` dans `build.bat` si présent, et affiché dans `README.md`).
- **REPL & Aide CLI refondus** : bannière ASCII `VORTEX` lisible dans le terminal interactif, sous-titre dynamique selon l'architecture/SIMD et le mode actif, nouvelles commandes REPL `:mode` et `:enums` (et alias `help` / `vortex help` / `vortex version`), aide `--help`, `pkg --help` et `vortex spec` synchronisées.
- **Levée des irritants langage** :
  - Types acceptés en annotations (`bool`, `i8`, `u8`, `i16`, `u16`, `u32`, `u64`, `usize`, `isize`, `char`, `<Enum>`).
  - Littéraux numériques & caractère : séparateurs `_` (`1_000_000`), binaire `0b...`, octal `0o...`, notation scientifique `1.5e-3`, caractères `'A'` / `'\n'`.
  - Littéraux de tableau `[10, 20, 30]` / `[1.5, 2.5]` (alloués en arène avec `slice_len` et bornes tracées).
  - Instruction `match` (bras multi-motifs `|` et joker `_`), opérateur ternaire `cond ? a : b`, et écriture chaînée multi-niveaux sur sous-structures (`world.player.pos.x += 1.0;`).
- **Benchmarks reproductibles multi-OS** : ajout de `benchmarks/suite_10_langs/run_benchmarks.py` (Python 3 stdlib, compatible Windows / Linux / macOS).
- **Extension VS Code complète (`editors/vscode/`)** : thème d'icônes dédié `Vortex File Icons` (`.vx`), support Mode Restreint (`untrustedWorkspaces`), icône Vortex sur les fichiers `.vx` (`languages[].icon`), bouton split natif **▶ ⌄** dans `editor/title/run` (`Run`, `--release`, `--ultrasafe 1`, `--ultrasafe 2`, `--ultrasafe 3`, `--ultrasafe 3 --check`, `vortex test`, `build`, `REPL`) + menu clic droit Explorateur, détection automatique de `vortex.exe` (dossier workspace, dossiers frères `../vortex`, `PATH`, emplacements usuels + sélecteur `Locate vortex.exe`), coloration syntaxique mise à jour (`match`, `enum`, `bool`, `u8..usize`, `/* */`, `'a'`, `0b`/`0o`/`1e-5`), packaging `.vsix` (`Gr4phO.vortex-systems` `1.1.9`) et job automatique `build-vscode-vsix` dans `.github/workflows/release.yml` (+ publication Open VSX optionnelle).
- **Modèles GitHub Issues** : ajout de `.github/ISSUE_TEMPLATE/` (`bug_report.yml`, `feature_request.yml`, `config.yml`).
- **Tests** : ajout de `tests/test_v12_features.vx` à la suite automatisée (`run_tests.bat` / `run_tests.sh`).
- **Diagnostics `--ultrasafe` & portée lexicale (`src/parser_stmt.c`, `src/runtime.c`)** : fermeture de la portée lexicale des blocs `if` / `else`, `for`, `while`, bras `match` et blocs `unsafe` (fini les faux positifs `W103` entre blocs frères, avec audit `W102`/`W104` en fin de bloc), table de correspondance ligne/fichier `VortexSourceMap` à travers les `import` et filtrage du bruit stdlib/`W105` sur les modules importés.

---

## `v1.1.6` — 2026-11-27

- **Documentation alignée** : synchronisation complète de `README.md` et `VORTEX_BIBLE.md` sur l'état exact du compilateur `v1.1.5` (`src/vortex.h`, `src/lexer.c`, `src/parser_expr.c`, `src/parser_stmt.c`, `src/runtime.c`, `src/main.c`).
- **Vérité terrain unifiée** :
  - Correction des plafonds (`MAX_FIELDS = 64`, `MAX_STRUCT_TYPES = 1024`, `MAX_ENUM_TYPES = 256`, `MAX_SYMBOLS = 1024`, `MAX_USER_FUNCS = 8192`, chaînes littérales `4095` octets, tampon JIT `16 MiB`, `MAX_IR_INSTRS = 262 144`).
  - Correction de la liste `std/` (6 modules réels : `net`, `http`, `json`, `crypto`, `db`, `gpu`), catalogue des **72 builtins**, table de précédence exacte à **12 niveaux** (avec rappel sur `&`/`^`/`|` vs `==`/`!=`).
- **Workflow Git & suivi** : ajout de `CHANGELOG.md` et séparation `main` (stable / releases) + `dev` (CI active sur Windows, Linux et macOS via `.github/workflows/ci.yml`).

---

## `v1.1.5` — 2026-11-26 / 2026-11-27

- **CI & Releases multi-plateformes 100 % vertes** : Windows x64 (`Clang`), Linux x86-64 (`GCC` + `Clang`) et macOS (`Clang` ARM64 + x64).
- **Artéfacts publiés** : binaires CLI + bibliothèques partagées (`vortex.dll`, `libvortex.so`, `libvortex.dylib`) avec `SHA256SUMS.txt`.
- **Correctif LSP & SemVer** : suppression du BOM UTF-8 (`0xEF 0xBB 0xBF`) en tête des trames LSP et durcissement du push de tag dans `.github/workflows/auto-version.yml`.

---

## `v1.1.1` – `v1.1.4` — 2026-11-26 / 2026-11-27

- **Transport LSP Windows** : passage aux appels natifs Win32 `ReadFile`/`WriteFile` sans buffering stdio pour éliminer les blocages sous CI Windows.
- **Compatibilité multi-OS `std/` & FFI** :
  - `std/db.vx` : priorité à `winsqlite3.dll` sous Windows.
  - `std/crypto.vx` & `src/ffi.c` : chargement versionné OpenSSL / fallback `libSystem.B.dylib` (`CC_SHA256`, `arc4random_buf`) évitant l'abort `dyld` sur macOS.
  - Linux GCC : `-fno-omit-frame-pointer` pour des stacktraces exactes, et support `--path` sur `vortex pkg`.
  - Migration des liens et dépôts vers `https://github.com/Gr4phO/Vortex`.

---

## `v1.1.0` — 2026-11-26

- **Modes de sûreté progressifs `--ultrasafe 1 / 2 / 3`** (cumulatifs au-dessus de `--safe`) :
  - `--ultrasafe 1` : garde runtime division/modulo par zéro (`IR_DIV_CHECK`) + warnings statiques `W101`–`W104` (code mort, `mut` non réassigné, shadowing, locale inutilisée).
  - `--ultrasafe 2` : erreurs statiques bloquantes `E201`–`E204` (div/mod constante `0`, slice/indice OOB constant, null deref statique, `return` manquant sur `-> T`), warning global `W105`, et garde runtime de récursion (max `2048` sur fonctions non feuilles).
  - `--ultrasafe 3` : récupération multi-erreurs en une passe (`setjmp` + `vortex_lexer_sync`), remontant `E001`, `E101`, toutes les violations d'arènes `E301` et les appels `unsafe` illégaux `E302` (jusqu'à `256` diagnostics dédupliqués).
- **Outillage AI-friendly** : `--check` (validation sans JIT), `--json` / `--error-format=json`, `vortex test [chemin] [--json]`, et `vortex spec [--json]` générée depuis le compilateur.
- **Ergonomie du langage** : opérateurs logiques court-circuit `&&` / `||` / `!`, bit-à-bit `&` / `|` / `^` / `~` / `<<` / `>>`, affectations composées (`+=`..`>>=`), `break` / `continue`, `true` / `false`, `enum` (`Enum::Var` / `Enum.Var`), commentaires `/* ... */`, chaînes littérales jusqu'à `4095` octets.
- **Builtins & capacités** : ajout de `assert`, `assert_eq`, `sinf_rad`, `cosf_rad`, `sqrtf_val` (`72` builtins au total) et relèvement des limites (`64` champs/struct, `1024` structs, `8192` fonctions, tampon JIT `16 MiB`).

---

## `v1.0.5` — 2026-11-14

- **Performance & architecture** : modularisation complète du compilateur C99 (`src/`), contrôles de bornes `--release` inline avec *cold traps*, vectorisation AVX2 sur T2/T5 et parseur `std/json` linéaire `O(N)`.

---

## `v1.0.4` — 2026-11-14

- **Fondations & sûreté** : ajout de la licence `Apache-2.0`, découpage lexer/parser, garde pointeurs NULL sur `net`/FFI, première CI GitHub et baseline C + arène sur T3.
