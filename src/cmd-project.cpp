#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

namespace commands {

// ---------------------------------------------------------------------------
// new / add : génération de projets et de modules
// ---------------------------------------------------------------------------
std::string ident(const std::string& name) {
	std::string r;
	for (char ch : name) r += std::isalnum((unsigned char)ch) ? ch : '_';
	if (r.empty() || std::isdigit((unsigned char)r[0])) r = "_" + r;
	return r;
}
std::string upper(std::string s) {
	for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
	return s;
}

std::string toml_array(const std::vector<std::string>& v) {
	std::string r = "[";
	for (size_t i = 0; i < v.size(); ++i) r += (i ? ", \"" : "\"") + v[i] + "\"";
	return r + "]";
}

const char* TEST_HPP = R"TPL(#pragma once
// Mini-framework de tests généré par cpp-maker.
//
// Deux styles acceptés :
//   TEST(Nom)          { CHECK(x); CHECK_EQ(a, b); REQUIRE(ptr); }
//   TEST(Suite, Nom)   { EXPECT_TRUE(x); EXPECT_EQ(a, b); ASSERT_TRUE(ptr); }
// CHECK/EXPECT_* continuent le test en cas d'échec ; REQUIRE/ASSERT_*
// l'arrêtent (return). SKIP("raison") marque le test comme ignoré (ex. pas
// d'affichage ou de GPU disponible) ; si tous les tests exécutés sont
// ignorés, l'exécutable sort avec le code 77 (test ignoré pour cpp-maker).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> fn; };

inline std::vector<TestCase>& test_registry() {
	static std::vector<TestCase> r;
	return r;
}
inline int& test_failures() {
	static int f = 0;
	return f;
}
inline bool& test_skipped() {
	static bool s = false;
	return s;
}

struct TestRegistrar {
	TestRegistrar(const std::string& n, std::function<void()> f) {
		test_registry().push_back({n, std::move(f)});
	}
};

inline void test_report(const char* kind, const char* expr, const char* file, int line) {
	std::fprintf(stderr, "\n  %s failed: %s (%s:%d)", kind, expr, file, line);
	++test_failures();
}

namespace test {
/// Signale un échec depuis du code de test (compatibilité anciens tests).
inline void ReportFailure(const char* file, int line, const char* cond) {
	test_report("Failure", cond, file, line);
}
} // namespace test

#define TEST_CAT_(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT_(a, b)
#define TEST_1_(name) \
	static void test_fn_##name(); \
	static TestRegistrar test_reg_##name(#name, test_fn_##name); \
	static void test_fn_##name()
#define TEST_2_(suite, name) \
	static void test_fn_##suite##_##name(); \
	static TestRegistrar test_reg_##suite##_##name(#suite "." #name, test_fn_##suite##_##name); \
	static void test_fn_##suite##_##name()
#define TEST_PICK_(_1, _2, M, ...) M
#define TEST(...) TEST_PICK_(__VA_ARGS__, TEST_2_, TEST_1_, unused)(__VA_ARGS__)

#define CHECK(cond) do { \
	if (!(cond)) test_report("CHECK", #cond, __FILE__, __LINE__); \
} while (0)

#define CHECK_EQ(a, b) do { \
	if (!((a) == (b))) test_report("CHECK_EQ", #a " == " #b, __FILE__, __LINE__); \
} while (0)

#define CHECK_NE(a, b) do { \
	if ((a) == (b)) test_report("CHECK_NE", #a " != " #b, __FILE__, __LINE__); \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
	if (!(std::fabs(double(a) - double(b)) <= double(eps))) \
		test_report("CHECK_NEAR", #a " ~ " #b, __FILE__, __LINE__); \
} while (0)

#define REQUIRE(cond) do { \
	if (!(cond)) { test_report("REQUIRE", #cond, __FILE__, __LINE__); return; } \
} while (0)

#define REQUIRE_EQ(a, b) do { \
	if (!((a) == (b))) { test_report("REQUIRE_EQ", #a " == " #b, __FILE__, __LINE__); return; } \
} while (0)

#define SKIP(reason) do { \
	std::fprintf(stderr, "\n  skipped: %s", reason); \
	test_skipped() = true; \
	return; \
} while (0)

// Style GTest (compatibilité avec les anciens tests du projet).
#define EXPECT_TRUE(c)   CHECK(c)
#define EXPECT_FALSE(c)  CHECK(!(c))
#define EXPECT_EQ(a, b)  CHECK_EQ(a, b)
#define EXPECT_NE(a, b)  CHECK_NE(a, b)
#define ASSERT_TRUE(c)   REQUIRE(c)
#define ASSERT_FALSE(c)  REQUIRE(!(c))
#define ASSERT_EQ(a, b)  REQUIRE_EQ(a, b)
)TPL";

const char* TEST_RUNNER_CPP = R"TPL(#include "test.hpp"

int main(int argc, char** argv) {
	std::string filter;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--filter" && i + 1 < argc) filter = argv[++i];
	}
	int failed = 0, skipped = 0, total = 0;
	for (auto& tc : test_registry()) {
		if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;
		++total;
		int before = test_failures();
		test_skipped() = false;
		std::printf("  \033[0;33m[ RUN  ]\033[0m %s", tc.name.c_str());
		std::fflush(stdout);
		tc.fn();
		if (test_failures() > before) {
			std::printf("\r  \033[1;31m[ FAIL ]\033[0m %s\n", tc.name.c_str());
			++failed;
		} else if (test_skipped()) {
			std::printf("\r  \033[1;33m[ SKIP ]\033[0m %s\n", tc.name.c_str());
			++skipped;
		} else {
			std::printf("\r  \033[1;32m[ PASS ]\033[0m %s\n", tc.name.c_str());
		}
	}
	std::printf("\n%d/%d tests passed", total - failed - skipped, total);
	if (skipped) std::printf(", %d skipped", skipped);
	std::printf("\n");
	if (failed > 0) return 1;
	return (total > 0 && skipped == total) ? 77 : 0;
}
)TPL";

