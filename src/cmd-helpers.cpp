#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

namespace commands::help {

// ---------------------------------------------------------------------------
// Mise en forme
// ---------------------------------------------------------------------------

// Largeur cible pour le texte courant (au-delà : word-wrap). 78 = 80 - 2
// (marge pour un terminal standard). Les lignes indentées (arbres, tableaux)
// échappent au wrap.
static constexpr size_t HELP_WIDTH = 78;

// Imprime `text` avec `indent`, en préservant les sauts de ligne et les
// blocs déjà indentés. Les paragraphes (lignes non indentées) sont rewrappés.
static void print_wrapped(const std::string& text, std::string_view indent) {
	std::istringstream in(text);
	std::string line;
	std::string current;
	auto flush = [&]() {
		if (!current.empty()) { util::outln("{}{}", indent, current); current.clear(); }
	};
	while (std::getline(in, line)) {
		if (line.empty()) { flush(); util::outln(""); continue; }
		if (line[0] == ' ' || line[0] == '\t') {
			flush();
			util::outln("{}{}", indent, line);
			continue;
		}
		std::istringstream words(line);
		std::string word;
		while (words >> word) {
			if (current.empty()) current = word;
			else if (current.size() + 1 + word.size() + indent.size() <= HELP_WIDTH)
				current += " " + word;
			else { flush(); current = word; }
		}
	}
	flush();
}

// Imprime une liste (options, sous-commandes) avec alignement vertical des
// `flags` sur la colonne la plus longue + 2 espaces. Le `description` est
// rewrappé avec un retrait aligné.
static void print_aligned_list(const std::vector<OptionHelp>& items) {
	if (items.empty()) return;
	size_t w = 0;
	for (auto& it : items) w = std::max(w, it.flags.size());
	std::string indent(4 + w + 2, ' ');
	for (auto& it : items) {
		// Reformate la description dans une colonne fixe.
		std::istringstream words(it.description);
		std::string word, current;
		bool first = true;
		auto emit = [&](const std::string& s) {
			if (first) {
				util::outln("    {:<{}}  {}", it.flags, w, s);
				first = false;
			} else {
				util::outln("{}{}", indent, s);
			}
		};
		while (words >> word) {
			if (current.empty()) current = word;
			else if (indent.size() + current.size() + 1 + word.size() <= HELP_WIDTH)
				current += " " + word;
			else { emit(current); current = word; }
		}
		if (!current.empty()) emit(current);
		else if (first) util::outln("    {:<{}}  {}", it.flags, w, "");
	}
}

// ---------------------------------------------------------------------------
// Registre
//
// Chaque entrée documente :
//   - ce que fait la commande (description) ;
//   - comment l'appeler (usage) ;
//   - chaque option (options / subcommands) ;
//   - des exemples concrets (examples) ;
//   - les pièges et effets de bord (notes) ;
//   - les commandes liées (see_also).
// ---------------------------------------------------------------------------
// Options de sanitizer, communes aux commandes qui compilent.
static std::vector<OptionHelp> with_sanitize(std::vector<OptionHelp> v) {
	v.insert(v.end(), {
		{"--mode <nom>",                 "Mode de compilation : release, debug, relwithdebinfo, minsize, asan, tsan, ubsan, msan, lsan, valgrind, ou un mode du projet. Détails : cpp-maker help modes."},
		{"--sanitize <a,b|all>",         "Ajoute des sanitizers au mode (répétable ; alias --sanitizer). Ex. address, thread, memory, leak, undefined, shift, null, bounds, cfi… « all » : une passe par runtime (address+undefined, thread, leak, memory, hwaddress). Liste et explications : cpp-maker help sanitize."},
		{"--sanitize-trap <a,b|all>",    "Remplace les rapports textuels par une instruction trap (binaire plus petit, pas de runtime de rapport)."},
		{"--sanitize-ignorelist <f>",    "Fichier listant fonctions / sources à ne pas instrumenter (clang)."},
		{"--no-sanitize-recover <a,b|all>", "Arrête le programme (abort) à la première erreur au lieu d'avertir et continuer."},
	});
	return v;
}

const std::vector<CommandSpec>& registry() {
	static const std::vector<CommandSpec> r = {
		// ---------------------------------------------------------------------
		{
			.name = "new",
			.aliases = {},
			.brief = "Crée un projet complet (mono-module ou workspace).",
			.description =
				"Crée un nouveau projet cpp-maker dans le dossier <nom>.\n"
				"\n"
				"Deux structures possibles :\n"
				"  - mono-module (défaut) : un seul module, plus simple pour\n"
				"    démarrer. Les sources vont dans src/, les en-têtes publics\n"
				"    dans include/<nom>/, les tests dans tests/.\n"
				"  - workspace (--workspace) : plusieurs modules dans des\n"
				"    dossiers séparés, chacun avec son module.toml. Recommandé\n"
				"    dès qu'un projet dépasse ~5000 lignes ou doit produire\n"
				"    plusieurs binaires.\n"
				"\n"
				"Types de projet (--type) :\n"
				"  normal       exécutable (binaire avec main()) — défaut\n"
				"  static       bibliothèque statique (lib<nom>.a)\n"
				"  shared       bibliothèque partagée (lib<nom>.so/.dylib/.dll)\n"
				"  header-only  uniquement des en-têtes\n"
				"  modular      alias de --workspace (libs/core + apps/<nom>)\n"
				"\n"
				"Avant toute création, cpp-maker vérifie qu'un compilateur\n"
				"compile effectivement un fichier trivial dans le standard\n"
				"demandé. Si le premier candidat n'est pas le compilateur du\n"
				"PATH (/usr/bin/g++), il est épinglé dans [toolchains.host]\n"
				"pour garantir la reproductibilité.",
			.usage =
				"cpp-maker new <nom> [options]",
			.options = {
				{"--lang <c|c++>",           "Langage principal du projet (défaut : c++)."},
				{"--type <type>",            "Type de projet : normal | static | shared | header-only | modular (défaut : normal). Voir description."},
				{"--workspace",              "Crée un workspace (cpp-maker.toml avec [workspace] + chaque module dans son dossier)."},
				{"--standard <c++NN>",       "Standard C++ (c++17, c++20, c++23…). Défaut : c++23."},
				{"--c-standard <cNN>",       "Standard C (c11, c17, c23…). Défaut : c17."},
				{"--compiler <chemin>",      "Compilateur C++ imposé (au lieu de l'auto-détection). Ex. : /opt/gcc-14/bin/g++."},
			},
			.subcommands = {},
			.examples =
				"# Exécutable C++ (mode mono-module, le plus simple)\n"
				"cpp-maker new hello\n"
				"\n"
				"# Workspace avec un module lib + un module app\n"
				"cpp-maker new myapp --workspace\n"
				"\n"
				"# Bibliothèque C statique\n"
				"cpp-maker new mylib --type static --lang c\n"
				"\n"
				"# Projet avec un standard précis et un compilateur épinglé\n"
				"cpp-maker new legacy --standard c++17 --compiler /usr/bin/g++-12",
			.notes = {
				"Échoue si le dossier existe déjà.",
				"Ne compile rien : lancez 'cpp-maker build' ensuite.",
				"Génère README.md et .gitignore, mais pas de dépôt Git.",
			},
			.see_also = {"add", "build", "doctor"},
			.run = [](const std::vector<std::string>& a) { commands::cmd_new(a); return 0; }
		},

		// ---------------------------------------------------------------------
		{
			.name = "add",
			.aliases = {},
			.brief = "Ajoute un module à un workspace existant.",
			.description =
				"Ajoute un nouveau module à un workspace et l'enregistre dans\n"
				"[workspace].modules de cpp-maker.toml. Le dossier est créé\n"
				"avec un module.toml prêt à l'emploi, plus des sources\n"
				"d'exemple (sauf --no-scaffold) :\n"
				"  - src/<nom>.cpp (ou .c)     une fonction add(a,b)\n"
				"  - include/<nom>/<nom>.hpp    l'en-tête public\n"
				"  - src/main.cpp               si --type executable\n"
				"  - tests/test_<nom>.cpp       un test de base\n"
				"\n"
				"Le chemin du module est déterminé ainsi :\n"
				"  - nom simple  (\"core\")       → <--dir>/<nom>   (défaut libs/core)\n"
				"  - chemin relatif (\"apps/gui\") → depuis le dossier courant,\n"
				"                                  ramené relatif à la racine\n"
				"  - chemin absolu                → tel quel\n"
				"\n"
				"Si une dépendance est une bibliothèque dont l'en-tête public\n"
				"déclare add(int,int), le main() généré l'appelle.",
			.usage =
				"cpp-maker add <nom|chemin> [options]",
			.options = {
				{"--type <type>",         "executable | static | shared | header-only (défaut : static)."},
				{"--lang <c|c++>",        "Langage des sources générées (défaut : c++)."},
				{"--deps <a,b,c>",        "Modules dont dépend le nouveau module (séparés par virgule)."},
				{"--name <nom>",          "Nom logique (par défaut : le nom du dossier)."},
				{"--dir <dossier>",       "Dossier par défaut si <nom> est un nom simple (défaut : libs)."},
				{"--no-scaffold",         "Ne génère pas les sources d'exemple, seulement le module.toml."},
			},
			.subcommands = {},
			.examples =
				"# Module bibliothèque statique dans libs/core\n"
				"cpp-maker add core\n"
				"\n"
				"# Module exécutable dépendant de core\n"
				"cpp-maker add net --type static --deps core\n"
				"cpp-maker add gui --dir apps --type executable --deps net\n"
				"\n"
				"# Chemin explicite (relatif au dossier courant)\n"
				"cpp-maker add libs/util\n"
				"\n"
				"# Coquille vide (module.toml seul)\n"
				"cpp-maker add test --no-scaffold --type header-only",
			.notes = {
				"La modification de cpp-maker.toml est atomique : en cas d'erreur de validation, tout est restauré.",
				"Refuse les dépendances inexistantes et les noms déjà pris.",
				"Requiert un workspace ; sinon utilisez 'cpp-maker new'.",
			},
			.see_also = {"new", "module", "build"},
			.run = commands::cmd_add
		},

		// ---------------------------------------------------------------------
		{
			.name = "module",
			.aliases = {},
			.brief = "Gère les modules d'un workspace (add, rename, remove, deps).",
			.description =
				"Point d'entrée unique pour manipuler les modules d'un workspace.\n"
				"Chaque sous-commande édite les fichiers TOML concernés de façon\n"
				"atomique : revalidation complète de la configuration, puis\n"
				"remplacement ou restauration.",
			.usage = "cpp-maker module <sous-commande> [args]",
			.options = {},
			.subcommands = {
				{"add <nom|chemin> [opts]",    "Alias de 'cpp-maker add'. Voir 'cpp-maker help add'."},
				{"rename <ancien> <nouveau>",  "Renomme un module. Met à jour name, les dépendances des autres modules, et le dossier si le nom du dossier == ancien nom. Rollback complet en cas d'erreur."},
				{"remove <nom> [--force] [--purge]", "Retire le module du workspace. Refuse si d'autres modules en dépendent (sauf --force). --purge supprime aussi le dossier."},
				{"deps <list|add|remove> <module> [<nom>]", "Gère les dépendances d'un module (voir ci-dessous)."},
			},
			.examples =
				"# Renommer un module (met à jour les deps et le dossier)\n"
				"cpp-maker module rename core base\n"
				"\n"
				"# Retirer un module dont plus personne ne dépend\n"
				"cpp-maker module remove core\n"
				"\n"
				"# Retirer un module encore référencé (deps nettoyées) et purger le dossier\n"
				"cpp-maker module remove exp --force --purge\n"
				"\n"
				"# Dépendances d'un module\n"
				"cpp-maker module deps list core\n"
				"cpp-maker module deps add app zlib --pkg-config\n"
				"cpp-maker module deps add app pthread --system\n"
				"cpp-maker module deps remove app zlib",
			.notes = {
				"rename et remove refusent les configurations v0.4 (plusieurs modules dans un seul fichier).",
				"module deps add déduit le type : --module/--pkg-config/--system, sinon pkg-config si le paquet existe, sinon système.",
				"Un nom de dépendance commençant par -l est normalisé (-lz → z).",
			},
			.see_also = {"add", "new", "deps"},
			.run = commands::cmd_module
		},

		// ---------------------------------------------------------------------
		{
			.name = "deps",
			.aliases = {},
			.brief = "Gère les dépendances Git et leur cache local.",
			.description =
				"Résout, verrouille et met à jour les dépendances Git déclarées\n"
				"dans [dependencies.git.<alias>] des module.toml / cpp-maker.toml.\n"
				"\n"
				"Fonctionnement :\n"
				"  - clone --mirror dans ~/.cache/cpp-maker/git/<hash>.git\n"
				"  - checkout dans ~/.cache/cpp-maker/git/<hash>/<commit16>\n"
				"  - le commit résolu est figé dans cpp-maker.lock à la racine\n"
				"\n"
				"Emplacement du cache, par ordre de priorité :\n"
				"  $CPP_MAKER_CACHE/git\n"
				"  $XDG_CACHE_HOME/cpp-maker/git\n"
				"  ~/.cache/cpp-maker/git\n"
				"  %LOCALAPPDATA%\\cpp-maker\\git (Windows)\n"
				"\n"
				"Le lock évite les upgrades silencieux : pour changer de version,\n"
				"modifiez 'version' puis lancez 'cpp-maker deps update'.",
			.usage = "cpp-maker deps <list|update|clean>",
			.options = {},
			.subcommands = {
				{"list (défaut)", "Affiche les alias, URLs et commits verrouillés."},
				{"update",       "Ignore le lock, relance la résolution, réécrit cpp-maker.lock."},
				{"clean",        "Supprime le cache local ~/.cache/cpp-maker/git."},
			},
			.examples =
				"cpp-maker deps list\n"
				"cpp-maker deps update\n"
				"cpp-maker deps clean",
			.notes = {
				"Le lock (cpp-maker.lock) est versionné avec le projet : committez-le.",
				"Format d'une dép :\n"
				"    [dependencies.git.mylib]\n"
				"    url     = \"https://github.com/user/mylib\"\n"
				"    version = \"v2.1.0\"   # tag, branche ou commit\n"
				"    module  = \"core\"     # module à importer (défaut : nom du dépôt)\n"
				"    subdir  = \"\"         # sous-dossier",
			},
			.see_also = {"module", "build"},
			.run = commands::cmd_deps
		},

		// ---------------------------------------------------------------------
		{
			.name = "build",
			.aliases = {},
			.brief = "Compile le projet (incrémental, parallèle, multi-toolchain).",
			.description =
				"Compile les modules demandés et leurs dépendances dans\n"
				"build/<toolchain>/<mode>/. La compilation est incrémentale :\n"
				"seules les tâches dont les entrées ont changé sont ré-exécutées.\n"
				"\n"
				"L'empreinte d'une tâche couvre :\n"
				"  - la version du compilateur\n"
				"  - la ligne de commande complète\n"
				"  - pour chaque entrée : chemin, taille, mtime\n"
				"  - les #include transitifs (via -MMD -MP)\n"
				"Un simple 'touch' sur un header inclus recompile tous les\n"
				"fichiers qui l'incluent.\n"
				"\n"
				"Artefacts produits :\n"
				"  build/<tc>/<mode>/bin/     exécutables\n"
				"  build/<tc>/<mode>/lib/     bibliothèques statiques et partagées\n"
				"  build/<tc>/<mode>/obj/     fichiers .o intermédiaires\n"
				"  build/<tc>/<mode>/tests/   exécutables de test (si --tests)\n"
				"\n"
				"La compilation est parallèle. Par défaut, workers = cœurs - 1\n"
				"(cpp-maker réserve un cœur pour lui-même). Vous pouvez\n"
				"demander moins avec -j, mais pas plus que le budget.",
			.usage = "cpp-maker build [mode] [options]",
			.options = with_sanitize({
				{"-m, --module <a,b>",     "Modules à construire. Défaut : tous, ou le module courant si vous êtes dans un sous-dossier de workspace. Les dépendances sont tirées automatiquement."},
				{"-t, --toolchain <a,b>",  "Toolchains à utiliser (séparées par virgule). 'all' = toutes celles configurées. Défaut : celle du projet."},
				{"--backend <native|ninja>", "Backend d'exécution. 'native' (défaut) : DAG interne. 'ninja' : génère build.ninja et l'exécute (ninja requis dans le PATH)."},
				{"--tests",                "Compile aussi les exécutables de test."},
				{"-j N, --jobs N",         "Workers parallèles. 0 = auto = cœurs - 1."},
				{"-k, --keep-going",       "Continue après un échec de tâche."},
				{"-n, --dry-run",          "Affiche les tâches à exécuter sans rien lancer."},
				{"-v, --verbose",          "Affiche chaque commande exécutée."},
				{"--clean",                "Supprime build/<tc>/<mode>/ avant le build."},
			}),
			.subcommands = {},
			.examples =
				"# Build debug (mode par défaut)\n"
				"cpp-maker build\n"
				"\n"
				"# Build release sur toutes les toolchains\n"
				"cpp-maker build release -t all\n"
				"\n"
				"# Build ninja + tests, 4 workers\n"
				"cpp-maker build --backend ninja --tests -j 4\n"
				"\n"
				"# Aperçu sans exécution\n"
				"cpp-maker build -n\n"
				"\n"
				"# Rebuild from scratch\n"
				"cpp-maker build --clean\n"
				"\n"
				"# Debug + ASan + UBSan ciblé, arrêt à la première erreur\n"
				"cpp-maker build debug --sanitize address,shift,null --no-sanitize-recover all\n"
				"# → build/<tc>/debug+address+shift+null+norecover/",
			.notes = {
				"Un verrou est posé sur .cpp-maker.lock pendant toute la durée du build.",
				"compile_commands.json est régénéré à chaque build (à côté du build.ninja).",
				"Les étapes [generate.*] sont exécutées automatiquement avant le build.",
				"Avec --sanitize*, le mode dérivé a son propre dossier (ex. debug+address) : les objets du mode d'origine ne sont pas écrasés.",
			},
			.see_also = {"run", "test", "clean", "generate"},
			.run = commands::cmd_build
		},

		// ---------------------------------------------------------------------
		{
			.name = "run",
			.aliases = {},
			.brief = "Compile et exécute un module exécutable.",
			.description =
				"Compile le module cible (via le même moteur que 'build') puis\n"
				"exécute son binaire. Le verrou de compilation est relâché avant\n"
				"l'exécution, ce qui permet au programme invoqué d'appeler\n"
				"cpp-maker à son tour.\n"
				"\n"
				"Chaîne de lancement (selon la toolchain et le mode) :\n"
				"  [runner cible] [runner mode] binaire [arguments…]\n"
				"Par exemple, pour une cible ARM avec --mode valgrind :\n"
				"  qemu-arm -L /usr/arm-linux-gnueabihf valgrind --leak-check=full …\n"
				"\n"
				"Choix du module cible :\n"
				"  - -m <nom>                    explicite\n"
				"  - module courant               si vous êtes dans apps/<nom>/\n"
				"  - unique exécutable            si le projet n'en a qu'un\n"
				"  - sinon                        erreur avec la liste",
			.usage = "cpp-maker run [mode] [options] [-- ARG…]",
			.options = with_sanitize({
				{"-m, --module <nom>",  "Module exécutable à lancer. Un seul à la fois."},
				{"-t, --toolchain <tc>", "Toolchain cible (une seule). Défaut : celle du projet."},
				{"--args ARG…",         "Arguments transmis à l'exécutable (aucune interprétation par cpp-maker)."},
				{"-- ARG…",             "Idem --args (séparateur standard)."},
				{"-v, --verbose",       "Affiche la ligne de commande lancée."},
				{"-n, --dry-run",       "Compile seulement, n'exécute pas."},
			}),
			.subcommands = {},
			.examples =
				"# Lance l'unique exécutable du projet\n"
				"cpp-maker run\n"
				"\n"
				"# Lance myapp en mode release\n"
				"cpp-maker run release -m myapp\n"
				"\n"
				"# Transmet des arguments\n"
				"cpp-maker run --args --port 8080 --verbose\n"
				"cpp-maker run -- --port 8080 --verbose\n"
				"\n"
				"# Lance sous valgrind (raccourci : cpp-maker valgrind …)\n"
				"cpp-maker run --mode valgrind",
			.notes = {
				"Relâche le verrou avant exécution : un programme long peut coexister avec un autre build.",
				"Si aucun runner n'est disponible pour la cible (cross), l'exécution est refusée (code 2).",
			},
			.see_also = {"build", "test"},
			.run = commands::cmd_run
		},

		// ---------------------------------------------------------------------
		{
			.name = "test",
			.aliases = {},
			.brief = "Compile et exécute les tests d'un mode.",
			.description =
				"Compile les exécutables de test (dans tests/ de chaque module\n"
				"sélectionné) puis les lance. Le résultat de chaque exécutable\n"
				"est agrégé et un résumé est affiché.\n"
				"\n"
				"Squelette de test fourni par cpp-maker (tests/test.hpp) :\n"
				"  TEST(Nom)          { CHECK(x); CHECK_EQ(a,b); }\n"
				"  TEST(Suite, Nom)   { EXPECT_TRUE(x); ASSERT_EQ(a,b); }\n"
				"  SKIP(\"raison\")     marque le test comme ignoré\n"
				"\n"
				"Codes retour de l'exécutable de test :\n"
				"  0  tous les tests exécutés réussissent\n"
				"  1  au moins un échec\n"
				"  77 tous les tests exécutés ont été SKIPpés\n"
				"  \n"
				"Le runner (qemu/wine) et --filter sont transmis à l'exécutable.\n"
				"Le timeout par défaut est [tests].timeout (300 s).",
			.usage = "cpp-maker test [mode] [options]",
			.options = with_sanitize({
				{"-m, --module <a,b>",       "Modules dont on teste les modules (défaut : tous / courant)."},
				{"-t, --toolchain <tc>",     "Une seule toolchain."},
				{"--filter <sous-chaîne>",   "Ne lance que les tests dont le nom contient cette sous-chaîne."},
				{"--junit <fichier.xml>",    "Écrit un rapport JUnit XML (agrégation multi-suite)."},
				{"--timeout <secondes>",     "Timeout par exécutable (défaut : [tests].timeout)."},
				{"-- ARG…",                  "Arguments supplémentaires transmis à chaque exécutable."},
			}),
			.subcommands = {},
			.examples =
				"# Tests en mode debug\n"
				"cpp-maker test\n"
				"\n"
				"# Tests en mode release avec filtre\n"
				"cpp-maker test release --filter add\n"
				"\n"
				"# Rapport JUnit pour la CI\n"
				"cpp-maker test --junit report.xml\n"
				"\n"
				"# Cible ARM via qemu-arm (si la toolchain le permet)\n"
				"cpp-maker test -t armhf release",
			.notes = {
				"Les modules sans dossier tests/ sont ignorés silencieusement.",
				"Un code 77 est compté comme 'skip' dans le résumé et le JUnit.",
				"Le répertoire de travail de l'exécutable est la racine du projet.",
			},
			.see_also = {"check", "run", "build"},
			.run = commands::cmd_test
		},

		// ---------------------------------------------------------------------
		{
			.name = "check",
			.aliases = {},
			.brief = "Exécute les tests dans plusieurs modes (debug, asan, tsan).",
			.description =
				"Compile et exécute les tests dans chaque mode demandé, en\n"
				"accumulant les résultats dans un seul rapport. Utile pour la\n"
				"CI et pour valider un commit sur plusieurs configurations.\n"
				"\n"
				"Modes par défaut : debug, asan, tsan\n"
				"(détection d'erreurs mémoire + data races).\n"
				"\n"
				"Si un mode échoue à compiler, une entrée 'build/fail' est\n"
				"ajoutée au rapport ; les autres modes continuent.",
			.usage = "cpp-maker check [options]",
			.options = with_sanitize({
				{"--modes <m1,m2,…>",     "Modes à tester (défaut : debug,asan,tsan)."},
				{"-t, --toolchain <tc>",  "Une seule toolchain."},
				{"--filter <sous-chaîne>","Filtre sur le nom du test."},
				{"--junit <fichier.xml>", "Rapport JUnit agrégé (toutes suites, tous modes)."},
				{"--timeout <secondes>",  "Timeout par exécutable."},
			}),
			.examples =
				"# Vérifie debug + asan + tsan\n"
				"cpp-maker check\n"
				"\n"
				"# Ajoute ubsan, retire tsan\n"
				"cpp-maker check --modes debug,asan,ubsan\n"
				"\n"
				"# Rapport JUnit complet\n"
				"cpp-maker check --junit ci.xml",
			.notes = {
				"Les modes asan/tsan/ubsan/msan/lsan peuvent être indisponibles sur certaines toolchains cross (msan : clang uniquement).",
				"--sanitize s'applique à chaque mode testé (ex. --modes debug,release --sanitize undefined).",
				"Retourne 1 si au moins un mode a échoué.",
			},
			.see_also = {"test"},
			.run = commands::cmd_check
		},

		// ---------------------------------------------------------------------
		{
			.name = "generate",
			.aliases = {},
			.brief = "Génère build.ninja ou compile_commands.json.",
			.description =
				"Écrit des fichiers pour des outils externes :\n"
				"  - ninja : build/<tc>/<mode>/build.ninja exécutable par\n"
				"            'ninja -f build/host/release/build.ninja'\n"
				"  - compdb : compile_commands.json (clangd, VSCode C/C++,\n"
				"            ccls…). Écrit à la racine du projet.\n"
				"\n"
				"Utile si vous préférez ninja au backend natif, ou pour\n"
				"l'intégration IDE. 'build' régénère déjà compdb dans le\n"
				"répertoire du build à chaque invocation.",
			.usage = "cpp-maker generate <ninja|compdb> [mode] [options]",
			.options = {
				{"-m, --module <a,b>",    "Modules à inclure (défaut : tous / courant)."},
				{"-t, --toolchain <a,b>", "Toolchains à générer."},
			},
			.subcommands = {},
			.examples =
				"# build.ninja pour le mode release\n"
				"cpp-maker generate ninja release\n"
				"\n"
				"# Lancer ensuite depuis la racine\n"
				"ninja -f build/host/release/build.ninja\n"
				"\n"
				"# compile_commands.json à la racine\n"
				"cpp-maker generate compdb",
			.notes = {
				"--backend ninja sur 'build' combine génération et exécution.",
				"Le fichier compdb suit la spec clangd (arguments[], output, directory).",
			},
			.see_also = {"build"},
			.run = commands::cmd_generate
		},

		// ---------------------------------------------------------------------
		{
			.name = "clean",
			.aliases = {},
			.brief = "Supprime les artefacts de build (et dist avec --all).",
			.description =
				"Sans argument, supprime tout build/. Avec -t <tc>, limite à\n"
				"build/<tc>/. Avec <mode>, limite en plus au mode.\n"
				"\n"
				"Ne touche jamais aux sources, ni au cache Git, ni au lock\n"
				"(le lock est posé pendant l'opération, pour éviter un clean\n"
				"concurrent avec un build).",
			.usage = "cpp-maker clean [mode] [options]",
			.options = {
				{"-t, --toolchain <a,b>", "Nettoie seulement build/<tc>/."},
				{"--all",                 "Supprime aussi dist/ (paquets produits)."},
			},
			.subcommands = {},
			.examples =
				"cpp-maker clean\n"
				"cpp-maker clean release\n"
				"cpp-maker clean -t host release\n"
				"cpp-maker clean --all",
			.notes = {
				"Aucun effet sur build/generate/ (stamps des étapes [generate.*]) hors --all.",
			},
			.see_also = {"build"},
			.run = commands::cmd_clean
		},

		// ---------------------------------------------------------------------
		{
			.name = "package",
			.aliases = {},
			.brief = "Produit les paquets installables (tar.gz, tar.xz, zip, deb).",
			.description =
				"Compile (si nécessaire), copie les artefacts dans un staging,\n"
				"puis crée les archives demandées dans dist/.\n"
				"\n"
				"Contenu typique :\n"
				"  bin/                     exécutables\n"
				"  lib/                     .a, .so/.dylib/.dll, .pc\n"
				"  include/                 en-têtes publics (par module)\n"
				"  share/doc/<nom>/         README, LICENSE, extra_files\n"
				"\n"
				"Les binaires sont strippés si [package].strip = true et que le\n"
				"mode n'inclut pas -g. Chaque module avec install = true produit\n"
				"son fichier .pc (pkg-config).\n"
				"\n"
				"Un checksum SHA256 est ajouté si [package].checksums = true.",
			.usage = "cpp-maker package [mode] [options]",
			.options = {
				{"--format <f1,f2>",       "Formats : tar.gz | tar.xz | zip | deb | stage. Défaut : [package].formats."},
				{"--no-strip",             "Ne pas stripper, même si le mode est release."},
				{"-t, --toolchain <a,b>",  "Toolchains pour lesquelles packager."},
				{"-m, --module <a,b>",     "Modules à inclure."},
			},
			.subcommands = {},
			.examples =
				"# Archive tar.gz en mode release\n"
				"cpp-maker package\n"
				"\n"
				"# Plusieurs formats d'un coup\n"
				"cpp-maker package release --format tar.gz,deb\n"
				"\n"
				"# Pour ARM\n"
				"cpp-maker package release -t armhf --format tar.xz",
			.notes = {
				"deb requiert dpkg-deb dans le PATH.",
				"zip requiert zip ; tar.gz/tar.xz requièrent tar.",
				"Le nom du fichier suit le schéma <nom>-<version>-<os>-<arch>.<fmt>.",
			},
			.see_also = {"install", "deploy"},
			.run = commands::cmd_package
		},

		// ---------------------------------------------------------------------
		{
			.name = "install",
			.aliases = {},
			.brief = "Installe localement dans <prefix> (ou <destdir><prefix>).",
			.description =
				"Compile puis copie les artefacts, en-têtes publics et fichiers\n"
				".pc dans un préfixe local. Équivalent d'un 'make install' mais\n"
				"piloté par la config ([package] et flags install par module).\n"
				"\n"
				"--destdir permet d'installer dans un répertoire de staging\n"
				"(pour construire un paquet Debian, un conteneur, etc.) :\n"
				"   chemin effectif = <destdir><prefix>",
			.usage = "cpp-maker install [mode] [options]",
			.options = {
				{"--prefix <chemin>",    "Préfixe d'installation (défaut : /usr/local)."},
				{"--destdir <chemin>",   "Racine alternative ; installe dans <destdir><prefix>."},
				{"--no-strip",           "Ne pas stripper."},
				{"-t, --toolchain <tc>", "Une seule toolchain."},
			},
			.subcommands = {},
			.examples =
				"cpp-maker install\n"
				"cpp-maker install --prefix /opt/myapp\n"
				"cpp-maker install --destdir /tmp/stage --prefix /usr",
			.notes = {
				"Écrit build/<tc>/<mode>/install_manifest.txt (liste des fichiers posés).",
				"Les modules avec install = false sont ignorés.",
			},
			.see_also = {"package", "deploy"},
			.run = commands::cmd_install
		},

		// ---------------------------------------------------------------------
		{
			.name = "deploy",
			.aliases = {},
			.brief = "Build + package + transfert vers une cible distante.",
			.description =
				"Déploie un projet sur une cible déclarée dans [deploy.<nom>].\n"
				"Enchaîne : build, package (si artifact != stage), puis\n"
				"transfert par rsync/scp/copy/script.\n"
				"\n"
				"Méthodes :\n"
				"  rsync    delta + compression + --delete optionnel\n"
				"  scp      copie brute (portable)\n"
				"  copy     local (pas de SSH)\n"
				"  script   lance un script local avec $CPP_MAKER_DIST\n"
				"\n"
				"Variables d'expansion dans path / before / after / script :\n"
				"  {name}  {version}  {toolchain}  {mode}  {host}\n"
				"  {artifact}  {path}\n"
				"\n"
				"Ordre d'exécution :\n"
				"  before (local) → mkdir distant → transfert\n"
				"  → after (distant) → after_local (local)",
			.usage = "cpp-maker deploy <cible> [options]",
			.options = {
				{"--list",                "Affiche les cibles configurées."},
				{"-t, --toolchain <tc>",  "Force une toolchain (au lieu de celle de la cible)."},
				{"-m, --module <a,b>",    "Force une liste de modules."},
				{"-n, --dry-run",         "Simule sans transférer."},
				{"-v, --verbose",         "Affiche toutes les commandes distantes."},
			},
			.subcommands = {},
			.examples =
				"# Voir les cibles\n"
				"cpp-maker deploy --list\n"
				"\n"
				"# Déployer sur rpi (déclaré dans [deploy.rpi])\n"
				"cpp-maker deploy rpi\n"
				"\n"
				"# Simulation\n"
				"cpp-maker deploy rpi --dry-run",
			.notes = {
				"Un échec de transfert interrompt le déploiement (code non nul).",
				"Pour du staging simple, préférez 'cpp-maker package' + copie manuelle.",
			},
			.see_also = {"package", "install"},
			.run = commands::cmd_deploy
		},

		// ---------------------------------------------------------------------
		{
			.name = "graph",
			.aliases = {},
			.brief = "Affiche le graphe des dépendances entre modules.",
			.description =
				"Sans --dot : arbre ASCII depuis chaque module racine (les\n"
				"modules qui ne sont dépendance d'aucun autre). Avec -a/--all,\n"
				"affiche aussi les dépendances.\n"
				"\n"
				"Avec --dot : sortie Graphviz (digraph). Les modules sont\n"
				"regroupés en sous-graphes par couche, si [layers] est\n"
				"configuré. Les dépendances pkg-config apparaissent en\n"
				"pointillés (style=dashed).",
			.usage = "cpp-maker graph [options]",
			.options = {
				{"--dot",  "Sortie Graphviz à piper vers 'dot'."},
				{"-a, --all", "Inclut les dépendances dans l'arbre."},
			},
			.subcommands = {},
			.examples =
				"# Arbre lisible dans le terminal\n"
				"cpp-maker graph\n"
				"\n"
				"# Graphviz SVG\n"
				"cpp-maker graph --dot | dot -Tsvg > graph.svg",
			.notes = {
				"Profondeur limitée à 32 pour éviter les cycles non détectés.",
			},
			.see_also = {"modules", "info"},
			.run = commands::cmd_graph
		},

		// ---------------------------------------------------------------------
		{
			.name = "modules",
			.aliases = {},
			.brief = "Liste les modules du workspace.",
			.description =
				"Affiche un tableau : nom, type, couche, dossier, dépendances\n"
				"(modules, pkg-config, système).",
			.usage = "cpp-maker modules",
			.options = {},
			.subcommands = {},
			.examples = "cpp-maker modules",
			.notes = {},
			.see_also = {"graph", "info"},
			.run = commands::cmd_modules
		},

		// ---------------------------------------------------------------------
		{
			.name = "info",
			.aliases = {},
			.brief = "Résumé du projet, ou détail d'un module (-m).",
			.description =
				"Sans argument : affiche un résumé du projet (racine, standards,\n"
				"défauts, modes, toolchains, modules, déploiements) et le détail de\n"
				"la toolchain par défaut.\n"
				"\n"
				"Avec -m <nom> : affiche la configuration complète du module demandé —\n"
				"type, couche, version, description, sources réellement découvertes,\n"
				"includes (public/private), dépendances (modules, pkg-config, système,\n"
				"git), flags, overrides de plateforme, fermeture transitive et\n"
				"modules qui l'utilisent (reverse deps).\n"
				"\n"
				"Recherche du module : d'abord par nom logique (champ name = du\n"
				"module.toml), puis en secours par nom de dossier. Ce second mode\n"
				"rend '-m ui_patchbay' fonctionnel quand le dossier s'appelle\n"
				"ui_patchbay mais que module.toml déclare name = \"audio_patchbay\".",
			.usage = "cpp-maker info [-m <module>[,<module>…]]",
			.options = {
				{"-m, --module <a,b>", "Affiche le détail des modules demandés. Sans cet argument, affiche le résumé du projet."},
			},
			.subcommands = {},
			.examples =
				"# Résumé du projet (comportement historique)\n"
				"cpp-maker info\n"
				"\n"
				"# Détail d'un module par son nom logique\n"
				"cpp-maker info -m audio_patchbay\n"
				"\n"
				"# Détail par nom de dossier (si différent du name)\n"
				"cpp-maker info -m ui_patchbay\n"
				"\n"
				"# Plusieurs modules d'un coup\n"
				"cpp-maker info -m audio_patchbay,core",
			.notes = {
				"Sans -m, sortie identique aux versions précédentes.",
				"Le détail inclut la fermeture transitive des dépendances et les sources effectivement découvertes (vérifie exclude et source_files).",
				"Utile pour diagnostiquer 'mon module ne compile pas les bons fichiers' : la liste des sources affichées est celle qui sera réellement compilée.",
			},
			.see_also = {"modules", "graph", "config"},
			.run = [](const std::vector<std::string>& a) { return commands::cmd_info(a); }
		},

		// ---------------------------------------------------------------------
		{
			.name = "size",
			.aliases = {},
			.brief = "Analyse ELF des binaires construits (text/data/bss/dec).",
			.description =
				"Analyse les binaires de build/<tc>/<mode>/{bin,lib,tests} avec\n"
				"un parser ELF intégré (aucune dépendance à binutils — donc\n"
				"lisible pour les binaires croisés même sans binutils cible).\n"
				"\n"
				"Colonnes (sémantique Berkeley de 'size') :\n"
				"  text   sections ALLOC non inscriptibles (code, .rodata)\n"
				"  data   sections ALLOC inscriptibles (.data…)\n"
				"  bss    sections SHT_NOBITS allocables (.bss, .tbss)\n"
				"  dec    text + data + bss\n"
				"  hex    dec en hexadécimal\n"
				"\n"
				"Pour les archives .a, les membres ELF sont sommés.",
			.usage = "cpp-maker size [mode] [options]",
			.options = {
				{"-m, --module <a,b>",    "Filtre : exécutables <nom>, <nom>-tests, lib<nom>.*."},
				{"-t, --toolchain <a,b>", "Toolchains à inspecter."},
				{"-v, --verbose",         "Top 10 des sections par taille."},
			},
			.subcommands = {},
			.examples =
				"# Taille des binaires release\n"
				"cpp-maker size release\n"
				"\n"
				"# Détail des sections du module hello\n"
				"cpp-maker size release -m hello -v",
			.notes = {
				"Nécessite d'avoir lancé 'cpp-maker build' au préalable.",
				"Les fichiers PE (Windows) ne sont pas parsés.",
			},
			.see_also = {"build"},
			.run = commands::cmd_size
		},

		// ---------------------------------------------------------------------
		{
			.name = "doctor",
			.aliases = {},
			.brief = "Diagnostic de l'environnement + auto-fix des deps implicites.",
			.description =
				"Vérifie la configuration, les outils externes, les toolchains\n"
				"et les dépendances entre modules.\n"
				"\n"
				"1. Configuration : cpp-maker.toml présent et valide.\n"
				"2. Outils : ar, pkg-config, ninja, tar, zip, dpkg-deb,\n"
				"   sha256sum, rsync, ssh, valgrind, qemu-arm, qemu-aarch64,\n"
				"   wine (chacun avec son usage).\n"
				"3. Toolchains : smoke_compile C et C++, sanitizers,\n"
				"   disponibilité de l'exécution cross (runner).\n"
				"4. Dépendances entre modules : scanne les #include et détecte\n"
				"   celles qui manquent. Par défaut, corrige les module.toml\n"
				"   concernés (revalidation + rollback).\n"
				"\n"
				"Code retour : 0 si aucun problème, 1 sinon.",
			.usage = "cpp-maker doctor [options]",
			.options = {
				{"--no-fix",      "Signale les deps manquantes sans les corriger."},
				{"--check-deps",  "Idem --no-fix et compte les deps manquantes comme des problèmes (utile en CI)."},
			},
			.subcommands = {},
			.examples =
				"# Diagnostic complet + auto-fix\n"
				"cpp-maker doctor\n"
				"\n"
				"# Diagnostic sans modification\n"
				"cpp-maker doctor --no-fix\n"
				"\n"
				"# Vérification stricte (CI)\n"
				"cpp-maker doctor --check-deps",
			.notes = {
				"L'auto-fix n'édite que les module.toml dédiés ; les configurations v0.4 (modules partagés) sont refusées.",
				"Les problèmes bloquants (ar manquant, config invalide) ont toujours un impact sur le code retour.",
			},
			.see_also = {"config", "toolchain"},
			.run = commands::cmd_doctor
		},

		// ---------------------------------------------------------------------
		{
			.name = "config",
			.aliases = {},
			.brief = "Inspecte, valide ou modifie cpp-maker.toml.",
			.description =
				"Manipule la configuration effective (après défauts, sans\n"
				"recharger les fichiers TOML à la main).",
			.usage = "cpp-maker config <show|validate|set> [args]",
			.subcommands = {
				{"show [clé…]",            "Affiche la configuration au format clé = valeur. Sans argument : tout. Avec arguments : clé exacte ou préfixe (build, toolchains.host, modes.release…)."},
				{"validate",               "Charge et valide la configuration. Code 0 = valide, 1 = invalide (message d'erreur complet)."},
				{"set <clé> <valeur>",     "Modifie une clé scalaire autorisée (project.name, build.jobs…). Édite cpp-maker.toml, revalide, remplace atomiquement. Refuse et restaure en cas d'erreur."},
			},
			.examples =
				"cpp-maker config show\n"
				"cpp-maker config show build\n"
				"cpp-maker config show build.jobs\n"
				"cpp-maker config validate\n"
				"cpp-maker config set build.jobs 4\n"
				"cpp-maker config set project.version \"1.2.0\"",
			.notes = {
				"config set refuse les clés absentes du fichier (pas de création).",
				"config show indique une suggestion en cas de clé inconnue.",
				"Liste blanche de set : project.{name,version,type,standard,c_standard,description}, module.type, compiler.{cxx,cc,family}, build.{default_mode,default_toolchain,jobs}.",
			},
			.see_also = {"doctor", "toolchain"},
			.run = commands::cmd_config
		},

		// ---------------------------------------------------------------------
		{
			.name = "toolchain",
			.aliases = {"tc"},
			.brief = "Gère les toolchains et les versions gcc/g++ de l'hôte.",
			.description =
				"Manipule les toolchains disponibles et, sur Debian/Ubuntu,\n"
				"installe ou sélectionne des versions de gcc/g++ via apt et\n"
				"update-alternatives.\n"
				"\n"
				"La résolution d'une toolchain se fait dans cet ordre :\n"
				"  1. profil explicite dans [toolchains.<nom>] ;\n"
				"  2. profils intégrés host / gcc / clang ;\n"
				"  3. triplet dans le PATH (arm-linux-gnueabihf-…) ;\n"
				"  4. candidats par défaut (CC/CXX puis cl, tcc, gcc, clang…).",
			.usage = "cpp-maker toolchain <list|show|which|use|install|remove> [args]",
			.subcommands = {
				{"list (ou ls)",         "Liste les toolchains du projet, les intégrées, les préfixes détectés et les versions gcc/g++ installées."},
				{"show [nom]",           "Détail d'une toolchain : cc, cxx, ar, strip, triple, arch, os, runner, pkg-config."},
				{"which [nom]",          "Affiche les chemins du cxx et du cc."},
				{"use <version>",        "Debian/Ubuntu : configure gcc-<N>/g++-<N> comme défaut via update-alternatives."},
				{"install <ver|triplet>","Debian/Ubuntu : installe gcc-<N> (via PPA) ou un triplet croisé (+ qemu-user)."},
				{"remove <version>",     "Debian/Ubuntu : purge gcc-<N>/g++-<N> (refuse si c'est le défaut)."},
			},
			.examples =
				"cpp-maker toolchain list\n"
				"cpp-maker toolchain show armhf\n"
				"cpp-maker toolchain which host\n"
				"cpp-maker toolchain install 14\n"
				"cpp-maker toolchain use 14\n"
				"cpp-maker toolchain install arm-linux-gnueabihf\n"
				"cpp-maker toolchain remove 12",
			.notes = {
				"install/use/remove ne fonctionnent que sur Debian/Ubuntu (détection via /etc/debian_version + apt-get).",
				"install demande confirmation ; ajoutez -y pour automatiser.",
				"Après 'install 14', lancez 'use 14' pour le rendre défaut.",
			},
			.see_also = {"build", "config", "doctor"},
			.run = commands::cmd_toolchain
		},

		// ---------------------------------------------------------------------
		{
			.name = "help",
			.aliases = {},
			.brief = "Aide générale, par commande, ou liste des commandes.",
			.description =
				"Sans argument (ou avec 'list') : liste par catégorie toutes les\n"
				"commandes, suivie des options communes et d'un exemple de\n"
				"dépendance Git.\n"
				"\n"
				"Avec <commande> : affiche la description complète (description,\n"
				"usage, options, exemples, notes, voir aussi).\n"
				"\n"
				"'cpp-maker <commande> --help' est équivalent.\n"
				"\n"
				"Sujets supplémentaires :\n"
				"  modes      modes de compilation prédéfinis (flags, usage)\n"
				"  sanitize   tous les sanitizers et les options --sanitize*",
			.usage = "cpp-maker help [<commande>|modes|sanitize|list]",
			.options = {},
			.subcommands = {},
			.examples =
				"cpp-maker help\n"
				"cpp-maker help build\n"
				"cpp-maker build --help    # équivalent\n"
				"cpp-maker help modes\n"
				"cpp-maker help sanitize",
			.see_also = {},
			.run = cmd_help
		},

		// ---------------------------------------------------------------------
				{
			.name = "backup",
			.aliases = {},
			.brief = "Exporte le code d'un module dans un seul fichier XML.",
			.description =
				"Écrit dans un fichier XML le contenu de tous les fichiers\n"
				"sources, en-têtes et tests d'un module (ou de plusieurs).\n"
				"\n"
				"Par défaut, le fichier va dans le répertoire configuré par\n"
				"[paths].backups_dir (défaut : .backups/) :\n"
				"  .backups/backup-<module>.xml    (un module)\n"
				"  .backups/backup.xml             (plusieurs modules)\n"
				"\n"
				"Pour choisir un fichier précis, utiliser --file <chemin>.\n"
				"Pour écrire sur stdout, utiliser --file -  (ou --to-stdout).\n"
				"Les options globales --stdout / --stderr / --output s'appliquent\n"
				"aussi (voir cpp-maker help) — un backup sur stdout via --stdout\n"
				"global atterrit dans le fichier correspondant.",
			.usage = "cpp-maker backup [module] [-m <nom>] [--file <fichier>] [options]",
			.options = {
				{"-m, --module <a,b>",  "Modules à sauvegarder. Défaut : module courant, ou tous ceux du workspace si vous êtes à la racine."},
				{"--file <fichier>",    "Fichier XML de sortie. '-' = stdout. Accepte un chemin absolu ou relatif. --out reste accepté en alias pour la rétrocompatibilité."},
				{"--to-stdout",         "Écrit sur stdout (équivalent à --file -)."},
				{"--no-sources",        "N'inclut pas les sources."},
				{"--no-headers",        "N'inclut pas les en-têtes."},
				{"--no-tests",          "N'inclut pas les tests."},
			},
			.subcommands = {},
			.examples =
				"# Backup du module courant, écrit dans .backups/\n"
				"cpp-maker backup\n"
				"\n"
				"# Backup d'un module nommé\n"
				"cpp-maker backup -m ui\n"
				"\n"
				"# Chemin explicite\n"
				"cpp-maker backup -m ui --file /tmp/ui-$(date +%F).xml\n"
				"\n"
				"# Sur stdout, pipe vers xmllint\n"
				"cpp-maker backup -m ui --file - | xmllint --format -\n"
				"\n"
				"# stdout global (fichier .out/) — stdout de la commande\n"
				"cpp-maker backup -m ui --stdout snapshot.xml   # → .out/snapshot.xml\n"
				"\n"
				"# Plusieurs modules → racine <backup> + enfants <module>\n"
				"cpp-maker backup -m ui,core,audio\n"
				"\n"
				"# Tout le workspace d'un coup, sans tests\n"
				"cpp-maker backup --no-tests",
			.notes = {
				"Les chemins internes sont relatifs au dossier du module ; l'attribut dir du <module> permet de reconstituer l'emplacement exact.",
				"Le contenu est encodé en CDATA : lisible et sans risque de collision.",
				"Le répertoire de destination est configurable dans cpp-maker.toml : [paths] backups_dir = \".backups\".",
				"Les options --stdout/--stderr/--output (globales) restent orthogonales à --file.",
			},
			.see_also = {"modules", "info", "package"},
			.run = commands::cmd_backup
		},
	};
	return r;
}

// Catégories pour 'cpp-maker help' (liste) — regroupement logique pour la
// découverte. Les commandes absentes du registre sont ignorées.
const std::vector<Category>& categories() {
	static const std::vector<Category> c = {
		{"DÉMARRAGE",     {"new", "add", "module", "deps"}},
		{"CONSTRUCTION",  {"build", "run", "test", "check", "generate", "clean"}},
		{"INSPECTION",    {"graph", "modules", "info", "size", "backup"}},
		{"DISTRIBUTION",  {"package", "install", "deploy"}},
		{"ENVIRONNEMENT", {"doctor", "config", "toolchain"}},
		{"AIDE",          {"help"}},
	};
	return c;
}

const CommandSpec* find(const std::string& name) {
	for (auto& s : registry()) {
		if (s.name == name) return &s;
		for (auto& a : s.aliases) if (a == name) return &s;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Affichage
// ---------------------------------------------------------------------------
void print_list() {
	util::outln("\033[1;36mcpp-maker {}\033[0m — moteur de production unifié pour C et C++",
				CPP_MAKER_VERSION);
	util::outln("");
	util::outln("\033[1mUSAGE\033[0m");
	util::outln("    cpp-maker <commande> [options]");
	util::outln("    cpp-maker help <commande>       aide détaillée d'une commande");
	util::outln("    cpp-maker <commande> --help     idem, sans quitter le contexte");
	util::outln("");
	util::outln("\033[1mCOMMANDES\033[0m (par catégorie)");
	util::outln("");
	for (auto& cat : categories()) {
		util::outln("\033[1;35m{}\033[0m", cat.title);
		size_t w = 0;
		for (auto& n : cat.commands) w = std::max(w, n.size());
		for (auto& n : cat.commands) {
			auto* s = find(n);
			if (!s) continue;
			util::outln("    {:<{}}  {}", n, w, s->brief);
		}
		util::outln("");
	}

	util::outln("\033[1mSORTIE\033[0m");
	util::outln("    --output <f>             stdout + stderr combinés dans <f>");
	util::outln("    --stdout <f>             stdout seul dans <f>");
	util::outln("    --stderr <f>             stderr seul dans <f>");
	util::outln("    --file <f>               fichier spécifique à la commande (backup, …)");
	util::outln("    --filter <motif>         filtre les lignes (regex ou littéral) ; --grep idem");
	util::outln("    Les chemins sans répertoire vont dans .out/ (configurable [paths]).");
	util::outln("");

	util::outln("\033[1mOPTIONS COMMUNES\033[0m (analyse et filtrage ; certaines commandes les ignorent)");
	util::outln("    -m, --module <a,b>       Modules ciblés (défaut : tous / module courant)");
	util::outln("    -t, --toolchain <a,b>    Toolchains ('all' = toutes celles configurées)");
	util::outln("    --mode <nom>             Mode de compilation (release, debug, asan, msan…)");
	util::outln("    --sanitize <a,b>         Ajoute des sanitizers au mode (address, leak, null…)");
	util::outln("    -j N, --jobs N           Workers parallèles (0 = auto = cœurs - 1)");
	util::outln("    -v, --verbose            Affiche les commandes exécutées");
	util::outln("    -n, --dry-run            Affiche sans exécuter");
	util::outln("    -k, --keep-going         Continue après un échec");
	util::outln("    -a, --all                Toute la portée (workspace, toolchains…)");
	util::outln("    -- ARG…                  Arguments transmis à l'exécutable (run/test/check)");
	util::outln("");

	util::outln("\033[1mDÉPENDANCES GIT\033[0m");
	util::outln("    [dependencies.git.<alias>]");
	util::outln("    url     = \"https://…\"");
	util::outln("    version = \"v2.1.0\"         # tag, branche ou commit");
	util::outln("    module  = \"core\"           # module à importer");
	util::outln("    subdir  = \"\"               # sous-dossier");
	util::outln("");

	util::outln("\033[1mSUJETS D'AIDE\033[0m");
	util::outln("    cpp-maker help modes      modes de compilation prédéfinis");
	util::outln("    cpp-maker help sanitize   sanitizers et options --sanitize*");
	util::outln("");

	util::outln("Détails et exemples : \033[1mcpp-maker help <commande>\033[0m");
}

void print_command(const CommandSpec& s) {
	util::outln("\033[1;36mcpp-maker {}\033[0m — {}\n", s.name, s.brief);
	if (!s.aliases.empty())
		util::outln("\033[1;2mAlias : {}\033[0m\n", util::join(s.aliases, ", "));

	if (!s.description.empty()) {
		util::outln("\033[1mDESCRIPTION\033[0m");
		print_wrapped(s.description, "    ");
		util::outln("");
	}
	if (!s.usage.empty()) {
		util::outln("\033[1mUSAGE\033[0m");
		for (auto& l : util::split_list(s.usage, '\n'))
			util::outln("    {}", l);
		util::outln("");
	}
	if (!s.subcommands.empty()) {
		util::outln("\033[1mSOUS-COMMANDES\033[0m");
		print_aligned_list(s.subcommands);
		util::outln("");
	}
	if (!s.options.empty()) {
		util::outln("\033[1mOPTIONS\033[0m");
		print_aligned_list(s.options);
		util::outln("");
	}
	if (!s.examples.empty()) {
		util::outln("\033[1mEXEMPLES\033[0m");
		for (auto& l : util::split_list(s.examples, '\n'))
			util::outln("    {}", l);
		util::outln("");
	}
	if (!s.notes.empty()) {
		util::outln("\033[1mNOTES\033[0m");
		for (auto& n : s.notes)
			print_wrapped("• " + n, "    ");
		util::outln("");
	}
	if (!s.see_also.empty()) {
		util::outln("\033[1mVOIR AUSSI\033[0m");
		util::outln("    {}", util::join(s.see_also, ", "));
		util::outln("");
	}
}

// ---------------------------------------------------------------------------
// Sujets : modes et sanitizers
// ---------------------------------------------------------------------------
struct ModeHelp { std::string_view name, purpose, detail; };

static const std::vector<ModeHelp>& mode_help() {
	static const std::vector<ModeHelp> m = {
		{"release", "Production : optimisé, assertions désactivées.",
		 "Le binaire à distribuer (package, install, deploy)."},
		{"debug", "Développement (mode par défaut) : sans optimisation, symboles complets.",
		 "Pas à pas fiable dans gdb/lldb, variables jamais « optimized out »."},
		{"relwithdebinfo", "Optimisé avec symboles.",
		 "Profilage (perf, callgrind) et analyse de crash sur un binaire proche de la production."},
		{"minsize", "Optimisé pour la taille.",
		 "Embarqué, binaires distribués où la taille compte ; voir aussi 'cpp-maker size'."},
		{"asan", "AddressSanitizer : erreurs mémoire.",
		 "Débordements de tas/pile/globales, use-after-free, double free, fuites (Linux). "
		 "Réglages à l'exécution : ASAN_OPTIONS=detect_leaks=1:abort_on_error=1."},
		{"tsan", "ThreadSanitizer : data races.",
		 "Accès concurrents non synchronisés, interblocages. Incompatible avec asan/msan. "
		 "Réglages : TSAN_OPTIONS=halt_on_error=1."},
		{"ubsan", "UndefinedBehaviorSanitizer : comportements indéfinis.",
		 "Dépassements signés, décalages invalides, pointeurs nuls, accès hors limites, "
		 "mauvais alignement… Combinable avec asan (--sanitize undefined). "
		 "Réglages : UBSAN_OPTIONS=print_stacktrace=1."},
		{"msan", "MemorySanitizer : lectures de mémoire non initialisée (clang uniquement).",
		 "Suit l'origine des valeurs (track-origins=2). Tout le code, libc++ comprise, doit "
		 "être instrumenté sinon faux positifs. Réglages : MSAN_OPTIONS=halt_on_error=1."},
		{"lsan", "LeakSanitizer seul : fuites mémoire.",
		 "Rapport de fuites à la sortie du programme, quasiment sans surcoût à l'exécution. "
		 "Réglages : LSAN_OPTIONS=suppressions=lsan.supp."},
		{"valgrind", "Build debug exécuté sous valgrind (memcheck).",
		 "Aucune instrumentation à la compilation ; fuites, mémoire non initialisée, "
		 "accès invalides. Lent (~20-50x). Raccourci : cpp-maker valgrind."},
	};
	return m;
}

static void print_modes_help() {
	util::outln("\033[1;36mcpp-maker — modes de compilation\033[0m\n");
	print_wrapped("Un mode fixe les flags de compilation, d'édition de liens, les defines "
				  "et éventuellement un runner. Choix : positionnel (cpp-maker build release) "
				  "ou --mode <nom>. Les artefacts vont dans build/<toolchain>/<mode>/.", "    ");
	util::outln("");
	util::outln("\033[1mMODES PRÉDÉFINIS\033[0m");
	auto defaults = cfg::default_modes();
	for (auto& h : mode_help()) {
		util::outln("  \033[1m{}\033[0m — {}", h.name, h.purpose);
		auto it = std::find_if(defaults.begin(), defaults.end(),
							   [&](auto& d) { return d.name == h.name; });
		if (it != defaults.end()) {
			std::vector<std::string> f = it->flags;
			for (auto& d : it->defines) f.push_back("-D" + d);
			if (!it->runner.empty()) f.push_back("runner: " + it->runner);
			util::outln("      \033[2m{}\033[0m", util::join(f, " "));
		}
		print_wrapped(std::string(h.detail), "      ");
	}
	util::outln("");
	util::outln("\033[1mMODES PERSONNALISÉS\033[0m (cpp-maker.toml)");
	util::outln("    [build.modes.asan-ub]");
	util::outln("    base      = \"debug\"");
	util::outln("    sanitizer = \"address,undefined\"   # liste : cpp-maker help sanitize");
	util::outln("");
	util::outln("\033[1mSANITIZERS À LA DEMANDE\033[0m");
	util::outln("    cpp-maker build debug --sanitize address,undefined");
	util::outln("    cpp-maker test --sanitize leak");
	util::outln("    → mode dérivé « debug+address+undefined », dossier séparé. Voir : cpp-maker help sanitize");
}

static void print_sanitize_help() {
	util::outln("\033[1;36mcpp-maker — sanitizers\033[0m\n");
	print_wrapped("--sanitize <nom>[,<nom>…] ajoute des sanitizers au mode courant "
				  "(-fsanitize=… à la compilation et à l'édition de liens, plus "
				  "-fno-omit-frame-pointer et -g). L'option est répétable ; les noms "
				  "acceptent aussi la forme -fsanitize=<nom> et les alias asan, tsan, "
				  "msan, lsan, ubsan, hwasan.", "    ");
	util::outln("");
	print_wrapped("--sanitize all (build, run, test, check) enchaîne une passe par runtime, "
				  "puisqu'ils ne cohabitent pas dans un même binaire : address+undefined, "
				  "thread, leak, memory, hwaddress. Chaque passe a son mode dérivé et son "
				  "dossier ; celles que la toolchain ou le mode ne permettent pas (memory "
				  "sans clang, hwaddress hors AArch64/RISC-V…) sont ignorées avec un "
				  "avertissement. Les autres noms donnés avec all (ex. all,float-divide-by-zero) "
				  "s'ajoutent à chaque passe.", "    ");
	util::outln("");
	auto section = [](std::string_view title, auto pred) {
		util::outln("\033[1m{}\033[0m", title);
		for (auto& i : sanitize::catalog()) {
			if (!pred(i)) continue;
			util::outln("  \033[1m--sanitize {}\033[0m  \033[2m({})\033[0m", i.name, i.family);
			print_wrapped(std::string(i.detail), "      ");
		}
		util::outln("");
	};
	static const std::set<std::string_view> hardening = {"cfi", "safe-stack", "shadow-call-stack"};
	section("RUNTIMES (un seul à la fois ; leak se combine avec address)",
			[&](auto& i) { return i.runtime && !hardening.count(i.name); });
	section("COMPORTEMENTS INDÉFINIS — UBSan (combinables entre eux et avec un runtime)",
			[&](auto& i) { return !i.runtime && !hardening.count(i.name); });
	section("DURCISSEMENT (protections utilisables en production)",
			[&](auto& i) { return hardening.count(i.name) > 0; });

	util::outln("\033[1mOPTIONS ASSOCIÉES\033[0m");
	print_aligned_list({
		{"--sanitize-trap <a,b|all>",
		 "Remplace les rapports textuels par une instruction d'interruption matérielle (trap). "
		 "Plus de runtime de rapport : binaire nettement plus léger, utile en embarqué ou en "
		 "production, mais diagnostic à faire au débogueur. Surtout pour les vérifications UBSan "
		 "(ex. --sanitize-trap undefined)."},
		{"--sanitize-ignorelist <fichier>",
		 "Fichier listant des fonctions ou des sources à exclure de l'instrumentation "
		 "(code tiers, code legacy). Format : 'fun:nom_*' ou 'src:chemin/*.cpp', une règle "
		 "par ligne. Clang uniquement. Après modification du fichier, relancez avec --clean."},
		{"--no-sanitize-recover <a,b|all>",
		 "Force le programme à s'arrêter (abort) dès la première erreur du sanitizer indiqué "
		 "au lieu d'afficher un avertissement et de continuer. Indispensable pour que les "
		 "erreurs UBSan fassent échouer les tests."},
	});
	util::outln("");
	util::outln("\033[1mEXEMPLES\033[0m");
	util::outln("    cpp-maker build --sanitize address,undefined");
	util::outln("    cpp-maker test --sanitize signed-integer-overflow,null --no-sanitize-recover all");
	util::outln("    cpp-maker test --sanitize all");
	util::outln("    cpp-maker run --cxx clang++ --cc clang --sanitize memory");
	util::outln("    cpp-maker build release --sanitize cfi --cxx clang++ --cc clang");
	util::outln("    cpp-maker build --sanitize undefined --sanitize-trap undefined");
	util::outln("    cpp-maker build --sanitize address --sanitize-ignorelist third_party.ignore");
	util::outln("");
	util::outln("\033[1mNOTES\033[0m");
	print_wrapped("• Le mode dérivé s'appelle <mode>+<sanitizers> (ex. debug+address) et a son "
				  "propre dossier build/<tc>/<mode>+…/.", "    ");
	print_wrapped("• memory, cfi, safe-stack, unsigned-integer-overflow et --sanitize-ignorelist "
				  "nécessitent clang : cpp-maker s'arrête avec un message clair sinon.", "    ");
	print_wrapped("• Dans cpp-maker.toml, le champ 'sanitizer' d'un mode accepte la même liste "
				  "(ex. sanitizer = \"address,undefined\").", "    ");
	print_wrapped("• 'cpp-maker doctor' indique quels sanitizers chaque toolchain supporte.", "    ");
}

int cmd_help(const std::vector<std::string>& args) {
	if (args.empty() || args[0] == "list") { print_list(); return 0; }
	if (args[0] == "modes" || args[0] == "mode") { print_modes_help(); return 0; }
	if (args[0] == "sanitize" || args[0] == "sanitizer" || args[0] == "sanitizers") {
		print_sanitize_help();
		return 0;
	}
	if (auto* s = find(args[0])) { print_command(*s); return 0; }
	util::die("commande inconnue : " + args[0] + " (cpp-maker help list)");
}

// ---------------------------------------------------------------------------
// Validation (inchangé)
// ---------------------------------------------------------------------------
const ModuleConfig& require_module(const Ctx& c, const std::string& name, std::string_view cmd) {
	auto m = c.p.find_module(name);
	if (!m) {
		std::vector<std::string> names;
		for (auto& x : c.p.modules) names.push_back(x.name);
		util::die(std::format("{}: module inconnu '{}' (modules : {})",
							  cmd, name, util::join(names, ", ")));
	}
	return *m;
}

const ModuleConfig& require_executable(const Ctx& c, const std::string& name, std::string_view cmd) {
	auto& m = require_module(c, name, cmd);
	if (m.type != "executable")
		util::die(std::format("{}: '{}' est de type '{}' — exécutable requis",
							  cmd, name, m.type));
	return m;
}

const ModeConfig& require_mode(const Ctx& c, const std::string& name, std::string_view cmd) {
	auto m = c.p.find_mode(name);
	if (!m) {
		std::vector<std::string> names;
		for (auto& x : c.p.modes) names.push_back(x.name);
		util::die(std::format("{}: mode inconnu '{}' (modes : {})",
							  cmd, name, util::join(names, ", ")));
	}
	return *m;
}

void require_workspace(const Ctx& c, std::string_view cmd) {
	if (!c.p.workspace)
		util::die(std::format("{}: commande disponible seulement en workspace "
							  "([workspace] modules = [...] ou 'cpp-maker new <nom> --workspace')", cmd));
}

void require_project_file(const Ctx& c, std::string_view cmd) {
	if (!c.p.has_file)
		util::die(std::format("{}: cpp-maker.toml introuvable à {}", cmd, c.p.root.string()));
}

void require_no_positional(const Opts& o, std::string_view cmd) {
	if (!o.positional.empty()) usage_error(cmd, "argument inattendu : " + o.positional[0]);
}

void require_no_passthrough(const Opts& o, std::string_view cmd) {
	if (!o.passthrough.empty())
		util::die(std::format("{}: arguments après '--' non acceptés "
							  "(transmis à l'exécutable uniquement par run/test/check)", cmd));
}

std::string require_single_toolchain(const Ctx& c, const Opts& o,
									 const std::string& def, std::string_view cmd) {
	auto tcs = pick_toolchains(c, o, def);
	if (tcs.size() != 1)
		util::die(std::format("{}: une seule toolchain à la fois (reçu : {})",
							  cmd, util::join(tcs, ", ")));
	return tcs[0];
}

void usage_error(std::string_view cmd, std::string_view detail) {
	std::string name(cmd);
	util::errln("\033[1;31merror:\033[0m {}", detail);
	if (auto* spec = find(name)) {
		// Première ligne d'usage uniquement (les suivantes seraient du bruit).
		auto lines = util::split_list(spec->usage, '\n');
		if (!lines.empty()) util::errln("  {}", lines.front());
		if (!spec->examples.empty()) {
			auto ex = util::split_list(spec->examples, '\n');
			if (!ex.empty() && !ex.front().empty())
				util::errln("  ex. {}", ex.front());
		}
	}
	util::errln("  (cpp-maker help {})", name);
	std::exit(2);
}

void banner(std::string_view cmd, const Ctx& c) {
	util::outln("\033[1;34m[cpp-maker]\033[0m \033[1m{}\033[0m · {}{}",
				cmd, c.p.name, c.p.workspace ? "  (workspace)" : "");
}

} // namespace commands::help