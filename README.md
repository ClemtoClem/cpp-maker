# Manuel utilisateur cpp-maker v0.7.0

> Moteur de production unifié pour C et C++ : compilation incrémentale, tests, packaging, déploiement, cross-compilation — sans dépendance externe (ni CMake, ni Make, ni shell obligatoire).

---

## Table des matières

1. [Vue d'ensemble](#1-vue-densemble)
2. [Installation et premier projet](#2-installation-et-premier-projet)
3. [Concepts clés](#3-concepts-clés)
4. [Structure d'un projet](#4-structure-dun-projet)
5. [Fichiers de configuration](#5-fichiers-de-configuration)
6. [Modes de compilation](#6-modes-de-compilation)
7. [Toolchains](#7-toolchains)
8. [Les commandes en détail](#8-les-commandes-en-détail)
9. [Workflows courants](#9-workflows-courants)
10. [Dépannage](#10-dépannage)
11. [Compilation de cpp-maker](#11-compilation-de-cpp-maker)

---

## 1. Vue d'ensemble

### Qu'est-ce que cpp-maker ?

cpp-maker est un **outil en ligne de commande** (un seul binaire) qui :

- **Découvre** les sources C/C++ d'un projet à partir de fichiers de configuration TOML ;
- **Calcule** un graphe de tâches (compilation, archivage, édition de liens, tests) ;
- **Exécute** ce graphe en parallèle avec une compilation **incrémentale** fiable (empreinte SHA/FNV des entrées) ;
- **Produit** exécutables, bibliothèques statiques/shared, tests, paquets, et les **déploie** (rsync/scp/local).

### Ce que cpp-maker n'est PAS

- Ce n'est pas un **gestionnaire de paquets** (pas d'équivalent `find_package`, `conan`, `vcpkg`). Il consomme `pkg-config` et des dépendances Git déclaratives.
- Ce n'est pas un **système de build générique** : la topologie des tâches est fixée (compile → archive/link → test).
- Ce n'est pas **CMake** : pas de scripts, pas de `try_compile`, pas de génération de projets IDE.

### Philosophie

| Principe | Conséquence concrète |
|---|---|
| **Convention sur configuration** | Un module est un dossier avec `src/`, `include/<nom>/`, `tests/` ; zéro config obligatoire. |
| **Configuration déclarative** | Tout dans `cpp-maker.toml` + `module.toml`, pas de code exécutable. |
| **Zéro dépendance** | Utilise `gcc`/`clang` directement, plus `ar`, `pkg-config`, `strip` s'ils existent. |
| **Multi-toolchain** | Une seule config, plusieurs compilateurs/cibles (`-t host,armhf,win64`). |
| **Incrémental sûr** | Empreinte = hash(tool_version, cmd, size+mtime de chaque entrée). Faux positifs impossibles côté format ; faux négatifs possibles si l'horloge recule. |

---

## 2. Installation et premier projet

### 2.1 Build de cpp-maker

cpp-maker se compile lui-même avec un compilateur C++23 :

```sh
g++ -std=c++23 -O2 -o cpp-maker \
    main.cpp util.cpp proc.cpp toml.cpp toolchain.cpp cfg.cpp \
    build.cpp deps.cpp elf.cpp flags.cpp cmd-build.cpp cmd-config.cpp \
    cmd-deploy.cpp cmd-helpers.cpp cmd-project.cpp
```

Placez le binaire dans `$PATH` (`/usr/local/bin/cpp-maker`).

### 2.2 Créer un projet simple (exécutable C++)

```sh
cpp-maker new hello
cd hello
cpp-maker build
cpp-maker run
```

Sortie attendue :

```
[cpp-maker] compilateur hôte : /usr/bin/g++ (g++ (Ubuntu 13.2.0) …)
Projet 'hello' créé.

  cd hello
  cpp-maker build                  # compile (debug)
  cpp-maker run                    # compile et exécute
  cpp-maker test                   # tests
  cpp-maker build release -t all   # toutes les toolchains configurées
  cpp-maker doctor                 # diagnostic de l'environnement
```

Arborescence créée :

```
hello/
├── cpp-maker.toml
├── include/hello/hello.hpp
├── src/hello.cpp
├── src/main.cpp
├── tests/
│   ├── test.hpp
│   ├── test_runner.cpp
│   └── test_hello.cpp
├── .gitignore
└── README.md
```

### 2.3 Créer un workspace multi-modules

```sh
cpp-maker new myapp --workspace
```

Crée :

```
myapp/
├── cpp-maker.toml           # [workspace] modules = ["libs/core", "apps/myapp"]
├── libs/core/
│   ├── module.toml
│   ├── include/core/core.hpp
│   ├── src/core.cpp
│   └── tests/test_core.cpp
└── apps/myapp/
    ├── module.toml
    └── src/main.cpp
```

### 2.4 Premier diagnostic

```sh
cpp-maker doctor
```

Vérifie : configuration, outils (`ar`, `pkg-config`, `ninja`, …), toolchains, dépendances implicites entre modules.

---

## 3. Concepts clés

### 3.1 Projet

Un **projet** = un dossier racine contenant `cpp-maker.toml`. Il définit :

- Le nom, la version, les standards C/C++ par défaut ;
- Les **modes** de compilation (release, debug, asan, …) ;
- Les **toolchains** (host, gcc, clang, cross) ;
- Les **formats de packaging** et **cibles de déploiement**.

### 3.2 Module

Un **module** = une unité de compilation/linkage. Quatre types :

| Type | Produit | Exemple |
|---|---|---|
| `executable` | un binaire (avec `main()`) | `hello`, `myapp` |
| `static` | `lib<nom>.a` | `libcore.a` |
| `shared` | `lib<nom>.so` (ou `.dylib`, `.dll`) | `libfoo.so` |
| `header-only` | rien (juste des en-têtes publics) | `libstack` |

Chaque module déclare :
- Ses **sources** (`[sources].dirs` = `["src"]` par défaut) ;
- Ses **en-têtes publics** (`[include].public` = `["include"]` par défaut), propagés aux dépendants ;
- Ses **dépendances** (modules, pkg-config, bibliothèques système).

### 3.3 Workspace

Un **workspace** est un projet multi-modules. Chaque module est dans son propre dossier avec un `module.toml`. Le `cpp-maker.toml` racine liste les modules :

```toml
[workspace]
modules = [
    "libs/core",
    "libs/net",
    "apps/myapp",
]
```

**Sélection automatique** : si vous êtes dans `libs/core/`, `cpp-maker build` ne compile que `core` et ses dépendances. `--all` (ou `-a`) force tout.

### 3.4 Tâche

Une **tâche** est une unité d'exécution (compilation, archivage, link, test-link). Un plan de build est un **DAG** de tâches. Chaque tâche :
- A des **entrées** (sources, headers, objets) et **sorties** (objets, binaire, archive) ;
- A une **commande** (argv) et une **empreinte** (hash) qui permet de savoir si elle est à jour ;
- A des **dépendances** (autres tâches).

Le DAG est exécuté par `parallel_jobs()` workers = `min(ncpus - 1, jobs demandés)`.

### 3.5 Empreinte (fingerprint)

Une tâche est **à jour** si :
1. Toutes ses sorties existent ;
2. L'empreinte actuelle (hash) est identique à celle stockée dans `<sortie>.stamp`.

L'empreinte couvre :
- La version du compilateur (`tool_version`) ;
- La commande complète (`cmd`) ;
- Pour chaque entrée : son chemin, sa taille, son mtime. Pour les fichiers sources C/C++, les **dépendances incluses** (via `.d` généré par `-MMD -MP`) sont également hashed.

**Important** : un `touch` sur un header inclus invalide les tâches qui le compilent. Un changement d'options (`-O2` → `-O3`) invalide aussi.

### 3.6 Lock file

cpp-maker pose un verrou (`flock`) sur `.cpp-maker.lock` pendant toute la durée d'un build/package/install/deploy. Deux invocations concurrentes → la seconde attend en affichant :

```
Processus de compilation déjà lancé, Veuillez attendre... (PID 12345)
```

---

## 4. Structure d'un projet

### 4.1 Mono-module (recommandé pour commencer)

```
monprojet/
├── cpp-maker.toml       ← contient [module] + [build] + [toolchains] + [package]
├── src/*.cpp            ← sources
├── include/<nom>/*.hpp  ← en-têtes publics
└── tests/*.cpp          ← tests
```

### 4.2 Workspace (multi-modules)

```
monworkspace/
├── cpp-maker.toml       ← uniquement [workspace] + [build] + [toolchains] + [package]
├── cpp-maker.lock       ← généré : versions git verrouillées
├── libs/
│   ├── core/
│   │   ├── module.toml  ← [module] + [sources] + [include] + [dependencies]
│   │   ├── include/core/*.hpp
│   │   ├── src/*.cpp
│   │   └── tests/*.cpp
│   └── net/
│       ├── module.toml
│       └── ...
└── apps/
    └── myapp/
        ├── module.toml
        └── src/main.cpp
```

### 4.3 Répertoires générés

- `build/<toolchain>/<mode>/obj/<module>/…` — fichiers objet (`.o`) et stamps ;
- `build/<toolchain>/<mode>/bin/` — exécutables ;
- `build/<toolchain>/<mode>/lib/` — bibliothèques ;
- `build/<toolchain>/<mode>/tests/` — exécutables de test ;
- `build/<toolchain>/<mode>/compile_commands.json` — pour clangd/IDE ;
- `build/<toolchain>/<mode>/build.ninja` — si `--backend ninja` ;
- `dist/` — paquets produits par `cpp-maker package`.

---

## 5. Fichiers de configuration

### 5.1 `cpp-maker.toml` — sections communes

```toml
[project]
name = "hello"
version = "0.1.0"
description = "Mon premier projet"
standard = "c++23"           # standard C++ (c++11..c++26, gnu++11..)
c_standard = "c17"           # standard C (c89..c23)
minimum_cpp_maker_version = "0.7.0"

[build]
default_mode = "debug"       # mode par défaut
default_toolchain = "host"   # toolchain par défaut
jobs = 0                     # 0 = ncpus - 1 ; N>0 = N (plafonné à ncpus-1)
warnings = ["-Wall", "-Wextra", …]  # options d'avertissement
static_pic = true            # -fPIC sur les bibliothèques statiques
defines = []                 # -D supplémentaires
cflags = []                  # -cflags communs C
cxxflags = []                # flags communs C++
ldflags = []                 # flags de link communs
```

### 5.2 Section `[module]` (mono-module) ou `module.toml` (workspace)

```toml
[module]
name = "hello"
type = "executable"          # executable | static | shared | header-only
version = "0.1.0"
standard = "c++23"
main = "src/main.cpp"        # optionnel : détection auto sinon

[sources]
dirs = ["src"]               # dossiers scannés récursivement
files = ["extra.cpp"]        # sources supplémentaires hors dirs
exclude = ["src/exp/**"]     # motifs d'exclusion (glob)
main = "src/main.cpp"

[include]
public = ["include"]         # propagé aux dépendants
private = ["src"]            # défaut = source_dirs

[dependencies]
modules = ["core", "net"]    # dépendances inter-modules
pkg_config = ["zlib", "libcurl"]
system = ["m", "pthread"]    # -lm -lpthread

[flags]
defines = []                 # -D pour CE module
public_defines = []          # -D propagés aux dépendants
cflags = []
cxxflags = []
ldflags = []

[tests]
dirs = ["tests"]             # scanné uniquement si --tests / test / check

[platform.windows]           # surcharge par OS cible
sources = ["src/win.cpp"]
defines = ["WIN32_LEAN_AND_MEAN"]
system = ["ws2_32"]
```

### 5.3 Section `[workspace]`

```toml
[workspace]
modules = [
    "libs/core",
    "libs/net",
    "apps/myapp",
]
```

Chaque chemin est relatif à la racine du projet. Le nom du module est le `name` de son `module.toml` (par défaut le nom du dossier).

### 5.4 Section `[toolchains.<nom>]`

Voir §7 pour le détail. Exemple :

```toml
[toolchains.armhf]
prefix = "arm-linux-gnueabihf-"
sysroot = "/usr/arm-linux-gnueabihf"
runner = ["qemu-arm", "-L", "/usr/arm-linux-gnueabihf"]   # optionnel
```

### 5.5 Section `[build.modes.<nom>]`

Voir §6.

### 5.6 Section `[tests]`

```toml
[tests]
timeout = 300                # secondes par exécutable de test
[tests.env]
HOME = "/tmp/testhome"       # {root} remplacé par la racine du projet
```

### 5.7 Section `[package]`

```toml
[package]
formats = ["tar.gz", "deb"]  # tar.gz | tar.xz | zip | deb | stage
strip = true                 # strip des binaires (sauf si -g dans le mode)
maintainer = "Jean <jean@x>"
description = "Mon logiciel"
homepage = "https://…"
license = "MIT"
deb_depends = ["libc6", "libstdc++6"]
deb_prefix = "/usr"          # préfixe d'installation
extra_files = ["docs/README.md"]
checksums = true             # génère SHA256SUMS
```

### 5.8 Section `[deploy.<nom>]`

Voir `cpp-maker deploy` (§8.14).

### 5.9 Section `[generate.<nom>]`

```toml
[generate.protobuf]
command = "protoc --cpp_out=src foo.proto"
inputs = ["foo.proto"]
outputs = ["src/foo.pb.cc", "src/foo.pb.h"]
```

Exécuté avant chaque build si une entrée a changé.

### 5.10 Section `[layers]`

Interdit les dépendances ascendantes (architecture en couches) :

```toml
[layers]
order = ["core", "domain", "app"]
[layers.app]
forbid_pkg_config = ["gtk", "qt"]   # app ne peut pas utiliser de GUI
```

Un module de couche `app` ne peut dépendre que de `app` ou de couches inférieures (`domain`, `core`). La validation est faite par `cpp-maker config validate` et au chargement.

### 5.11 Section `[dependencies.git.<alias>]`

```toml
[dependencies.git.mylib]
url = "https://github.com/user/mylib"
version = "v2.1.0"           # tag, branche ou commit
module = "core"              # module à importer (défaut = name du dépôt)
subdir = ""                  # sous-dossier du dépôt
```

Résolution :
- Clone `--mirror` dans `~/.cache/cpp-maker/git/<hash>.git` ;
- Checkout dans `~/.cache/cpp-maker/git/<hash>/<commit>` ;
- Lock dans `cpp-maker.lock` (ne pas éditer).

---

## 6. Modes de compilation

Un **mode** = un ensemble de flags + defines pour compiler et linker. Les modes sont déclarés dans `cpp-maker.toml` :

```toml
[build.modes.release]
flags = ["-O2"]
defines = ["NDEBUG"]

[build.modes.debug]
flags = ["-O0", "-g3"]
defines = ["DEBUG"]

[build.modes.asan]
base = "debug"               # hérite de "debug"
sanitizer = "address"        # ajoute -fsanitize=address

[build.modes.custom]
base = "release"
flags = ["-O3", "-march=native"]
link_flags = ["-Wl,--gc-sections"]
```

### Modes prédéfinis

| Nom | Flags | Sanitizer / runner | Usage |
|---|---|---|---|
| `release` | `-O2`, `NDEBUG` | — | Production |
| `debug` | `-O0 -g3`, `DEBUG` | — | Développement |
| `relwithdebinfo` | `-O2 -g`, `NDEBUG` | — | Profiling |
| `minsize` | `-Os`, `NDEBUG` | — | Embarqué |
| `asan` | `-O1 -g3 -fsanitize=address` | address | Détection d'erreurs mémoire |
| `tsan` | `-O1 -g3 -fsanitize=thread` | thread | Détection de data races |
| `ubsan` | `-O1 -g3 -fsanitize=undefined` | undefined | Comportement indéfini |
| `msan` | `-O1 -g3 -fsanitize=memory -fsanitize-memory-track-origins=2` | memory | Lecture de mémoire non initialisée (**clang uniquement**) |
| `lsan` | `-O1 -g3 -fsanitize=leak` | leak | Fuites mémoire, sans le surcoût d'ASan |
| `valgrind` | `-O0 -g3` | valgrind | Fuites mémoire |

### Sélection d'un mode

```sh
cpp-maker build                # mode par défaut (debug)
cpp-maker build release        # mode "release" (positionnel)
cpp-maker build -m release     # équivalent
cpp-maker build --mode=asan    # équivalent
```

### Surcharge par mode

```sh
cpp-maker test --modes debug,asan,tsan   # check multi-modes
```

### Sanitizers à la demande (`--sanitize`)

Au lieu de créer un mode, on peut ajouter des sanitizers au mode choisi depuis la ligne de commande (`build`, `run`, `test`, `check`, `package`…). Le résultat est un mode dérivé, nommé `<mode>+<sanitizers>`, avec son propre dossier `build/<tc>/<mode>+…/` : les objets du mode d'origine ne sont pas écrasés.

```sh
cpp-maker build --sanitize address,undefined         # → build/host/debug+address+undefined/
cpp-maker test --sanitize signed-integer-overflow,null --no-sanitize-recover all
cpp-maker test --sanitize all                        # une passe par runtime (voir ci-dessous)
cpp-maker run --cc clang --cxx clang++ --sanitize memory
cpp-maker build release --sanitize cfi --cc clang --cxx clang++
```

| Option | Effet |
|---|---|
| `--sanitize <a,b>` (alias `--sanitizer`) | Ajoute `-fsanitize=<a,b>` (compilation + édition de liens), `-fno-omit-frame-pointer` et `-g`. Répétable. Accepte aussi `-fsanitize=<nom>` et les alias `asan`, `tsan`, `msan`, `lsan`, `ubsan`, `hwasan`. |
| `--sanitize all` | Une passe par runtime, puisqu'ils ne cohabitent pas dans un même binaire : `address+undefined`, `thread`, `leak`, `memory`, `hwaddress`. Chaque passe a son mode dérivé et son dossier. Les passes impossibles (`memory` sans clang, `hwaddress` hors AArch64/RISC-V, conflit avec le sanitizer du mode) sont ignorées avec un avertissement. Les autres noms (`all,float-divide-by-zero`) s'ajoutent à chaque passe. Pour `build`, `run`, `test` et `check`. |
| `--sanitize-trap <a,b\|all>` | `-fsanitize-trap=…` : une instruction trap remplace le rapport textuel, ce qui donne un binaire plus léger. |
| `--sanitize-ignorelist <fichier>` | `-fsanitize-ignorelist=…` : fonctions (`fun:…`) ou sources (`src:…`) à ne pas instrumenter. Clang uniquement. |
| `--no-sanitize-recover <a,b\|all>` | `-fno-sanitize-recover=…` : le programme s'arrête (abort) à la première erreur au lieu de continuer. |

Sanitizers disponibles (détails : `cpp-maker help sanitize`) :

| Nom | Ce qui est détecté / protégé |
|---|---|
| `address` | Accès mémoire invalides : débordements, use-after-free, double free (+ fuites sous Linux). |
| `thread` | Data races, interblocages. |
| `memory` | Lectures de mémoire non initialisée (clang). |
| `leak` | Fuites mémoire. |
| `hwaddress` | ASan assisté par le matériel (marquage des pointeurs, AArch64/Android). |
| `undefined` | Groupe UBSan général. |
| `shift` | Décalages de bits invalides (plus de bits que la taille du type, valeur négative). |
| `integer-divide-by-zero` | Division entière par zéro. |
| `float-divide-by-zero` | Division flottante par zéro (hors du groupe `undefined`). |
| `null` | Utilisation ou déréférencement d'un pointeur nul. |
| `signed-integer-overflow` | Dépassement de capacité des entiers signés. |
| `unsigned-integer-overflow` | Dépassement des entiers non signés : pas un UB, mais utile pour les bugs logiques (clang). |
| `bounds` | Accès hors limites d'un tableau de taille connue. |
| `alignment` | Pointeurs mal alignés. |
| `vptr` | Type dynamique incohérent lors d'appels virtuels ou de `dynamic_cast` (C++). |
| `cfi` | Control Flow Integrity : cibles d'appels indirects valides (clang ; ajoute `-flto -fvisibility=hidden`). |
| `safe-stack` | Pile séparée en une partie sûre et une partie non sûre (clang). |
| `shadow-call-stack` | Adresse de retour sauvegardée dans une pile dédiée (AArch64/RISC-V). |

Règles de compatibilité vérifiées par cpp-maker : un seul runtime parmi `address`, `hwaddress`, `thread`, `memory` et `safe-stack` ; `leak` ne se combine qu'avec `address`. Les sanitizers réservés à clang provoquent une erreur claire avec GCC. Dans `cpp-maker.toml`, le champ `sanitizer` d'un mode accepte la même liste (`sanitizer = "address,undefined"`).

### Définir un mode personnalisé

```toml
[build.modes.profiling]
base = "relwithdebinfo"
flags = ["-fno-omit-frame-pointer", "-g"]
link_flags = ["-fno-omit-frame-pointer"]
```

---

## 7. Toolchains

Une **toolchain** = un ensemble de compilateurs (`cc`, `cxx`), d'outils (`ar`, `strip`, `pkg_config`) et d'une cible (`triple`, `arch`, `os`).

### Toolchains intégrées

- `host` : `CC`/`CXX` (ou auto-détection : `cl`, `gcc`, `clang`, `tcc`…) ;
- `gcc` : force `gcc`/`g++` ;
- `clang` : force `clang`/`clang++` ;
- Tout triplet du `$PATH` : `cpp-maker build -t arm-linux-gnueabihf`, `cpp-maker build -t x86_64-w64-mingw32`.

### Toolchains configurées

```toml
[toolchains.armhf]
prefix = "arm-linux-gnueabihf-"   # préfixe des outils
sysroot = "/usr/arm-linux-gnueabihf"   # --sysroot=…
os = "linux"                       # "" = déduit du triple
runner = ["qemu-arm", "-L", "/usr/arm-linux-gnueabihf"]
extra_flags = ["-mcpu=cortex-a53"] # ajouté aux cflags et cxxflags

[toolchains.win64]
prefix = "x86_64-w64-mingw32-"

[toolchains.clang-arm64]
cc = "clang"
cxx = "clang++"
target = "aarch64-linux-gnu"
sysroot = "/usr/aarch64-linux-gnu"

[toolchains.custom]
cc = "/opt/gcc-14/bin/gcc"
cxx = "/opt/gcc-14/bin/g++"
ar = "/opt/gcc-14/bin/gcc-ar"
family = "gcc"                     # auto | gcc | clang
```

### Détection automatique

- `detect_prefixes()` scanne `$PATH` pour les `X-Y-Z-gcc`/`g++` ;
- `host_candidates()` essaie `$CC`/`$CXX`, puis `cl`, `tcc`, `gcc`, `gcc-14`… `clang`, `clang-20`…
- `smoke_compile()` valide qu'un compilateur compile un fichier trivial (par défaut, `cpp-maker new` ne choisit qu'un compilateur qui fonctionne).

### Cross-compilation

```sh
# Utiliser le prefix auto-détecté
cpp-maker build -t arm-linux-gnueabihf

# Utiliser une toolchain configurée
cpp-maker build -t armhf

# Toutes les toolchains configurées
cpp-maker build -t all

# Plusieurs à la fois
cpp-maker build -t host,armhf,win64
```

### Exécution cross

Pour `run`, `test`, `check` :

- Si `runner` est défini dans la toolchain → préfixe la ligne de commande ;
- Sinon, auto-détection : `qemu-arm`/`qemu-aarch64`… pour Linux ARM/RISC-V, `wine`/`wine64` pour Windows ;
- Sinon, l'exécution est refusée avec un message (`SKIP` en test).

### Surcharge ad-hoc

```sh
cpp-maker build --cc=clang --cxx=clang++
cpp-maker build --cxx=/opt/gcc-14/bin/g++
```

---

## 8. Les commandes en détail

Toutes les commandes acceptent `--help` ou `-h` :

```sh
cpp-maker build --help
```

Toutes les commandes qui prennent un mode acceptent :
- Un **positionnel** : `cpp-maker build release` ;
- `-m|--mode` : `cpp-maker build -m release`.

Toutes les commandes qui prennent une toolchain acceptent :
- `-t|--toolchain <nom>` : `-t host`, `-t all` ;
- Multi-valeurs séparées par virgule : `-t host,armhf`.

Toutes les commandes qui prennent des modules acceptent :
- `-m <nom>` ou `--module <nom>` : `-m core` ;
- Multi-valeurs : `-m core,net` ;
- Défaut : tous les modules du workspace, ou le module courant si vous êtes dans un sous-dossier.

**Note importante** : `-m` est ambigu (module ou mode). cpp-maker résout ainsi :
- `-m <nom>` (pas de `=`, une seule valeur) → **module** ;
- `--mode <nom>` ou `--mode=<nom>` → **mode** ;
- Positionnel + `-m` : `cpp-maker run release -m hello` → mode=release, module=hello.

**Séparateurs `--` et `--args`** (run/test/check) : tout ce qui suit est **transmis à l'exécutable**, sans interprétation par cpp-maker :

```sh
cpp-maker run -- --verbose --port=8080
cpp-maker run --args --verbose --port=8080
```

**Sortie dans un fichier et filtrage (`--output`, `--filter`)** — options globales, acceptées par toutes les commandes, avant ou après le nom de la commande :

| Option | Effet |
|---|---|
| `--output <fichier>` | stdout **et** stderr (cpp-maker, compilateur, éditeur de liens, programme lancé…) sont écrits dans `<fichier>` au lieu du terminal. Le fichier est écrasé, sans codes couleur. Un résumé `sortie écrite dans … (N ligne(s))` s'affiche à la fin. |
| `--filter <motif>` | Ne garde que les lignes qui correspondent au motif, comme `grep` : regex ECMAScript (`error\|warning`), ou texte littéral si le motif n'est pas une regex valide. Répétable : `--filter A --filter B` garde les lignes contenant A **ou** B. S'applique au terminal, ou au fichier si `--output` est donné. |
| `--grep <motif>` | Synonyme de `--filter`. |

```sh
cpp-maker build --output build.log                    # tout le journal dans build.log
cpp-maker build --filter 'error|warning'              # n'affiche que les diagnostics
cpp-maker build release --output err.log --filter error
cpp-maker test --grep FAIL                            # filtre la sortie des tests
cpp-maker --filter FAIL test --filter add             # idem, avec filtre sur les noms de tests
```

Le code de retour reste celui de la commande (il ne dépend pas du nombre de lignes gardées). Pour `test` et `check`, un `--filter` placé **après** la commande garde son sens d'origine (filtre sur les noms de tests) : utilisez `--grep`, ou placez `--filter` **avant** la commande. Ce qui suit `--` / `--args` n'est jamais interprété.

### 8.1 `new` — créer un projet

```sh
cpp-maker new <nom> [options]
```

| Option | Valeur | Défaut | Description |
|---|---|---|---|
| `--lang` | `c` \| `c++` | `c++` | Langage principal |
| `--type` | `normal` \| `static` \| `shared` \| `header-only` \| `modular` | `normal` | Type de projet |
| `--workspace` | flag | — | Workspace multi-modules |
| `--standard` | `c++17`, `c++23`… | `c++23` | Standard C++ |
| `--c-standard` | `c11`, `c17`… | `c17` | Standard C |
| `--compiler` | chemin | auto | Compilateur C++ à utiliser |

**Types de projet** :

- `normal` (ou `executable`) : un seul module, produit un binaire ;
- `static` : un seul module, produit `lib<nom>.a` ;
- `shared` : un seul module, produit `lib<nom>.so` (ou `.dll`, `.dylib`) ;
- `header-only` : un seul module, juste des en-têtes ;
- `modular` : **alias** de `--workspace` (crée `libs/core` + `apps/<nom>`).

**Comportement** :

1. Vérifie qu'un compilateur **compile effectivement** un fichier trivial en `-std=<standard>` (essai `smoke_compile`). Si aucun compilateur ne fonctionne, échec avec log complet.
2. Si le premier candidat n'est pas celui du `$PATH` (`/usr/bin/g++`), **épingle** le compilateur dans `[toolchains.host]` (`cxx = "/chemin/vers/g++"`).
3. Génère `cpp-maker.toml`, sources d'exemple, test runner, `README.md`, `.gitignore`.
4. Ne fait **aucun build**.

**Exemples** :

```sh
cpp-maker new hello                              # exécutable C++
cpp-maker new mylib --type static                # bibliothèque statique
cpp-maker new mylib --type header-only           # header-only
cpp-maker new app --workspace                    # workspace vide
cpp-maker new app --workspace --lang c           # workspace en C
cpp-maker new tiny --type static --lang c        # bibliothèque C
```

### 8.2 `add` — ajouter un module à un workspace

```sh
cpp-maker add <nom|chemin> [options]
```

| Option | Valeur | Défaut | Description |
|---|---|---|---|
| `--type` | `executable` \| `static` \| `shared` \| `header-only` | `static` | Type |
| `--lang` | `c` \| `c++` | `c++` | Langage |
| `--deps` | `a,b,c` | — | Dépendances |
| `--name` | nom | nom du dossier | Nom logique |
| `--dir` | `libs` \| `apps` \| chemin | `libs` | Dossier par défaut si nom simple |
| `--no-scaffold` | flag | — | Ne pas générer les sources |

**Résolution du chemin** :

- Nom simple (`core`) → `<--dir>/<nom>` (par défaut `libs/core`) ;
- Chemin relatif (`apps/truc`) → interprété **depuis le répertoire d'invocation**, ramené relatif à la racine ;
- Chemin absolu → tel quel.

**Comportement** :

1. Modifie `[workspace].modules` dans `cpp-maker.toml` (remplacement atomique, revalidation complète) ;
2. Crée le dossier du module et son `module.toml` ;
3. Génère des sources d'exemple (sauf `--no-scaffold`) : header, source, test, et `main()` si `executable` ;
4. Si une dépendance est un module de type bibliothèque avec un `add(int,int)` dans son en-tête public, `main()` généré l'appelle ;
5. En cas d'erreur de validation, restaure **tout** (fichier + dossier).

**Exemples** :

```sh
cpp-maker add core                                  # libs/core, static
cpp-maker add net --type static --deps core         # libs/net, dépend de core
cpp-maker add gui --dir apps --type executable      # apps/gui
cpp-maker add libs/util                             # chemin explicite
cpp-maker add libs/util --name util_core            # nom différent du dossier
cpp-maker add test --no-scaffold --type header-only # coquille vide
```

### 8.3 `module` — gestion des modules

Sous-commandes : `add`, `rename`, `remove`, `deps`.

#### `module add`

Alias : `cpp-maker add` (voir §8.2).

#### `module rename <ancien> <nouveau>`

Renomme un module :

1. Met à jour `name = "<nouveau>"` dans son `module.toml` ;
2. Met à jour les `[dependencies].modules` de tous les modules qui le référencent ;
3. Si le nom du dossier == ancien nom, **renomme le dossier** et met à jour `[workspace].modules` ;
4. En cas d'erreur, restaure tout.

```sh
cpp-maker module rename core base
```

#### `module remove <nom> [--force] [--purge]`

1. Refuse si d'autres modules dépendent de `<nom>` (sauf `--force`, qui retire la dép) ;
2. Retire `<nom>` de `[workspace].modules` et de toutes les listes de deps ;
3. `--purge` supprime aussi le dossier sur disque.

```sh
cpp-maker module remove core
cpp-maker module remove core --force --purge
```

#### `module deps <list|add|remove> <module> [<nom>] [--pkg-config|--system|--module]`

Gère les dépendances d'un module.

- `list <module>` : affiche modules, pkg-config, système ;
- `add <module> <nom>` : ajoute `<nom>` ; type déduit ainsi :
  1. flag explicite (`--module` / `--pkg-config` / `--system`) ;
  2. `--pkg-config` si `pkg-config --exists <nom>` réussit ;
  3. `--system` sinon ;
- `remove <module> <nom>` : retire `<nom>` de la liste où il figure.

```sh
cpp-maker module deps list core
cpp-maker module deps add core net                       # module (auto-détecté)
cpp-maker module deps add app zlib --pkg-config          # pkg-config
cpp-maker module deps add app pthread --system           # système (-lpthread)
cpp-maker module deps remove app zlib
```

### 8.4 `deps` — dépendances Git

Sous-commandes : `list` (défaut), `update`, `clean`.

```sh
cpp-maker deps list      # affiche les versions verrouillées
cpp-maker deps update    # ignore le lock, résout les versions, réécrit le lock
cpp-maker deps clean     # supprime ~/.cache/cpp-maker/git
```

Cache : `~/.cache/cpp-maker/git/` (ou `$XDG_CACHE_HOME/cpp-maker/git`, ou `$CPP_MAKER_CACHE/git`).

### 8.5 `build` — compiler

```sh
cpp-maker build [mode] [options]
```

| Option | Description |
|---|---|
| `--backend native\|ninja` | Backend d'exécution (défaut : native) |
| `--tests` | Compile aussi les exécutables de test |
| `-m <modules>` | Modules à construire (+ leurs dépendances) |
| `-t <toolchains>` | Toolchains (défaut : celle du projet) |
| `-j N` / `--jobs N` | Workers (défaut : ncpus - 1) |
| `-k` / `--keep-going` | Continue après les échecs |
| `-n` / `--dry-run` | Affiche sans exécuter |
| `-v` / `--verbose` | Affiche chaque commande |
| `--clean` | Supprime `build/<tc>/<mode>` avant |

**Backends** :

- `native` (défaut) : DAG exécuté par cpp-maker (threads) ;
- `ninja` : génère `build/<tc>/<mode>/build.ninja` et l'exécute. Ninja n'est requis que pour ce backend.

**Sortie typique** :

```
[cpp-maker] hello · toolchain host (x86_64-linux-gnu) · mode release
            modules : hello
[1/3] CXX   src/main.cpp
[2/3] CXX   src/hello.cpp
[3/3] LINK  build/host/release/bin/hello
✓ 3 tâche(s) exécutée(s), 0 à jour — 0.54s
  → hello          build/host/release/bin/hello
```

Relance immédiate :

```
[cpp-maker] hello · toolchain host · mode release
            modules : hello
✓ rien à faire, tout est à jour (3 tâches)
```

**Multi-toolchains** :

```sh
cpp-maker build -t host,armhf,win64
```

Chaque toolchain est compilée dans son propre répertoire `build/<tc>/<mode>/`. Échec d'une toolchain → code retour 1, les autres continuent.

**`--clean`** : supprime **uniquement** `build/<tc>/<mode>/`, pas tout `build/`.

### 8.6 `run` — compiler et exécuter

```sh
cpp-maker run [mode] [options] [-- <args>]
```

| Option | Description |
|---|---|
| `-m <module>` | Module exécutable (défaut : module courant si dans un sous-dossier, sinon unique exécutable du projet) |
| `-t <toolchain>` | Une seule toolchain |
| `--args ARG…` | Arguments pour l'exécutable |
| `-- ARG…` | Idem |

**Comportement** :

1. Compile (via `run_plan`) ;
2. **Relâche le lock** avant d'exécuter (pour permettre à l'exécutable d'invoquer cpp-maker) ;
3. Applique la chaîne de lancement (runner qemu/wine + valgrind si mode valgrind) ;
4. Transmet `positional` + `passthrough` à l'exécutable.

**Exemples** :

```sh
cpp-maker run                              # exécute le seul exécutable
cpp-maker run -m myapp                     # exécutable myapp
cpp-maker run release                      # en mode release
cpp-maker run --args --port 8080 --dev     # arguments typés
cpp-maker run -- --port 8080 --dev         # idem
cpp-maker run valgrind                     # via `cpp-maker valgrind` aussi
```

### 8.7 `test` — compiler et exécuter les tests

```sh
cpp-maker test [mode] [options]
```

| Option | Description |
|---|---|
| `--filter <str>` | Sous-chaîne de nom de test (passée comme `--filter` à l'exécutable) |
| `--junit <file>` | Génère un rapport JUnit XML |
| `--timeout <s>` | Timeout par exécutable (défaut : `[tests].timeout`) |
| `-t <toolchain>` | Une seule toolchain |

**Squelette de test généré** (workspace ou projet) :

```cpp
#include "test.hpp"
#include "hello/hello.hpp"

TEST(add_works) { CHECK_EQ(hello::add(2, 3), 5); }
TEST(add_neg)   { CHECK_EQ(hello::add(-1, 1), 0); }
```

Macros disponibles :
- `CHECK(cond)` / `CHECK_EQ(a,b)` / `CHECK_NE` / `CHECK_NEAR` : continuent après échec ;
- `REQUIRE(cond)` / `REQUIRE_EQ` : arrêtent le test ;
- `SKIP("raison")` : marque le test comme ignoré ;
- Alias GTest : `EXPECT_*`, `ASSERT_*`.

**Codes retour** :
- `0` : tous les tests exécutés réussissent (ou aucun n'est trouvé) ;
- `1` : au moins un échec ;
- `77` : tous les tests exécutés ont été SKIPpés (comptabilisé comme « skip » par cpp-maker).

### 8.8 `check` — tests multi-modes

```sh
cpp-maker check [options]
```

| Option | Description |
|---|---|
| `--modes <m1,m2,…>` | Modes à tester (défaut : `debug,asan,tsan`) |
| `--filter`, `--junit`, `--timeout` | Idem `test` |

Pour chaque mode : compile les tests, exécute, agrège les résultats dans un seul rapport.

**Exemple** :

```sh
cpp-maker check --modes debug,asan,ubsan --junit report.xml
```

### 8.9 `generate` — génération de fichiers pour IDE/ninja

```sh
cpp-maker generate <ninja|compdb> [mode] [options]
```

- `ninja` : écrit `build/<tc>/<mode>/build.ninja` (utilisable via `ninja -f build/host/release/build.ninja`) ;
- `compdb` : écrit `compile_commands.json` à la racine (pour clangd, VSCode C/C++, etc.).

```sh
cpp-maker generate compdb
cpp-maker generate ninja release
```

**Note** : `build` et `run` régénèrent automatiquement `compile_commands.json` dans le répertoire du build.

### 8.10 `clean` — supprimer les artefacts

```sh
cpp-maker clean [mode] [options]
```

| Option | Description |
|---|---|
| `-t <toolchains>` | Nettoie `build/<tc>/` ou `build/<tc>/<mode>/` |
| `--all` | Supprime aussi `dist/` |

**Comportement** :

- Sans arg : supprime **tout** `build/` ;
- Avec `-t host` : supprime `build/host/` ;
- Avec `-t host release` : supprime `build/host/release/`.

```sh
cpp-maker clean
cpp-maker clean release
cpp-maker clean -t host release
cpp-maker clean --all
```

### 8.11 `package` — créer des paquets

```sh
cpp-maker package [mode] [options]
```

| Option | Description |
|---|---|
| `--format <fmt1,fmt2>` | Formats (défaut : `[package].formats`) : `tar.gz`, `tar.xz`, `zip`, `deb`, `stage` |
| `--no-strip` | Désactive le strip (défaut : strip si pas de `-g` dans le mode) |
| `-t <toolchains>` | Une ou plusieurs toolchains |

**Contenu typique** d'un paquet :

```
<name>-<version>-<os>-<arch>/
├── bin/hello                        # exécutables
├── lib/libcore.a                    # bibliothèques statiques
├── lib/libnet.so                    # bibliothèques partagées
├── include/core/*.hpp               # en-têtes publics
├── lib/pkgconfig/core.pc            # fichiers .pc
└── share/doc/hello/README.md
```

**Debian** : produit un `.deb` valide (`dpkg-deb` requis) avec `DEBIAN/control`, `Installed-Size`, `Depends`.

**Checksums** : si `[package].checksums = true`, un fichier `<nom>-<version>-<os>-<arch>.sha256` est ajouté.

```sh
cpp-maker package release --format tar.gz,deb
cpp-maker package -t armhf --format tar.xz
```

### 8.12 `install` — installer localement

```sh
cpp-maker install [mode] [options]
```

| Option | Description |
|---|---|
| `--prefix <chemin>` | Préfixe d'installation (défaut : `/usr/local`) |
| `--destdir <chemin>` | Racine alternative pour staging (`<destdir><prefix>`) |
| `--no-strip` | Idem package |

**Comportement** : copie les artefacts + en-têtes + .pc dans `<prefix>` (`bin/`, `lib/`, `include/`, `share/doc/`). Écrit `build/<tc>/<mode>/install_manifest.txt`.

```sh
cpp-maker install --prefix /usr/local
cpp-maker install --destdir /tmp/stage --prefix /usr
```

### 8.13 `deploy` — déployer sur une cible

```sh
cpp-maker deploy <cible> [options]
cpp-maker deploy --list
```

Cibles déclarées dans `cpp-maker.toml` :

```toml
[deploy.rpi]
method = "rsync"             # rsync | scp | copy | script
host = "raspberrypi.local"
user = "pi"
port = 22
path = "/home/pi/{name}"     # variables : {name} {version} {toolchain} {mode} {artifact} {path}
toolchain = "armhf"
mode = "release"
artifact = "stage"           # stage | tar.gz | tar.xz | zip | deb
modules = ["myapp"]
ssh_options = ["-o", "StrictHostKeyChecking=no"]
before = ["echo starting deploy"]              # commandes locales
after = ["{path}/bin/{name} --version"]        # commandes distantes
after_local = ["echo done"]                    # commandes locales
delete = false               # rsync --delete
```

Forme simplifiée (v0.4) :

```toml
[deploy]
method = "rsync"
host = "user@machine"
target_dir = "/opt/myapp"
pre = ["…"]
post = ["…"]                 # exécuté localement
script = "deploy.sh"
```

`method = "script"` : exécute `<script>` localement avec `$CPP_MAKER_DIST` = chemin du staging.

**Variables d'expansion** : `{name}`, `{version}`, `{toolchain}`, `{mode}`, `{host}`, `{artifact}`, `{path}`.

```sh
cpp-maker deploy rpi
cpp-maker deploy rpi --dry-run
cpp-maker deploy --list
```

### 8.14 `graph` — graphe des dépendances

```sh
cpp-maker graph [--dot] [-a]
```

Sans option : arbre ASCII des modules et de leurs dépendances.

Avec `--dot` : sortie Graphviz (à piper vers `dot -Tsvg`).

`-a` (ou `--all`) : inclut les dépendances (par défaut, seuls les modules racines sont affichés).

```sh
cpp-maker graph
cpp-maker graph --dot > graph.dot && dot -Tsvg graph.dot -o graph.svg
```

### 8.15 `modules` — liste des modules

```sh
cpp-maker modules
```

Sortie tabulaire : nom, type, couche, dossier, dépendances.

### 8.16 `info` — résumé

```sh
cpp-maker info
```

Affiche : racine, projet, standards, défauts, modes, toolchains, modules, déploiements, et le détail de la toolchain par défaut (cc, cxx, ar, strip, triple, arch, os, runner).

### 8.17 `size` — analyse ELF

```sh
cpp-maker size [mode] [options]
```

Analyse les binaires de `build/<tc>/<mode>/{bin,lib,tests}` avec un **parser ELF intégré** (aucune dépendance à `binutils`).

| Colonne | Signification |
|---|---|
| `text` | sections allocables **non inscriptibles** (code, `.rodata`) — sémantique Berkeley `size` |
| `data` | sections allocables **inscriptibles** (`.data`, `.bss` compté séparément) |
| `bss` | sections `SHT_NOBITS` allocables (`.bss`, `.tbss`) |
| `dec` | `text + data + bss` |

Pour les **archives `.a`**, les membres ELF sont sommés.

**Sortie typique** :

```
build/host/release — toolchain host · mode release
  bin/hello — ELF64 little endian, machine x86-64, ABI Linux,
              DYN (position indépendante — PIE ou .so), entrée 0x1060 — 15 608 octets
        text       data        bss        dec        hex
        1243        512          8       1763        6e3
```

`-v` : top 10 des sections par taille.

### 8.18 `doctor` — diagnostic

```sh
cpp-maker doctor [--no-fix|--check-deps]
```

Vérifie :

1. **Configuration** : `cpp-maker.toml` présent, valide (`cfg::validate_errors`) ;
2. **Outils** : `ar`, `pkg-config`, `ninja`, `tar`, `zip`, `dpkg-deb`, `sha256sum`, `rsync`, `ssh`, `valgrind`, `qemu-arm`, `qemu-aarch64`, `wine` ;
3. **Toolchains** : pour chacune, `smoke_compile` C et C++, sanitizers, runner ;
4. **Dépendances entre modules** : analyse statique des `#include`, détecte les dépendances implicites manquantes. Par défaut, **corrige** les `module.toml` (avec revalidation + rollback).

| Option | Effet |
|---|---|
| `--no-fix` | Signale mais ne modifie pas |
| `--check-deps` | Signale sans modifier, et compte comme problème |

### 8.19 `config` — inspecter/modifier la config

#### `config show [clé…]`

Affiche la configuration effective (après défauts) au format `clé = valeur`.

```sh
cpp-maker config show                 # tout
cpp-maker config show build           # préfixe "build."
cpp-maker config show build.jobs      # clé exacte
cpp-maker config show project.name build.warnings
```

Suggestion pour les clés inconnues : `nam` → `(essayez : project.name)`.

#### `config validate`

Charge et valide la configuration. Code retour 0 si valide, 1 sinon.

```sh
cpp-maker config validate
```

#### `config set <clé> <valeur>`

Modifie une clé **scalaire** de `cpp-maker.toml` de manière contrôlée :

1. La clé doit être dans la liste blanche (`project.name`, `project.version`, `build.jobs`, `module.type`, `compiler.cxx`…) ;
2. Réécrit la ligne (`indent = clé = nouvelle_valeur`) ;
3. Écrit dans `.tmp`, **recharge et revalide** la config complète ;
4. Si valide : `rename(tmp, file)`, sinon supprime le tmp et laisse le fichier **intact**.

```sh
cpp-maker config set build.jobs 4
cpp-maker config set project.version "1.2.0"
cpp-maker config set project.standard c++20
```

### 8.20 `toolchain` — gestion des toolchains et versions

#### `toolchain list` (ou `ls`)

Affiche : toolchains du projet, toolchains intégrées, préfixes détectés dans le `$PATH`, versions gcc/g++ installées.

#### `toolchain show [nom]`

Affiche en détail la toolchain résolue (cc, cxx, ar, strip, triple, arch, os, runner, pkg-config).

#### `toolchain which [nom]`

Affiche le chemin du `cxx` puis du `cc`.

#### `toolchain use <version>`

Debian/Ubuntu uniquement. Configure `/usr/bin/gcc-<N>` comme défaut via `update-alternatives` :

```sh
cpp-maker toolchain use 14
```

Vérifie que `g++` pointe bien sur la version demandée ; sinon, affiche un diagnostic.

#### `toolchain install <version|triplet>`

Debian/Ubuntu uniquement.

- `cpp-maker toolchain install 14` : ajoute le PPA `ubuntu-toolchain-r/test`, installe `gcc-14` + `g++-14` ;
- `cpp-maker toolchain install arm-linux-gnueabihf` : installe `gcc-<triplet>` + `g++-<triplet>` + `qemu-user` ;
- `cpp-maker toolchain install x86_64-w64-mingw32` : installe `mingw-w64`.

`-y`/`--yes` : pas de confirmation.

#### `toolchain remove <version>`

Purge `gcc-<N>` et `g++-<N>` (refuse si c'est la version par défaut).

### 8.21 `help` — aide

```sh
cpp-maker help               # liste toutes les commandes
cpp-maker help build         # aide détaillée de "build"
cpp-maker help list          # idem "help" sans argument
cpp-maker <cmd> --help       # équivalent
```

---

## 9. Workflows courants

### 9.1 Développement quotidien

```sh
cpp-maker build              # debug, toolchain host
cpp-maker test               # compile et lance les tests
cpp-maker run -- --dev       # lance l'app avec arguments
```

### 9.2 Avant commit

```sh
cpp-maker check --modes debug,asan,ubsan    # multi-modes
cpp-maker build release --tests             # build release + tests compilés
```

### 9.3 Cross-compilation

```sh
# Une fois : configurer la toolchain
cpp-maker toolchain install arm-linux-gnueabihf
cpp-maker build -t arm-linux-gnueabihf release
cpp-maker test  -t arm-linux-gnueabihf release     # via qemu-arm si dispo
```

### 9.4 Packaging et release

```sh
cpp-maker package release --format tar.gz,deb
cpp-maker deploy rpi
```

### 9.5 Intégration IDE

```sh
cpp-maker generate compdb
# → compile_commands.json à la racine
# Ouvrir dans VSCode + clangd
```

### 9.6 Ajouter une dépendance Git

```toml
[dependencies.git.mylib]
url = "https://github.com/user/mylib"
version = "v2.1.0"
```

```sh
cpp-maker build       # résout et verrouille
cpp-maker deps list   # affiche les versions verrouillées
cpp-maker deps update # pour passer à la dernière version de la branche
```

### 9.7 Ajouter un module dans un workspace

```sh
cpp-maker add libs/net --type static --deps core
cpp-maker add apps/gui --dir apps --type executable --deps net
```

### 9.8 Mettre à jour un projet

```sh
cpp-maker doctor                # détecte et corrige les deps manquantes
cpp-maker config set project.standard c++23
cpp-maker module rename core base
```

---

## 10. Dépannage

### « aucun compilateur ne compile un fichier trivial »

Cause : aucun `gcc`/`clang` fonctionnel dans le `$PATH`, ou standard trop récent pour le compilateur.

```sh
cpp-maker doctor                       # diagnostic détaillé
cpp-maker toolchain list               # versions installées
cpp-maker toolchain install 14         # installer gcc-14
cpp-maker new hello --standard c++17   # ou baisser le standard
```

### « dépendance circulaire : A → B → A »

`cpp-maker graph` pour voir le cycle. Restructurez en extrayant une interface.

### « module inconnu »

`cpp-maker modules` pour lister les modules disponibles. Vérifiez `[workspace].modules` et le nom déclaré dans `module.toml`.

### « dépendance git introuvable »

```sh
cpp-maker deps clean
cpp-maker deps list
git ls-remote <url>    # vérifier que la version existe
```

### « aucun runner pour l'architecture cible »

```sh
cpp-maker doctor       # détecte les qemu/wine manquants
sudo apt install qemu-user
```

Ou désactivez l'exécution : `[toolchains.armhf] runner = []`.

### « Processus de compilation déjà lancé »

Un autre `cpp-maker` (ou une session interrompue) tient `.cpp-maker.lock`. Attendez, ou supprimez le fichier **manuellement** si vous êtes certain qu'aucune instance ne tourne :

```sh
ps aux | grep cpp-maker
rm -f .cpp-maker.lock
```

### « édition annulée : configuration v0.4 (plusieurs modules dans le même fichier) »

Vous utilisez l'ancien format `[modules.<nom>]` dans `cpp-maker.toml`. Migrez vers un workspace :

```sh
mkdir -p libs/core libs/net
# déplacer les sources et créer un module.toml par module
# remplacer [modules.*] par [workspace].modules = [...]
```

### « strip : binaire non strippable »

Le binaire est peut-être un script ou corrompu ; vérifiez avec `file <binaire>`. Ou passez `--no-strip`.

### Build qui semble toujours tout recompiler

- Vérifiez que vous n'avez pas `--clean` ou `-a` dans un script ;
- Vérifiez que le mtime des sources est stable (`git checkout` peut changer les mtimes) ;
- `cpp-maker build -v` pour voir les commandes et diagnostiquer.

### « aucune règle pour X.o, requis par Y »

Un `[generate]` produit une source qui n'est pas encore générée. Lancez `cpp-maker build` (les étapes `[generate]` sont exécutées avant le build).

### Tests ignorés en cross-compilation

Normal si aucun runner n'est disponible pour l'architecture cible. Installez `qemu-user` ou configurez `runner` dans la toolchain.

---

## Annexe A — Variables et chemins

| Variable | Effet |
|---|---|
| `$CC` / `$CXX` | Compilateur par défaut de la toolchain `host` |
| `$PATH` | Sert à localiser les outils et les préfixes de triplet |
| `$HOME` / `$XDG_CACHE_HOME` / `$CPP_MAKER_CACHE` | Cache des dépôts Git |
| `$NO_COLOR` | Désactive la couleur |

Chemins produits :

| Chemin | Contenu |
|---|---|
| `build/<tc>/<mode>/obj/` | Fichiers `.o` + `.d` + `.stamp` |
| `build/<tc>/<mode>/bin/` | Exécutables |
| `build/<tc>/<mode>/lib/` | Bibliothèques |
| `build/<tc>/<mode>/tests/` | Exécutables de test |
| `build/<tc>/<mode>/compile_commands.json` | Pour clangd |
| `build/<tc>/<mode>/build.ninja` | Backend ninja |
| `build/<tc>/<mode>/install_manifest.txt` | Fichiers installés |
| `dist/` | Paquets produits |
| `.cpp-maker.lock` | Verrou de concurrence |
| `cpp-maker.lock` | Versions Git verrouillées |

---

## Annexe B — Aide-mémoire

```sh
# Projet
cpp-maker new hello                          # projet C++ simple
cpp-maker new app --workspace                # workspace
cpp-maker add libs/core --type static        # ajouter un module
cpp-maker modules                            # lister les modules
cpp-maker graph                              # arbre des deps
cpp-maker info                               # résumé config

# Build
cpp-maker build                              # debug
cpp-maker build release                      # release
cpp-maker build -t all                       # toutes toolchains
cpp-maker build -j 8                         # 8 workers
cpp-maker build -k                           # keep-going
cpp-maker build --backend ninja              # backend ninja
cpp-maker generate compdb                    # compile_commands.json

# Test
cpp-maker test                               # tests debug
cpp-maker test release                       # tests release
cpp-maker test --filter add                  # tests contenant "add"
cpp-maker test --junit r.xml                 # rapport JUnit
cpp-maker check                              # debug+asan+tsan
cpp-maker check --modes debug,ubsan          # autre combinaison

# Run
cpp-maker run                                # exécute l'unique binaire
cpp-maker run -m myapp                       # exécutable myapp
cpp-maker run --args --port 8080             # avec arguments

# Clean
cpp-maker clean                              # tout build/
cpp-maker clean release                      # un mode
cpp-maker clean -t host release              # un mode d'une toolchain
cpp-maker clean --all                        # build/ + dist/

# Package/Install/Deploy
cpp-maker package release --format tar.gz,deb
cpp-maker install --prefix /usr/local
cpp-maker deploy rpi
cpp-maker deploy --list

# Diagnostics
cpp-maker doctor                             # auto-fix
cpp-maker doctor --no-fix                    # diagnostic seul
cpp-maker config show build                  # inspecter
cpp-maker config validate                    # valider
cpp-maker config set build.jobs 4            # modifier

# Toolchains
cpp-maker toolchain list
cpp-maker toolchain show armhf
cpp-maker toolchain which gcc
cpp-maker toolchain install 14
cpp-maker toolchain use 14
cpp-maker toolchain remove 12
```

Avec cette documentation, un utilisateur n'ayant jamais vu cpp-maker peut :
1. Créer un projet (`cpp-maker new`) et comprendre son arborescence ;
2. Comprendre les 4 types de modules et la différence mono-module/workspace ;
3. Configurer une toolchain cross-compilée ;
4. Définir un mode personnalisé ;
5. Écrire un `module.toml` complet ;
6. Utiliser les 22 commandes avec toutes leurs options ;
7. Diagnostiquer et résoudre les erreurs courantes ;
8. Intégrer cpp-maker dans un workflow CI (package + deploy) ou IDE (compdb).

---

## 11. Compilation de cpp-maker

### Résumé des commandes

| Commande              | Effet                                              |
|-----------------------|----------------------------------------------------|
| `make release`        | build `-O2` dans `build/release/`                  |
| `make debug`          | build `-O0 -g3` dans `build/debug/`                |
| `make asan`           | build ASan+UBSan dans `build/asan/`                |
| `make tsan`           | build TSan dans `build/tsan/`                      |
| `make test`           | tests de fumée sur le mode courant                 |
| `make test-asan`      | tests sous ASan+UBSan                              |
| `make test-tsan`      | tests sous TSan                                    |
| `make test-valgrind`  | tests sous valgrind (binaire debug)                |
| `make test-all`       | les quatre suites ci-dessus                        |

Les trois modes instrumentés (`asan`, `tsan`, `debug`) coexistent dans
des sous-répertoires distincts : vous pouvez basculer de l'un à l'autre
sans `clean` intermédiaire.
