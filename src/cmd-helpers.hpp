// cmd-helpers.hpp — helpers communs aux sous-commandes + registre.
#pragma once
#include "cpp-maker.hpp"

namespace commands::help {

// ---------------------------------------------------------------------------
// Métadonnées d'une commande.
//
// Deux niveaux de lecture :
//   - `brief`      : 1 ligne, pour la liste (`cpp-maker help`)
//   - `description`: paragraphes complets, pour le détail (`cpp-maker help <cmd>`)
//
// Convention de mise en forme :
//   - `description` et `notes` : texte libre ; les lignes commençant par un
//     espace sont laissées telles quelles (arbres, tableaux), les autres sont
//     rewrappées à la largeur du terminal.
//   - `examples` : lignes courtes, imprimées telles quelles (commentaires `#`
//     préservés).
//   - `see_also` : noms de commandes (affichés séparés par `, `).
// ---------------------------------------------------------------------------
struct OptionHelp {
	std::string flags;         // ex. "-m, --module <a,b>"
	std::string description;   // explication complète
};

struct CommandSpec {
	std::string name;                          // obligatoire
	std::vector<std::string> aliases;          // optionnel
	std::string brief;                         // obligatoire
	std::string description;                   // optionnel
	std::string usage;                         // optionnel
	std::vector<OptionHelp> options;           // optionnel
	std::vector<OptionHelp> subcommands;       // optionnel
	std::string examples;                      // optionnel
	std::vector<std::string> notes;            // optionnel
	std::vector<std::string> see_also;         // optionnel
	std::function<int(const std::vector<std::string>&)> run;   // obligatoire
};

struct Category {
	std::string title;
	std::vector<std::string> commands;          // noms dans le registre
};

const std::vector<CommandSpec>& registry();
const std::vector<Category>&    categories();
const CommandSpec* find(const std::string& name);

void print_list();
void print_command(const CommandSpec& spec);
int  cmd_help(const std::vector<std::string>& args);

// ---------------------------------------------------------------------------
// Validation rapide (die() avec message uniforme)
// ---------------------------------------------------------------------------
[[noreturn]] void usage_error(std::string_view cmd, std::string_view detail);

const ModuleConfig& require_module(const Ctx& c, const std::string& name, std::string_view cmd);
const ModuleConfig& require_executable(const Ctx& c, const std::string& name, std::string_view cmd);
const ModeConfig&   require_mode(const Ctx& c, const std::string& name, std::string_view cmd);

void require_workspace(const Ctx& c, std::string_view cmd);
void require_project_file(const Ctx& c, std::string_view cmd);
void require_no_positional(const Opts& o, std::string_view cmd);
void require_no_passthrough(const Opts& o, std::string_view cmd);

std::string require_single_toolchain(const Ctx& c, const Opts& o,
									 const std::string& def, std::string_view cmd);

// ---------------------------------------------------------------------------
// Composition : regroupe les patterns répétés dans les commandes.
// ---------------------------------------------------------------------------
void banner(std::string_view cmd, const Ctx& c);

template <class F>
int with_session(const Ctx& c, const Opts& o, const std::string& tc,
				 const std::string& mode, const std::vector<std::string>& mods,
				 bool tests, F&& f) {
	// --sanitize all : une session par passe ; premier code d'erreur renvoyé.
	int rc = 0;
	for (auto& po : sanitize::expand_all(c.p, o, tc, mode)) {
		auto s = prepare(c, po, tc, mode, mods, tests);
		int r = run_plan(c, s, po) != 0 ? 1 : po.dry_run ? 0 : f(s);
		if (rc == 0) rc = r;
	}
	return rc;
}

template <class Runner, class After>
int for_each_build(const Ctx& c, const Opts& o,
				   const std::string& tc_default, const std::string& mode,
				   const std::vector<std::string>& mods, bool tests,
				   std::string_view label, Runner&& runner, After&& after) {
	std::vector<std::string> failed;
	for (auto& tcn : pick_toolchains(c, o, tc_default)) {
		for (auto& po : sanitize::expand_all(c.p, o, tcn, mode)) {
			auto s = prepare(c, po, tcn, mode, mods, tests);
			int rc = runner(c, s, po);
			if (rc != 0) { failed.push_back(std::format("{} · {}", s.tc->name, s.mode->name)); continue; }
			if (!po.dry_run) after(s);
		}
	}
	if (!failed.empty()) {
		util::errln("\033[1;31m{} en échec pour : {}\033[0m",
					label, util::join(failed, ", "));
		return 1;
	}
	return 0;
}

} // namespace commands::help