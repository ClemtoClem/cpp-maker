#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

namespace commands {

// ---------------------------------------------------------------------------
// Contexte : racine du projet, configuration, module courant
// ---------------------------------------------------------------------------

std::vector<int> parse_semver(const std::string& s) {
	std::vector<int> v;
	for (auto& part : util::split_list(s, '.')) {
		int x = 0;
		for (char c : part) { if (!std::isdigit((unsigned char)c)) break; x = x * 10 + (c - '0'); }
		v.push_back(x);
	}
	while (v.size() < 3) v.push_back(0);
	return v;
}

std::optional<fs::path> find_root(const fs::path& from) {
	fs::path p = from;
	for (int i = 0; i < 64; ++i) {
		std::error_code ec;
		if (fs::exists(p / "cpp-maker.toml", ec)) return p;
		if (!p.has_parent_path() || p.parent_path() == p) break;
		p = p.parent_path();
	}
	return std::nullopt;
}

Ctx load_ctx(bool validate) {
	fs::path cwd = fs::current_path();
	fs::path root;
	if (auto r = find_root(cwd)) root = *r;
	else if (fs::exists(cwd / "src")) root = cwd; // projet v0.3 sans cpp-maker.toml
	else util::die(std::format("aucun cpp-maker.toml dans {} ni ses parents "
							   "(cpp-maker new <nom> pour créer un projet)", cwd.string()));
	// Tous les chemins internes sont relatifs à la racine.
	fs::current_path(root);
	Ctx c;
	try {
		c.p = cfg::load(root, "cpp-maker.toml");
	} catch (const std::exception& e) {
		util::die(e.what());
	}
	if (!c.p.min_version.empty() &&
		parse_semver(c.p.min_version) > parse_semver(std::string(CPP_MAKER_VERSION)))
		util::die(std::format("ce projet requiert cpp-maker ≥ {} (version actuelle {})",
							  c.p.min_version, CPP_MAKER_VERSION));
	if (validate) {
		auto e = cfg::validate_errors(c.p);
		if (!e.empty()) util::die(e);
	}
	if (c.p.workspace) {
		std::string rel = cwd.lexically_relative(root).generic_string();
		size_t best = 0;
		for (auto& m : c.p.modules) {
			std::string d = m.dir.generic_string();
			if ((rel == d || rel.starts_with(d + "/")) && d.size() > best) {
				best = d.size();
				c.cwd_module = m.name;
			}
		}
	}
	output::rebind_paths(c.p);
    return c;
}


// ---------------------------------------------------------------------------
// Options communes
// ---------------------------------------------------------------------------

// Analyse robuste d'un entier passé en option (die() avec message clair).
int parse_int_arg(const std::string& opt, const std::string& s) {
	try {
		size_t pos = 0;
		int v = std::stoi(s, &pos);
		if (pos != s.size()) throw std::invalid_argument(s);
		return v;
	} catch (const std::exception&) {
		util::die(std::format("valeur invalide pour {} : '{}'", opt, s));
	}
}

Opts parse_opts(const std::vector<std::string>& args,
					   const std::set<std::string>& value_opts,
					   const std::set<std::string>& bool_opts) {
	Opts o;
	for (size_t i = 0; i < args.size(); ++i) {
		std::string a = args[i];
		// « --args » et « -- » sont deux séparateurs équivalents : tout ce qui les
		// suit est transmis tel quel à l'exécutable (run/test/check), sans être
		// interprété par cpp-maker. C'est la seule façon de passer des options qui
		// ressemblent à celles de cpp-maker (--world=iles, --verbose…).
		if (a == "--" || a == "--args") {
			o.passthrough.assign(args.begin() + (long)i + 1, args.end());
			break;
		}
		std::string val;
		bool has_eq = false;
		if (a.starts_with("--") && a.find('=') != std::string::npos) {
			val = a.substr(a.find('=') + 1);
			a = a.substr(0, a.find('='));
			has_eq = true;
		}
		auto take = [&]() -> std::string {
			if (has_eq) return val;
			if (i + 1 >= args.size()) util::die("valeur manquante pour " + a);
			return args[++i];
		};
		if (a == "-m" || a == "--module") {
			for (auto& m : util::split_list(take())) o.modules.push_back(m);
		} else if (a == "-t" || a == "--toolchain") {
			for (auto& t : util::split_list(take())) o.toolchains.push_back(t);
		} else if (a == "--cc") o.cc = take();
		else if (a == "--cxx") o.cxx = take();
		else if (a == "--mode") o.mode = take();
		else if (a == "-j" || a == "--jobs") o.jobs = parse_int_arg("--jobs", take());
		else if (a.starts_with("-j") && a.size() > 2 && std::isdigit((unsigned char)a[2]))
			o.jobs = parse_int_arg("-j", a.substr(2));
		else if (a == "-v" || a == "--verbose") o.verbose = true;
		else if (a == "-n" || a == "--dry-run") o.dry_run = true;
		else if (a == "-k" || a == "--keep-going") o.keep_going = true;
		else if (a == "-a" || a == "--all") o.all = true;
		else if (a == "--clean") o.clean = true;
		else if (a == "--sanitize" || a == "--sanitizer") o.sanitize.push_back(take());
		else if (a == "--sanitize-trap") o.sanitize_trap.push_back(take());
		else if (a == "--no-sanitize-recover") o.no_sanitize_recover.push_back(take());
		else if (a == "--sanitize-ignorelist") o.sanitize_ignorelist = take();
		else if (value_opts.count(a)) o.values[a] = take();
		else if (bool_opts.count(a)) o.flags.insert(a);
		else if (a.starts_with("-") && a != "-") util::die("option inconnue : " + a);
		else o.positional.push_back(a);
	}
	if (o.jobs < 0) util::die("--jobs doit être ≥ 0");
	return o;
}

std::string pick_mode(const Ctx& c, const Opts& o, const std::string& def) {
	std::string m = !o.mode.empty() ? o.mode : !def.empty() ? def : c.p.default_mode;
	if (!c.p.find_mode(m)) {
		std::vector<std::string> names;
		for (auto& x : c.p.modes) names.push_back(x.name);
		util::die(std::format("mode inconnu : '{}' (modes : {})", m, util::join(names, ", ")));
	}
	return m;
}

// Mode passé en premier argument positionnel : "cpp-maker build release".
void mode_from_positional(Opts& o) {
	if (o.mode.empty() && !o.positional.empty()) {
		o.mode = o.positional.front();
		o.positional.erase(o.positional.begin());
	}
}

std::vector<std::string> pick_toolchains(const Ctx& c, const Opts& o, const std::string& def) {
	std::vector<std::string> r;
	for (auto& t : o.toolchains) {
		if (t == "all") {
			for (auto& [n, tc] : c.p.toolchains) if (tc.from_config || n == "host") r.push_back(n);
			if (r.empty()) r.push_back("host");
		} else {
			r.push_back(t);
		}
	}
	if (r.empty()) r.push_back(!def.empty() ? def : c.p.default_toolchain);
	std::vector<std::string> uniq;
	for (auto& x : r) if (std::find(uniq.begin(), uniq.end(), x) == uniq.end()) uniq.push_back(x);
	return uniq;
}

std::vector<std::string> select_modules(const Ctx& c, const Opts& o) {
	std::vector<std::string> r;
	if (!o.modules.empty()) {
		for (auto& m : o.modules) {
			if (!c.p.find_module(m)) {
				std::vector<std::string> names;
				for (auto& x : c.p.modules) names.push_back(x.name);
				util::die(std::format("module inconnu : '{}' (modules : {})", m, util::join(names, ", ")));
			}
			r.push_back(m);
		}
		return r;
	}
	if (!o.all && !c.cwd_module.empty()) {
		util::outln("\033[2m(module courant : {} — utilisez --all pour tout le workspace)\033[0m",
					c.cwd_module);
		return {c.cwd_module};
	}
	for (auto& m : c.p.modules) r.push_back(m.name);
	return r;
}

// ---------------------------------------------------------------------------
// Session de build : toolchain résolue + plan
// ---------------------------------------------------------------------------

Session prepare(const Ctx& c, const Opts& o, const std::string& tc_name,
					   const std::string& mode, const std::vector<std::string>& modules,
					   bool tests, bool lock) {           // ← param `lock`
	Session s;
	s.tc = std::make_unique<toolchain::Toolchain>(toolchain::resolve(c.p, tc_name, o.cc, o.cxx));
	s.mode = c.p.find_mode(mode);
	if (!s.mode) util::die("mode inconnu : " + mode);
	if (sanitize::requested(o) || !sanitize::from_mode(*s.mode).empty()) {
		s.derived_mode = std::make_unique<ModeConfig>(sanitize::apply(*s.mode, o, *s.tc));
		s.mode = s.derived_mode.get();
	}
	try {
		s.plan = std::make_unique<build::Plan>(build::make_plan(c.p, *s.tc, *s.mode, modules, tests));
	} catch (const cfg::ConfigError& e) {
		util::die(e.what());
	}
	// Sérialise avec toute autre instance cpp-maker sur le même projet.
	// Le verrou est tenu tant que la Session est vivante : pendant toute la
	// phase de compilation / édition de liens / packaging / staging.
	if (lock)
		s.lock.acquire(c.p.root / ".cpp-maker.lock",
					   "Processus de compilation déjà lancé, Veuillez attendre...");
	return s;
}

void print_header(const Ctx& c, const Session& s) {
	util::outln("\033[1;34m[cpp-maker]\033[0m {} · toolchain \033[1m{}\033[0m ({}{}) · mode \033[1m{}\033[0m",
				c.p.name, s.tc->name, s.tc->triple.empty() ? "?" : s.tc->triple,
				s.tc->cross ? ", croisée" : "", s.mode->name);
	std::vector<std::string> mods;
	for (auto& m : s.plan->order) {
		bool sel = std::find(s.plan->selected.begin(), s.plan->selected.end(), m) != s.plan->selected.end();
		mods.push_back(sel ? m : m + "*");
	}
	util::outln("\033[2m            modules : {}{}\033[0m", util::join(mods, ", "),
				mods.size() != s.plan->selected.size() ? "   (* = dépendance)" : "");
}

// [generate.<nom>] : exécute les étapes dont une entrée a changé.
bool run_generate_steps(const Project& p, bool verbose) {
	namespace chr = std::chrono;
	auto newest = [&](const std::vector<std::string>& paths, bool& missing) {
		fs::file_time_type t = fs::file_time_type::min();
		std::error_code ec;
		for (auto& rel : paths) {
			fs::path path = p.root / rel;
			if (!fs::exists(path, ec)) { missing = true; continue; }
			if (fs::is_directory(path, ec)) {
				for (auto& e : fs::recursive_directory_iterator(path, ec))
					if (e.is_regular_file(ec)) t = std::max(t, e.last_write_time(ec));
			} else {
				t = std::max(t, fs::last_write_time(path, ec));
			}
		}
		return t;
	};
	for (auto& g : p.generate_steps) {
		fs::path stamp = p.root / "build" / "generate" / (g.name + ".stamp");
		std::error_code ec;
		bool missing_in = false, missing_out = false;
		auto in = newest(g.inputs, missing_in);
		(void)newest(g.outputs, missing_out);
		bool stale = !fs::exists(stamp, ec) || missing_out || g.inputs.empty() ||
					 in > fs::last_write_time(stamp, ec);
		if (!stale) continue;
		util::outln("\033[1;35m[generate]\033[0m {} : {}", g.name, g.command);
		auto r = proc::run({"sh", "-c", g.command}, {.capture = !verbose, .cwd = p.root});
		if (r.exit_code != 0) {
			if (!verbose) std::cerr << r.output;
			util::errln("\033[1;31mÉCHEC\033[0m : generate.{} (code {})", g.name, r.exit_code);
			return false;
		}
		fs::create_directories(stamp.parent_path(), ec);
		util::write_file(stamp, std::format("{}\n", g.command));
	}
	return true;
}

int run_plan(const Ctx& c, Session& s, const Opts& o) {
	print_header(c, s);
	auto& pl = *s.plan;
	if (o.dry_run) {
		auto dirty = build::predict_dirty(pl.tasks);
		size_t n = 0;
		for (size_t i = 0; i < pl.tasks.size(); ++i) {
			if (!dirty[i]) continue;
			++n;
			util::outln("  \033[1;35m[dry-run]\033[0m {:<6} {}", pl.tasks[i].kind, pl.tasks[i].label);
			if (o.verbose) util::outln("      $ {}", util::shell_join(pl.tasks[i].cmd));
		}
		util::outln("{} tâche(s) à exécuter sur {}.", n, pl.tasks.size());
		return 0;
	}
	if (o.clean) {
		std::error_code ec;
		fs::remove_all(pl.build_dir, ec);
		util::outln("\033[1;35m[cpp-maker]\033[0m nettoyage de {}", pl.build_dir.string());
	}
	build::write_compdb(pl, pl.build_dir / "compile_commands.json");
	if (!run_generate_steps(c.p, o.verbose)) return 1;

	auto t0 = std::chrono::steady_clock::now();
	build::ExecOptions eo;
	eo.jobs = o.jobs > 0 ? o.jobs : c.p.jobs;
	eo.verbose = o.verbose;
	eo.keep_going = o.keep_going;
	auto st = build::execute(pl.tasks, eo);
	if (st.failed > 0) {
		util::errln("\033[1;31m✗ {} tâche(s) en échec, {} non exécutée(s)\033[0m ({})",
					st.failed, st.skipped, util::elapsed(t0));
		return 1;
	}
	if (st.ran == 0) util::outln("\033[1;32m✓ rien à faire\033[0m, tout est à jour ({} tâches)", st.uptodate);
	else util::outln("\033[1;32m✓ {} tâche(s) exécutée(s)\033[0m, {} à jour — {}", st.ran, st.uptodate,
					 util::elapsed(t0));
	return 0;
}

int run_ninja(const Ctx& c, Session& s, const Opts& o) {
	print_header(c, s);
	if (!util::have("ninja")) util::die("ninja introuvable dans le PATH (backend ninja)");
	auto file = build::write_ninja(*s.plan);
	build::write_compdb(*s.plan, s.plan->build_dir / "compile_commands.json");
	std::vector<std::string> cmd = {"ninja", "-f", file.string()};
	// --jobs (CLI) prioritaire sur [build].jobs ; dans les deux cas le
	// budget ncpus - 1 s'applique. 0 = auto → budget complet.
	int requested = o.jobs > 0 ? o.jobs : c.p.jobs;
	int jobs = build::parallel_jobs(requested);
	cmd.push_back("-j");
	cmd.push_back(std::to_string(jobs));
	if (o.verbose) cmd.push_back("-v");
	if (o.keep_going) { cmd.push_back("-k"); cmd.push_back("0"); }
	if (o.dry_run) cmd.push_back("-n");
	return proc::run(cmd).exit_code;
}

// Chaîne de lancement : [valgrind …] [émulateur …] programme
std::optional<std::vector<std::string>> launcher(const Session& s, std::string& why) {
	std::vector<std::string> cmd;
	if (!s.tc->can_execute()) {
		why = s.tc->runner_note.empty() ? "exécution impossible pour cette cible" : s.tc->runner_note;
		return std::nullopt;
	}
	if (!s.mode->runner.empty()) {
		if (!s.tc->runner.empty()) {
			why = std::format("le mode '{}' ({}) n'est pas compatible avec un émulateur",
							  s.mode->name, s.mode->runner);
			return std::nullopt;
		}
		if (!util::have(s.mode->runner)) {
			why = std::format("{} introuvable — mode '{}' indisponible", s.mode->runner, s.mode->name);
			return std::nullopt;
		}
		cmd.push_back(s.mode->runner);
		cmd.insert(cmd.end(), s.mode->runner_args.begin(), s.mode->runner_args.end());
	}
	cmd.insert(cmd.end(), s.tc->runner.begin(), s.tc->runner.end());
	return cmd;
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------
int cmd_build(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--backend"}, {"--tests"});
	mode_from_positional(o);
	help::require_no_positional(o, "build");
	help::require_no_passthrough(o, "build");

	auto c = load_ctx();
	std::string mode    = pick_mode(c, o);
	std::string backend = o.value("--backend", "native");
	if (backend != "native" && backend != "ninja")
		help::usage_error("build",
			"backend inconnu : " + backend + " (native|ninja)");

	auto mods  = select_modules(c, o);
	bool tests = o.flag("--tests");

	// Deux chemins explicites : ninja a son propre affichage (cibles ninja),
	// le backend natif liste les artefacts produits.
	if (backend == "ninja")
		return help::for_each_build(c, o, {}, mode, mods, tests, "build",
			[](const Ctx& cc, Session& s, const Opts& oo) {
				return run_ninja(cc, s, oo);
			},
			[](Session&) { /* ninja affiche déjà ses cibles */ });

	return help::for_each_build(c, o, {}, mode, mods, tests, "build",
		[](const Ctx& cc, Session& s, const Opts& oo) {
			return run_plan(cc, s, oo);
		},
		[](Session& s) {
			for (auto& m : s.plan->selected) {
				auto& mp = s.plan->mods.at(m);
				if (!mp.artifact.empty())
					util::outln("  → {:<14} {}", m, mp.artifact.string());
				if (!mp.test_exe.empty())
					util::outln("  → {:<14} {}", m + " (tests)", mp.test_exe.string());
			}
		});
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------
int cmd_run(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	mode_from_positional(o);

	auto c        = load_ctx();
	std::string mode = pick_mode(c, o);
	std::string tc   = help::require_single_toolchain(c, o, {}, "run");

	// Choix de la cible : -m explicite, sinon module courant, sinon unique
	// exécutable du projet. Dans tous les cas, require_executable valide.
	std::string target;
	if (!o.modules.empty()) {
		if (o.modules.size() != 1)
			help::usage_error("run", "un seul module à la fois");
		target = o.modules[0];
	} else if (!c.cwd_module.empty()
				&& c.p.find_module(c.cwd_module)->type == "executable") {
		target = c.cwd_module;
	} else {
		std::vector<std::string> exes;
		for (auto& m : c.p.modules) if (m.type == "executable") exes.push_back(m.name);
		if (exes.empty())
			util::die("run : aucun module exécutable dans ce projet "
						"(bibliothèques uniquement — utilisez 'cpp-maker build' "
						"ou 'cpp-maker test')");
		if (exes.size() > 1)
			help::usage_error("run",
				"plusieurs exécutables (" + util::join(exes, ", ") +
				") : précisez -m <module>");
		target = exes[0];
	}
	help::require_executable(c, target, "run");

	return help::with_session(c, o, tc, mode, {target}, false,
		[&](Session& s) {
			if (o.dry_run) return 0;
			// L'exécutable peut lui-même invoquer cpp-maker — on relâche le
			// verrou avant exécution (le Session reste vivant pour run/test).
			s.lock.release();

			std::string why;
			auto l = launcher(s, why);
			if (!l) { util::errln("\033[1;31m{}\033[0m", why); return 2; }

			std::vector<std::string> cmd = *l;
			cmd.push_back(s.plan->mods.at(target).artifact.string());
			// Positional résiduel (compatibilité `cpp-maker run release foo`)
			// puis arguments explicites (`--args …` / `-- …`).
			cmd.insert(cmd.end(), o.positional.begin(),  o.positional.end());
			cmd.insert(cmd.end(), o.passthrough.begin(), o.passthrough.end());

			if (o.verbose) util::outln("      $ {}", util::shell_join(cmd));
			util::outln("\033[1;34m[cpp-maker]\033[0m exécution de {}{}",
						target,
						o.passthrough.empty() ? ""
						: std::format(" ({} argument(s) transmis)",
										o.passthrough.size()));
			return proc::run(cmd).exit_code;
		});
}

// ---------------------------------------------------------------------------
// test
// ---------------------------------------------------------------------------
void write_junit(const fs::path& file, const std::vector<TestCaseResult>& results) {
	std::map<std::string, std::vector<const TestCaseResult*>> suites;
	for (auto& r : results) suites[r.suite].push_back(&r);
	std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n";
	for (auto& [name, rs] : suites) {
		int fail = 0, skip = 0;
		double t = 0;
		for (auto r : rs) { fail += r->status == "fail" || r->status == "timeout"; skip += r->status == "skip"; t += r->seconds; }
		s += std::format("  <testsuite name=\"{}\" tests=\"{}\" failures=\"{}\" skipped=\"{}\" time=\"{:.3f}\">\n",
						 util::xml_escape(name), rs.size(), fail, skip, t);
		for (auto r : rs) {
			s += std::format("    <testcase classname=\"{}\" name=\"{}\" time=\"{:.3f}\">\n",
							 util::xml_escape(name), util::xml_escape(r->name), r->seconds);
			if (r->status == "fail" || r->status == "timeout")
				s += std::format("      <failure message=\"{}\"/>\n",
								 r->status == "timeout" ? "délai dépassé" : std::format("code de sortie {}", r->code));
			if (r->status == "skip") s += "      <skipped/>\n";
			s += "      <system-out>" + util::xml_escape(r->output) + "</system-out>\n";
			s += "    </testcase>\n";
		}
		s += "  </testsuite>\n";
	}
	s += "</testsuites>\n";
	util::write_file(file, s);
}

// Compile et exécute les tests d'un mode ; ajoute les résultats à `results`.
int do_test(const Ctx& c, const Opts& opts, const std::string& base_mode,
				   std::vector<TestCaseResult>& results) {
	auto mods = select_modules(c, opts);
	int timeout = opts.values.count("--timeout") ? parse_int_arg("--timeout", opts.value("--timeout"))
												 : c.p.test_timeout;
	std::string filter = opts.value("--filter");
	bool capture = opts.values.count("--junit") > 0;
	int failures = 0;

	for (auto& tcn : pick_toolchains(c, opts))
	for (auto& o : sanitize::expand_all(c.p, opts, tcn, base_mode)) {
		auto s = prepare(c, o, tcn, base_mode, mods, true);
		// Nom du mode dérivé (debug+address…) : distingue les passes --sanitize.
		const std::string& mode = s.mode->name;
		if (run_plan(c, s, o) != 0) {
			results.push_back({std::format("{}/{}", s.tc->name, mode), "build", "fail", 0, "échec de compilation", 1});
			++failures;
			continue;
		}
		if (o.dry_run) continue;
		std::string why;
		auto l = launcher(s, why);
		bool any = false;
		for (auto& m : s.plan->selected) {
			auto& mp = s.plan->mods.at(m);
			if (mp.test_exe.empty()) continue;
			any = true;
			TestCaseResult r;
			r.suite = std::format("{}/{}", s.tc->name, mode);
			r.name = m;
			if (!l) {
				r.status = "skip";
				r.output = why;
				util::outln("  \033[1;33m[ SKIP ]\033[0m {} — {}", m, why);
				results.push_back(r);
				continue;
			}
			std::vector<std::string> cmd = *l;
			cmd.push_back(mp.test_exe.string());
			if (!filter.empty()) { cmd.push_back("--filter"); cmd.push_back(filter); }
			cmd.insert(cmd.end(), o.passthrough.begin(), o.passthrough.end());
			util::outln("\n\033[1;36m── tests : {} ({} · {}) ──\033[0m", m, s.tc->name, mode);
			if (o.verbose) util::outln("      $ {}", util::shell_join(cmd));
			auto t0 = std::chrono::steady_clock::now();
			std::vector<std::pair<std::string, std::string>> env;
			for (auto& [k, v] : c.p.test_env) {
				std::string val = v;
				for (size_t pos; (pos = val.find("{root}")) != std::string::npos;)
					val.replace(pos, 6, c.p.root.string());
				env.emplace_back(k, val);
			}
			auto pr = proc::run(cmd, {.capture = capture, .cwd = std::nullopt, .timeout_sec = timeout,
									  .env = env});
			r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			r.output = pr.output;
			r.code = pr.exit_code;
			if (capture && !pr.output.empty()) std::cout << pr.output;
			if (pr.timed_out) r.status = "timeout";
			else if (pr.exit_code == 0) r.status = "pass";
			else if (pr.exit_code == 77) r.status = "skip";
			else r.status = "fail";
			if (r.status == "fail" || r.status == "timeout") ++failures;
			results.push_back(r);
		}
		if (!any) util::outln("  (aucun test trouvé pour {})", util::join(s.plan->selected, ", "));
	}
	return failures;
}

int print_test_summary(const std::vector<TestCaseResult>& results) {
	if (results.empty()) return 0;
	util::outln("\n\033[1;36mRésumé des tests\033[0m");
	int fail = 0, pass = 0, skip = 0;
	for (auto& r : results) {
		std::string tag = r.status == "pass" ? "\033[1;32m[ PASS ]\033[0m"
						: r.status == "skip" ? "\033[1;33m[ SKIP ]\033[0m"
						: r.status == "timeout" ? "\033[1;31m[TMOUT ]\033[0m"
												: "\033[1;31m[ FAIL ]\033[0m";
		util::outln("  {} {:<20} {:<24} {:.2f}s", tag, r.name, r.suite, r.seconds);
		if (r.status == "pass") ++pass;
		else if (r.status == "skip") ++skip;
		else ++fail;
	}
	util::outln("  {} réussi(s), {} échec(s), {} ignoré(s)", pass, fail, skip);
	return fail;
}

int cmd_test(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--filter", "--junit", "--timeout"});
	mode_from_positional(o);
	if (!o.positional.empty()) util::die("argument inattendu : " + o.positional[0]);
	auto c = load_ctx();
	std::string mode = pick_mode(c, o);
	std::vector<TestCaseResult> results;
	do_test(c, o, mode, results);
	if (o.values.count("--junit")) {
		fs::path f = o.value("--junit");
		write_junit(f, results);
		util::outln("Rapport JUnit : {}", f.string());
	}
	return print_test_summary(results) > 0 ? 1 : 0;
}

int cmd_check(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--modes", "--filter", "--junit", "--timeout"});
	auto c = load_ctx();
	std::vector<std::string> modes = o.values.count("--modes") ? util::split_list(o.value("--modes"))
															   : std::vector<std::string>{"debug", "asan", "tsan"};
	std::vector<TestCaseResult> results;
	for (auto& m : modes) {
		Opts om = o;
		om.mode = m;
		pick_mode(c, om);
		util::outln("\n\033[1;35m===== tests :: {} =====\033[0m", m);
		do_test(c, om, m, results);
	}
	if (o.values.count("--junit")) write_junit(o.value("--junit"), results);
	return print_test_summary(results) > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// generate ninja | compdb
// ---------------------------------------------------------------------------
int cmd_generate(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	if (o.positional.empty()) util::die("usage : cpp-maker generate <ninja|compdb> [mode] [-t toolchain]");
	std::string what = o.positional[0];
	o.positional.erase(o.positional.begin());
	mode_from_positional(o);
	auto c = load_ctx();
	std::string mode = pick_mode(c, o);
	auto mods = select_modules(c, o);
	for (auto& tcn : pick_toolchains(c, o)) {
		auto s = prepare(c, o, tcn, mode, mods, true);
		if (what == "ninja") {
			auto f = build::write_ninja(*s.plan);
			util::outln("\033[1;32m{}\033[0m écrit — depuis {} : ninja -f {}", f.string(),
						c.p.root.string(), f.string());
		} else if (what == "compdb") {
			build::write_compdb(*s.plan, "compile_commands.json");
			util::outln("\033[1;32mcompile_commands.json\033[0m écrit ({} · {})", s.tc->name, mode);
		} else {
			util::die("generate : cible inconnue '" + what + "' (ninja|compdb)");
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// clean
// ---------------------------------------------------------------------------
int cmd_clean(const std::vector<std::string>& args) {
	auto o = parse_opts(args);
	mode_from_positional(o);
	auto c = load_ctx(false);
	util::FileLock lock;
	lock.acquire(c.p.root / ".cpp-maker.lock", "Processus de compilation déjà lancé, Veuillez attendre...");
	std::error_code ec;
	auto rm = [&](const fs::path& p) {
		if (!fs::exists(p, ec)) return;
		fs::remove_all(p, ec);
		util::outln("  supprimé : {}", p.string());
	};
	if (!o.mode.empty() || !o.toolchains.empty()) {
		std::vector<fs::path> tcdirs;
		if (!o.toolchains.empty()) for (auto& t : o.toolchains) tcdirs.push_back(fs::path("build") / t);
		else if (fs::is_directory("build", ec))
			for (auto& e : fs::directory_iterator("build", ec)) tcdirs.push_back(e.path());
		for (auto& d : tcdirs) rm(o.mode.empty() ? d : d / o.mode);
		return 0;
	}
	if (!fs::exists("build", ec) && !(o.all && fs::exists("dist", ec))) {
		util::outln("Rien à nettoyer.");
		return 0;
	}
	rm("build");
	if (o.all) rm("dist");
	return 0;
}

} // namespace commands