// Écrit les sources d'un module. `uses` : bibliothèque dont main() appelle add().
void write_module_sources(const fs::path& dir, const std::string& name, const std::string& type,
								 bool c_lang, const std::string& uses = {}) {
	std::string id = ident(name);
	std::string guard = upper(id) + "_H";
	if (c_lang) {
		util::write_file(dir / "include" / name / (name + ".h"), std::format(
R"(#ifndef {0}
#define {0}

#ifdef __cplusplus
extern "C" {{
#endif

{1}int {2}_add(int a, int b){3}

#ifdef __cplusplus
}}
#endif

#endif /* {0} */
)", guard, type == "header-only" ? "static inline " : "", id,
			type == "header-only" ? " { return a + b; }" : ";"));
		if (type != "header-only")
			util::write_file(dir / "src" / (name + ".c"), std::format(
R"(#include "{0}/{0}.h"

int {1}_add(int a, int b) {{
	return a + b;
}}
)", name, id));
		if (type == "executable") {
			std::string inc = uses.empty() ? name : uses;
			util::write_file(dir / "src" / "main.c", std::format(
R"(#include <stdio.h>
#include "{0}/{0}.h"

int main(void) {{
	printf("Hello from {1} — 2 + 3 = %d\n", {2}_add(2, 3));
	return 0;
}}
)", inc, name, ident(inc)));
		}
		if (type != "executable" || uses.empty())
			util::write_file(dir / "tests" / ("test_" + name + ".c"), std::format(
R"(#include <stdio.h>
#include "{0}/{0}.h"

static int failures = 0;
#define CHECK(c) do {{ if (!(c)) {{ \
	fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #c, __FILE__, __LINE__); ++failures; }} }} while (0)

int main(void) {{
	CHECK({1}_add(2, 3) == 5);
	CHECK({1}_add(-1, 1) == 0);
	printf("{0} : %s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}}
)", name, id));
		return;
	}
	util::write_file(dir / "include" / name / (name + ".hpp"), std::format(
R"(#pragma once

namespace {0} {{

{1}int add(int a, int b){2}

}} // namespace {0}
)", id, type == "header-only" ? "inline " : "", type == "header-only" ? " { return a + b; }" : ";"));
	if (type != "header-only")
		util::write_file(dir / "src" / (name + ".cpp"), std::format(
R"(#include "{0}/{0}.hpp"

namespace {1} {{

int add(int a, int b) {{
	return a + b;
}}

}} // namespace {1}
)", name, id));
	if (type == "executable") {
		std::string inc = uses.empty() ? name : uses;
		util::write_file(dir / "src" / "main.cpp", std::format(
R"(#include <iostream>
#include "{0}/{0}.hpp"

int main() {{
	std::cout << "Hello from {1} — 2 + 3 = " << {2}::add(2, 3) << '\n';
	return 0;
}}
)", inc, name, ident(inc)));
	}
	if (type != "executable" || uses.empty()) {
		util::write_file(dir / "tests" / "test.hpp", TEST_HPP);
		util::write_file(dir / "tests" / "test_runner.cpp", TEST_RUNNER_CPP);
		util::write_file(dir / "tests" / ("test_" + name + ".cpp"), std::format(
R"(#include "test.hpp"
#include "{0}/{0}.hpp"

TEST(add_works) {{ CHECK_EQ({1}::add(2, 3), 5); }}
TEST(add_neg)   {{ CHECK_EQ({1}::add(-1, 1), 0); }}
)", name, id));
	}
}

std::string module_toml(const std::string& name, const std::string& type,
							   const std::vector<std::string>& deps) {
	return std::format(R"([module]
name = "{}"
type = "{}"              # executable | static | shared | header-only
description = ""         # texte libre, affiché par 'cpp-maker info -m <nom>'

[sources]
dirs = ["src"]
# exclude = ["src/experimental/**"]

[include]
public = ["include"]      # propagé aux modules dépendants
# private = ["src"]

[dependencies]
modules = {}
pkg_config = []           # ex. ["zlib", "libcurl"]
system = []               # ex. ["m", "pthread"]

[flags]
defines = []
public_defines = []
cflags = []
cxxflags = []

[tests]
dirs = ["tests"]

# Surcharges par plate-forme cible (linux | windows | macos | android | unix…)
# [platform.windows]
# system = ["ws2_32"]
)", name, type, toml_array(deps));
}

std::string toolchains_toml() {
	std::string s = R"(# Toolchains : cpp-maker build -t <nom>[,<nom>…] (ou -t all).
# Sans profil, tout préfixe de triplet du PATH fonctionne : -t arm-linux-gnueabihf
[toolchains.host]
cc = "auto"
cxx = "auto"
)";
	struct Known { const char* name; const char* prefix; const char* runner; const char* comment; };
	for (auto k : {Known{"armhf", "arm-linux-gnueabihf-", "", "ARM 32 bits (Raspberry Pi…)"},
				   Known{"aarch64", "aarch64-linux-gnu-", "", "ARM 64 bits"},
				   Known{"win64", "x86_64-w64-mingw32-", "", "Windows 64 bits (MinGW-w64)"}}) {
		bool found = util::have(std::string(k.prefix) + "gcc") || util::have(std::string(k.prefix) + "g++");
		std::string c = found ? "" : "# ";
		s += std::format("\n# {}{}\n{}[toolchains.{}]\n{}prefix = \"{}\"\n", k.comment,
						 found ? " — détectée" : "", c, k.name, c, k.prefix);
		if (std::string(k.name) != "win64")
			s += "# sysroot = \"/usr/" + std::string(k.prefix).substr(0, std::strlen(k.prefix) - 1) + "\"\n";
		s += "# runner = []          # émulateur (auto : qemu-*/wine), [] = désactivé\n";
	}
	s += R"(
# Clang en compilation croisée :
# [toolchains.clang-arm64]
# cc = "clang"
# cxx = "clang++"
# target = "aarch64-linux-gnu"
)";
	return s;
}

std::string common_toml_tail() {
	return R"(
[build]
default_mode = "debug"
default_toolchain = "host"
jobs = 0  # 0 = cœurs - 1 (cpp-maker en réserve un)
warnings = [
	"-Wall",
	"-Wextra",
	"-Wpedantic",
	"-Wformat=2",
	"-Wshadow",
	"-Wconversion",
	"-Wsign-conversion",
	"-Wdouble-promotion",
	"-Wundef",
	"-Wcast-align",
	"-Wcast-qual",
	"-Woverloaded-virtual",
	"-Wnon-virtual-dtor"
]

# Modes : release, debug, relwithdebinfo, minsize, asan, tsan, ubsan, msan, lsan, valgrind
# sont prédéfinis ; on peut les redéfinir ou en ajouter (base = "<mode>").
[build.modes.release]
flags = ["-O2"]
defines = ["NDEBUG"]

[build.modes.debug]
flags = ["-O0", "-g3"]
defines = ["DEBUG"]

[build.modes.asan]
base = "debug"
sanitizer = "address"

[tests]
timeout = 300                  # secondes par exécutable de test

[package]
formats = ["tar.gz"]           # tar.gz | tar.xz | zip | deb
strip = true
maintainer = "Nom <email@example.com>"
description = ""
# deb_depends = ["libc6"]

[paths]
out_dir     = ".out"          # --output / --stdout / --stderr sans chemin
backups_dir = ".backups"      # cpp-maker backup sans --file

# Déploiement : cpp-maker deploy <nom>
# [deploy.rpi]
# method = "rsync"             # rsync | scp | copy
# host = "raspberrypi.local"
# user = "pi"
# path = "/home/pi/{name}"
# toolchain = "armhf"
# mode = "release"
# artifact = "stage"           # stage | tar.gz | tar.xz | zip | deb
# after = ["{path}/bin/{name} --version"]
)";
}

void cmd_new(const std::vector<std::string>& rest) {
	if (rest.empty()) util::die("usage : cpp-maker new <nom> [--lang c|c++] "
								"[--type normal|static|shared|header-only|modular] "
								"[--workspace] [--standard ...] [--compiler ...]");
	auto o = parse_opts(rest, {"--type", "--lang", "--standard", "--compiler", "--c-standard"},
						{"--workspace", "--modules"});
	if (o.positional.size() != 1) util::die("cpp-maker new : un seul nom de projet attendu");
	std::string name = o.positional[0];

	// --- Type de projet -----------------------------------------------------
	// `normal`   : exécutable mono-module (remplace `executable`).
	// `modular`  : workspace avec un module `core` par défaut — équivaut à
	//              `--workspace` (conservé pour la rétrocompatibilité).
	// `static` | `shared` | `header-only` : bibliothèque (mono-module).
	std::string cli_type = o.value("--type", "normal");
	std::string lang = o.value("--lang", "c++");
	bool ws = o.flag("--workspace") || o.flag("--modules");
	if (cli_type == "modular") { ws = true; cli_type = "normal"; }
	if (cli_type == "executable") cli_type = "normal";  // alias rétrocompatible
	if (cli_type == "library") cli_type = "static";     // alias rétrocompatible
	static const std::set<std::string> cli_types = {"normal", "static", "shared", "header-only"};
	if (!cli_types.count(cli_type))
		util::die("type invalide : " + cli_type +
				  " (normal|static|shared|header-only|modular)");
	// Type interne utilisé par la suite (write_module_sources, module_toml).
	std::string type = (cli_type == "normal") ? "executable" : cli_type;

	if (lang == "cpp" || lang == "cxx") lang = "c++";
	if (lang != "c" && lang != "c++") util::die("--lang : c ou c++");
	bool c_lang = lang == "c";
	std::string cxx_std = o.value("--standard", "c++23");
	std::string c_std = o.value("--c-standard", "c17");
	if (c_lang && o.values.count("--standard") && cfg::c_standards().count(cxx_std)) {
		c_std = cxx_std;
		cxx_std = "c++23";
	}

	fs::path root = name;
	std::string pname = root.filename().string();
	if (!util::is_safe_name(pname)) util::die("nom de projet invalide : '" + pname + "'");
	if (fs::exists(root)) util::die("le dossier existe déjà : " + root.string());
	if (!cfg::cxx_standards().count(cxx_std)) util::die("standard C++ invalide : " + cxx_std);
	if (!cfg::c_standards().count(c_std)) util::die("standard C invalide : " + c_std);
	
	// Vérifier un compilateur hôte AVANT de créer quoi que ce soit.
	std::string forced = o.value("--compiler", "auto");
	std::vector<toolchain::TrialResult> log;
	auto chosen = toolchain::detect_working(lang, "auto", forced, c_lang ? c_std : cxx_std, &log);
	if (!chosen) {
		for (auto& l : log) util::errln("  {:<14} {}", l.name, l.status);
		util::die(std::format("aucun compilateur {} ne compile un fichier trivial en -std={}\n"
							  "  Pistes : cpp-maker toolchain list | cpp-maker toolchain install 14",
							  lang, c_lang ? c_std : cxx_std));
	}
	bool pin = forced != "auto";
	if (!pin) {
		auto first = toolchain::host_candidates(!c_lang, "auto");
		if (!first.empty() && first[0] != chosen->exe) pin = true;
	}
	util::outln("\033[1;34m[cpp-maker]\033[0m compilateur hôte : {} ({})", chosen->exe, chosen->version);

	std::string tc = toolchains_toml();
	if (pin) {
		tc = util::replace_all(tc, std::format("{} = \"auto\"\n", c_lang ? "cc" : "cxx"),
							   std::format("{} = \"{}\"     # épinglé par cpp-maker new\n",
										   c_lang ? "cc" : "cxx", chosen->exe));
		util::outln("  → inscrit dans cpp-maker.toml : {} = \"{}\"", c_lang ? "cc" : "cxx", chosen->exe);
	}

	std::string head = std::format(R"([project]
name = "{}"
version = "0.1.0"
description = ""
standard = "{}"              # standard C++
c_standard = "{}"               # standard C
minimum_cpp_maker_version = "{}"
)", pname, cxx_std, c_std, CPP_MAKER_VERSION);

	fs::create_directories(root);
	if (ws) {
		std::string app = pname;
		std::string lib = "core";
		if (app == lib) lib = "corelib";
		std::string toml = head + std::format(R"(
[workspace]
modules = [
	"libs/{}",
	"apps/{}",
]
)", lib, app) + "\n" + tc + common_toml_tail();
		util::write_file(root / "cpp-maker.toml", toml);
		fs::path libdir = root / "libs" / lib;
		util::write_file(libdir / "module.toml", module_toml(lib, "static", {}));
		write_module_sources(libdir, lib, "static", c_lang);
		fs::path appdir = root / "apps" / app;
		util::write_file(appdir / "module.toml", module_toml(app, "executable", {lib}));
		write_module_sources(appdir, app, "executable", c_lang, lib);
		// l'application n'a pas d'en-tête propre : main() utilise la bibliothèque
		std::error_code ec;
		fs::remove_all(appdir / "include", ec);
		fs::remove(appdir / "src" / (app + (c_lang ? ".c" : ".cpp")), ec);
	} else {
		std::string toml = head + std::format(R"(
[module]
type = "{}"            # executable | static | shared | header-only

[sources]
dirs = ["src"]

[include]
public = ["include"]

[dependencies]
pkg_config = []                # ex. ["zlib"]
system = []                    # ex. ["m", "pthread"]

[tests]
dirs = ["tests"]
timeout = 300
)", type) + "\n" + tc + util::replace_all(common_toml_tail(), "\n[tests]\ntimeout = 300                  # secondes par exécutable de test\n", "");
		// [tests] est déjà défini par la section module
		util::write_file(root / "cpp-maker.toml", toml);
		write_module_sources(root, pname, type, c_lang);
	}

	util::write_file(root / ".gitignore",
		"build/\ndist/\n.out/\n.backups/\ncompile_commands.json\n.cpp-maker.lock\n");
	util::write_file(root / "README.md", std::format(
		"# {0}\n\nProjet {1} généré par cpp-maker {2}.\n\n## Utilisation\n\n```sh\n"
		"cpp-maker build                 # debug, toolchain hôte\n"
		"cpp-maker build release -t armhf # compilation croisée\n"
		"cpp-maker test                  # compile et lance les tests\n"
		"cpp-maker package               # dist/{0}-<version>-<os>-<arch>.tar.gz\n"
		"cpp-maker graph                 # dépendances entre modules\n```\n",
		pname, c_lang ? "C" : "C++", CPP_MAKER_VERSION));

	util::outln("\033[1;32mProjet '{}' créé{}.\033[0m\n", pname, ws ? " (workspace multi-modules)" : "");
	util::outln("  cd {}", root.string());
	util::outln("  cpp-maker build                  # compile (debug)");
	if (type == "executable" || ws) util::outln("  cpp-maker run                    # compile et exécute");
	util::outln("  cpp-maker test                   # tests");
	util::outln("  cpp-maker build release -t all   # toutes les toolchains configurées");
	if (ws) util::outln("  cpp-maker add libs/net --type static   # nouveau module");
	util::outln("  cpp-maker doctor                 # diagnostic de l'environnement");
}

// Remplace le tableau [workspace].modules dans le texte de cpp-maker.toml.
std::string set_workspace_modules(const std::string& text, const std::vector<std::string>& mods) {
	std::string arr = "modules = [\n";
	for (auto& m : mods) arr += "    \"" + m + "\",\n";
	arr += "]";
	auto ws = text.find("[workspace]");
	if (ws == std::string::npos) util::die("table [workspace] introuvable dans cpp-maker.toml");
	size_t pos = ws;
	for (;;) {
		pos = text.find("modules", pos);
		if (pos == std::string::npos) {
			auto eol = text.find('\n', ws);
			return text.substr(0, eol + 1) + arr + "\n" + text.substr(eol + 1);
		}
		size_t bol = text.rfind('\n', pos);
		bol = bol == std::string::npos ? 0 : bol + 1;
		if (util::trim(text.substr(bol, pos - bol)).empty()) break;
		pos += 7;
	}
	auto lb = text.find('[', pos);
	if (lb == std::string::npos) util::die("tableau modules mal formé");
	size_t rb = lb + 1;
	bool in_str = false;
	for (; rb < text.size(); ++rb) {
		char ch = text[rb];
		if (ch == '"') in_str = !in_str;
		else if (ch == '#' && !in_str) { while (rb < text.size() && text[rb] != '\n') ++rb; }
		else if (ch == ']' && !in_str) break;
	}
	if (rb >= text.size()) util::die("tableau modules non terminé");
	return text.substr(0, pos) + arr + text.substr(rb + 1);
}

int cmd_add(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--type", "--lang", "--deps", "--name", "--dir"},
						{"--no-scaffold"});
	if (o.positional.size() != 1)
		util::die("usage : cpp-maker add <nom|chemin> [--type T] [--lang c|c++] "
				  "[--deps a,b] [--name nom] [--dir libs|apps|<chemin>] [--no-scaffold]");
	fs::path invoked = fs::current_path();
	auto c = load_ctx();
	if (!c.p.workspace)
		util::die("ce projet n'est pas un workspace : ajoutez une table [workspace] avec "
				  "modules = [...] (ou créez-le avec 'cpp-maker new <nom> --workspace')");

	// --- Détermination du chemin du module ---------------------------------
	//   - nom simple ("core")            → <dir>/core, dir = "libs" par défaut
	//   - chemin relatif ("libs/x", "apps/y")
	//                                    → interprété depuis le répertoire
	//                                      d'invocation de la commande, puis
	//                                      ramené relatif à la racine
	//   - chemin absolu                  → tel quel
	// L'option --dir déplace la destination par défaut ("--dir apps" place
	// le module dans apps/<nom>).
	fs::path arg = o.positional[0];
	fs::path rel;
	if (arg.is_absolute()) {
		rel = arg.lexically_normal().lexically_relative(c.p.root);
	} else {
		std::string s = arg.generic_string();
		bool has_sep = s.find('/') != std::string::npos ||
					   s.find('\\') != std::string::npos;
		if (!has_sep) {
			std::string dir = o.value("--dir", "libs");
			if (dir.empty() || dir == "." || dir == "/")
				util::die("--dir invalide : '" + dir + "'");
			// On normalise et on retire un éventuel séparateur final.
			fs::path d = fs::path(dir).lexically_normal();
			if (d.empty() || *d.begin() == "..")
				util::die("--dir doit rester dans le workspace : '" + dir + "'");
			rel = d / arg;
		} else {
			rel = fs::absolute(invoked / arg).lexically_normal().lexically_relative(c.p.root);
		}
	}
	rel = rel.lexically_normal();

	std::string name = o.value("--name", rel.filename().string());
	std::string type = o.value("--type", "static");
	if (type == "library") type = "static";
	if (type == "normal") type = "executable";            // alias de --type normal
	std::string lang = o.value("--lang", "c++");
	if (lang == "cpp" || lang == "cxx") lang = "c++";
	if (lang != "c" && lang != "c++") util::die("--lang : c ou c++");
	auto deps = util::split_list(o.value("--deps"));
	if (c.p.find_module(name)) util::die("un module nommé '" + name + "' existe déjà");
	for (auto& d : deps) if (!c.p.find_module(d)) util::die("dépendance inconnue : " + d);
	if (fs::exists(rel)) util::die("le dossier existe déjà : " + rel.string());
	if (!util::is_safe_name(name)) util::die("nom de module invalide : " + name);

	// --- Enregistrement dans [workspace].modules ---------------------------
	std::string text;
	util::read_file("cpp-maker.toml", text);
	auto mods = c.p.module_paths;
	mods.push_back(rel.generic_string());
	std::string new_text = set_workspace_modules(text, mods);

	util::write_file(rel / "module.toml", module_toml(name, type, deps));
	if (!o.flag("--no-scaffold")) {
		// main() appelle <dep>::add() uniquement si l'en-tête de squelette de
		// <dep> la déclare ; sinon le programme généré n'utilise que lui-même.
		std::string uses;
		for (auto& d : deps) {
			auto dm = c.p.find_module(d);
			if (dm->type == "executable") continue;
			for (auto ext : {".hpp", ".h"}) {
				std::string hdr;
				if (util::read_file(util::under(dm->dir, fs::path("include") / d / (d + ext)), hdr) &&
					hdr.find("add(int a, int b)") != std::string::npos) { uses = d; break; }
			}
			if (!uses.empty()) break;
		}
		write_module_sources(rel, name, type, lang == "c", type == "executable" ? uses : "");
	}
	util::write_file_atomic("cpp-maker.toml", new_text);
	try {
		auto p = cfg::load(c.p.root, "cpp-maker.toml");
		auto e = cfg::validate_errors(p);
		if (!e.empty()) throw std::runtime_error(e);
	} catch (const std::exception& e) {
		util::write_file_atomic("cpp-maker.toml", text);
		util::die(std::format("ajout annulé (cpp-maker.toml restauré) : {}", e.what()));
	}
	util::outln("\033[1;32mModule '{}' ({}) ajouté dans {}\033[0m", name, type, rel.generic_string());
	util::outln("  cpp-maker build -m {}", name);
	return 0;
}

// ---------------------------------------------------------------------------
// module rename / module remove : gestion des modules du workspace
// ---------------------------------------------------------------------------
// Remplace la valeur "key = <chaîne>" de la table [section] d'un texte TOML ;
// la clé est insérée sous l'en-tête si elle est absente.
std::string replace_toml_string(const std::string& text, const std::string& section,
									   const std::string& key, const std::string& value) {
	auto sec = text.find("[" + section + "]");
	if (sec == std::string::npos) return text;
	size_t bol = text.find('\n', sec);
	if (bol == std::string::npos) return text;
	++bol;
	for (;;) {
		size_t eol = text.find('\n', bol);
		if (eol == std::string::npos) eol = text.size();
		size_t b = text.find_first_not_of(" \t", bol);
		if (b < eol && text[b] == '[') break;                    // section suivante
		if (b < eol && text.compare(b, key.size(), key) == 0 &&
			(b + key.size() >= eol || text[b + key.size()] == ' ' ||
			 text[b + key.size()] == '\t' || text[b + key.size()] == '=')) {
			auto eq = text.find('=', b);
			if (eq == std::string::npos || eq > eol) break;
			auto q1 = text.find('"', eq);
			if (q1 == std::string::npos || q1 > eol) break;
			auto q2 = text.find('"', q1 + 1);
			if (q2 == std::string::npos) break;
			return text.substr(0, q1 + 1) + value + text.substr(q2);
		}
		if (eol == text.size()) break;
		bol = eol + 1;
	}
	auto eol = text.find('\n', sec);
	if (eol == std::string::npos) return text + "\n" + key + " = \"" + value + "\"\n";
	return text.substr(0, eol + 1) + key + " = \"" + value + "\"\n" + text.substr(eol + 1);
}

// Remplace la valeur "key = [...]" de la table [section] par un tableau de
// chaînes ; insère la section puis la clé si l'une des deux est absente. Le
// tableau multi-lignes existant (commentaires compris) est intégralement
// remplacé, ce qui rend la fonction idempotente.
std::string replace_toml_array(const std::string& text, const std::string& section,
									  const std::string& key, const std::vector<std::string>& values) {
	std::string new_arr = key + " = " + toml_array(values);
	auto sec = text.find("[" + section + "]");
	if (sec == std::string::npos) {
		std::string s = text;
		if (!s.empty() && s.back() != '\n') s += '\n';
		s += "\n[" + section + "]\n" + new_arr + "\n";
		return s;
	}
	size_t bol = text.find('\n', sec);
	if (bol == std::string::npos) return text + "\n" + new_arr + "\n";
	++bol;
	for (;;) {
		size_t eol = text.find('\n', bol);
		if (eol == std::string::npos) eol = text.size();
		size_t b = text.find_first_not_of(" \t", bol);
		// Section suivante : la clé est absente, on l'insère juste avant.
		if (b < eol && text[b] == '[')
			return text.substr(0, bol) + new_arr + "\n" + text.substr(bol);
		if (b < eol && text.compare(b, key.size(), key) == 0 &&
			(b + key.size() >= eol || text[b + key.size()] == ' ' ||
			 text[b + key.size()] == '\t' || text[b + key.size()] == '=')) {
			auto eq = text.find('=', b);
			if (eq == std::string::npos || eq > eol) return text;
			auto lb = text.find('[', eq);
			if (lb == std::string::npos) return text;
			size_t rb = lb + 1;
			bool in_str = false;
			for (; rb < text.size(); ++rb) {
				char ch = text[rb];
				if (ch == '"') in_str = !in_str;
				else if (ch == '#' && !in_str) { while (rb < text.size() && text[rb] != '\n') ++rb; }
				else if (ch == ']' && !in_str) break;
			}
			if (rb >= text.size()) return text;
			size_t indent = b - bol;
			return text.substr(0, b) + key + " = " + toml_array(values) + text.substr(rb + 1);
			(void)indent;
		}
		if (eol == text.size()) {
			// Fin de fichier : append dans la section courante.
			std::string s = text;
			if (s.back() != '\n') s += '\n';
			s += new_arr + "\n";
			return s;
		}
		bol = eol + 1;
	}
}

// Détermine si le module peut être édité automatiquement (module.toml dédié,
// ou cpp-maker.toml d'un projet mono-module) — refuse les configs v0.4 avec
// plusieurs modules partageant le même fichier.
fs::path editable_module_file(const Project& p, const ModuleConfig& m) {
	if (m.from_git) util::die(std::format("module '{}' importé de '{}' : non éditable", m.name, m.dir.string()));
	if (m.file.filename() == "module.toml") return m.file;

	int shared = 0;
	for (auto& x : p.modules) if (x.file == m.file) ++shared;
	if (shared > 1)
		util::die(std::format("édition impossible : configuration v0.4 (plusieurs modules dans "
							  "{}) — convertissez en workspace (un module.toml par module)",
							  m.file.string()));
	return m.file;
}

// cpp-maker module add <chemin> [--type …] [--lang …] [--deps a,b] [--no-scaffold]
// cpp-maker module rename <ancien> <nouveau>
// cpp-maker module remove <nom> [--force] [--purge]
//
// Les fichiers TOML sont modifiés puis la configuration entière est
// revalidée ; en cas d'échec, tout est restauré (remplacement atomique).
int cmd_module(const std::vector<std::string>& args) {
	if (args.empty())
		util::die("usage : cpp-maker module <deps|add|rename|remove> … (voir 'cpp-maker help')");
	const std::string& sub = args[0];
	if (sub == "add") {
		std::vector<std::string> rest(args.begin() + 1, args.end());
		return cmd_add(rest);
	}

	auto c = load_ctx();
	if (!c.p.workspace && sub != "deps")
		util::die("la gestion des modules (rename/remove) requiert un workspace "
				  "([workspace] modules = […] ; ou 'cpp-maker new <nom> --workspace')");

	struct Edit { fs::path file; std::string before, after; };

		if (sub == "deps") {
		auto o = parse_opts(args.size() > 1
							? std::vector<std::string>(args.begin() + 1, args.end())
							: std::vector<std::string>{},
							{}, {"--pkg-config", "--system", "--module"});
		if (o.positional.size() < 2)
			util::die("usage : cpp-maker module deps <list|add|remove> <module> [<nom>] "
					  "[--pkg-config|--system|--module]");
		const std::string action = o.positional[0];
		const std::string mname  = o.positional[1];
		const ModuleConfig* m = c.p.find_module(mname);
		if (!m) util::die("module inconnu : " + mname);

		if (action == "list") {
			util::outln("\033[1;36mDépendances de '{}'\033[0m ({})", mname, m->file.generic_string());
			util::outln("  modules    : {}", m->deps.empty() ? "—" : util::join(m->deps, ", "));
			util::outln("  pkg-config : {}", m->pkg_config.empty() ? "—" : util::join(m->pkg_config, ", "));
			util::outln("  système    : {}", m->system_libs.empty() ? "—" : util::join(m->system_libs, ", "));
			return 0;
		}
		if (action != "add" && action != "remove")
			util::die("action inconnue : " + action + " (list|add|remove)");
		if (o.positional.size() < 3)
			util::die("cpp-maker module deps " + action + " <module> <nom>");

		std::string name = o.positional[2];
		if (name.starts_with("-l")) name = name.substr(2);  // -lz → z
		if (name.empty() || !util::is_safe_name(name))
			util::die("nom de dépendance invalide : '" + o.positional[2] + "'");

		enum Kind { KModule, KPkgConfig, KSystem };
		int nflags = (int)o.flag("--pkg-config") + (int)o.flag("--system") + (int)o.flag("--module");
		if (nflags > 1)
			util::die("--pkg-config, --system et --module sont mutuellement exclusifs");
		auto has = [&](const std::vector<std::string>& v) {
			return std::find(v.begin(), v.end(), name) != v.end();
		};
		Kind kind;
		if (o.flag("--pkg-config")) kind = KPkgConfig;
		else if (o.flag("--system")) kind = KSystem;
		else if (o.flag("--module")) kind = KModule;
		// remove : le type est celui de la liste où le nom figure déjà.
		else if (action == "remove" && has(m->deps)) kind = KModule;
		else if (action == "remove" && has(m->pkg_config)) kind = KPkgConfig;
		else if (action == "remove" && has(m->system_libs)) kind = KSystem;
		else if (c.p.find_module(name)) kind = KModule;
		else if (util::have("pkg-config") &&
				 proc::capture({"pkg-config", "--exists", name}).exit_code == 0)
			kind = KPkgConfig;
		else kind = KSystem;
		const char* label = kind == KModule ? "module" : kind == KPkgConfig ? "pkg-config" : "système";

		std::vector<std::string> mods = m->deps, pcs = m->pkg_config, sys = m->system_libs;
		std::vector<std::string>& L = kind == KModule ? mods : kind == KPkgConfig ? pcs : sys;

		if (action == "add") {
			if (kind == KModule && !c.p.find_module(name))
				util::die("module inconnu : " + name);
			if (kind == KModule && name == mname)
				util::die("un module ne peut pas dépendre de lui-même");
			if (std::find(L.begin(), L.end(), name) != L.end()) {
				util::outln("'{}' est déjà une dépendance ({}) de '{}'.", name, label, mname);
				return 0;
			}
			if (kind == KPkgConfig && util::have("pkg-config") &&
				proc::capture({"pkg-config", "--exists", name}).exit_code != 0)
				util::warn("pkg-config ne connaît pas '{}' sur l'hôte : le build échouera tant "
						   "que son paquet de développement n'est pas installé", name);
			L.push_back(name);
		} else {
			auto it = std::find(L.begin(), L.end(), name);
			if (it == L.end())
				util::die(std::format("'{}' n'est pas une dépendance ({}) de '{}'",
									  name, label, mname));
			L.erase(it);
		}

		fs::path file = editable_module_file(c.p, *m);
		std::string text;
		if (!util::read_file(file, text))
			util::die("impossible de lire " + file.string());
		std::string new_text = text;
		new_text = replace_toml_array(new_text, "dependencies", "modules",    mods);
		new_text = replace_toml_array(new_text, "dependencies", "pkg_config", pcs);
		new_text = replace_toml_array(new_text, "dependencies", "system",     sys);
		util::write_file_atomic(file, new_text);
		try {
			auto np = cfg::load(c.p.root, c.p.file);
			auto e = cfg::validate_errors(np);
			if (!e.empty()) throw std::runtime_error(e);
		} catch (const std::exception& e) {
			util::write_file_atomic(file, text);
			util::die(std::format("opération annulée ({} restauré) : {}", file.string(), e.what()));
		}
		util::outln("\033[1;32m{}\033[0m : '{}' {} ({})", mname, name,
					action == "add" ? "ajouté" : "retiré", label);
		return 0;
	}
	
	else if (sub == "rename") {
		if (args.size() != 3)
			util::die("usage : cpp-maker module rename <ancien> <nouveau>");
		const std::string& from = args[1];
		const std::string& to   = args[2];
		const ModuleConfig* m = c.p.find_module(from);
		if (!m) {
			std::vector<std::string> names;
			for (auto& x : c.p.modules) names.push_back(x.name);
			util::die(std::format("module inconnu : {} (modules : {})", from, util::join(names, ", ")));
		}
		if (from == to) { util::outln("rien à faire."); return 0; }
		if (!util::is_safe_name(to)) util::die("nom de module invalide : " + to);
		if (c.p.find_module(to)) util::die("le nom est déjà pris : " + to);

		std::vector<Edit> edits;
		// 1) module.toml : name = "<nouveau>".
		{
			std::string text;
			if (!util::read_file(m->file, text))
				util::die("impossible de lire " + m->file.string());
			edits.push_back({m->file, text, replace_toml_string(text, "module", "name", to)});
		}
		// 2) deps des autres modules.
		for (auto& o : c.p.modules) {
			if (o.name == from ||
				std::find(o.deps.begin(), o.deps.end(), from) == o.deps.end()) continue;
			std::string text;
			if (!util::read_file(o.file, text))
				util::die("impossible de lire " + o.file.string());
			std::vector<std::string> deps = o.deps;
			for (auto& d : deps) if (d == from) d = to;
			edits.push_back({o.file, text,
							 replace_toml_array(text, "dependencies", "modules", deps)});
		}
		// 3) Répertoire conventionnel (<ancien> comme nom de dossier) :
		//    renommé sur le disque, chemin mis à jour dans [workspace].
		fs::path old_dir = m->dir, new_dir;
		bool dir_rename = m->dir.filename().string() == from;
		if (dir_rename) {
			std::string text;
			if (!util::read_file(c.p.file, text))
				util::die("impossible de lire " + c.p.file.string());
			std::vector<std::string> paths = c.p.module_paths;
			for (auto& mp : paths)
				if (fs::path(mp).lexically_normal() == old_dir)
					mp = (old_dir.parent_path() / to).generic_string();
			edits.push_back({c.p.file, text, set_workspace_modules(text, paths)});
			new_dir = old_dir.parent_path() / to;
		}

		for (auto& e : edits) util::write_file_atomic(e.file, e.after);
		std::error_code ec;
		if (dir_rename) {
			fs::rename(old_dir, new_dir, ec);
			if (ec) util::warn("renommage du répertoire impossible : {} → {}",
							   old_dir.string(), new_dir.string());
		}
		try {
			auto p = cfg::load(c.p.root, c.p.file);
			auto e = cfg::validate_errors(p);
			if (!e.empty()) throw std::runtime_error(e);
		} catch (const std::exception& e) {
			if (dir_rename && !ec) { std::error_code ec2; fs::rename(new_dir, old_dir, ec2); }
			for (auto& ed : edits) util::write_file_atomic(ed.file, ed.before);
			util::die(std::format("renommage annulé (fichiers restaurés) : {}", e.what()));
		}
		util::outln("\033[1;32mmodule '{}' renommé en '{}'.\033[0m", from, to);
		if (dir_rename && !ec) util::outln("  répertoire : {}", new_dir.generic_string());
		return 0;
	}

	else if (sub == "remove" || sub == "rm") {
		auto o = parse_opts(std::vector<std::string>(args.begin() + 1, args.end()),
							{}, {"--force", "--purge"});
		if (o.positional.size() != 1)
			util::die("usage : cpp-maker module remove <nom> [--force] [--purge]");
		const std::string& name = o.positional[0];
		const ModuleConfig* m = c.p.find_module(name);
		if (!m) util::die("module inconnu : " + name);

		std::vector<std::string> dependents;
		for (auto& x : c.p.modules)
			if (x.name != name &&
				std::find(x.deps.begin(), x.deps.end(), name) != x.deps.end())
				dependents.push_back(x.name);
		if (!dependents.empty() && !o.flag("--force"))
			util::die(std::format("des modules dépendent de '{}' : {} — retirez d'abord la "
								  "dépendance ou réessayez avec --force",
								  name, util::join(dependents, ", ")));

		std::vector<Edit> edits;
		for (auto& x : c.p.modules) {
			if (x.name == name ||
				std::find(x.deps.begin(), x.deps.end(), name) == x.deps.end()) continue;
			std::string text;
			if (!util::read_file(x.file, text))
				util::die("impossible de lire " + x.file.string());
			std::vector<std::string> deps;
			for (auto& d : x.deps) if (d != name) deps.push_back(d);
			edits.push_back({x.file, text,
							 replace_toml_array(text, "dependencies", "modules", deps)});
		}
		{
			std::string text;
			if (!util::read_file(c.p.file, text))
				util::die("impossible de lire " + c.p.file.string());
			std::vector<std::string> paths;
			for (auto& mp : c.p.module_paths)
				if (fs::path(mp).lexically_normal() != m->dir) paths.push_back(mp);
			edits.push_back({c.p.file, text, set_workspace_modules(text, paths)});
		}
		for (auto& e : edits) util::write_file_atomic(e.file, e.after);
		try {
			auto p = cfg::load(c.p.root, c.p.file);
			auto e = cfg::validate_errors(p);
			if (!e.empty()) throw std::runtime_error(e);
		} catch (const std::exception& e) {
			for (auto& ed : edits) util::write_file_atomic(ed.file, ed.before);
			util::die(std::format("suppression annulée (fichiers restaurés) : {}", e.what()));
		}
		if (o.flag("--purge")) {
			std::error_code ec;
			if (fs::exists(m->dir, ec)) {
				fs::remove_all(m->dir, ec);
				util::outln("  répertoire supprimé : {}", m->dir.generic_string());
			}
		}
		util::outln("\033[1;32mmodule '{}' retiré du workspace.\033[0m", name);
		if (!o.flag("--purge"))
			util::outln("  (les sources de {} restent sur le disque ; --purge pour les supprimer)",
						m->dir.generic_string());
		return 0;
	}

	util::die("sous-commande module inconnue : " + sub + " (attendu : deps | add | rename | remove)");
}

// ---------------------------------------------------------------------------
// graph / modules / info
// ---------------------------------------------------------------------------
int cmd_graph(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {}, {"--dot"});
	auto c = load_ctx();
	if (o.flag("--dot")) {
		util::outln("digraph \"{}\" {{", c.p.name);
		util::outln("  rankdir=LR; node [shape=box, fontname=\"Helvetica\"];");
		auto node = [&](const ModuleConfig& m, const char* indent) {
			std::string shape = m.type == "executable" ? "box3d" : m.type == "header-only" ? "note" : "box";
			util::outln("{}\"{}\" [shape={}, label=\"{}\\n({})\"];", indent, m.name, shape, m.name, m.type);
		};
		// v0.6 : un sous-graphe par couche, de la plus basse à la plus haute.
		for (auto& l : c.p.layer_order) {
			util::outln("  subgraph \"cluster_{}\" {{ label=\"{}\"; style=rounded;", l, l);
			for (auto& m : c.p.modules) if (m.layer == l) node(m, "    ");
			util::outln("  }}");
		}
		for (auto& m : c.p.modules) {
			if (m.layer.empty()) node(m, "  ");
			for (auto& d : m.deps) util::outln("  \"{}\" -> \"{}\";", m.name, d);
			for (auto& d : m.pkg_config) util::outln("  \"{}\" -> \"pkg:{}\" [style=dashed];", m.name, d);
		}
		util::outln("}}");
		return 0;
	}
	std::function<void(const std::string&, const std::string&, int)> show =
		[&](const std::string& n, const std::string& indent, int depth) {
			auto m = c.p.find_module(n);
			for (size_t i = 0; i < m->deps.size(); ++i) {
				bool last = i + 1 == m->deps.size();
				auto d = c.p.find_module(m->deps[i]);
				util::outln("{}{}{} \033[2m({})\033[0m", indent, last ? "└─ " : "├─ ", d->name, d->type);
				if (depth < 32) show(d->name, indent + (last ? "   " : "│  "), depth + 1);
			}
		};
	std::set<std::string> is_dep;
	for (auto& m : c.p.modules) for (auto& d : m.deps) is_dep.insert(d);
	for (auto& m : c.p.modules) {
		if (is_dep.count(m.name) && !o.all) continue;
		util::outln("\033[1m{}\033[0m \033[2m({}{})\033[0m", m.name, m.type,
					m.layer.empty() ? "" : ", " + m.layer);
		show(m.name, "", 0);
	}
	return 0;
}

int cmd_modules(const std::vector<std::string>& args) {
	(void)parse_opts(args);
	auto c = load_ctx();
	util::outln("\033[1;36m{:<16} {:<12} {:<15} {:<34} {}\033[0m", "MODULE", "TYPE", "COUCHE", "DOSSIER",
				"DÉPENDANCES");
	for (auto& m : c.p.modules) {
		std::vector<std::string> deps = m.deps;
		for (auto& x : m.pkg_config) deps.push_back("pkg:" + x);
		for (auto& x : m.system_libs) deps.push_back("-l" + x);
		util::outln("{:<16} {:<12} {:<15} {:<34} {}", m.name, m.type, m.layer.empty() ? "—" : m.layer,
					m.dir.generic_string(), deps.empty() ? "—" : util::join(deps, ", "));
	}
	return 0;
}

void print_toolchain(const toolchain::Toolchain& t, const std::string& indent) {
	util::outln("{}cc        : {}", indent, t.cc ? t.cc->exe + "  (" + t.cc->version + ")" : "—");
	util::outln("{}cxx       : {}", indent, t.cxx ? t.cxx->exe + "  (" + t.cxx->version + ")" : "—");
	util::outln("{}ar/strip  : {} / {}", indent, t.ar, t.strip);
	util::outln("{}cible     : {} (arch {}, os {}){}", indent, t.triple.empty() ? "?" : t.triple, t.arch, t.os,
				t.cross ? " — compilation croisée" : "");
	if (t.cross)
		util::outln("{}exécution : {}", indent,
					t.runner.empty() ? "impossible — " + t.runner_note : util::join(t.runner, " "));
}

// ---------------------------------------------------------------------------
// info : résumé du projet (sans -m) ou détail d'un module (avec -m).
// ---------------------------------------------------------------------------

// Affiche la configuration complète d'un module. Utilisé par cmd_info -m.
static void print_module_info(const Project& p, const ModuleConfig& m) {
	util::outln("\033[1;36mcpp-maker {} — module '{}'\033[0m",
				CPP_MAKER_VERSION, m.name);

	if (!m.description.empty())
		util::outln("  description  : {}", m.description);
	if (!m.version.empty())
		util::outln("  version      : {}", m.version);
	util::outln("  type         : {}", m.type);
	if (!m.layer.empty())
		util::outln("  couche       : {}", m.layer);
	util::outln("  dossier      : {}", m.dir.generic_string());
	util::outln("  fichier      : {}", m.file.generic_string());
	if (m.from_git)
		util::outln("  source       : \033[1;35mdépendance git\033[0m");

	// Standard effectif (module ou hérité du projet).
	std::string cxx = m.cxx_standard.empty() ? p.cxx_standard + " (projet)" : m.cxx_standard;
	std::string c   = m.c_standard.empty()   ? p.c_standard   + " (projet)" : m.c_standard;
	util::outln("  standard     : {} / {}", cxx, c);

	if (!m.main.empty())
		util::outln("  main         : {}", m.main);

	// Sources
	util::outln("  source_dirs  : {}",
				m.source_dirs.empty() ? "—" : util::join(m.source_dirs, ", "));
	if (!m.source_files.empty())
		util::outln("  source_files : {}", util::join(m.source_files, ", "));
	if (!m.exclude.empty())
		util::outln("  exclude      : {}", util::join(m.exclude, ", "));

	// Includes
	util::outln("  includes     : public = [{}]",
				util::join(m.public_includes, ", "));
	if (!m.private_includes.empty())
		util::outln("                 private = [{}]",
					util::join(m.private_includes, ", "));

	if (!m.test_dirs.empty())
		util::outln("  test_dirs    : {}", util::join(m.test_dirs, ", "));

	// Dépendances directes
	if (!m.deps.empty())
		util::outln("  modules      : {}", util::join(m.deps, ", "));
	if (!m.pkg_config.empty())
		util::outln("  pkg_config   : {}", util::join(m.pkg_config, ", "));
	if (!m.system_libs.empty())
		util::outln("  système      : {}", util::join(m.system_libs, ", "));
	if (!m.frameworks.empty())
		util::outln("  frameworks   : {}", util::join(m.frameworks, ", "));
	for (auto& g : m.git_deps) {
		std::string sub = g.subdir.empty() ? "" : " (subdir: " + g.subdir + ")";
		std::string mod = g.module.empty() ? "" : " · module: " + g.module;
		util::outln("  git[{}]     : {} @ {}{}{}",
					g.alias, g.url, g.version, sub, mod);
	}

	// Flags
	if (!m.defines.empty())         util::outln("  defines      : {}", util::join(m.defines, ", "));
	if (!m.public_defines.empty())  util::outln("  pub_defines  : {}", util::join(m.public_defines, ", "));
	if (!m.cflags.empty())          util::outln("  cflags       : {}", util::join(m.cflags, " "));
	if (!m.cxxflags.empty())        util::outln("  cxxflags     : {}", util::join(m.cxxflags, " "));
	if (!m.ldflags.empty())         util::outln("  ldflags      : {}", util::join(m.ldflags, " "));

	util::outln("  install      : {}", m.install ? "oui" : "non");

	// Overrides de plateforme
	if (!m.platforms.empty()) {
		util::outln("  plateformes  :");
		for (auto& [os, po] : m.platforms) {
			util::outln("    [platform.{}]", os);
			if (!po.sources.empty())     util::outln("      sources    : {}", util::join(po.sources, ", "));
			if (!po.defines.empty())     util::outln("      defines    : {}", util::join(po.defines, ", "));
			if (!po.system_libs.empty()) util::outln("      système    : {}", util::join(po.system_libs, ", "));
			if (!po.cflags.empty())      util::outln("      cflags     : {}", util::join(po.cflags, " "));
			if (!po.cxxflags.empty())    util::outln("      cxxflags   : {}", util::join(po.cxxflags, " "));
			if (!po.ldflags.empty())     util::outln("      ldflags    : {}", util::join(po.ldflags, " "));
		}
	}

	// Fermeture transitive (toutes les dépendances, pas seulement directes).
	try {
		auto closure = cfg::topo_order(p, {m.name});
		if (!closure.empty()) closure.pop_back();   // retire le module lui-même
		if (!closure.empty())
			util::outln("  fermeture    : {}", util::join(closure, ", "));
	} catch (const std::exception&) { /* cycle : déjà signalé par validate */ }

	// Sources réellement découvertes (utile pour vérifier exclude/source_files).
	auto srcs = build::discover(m, m.source_dirs);
	if (!srcs.empty()) {
		util::outln("  sources ({} fichier{}) :", srcs.size(), srcs.size() > 1 ? "s" : "");
		for (auto& s : srcs) {
			const char* lang = s.lang == build::Lang::CXX ? "C++"
							 : s.lang == build::Lang::C   ? "C  "
							 :                              "ASM";
			util::outln("    [{}] {}", lang, s.path.generic_string());
		}
	}

	// Modules qui dépendent de celui-ci (pour évaluer l'impact d'une modif).
	std::vector<std::string> revdeps;
	for (auto& x : p.modules)
		if (x.name != m.name &&
			std::find(x.deps.begin(), x.deps.end(), m.name) != x.deps.end())
			revdeps.push_back(x.name);
	if (!revdeps.empty())
		util::outln("  utilisé par  : {}", util::join(revdeps, ", "));
}

// Recherche un module par nom logique, puis par nom de dossier en secours.
// Ce second mode rend `-m ui_patchbay` fonctionnel quand `dir = "ui_patchbay"`
// mais que le module.toml déclare `name = "audio_patchbay"`.
static const ModuleConfig* find_module_flex(const Project& p, const std::string& key) {
	if (auto m = p.find_module(key)) return m;
	for (auto& mod : p.modules)
		if (mod.dir.filename().string() == key) return &mod;
	return nullptr;
}

int cmd_info(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	// Pas de mode_from_positional : pour info, un positionnel serait ambigu
	// (le module se donne via -m). Les positionnels sont rejetés ci-dessous.
	help::require_no_positional(o, "info");
	help::require_no_passthrough(o, "info");

	auto c = load_ctx(false);
	auto& p = c.p;

	// ---------- Détail d'un ou plusieurs modules ----------
	if (!o.modules.empty()) {
		int rc = 0;
		bool first = true;
		for (auto& key : o.modules) {
			auto m = find_module_flex(p, key);
			if (!m) {
				util::errln("\033[1;31merror:\033[0m module inconnu : '{}'", key);
				rc = 1;
				continue;
			}
			if (!first) util::outln("");
			first = false;
			print_module_info(p, *m);
		}
		if (rc != 0) {
			std::vector<std::string> names;
			for (auto& x : p.modules) names.push_back(x.name);
			util::errln("  modules connus : {}", util::join(names, ", "));
		}
		return rc;
	}

	// ---------- Résumé du projet (comportement historique) ----------
	util::outln("\033[1;36mcpp-maker {} — info\033[0m", CPP_MAKER_VERSION);
	util::outln("  racine     : {}", p.root.string());
	util::outln("  projet     : {} {}{}", p.name, p.version, p.workspace ? "  (workspace)" : "");
	util::outln("  standards  : {} / {}", p.cxx_standard, p.c_standard);
	util::outln("  défauts    : mode {}, toolchain {}", p.default_mode, p.default_toolchain);
	std::vector<std::string> modes;
	for (auto& m : p.modes) modes.push_back(m.name);
	util::outln("  modes      : {}", util::join(modes, ", "));
	std::vector<std::string> tcs;
	for (auto& [n, t] : p.toolchains) tcs.push_back(n);
	util::outln("  toolchains : {}", tcs.empty() ? "(host)" : util::join(tcs, ", "));
	util::outln("  modules    :");
	for (auto& m : p.modules)
		util::outln("    - {:<14} {:<12} {}{}", m.name, m.type, m.dir.generic_string(),
					m.deps.empty() ? "" : "  ← " + util::join(m.deps, ", "));
	if (!p.deploys.empty()) {
		std::vector<std::string> d;
		for (auto& [n, x] : p.deploys) d.push_back(n);
		util::outln("  déploiements : {}", util::join(d, ", "));
	}
	util::outln("\n\033[1;36mtoolchain par défaut : {}\033[0m", p.default_toolchain);
	auto t = toolchain::resolve(p, p.default_toolchain);
	print_toolchain(t);
	util::outln("\n\033[2mDétail d'un module : cpp-maker info -m <nom>\033[0m");
	return 0;
}

// ---------------------------------------------------------------------------
// size : contenu détaillé des binaires construits
//   text/data/bss/dec/hex (sémantique Berkeley de `size`), en-tête ELF
//   (classe, endianness, machine, ABI, type, point d'entrée), archives .a
//   sommées. Sans dépendance à binutils : les binaires croisés sont lisibles
//   même sans binutils de la cible.
// ---------------------------------------------------------------------------
int cmd_size(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	mode_from_positional(o);
	auto c = load_ctx(false);
	std::string mode = pick_mode(c, o);
	std::vector<std::string> tcs = pick_toolchains(c, o);

	// Filtre -m <module> : exécutable <nom>[.exe], bibliothèque lib<nom>.*,
	// exécutable de tests <nom>-tests.
	auto wanted = [&](const std::string& file) {
		if (o.modules.empty()) return true;
		std::string stem = fs::path(file).stem().string();
		for (auto& m : o.modules) {
			if (stem == m || stem == m + "-tests" || stem == m + ".dll") return true;
			if (stem == "lib" + m || stem.starts_with("lib" + m + ".")) return true;
		}
		return false;
	};
	auto row_header = [] {
		util::outln("    {:>10} {:>10} {:>10} {:>10} {:>10}", "text", "data", "bss", "dec", "hex");
	};
	auto row = [](const elf::Info& in) {
		util::outln("    {:>10} {:>10} {:>10} {:>10} {:>10x}",
					in.text, in.data, in.bss, in.dec(), in.dec());
	};

	size_t listed = 0;
	for (auto& tc : tcs) {
		fs::path dir = build::build_dir_for(tc, mode);
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) {
			util::warn("rien de construit pour la toolchain '{}' en mode {} ({})",
					   tc, mode, dir.generic_string());
			continue;
		}
		util::outln("\033[1;36m{} — toolchain {} · mode {}\033[0m",
					dir.generic_string(), tc, mode);
		for (auto* sub : {"bin", "lib", "tests"}) {
			fs::path d = dir / sub;
			if (!fs::is_directory(d, ec)) continue;
			std::vector<fs::path> files;
			for (fs::directory_iterator it(d, ec), end; it != end && !ec; it.increment(ec))
				if (it->is_regular_file(ec) && it->path().extension() != ".tmp")
					files.push_back(it->path());
			std::sort(files.begin(), files.end());
			for (auto& f : files) {
				if (!wanted(f.filename().string())) continue;
				++listed;
				auto in = elf::read(f);
				std::string rel = f.lexically_relative(dir).generic_string();
				if (!in.ok) {
					util::outln("  \033[1m{}\033[0m — {} ({:L} octets)", rel, in.note, in.file_size);
					continue;
				}
				if (in.members > 0) {
					util::outln("  \033[1m{}\033[0m — archive statique, {} objet{} ELF ({:L} octets)",
								rel, in.members, in.members > 1 ? "s" : "", in.file_size);
				} else {
				util::outln("  \033[1m{}\033[0m — {} {}, machine {}, ABI {},\n"
							"                {}, entrée 0x{:x} — {:L} octets",
							rel, in.cls, in.endian, in.machine, in.abi, in.type,
							in.entry, in.file_size);
				}
				row_header();
				row(in);
				if (o.verbose) {
					auto secs = in.sections;
					std::sort(secs.begin(), secs.end(),
							  [](auto& a, auto& b) { return a.size > b.size; });
					size_t n = std::min<size_t>(secs.size(), 10);
					for (size_t i = 0; i < n; ++i)
						util::outln("      {:<20} {:>10} octets   [{}{}]",
									secs[i].name.empty() ? "«sans nom»" : secs[i].name,
									secs[i].size,
									secs[i].flags & 4 ? "X" : "-",
									secs[i].flags & 1 ? "W" : "R");
				}
			}
		}
	}
	if (listed == 0)
		util::die(std::format("aucun binaire à analyser — lancez 'cpp-maker build' d'abord "
							  "(mode {}) ou vérifiez -t/-m", mode));
	return 0;
}

int cmd_deps(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	auto c = load_ctx();                       // déclenche la résolution
	const std::string sub = o.positional.empty() ? "list" : o.positional[0];

	if (sub == "list" || sub == "show") {
		util::outln("\033[1;36mDépendances git ({})\033[0m", c.p.git_lock.size());
		if (c.p.git_lock.empty()) { util::outln("  (aucune)"); return 0; }
		for (auto& [url, commit] : c.p.git_lock) {
			std::string alias;
			for (auto& m : c.p.modules)
				if (m.from_git && m.dir.string().find(deps::url_key(url)) != std::string::npos) {
					alias = m.name; break;
				}
			util::outln("  {:<14} {} @ {}", alias.empty() ? "?" : alias, url, commit.substr(0, 10));
		}
		return 0;
	}
	if (sub == "update") {
		std::error_code ec;
		fs::remove(c.p.root / "cpp-maker.lock", ec);
		c.p.git_lock.clear();
		cfg::resolve_git_modules(c.p);
		cfg::write_lock_file(c.p);
		util::outln("\033[1;32m{} dépendance(s) résolue(s)\033[0m", c.p.git_lock.size());
		return 0;
	}
	if (sub == "clean") {
		auto root = deps::cache_root();
		std::error_code ec;
		if (!fs::exists(root, ec)) { util::outln("Cache vide."); return 0; }
		fs::remove_all(root, ec);
		util::outln("\033[1;32mCache supprimé : {}\033[0m", root.string());
		return 0;
	}
	util::die("cpp-maker deps <list|update|clean>");
}

} // namespace commands