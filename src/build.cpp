#include "cpp-maker.hpp"
#include "flags.hpp"

namespace build {

std::optional<Lang> lang_of(const fs::path& p) {
	auto e = p.extension().string();
	if (e == ".c") return Lang::C;
	if (e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c++" || e == ".C" || e == ".cp")
		return Lang::CXX;
	if (e == ".S" || e == ".s" || e == ".sx") return Lang::ASM;
	return std::nullopt;
}

// Parser de fichier .d généré par -MMD
std::vector<fs::path> parse_depfile(const fs::path& p) {
	std::vector<fs::path> deps;
	std::string all;
	if (!util::read_file(p, all)) return deps;
	std::string joined;
	for (size_t i = 0; i < all.size(); ++i) {
		if (all[i] == '\\' && i + 1 < all.size() && all[i + 1] == '\n') { ++i; continue; }
		if (all[i] == '\\' && i + 1 < all.size() && all[i + 1] == '\r') {
			++i;
			if (i + 1 < all.size() && all[i + 1] == '\n') ++i;
			continue;
		}
		joined += all[i];
	}
	// Seule la première règle compte (-MP ajoute des cibles vides ensuite).
	auto nl = joined.find('\n');
	std::string first = joined.substr(0, nl);
	auto colon = first.find(": ");
	if (colon == std::string::npos) colon = first.find(':');
	if (colon == std::string::npos) return deps;
	// Tokenisation respectant les échappements '\'x du makefile : un chemin
	// contenant des espaces est écrit "a\ b/c.cpp" et ne doit pas être coupé.
	std::string body = first.substr(colon + 1);
	std::string tok;
	auto flush = [&]() { if (!tok.empty()) { deps.push_back(tok); tok.clear(); } };
	for (size_t i = 0; i < body.size(); ++i) {
		char c = body[i];
		if (c == '\\' && i + 1 < body.size()) { tok += body[++i]; continue; }
		if (c == '$' && i + 1 < body.size() && body[i + 1] == '$') { tok += '$'; ++i; continue; }
		if (c == ' ' || c == '\t' || c == '\r') { flush(); continue; }
		tok += c;
	}
	flush();
	return deps;
}

std::string fingerprint(const Task& t) {
	util::Hasher h;
	h.feed(t.tool_version);
	for (auto& a : t.cmd) h.feed(a);
	std::vector<fs::path> files = t.inputs;
	if (!t.depfile.empty()) {
		auto d = parse_depfile(t.depfile);
		if (!d.empty()) files = std::move(d);
	}
	for (auto& f : files) {
		h.feed(f.string());
		std::error_code ec;
		auto sz = fs::file_size(f, ec);
		if (ec) { h.feed("<absent>"); continue; }
		h.feed_u64((uint64_t)sz);
		auto mt = fs::last_write_time(f, ec);
		if (!ec) h.feed_u64((uint64_t)mt.time_since_epoch().count());
	}
	return h.hex();
}

bool up_to_date(const Task& t) {
	std::error_code ec;
	for (auto& o : t.outputs) if (!fs::exists(o, ec)) return false;
	std::string old;
	if (!util::read_file(t.stamp, old)) return false;
	return old == fingerprint(t);
}

// Tâches qui seraient exécutées (périmées, ou dépendant d'une tâche périmée).
std::vector<char> predict_dirty(const std::vector<Task>& tasks) {
	std::vector<char> dirty(tasks.size(), 0);
	// Les tâches sont créées dépendances d'abord : un seul passage suffit.
	for (size_t i = 0; i < tasks.size(); ++i) {
		bool d = !up_to_date(tasks[i]);
		for (auto dep : tasks[i].deps) if (dirty[dep]) d = true;
		dirty[i] = d;
	}
	return dirty;
}

std::string kind_color(const std::string& k) {
	if (k == "CC" || k == "CXX" || k == "AS") return "\033[1;33m";
	if (k == "AR") return "\033[1;36m";
	return "\033[1;35m";
}

// Nombre de workers de compilation à utiliser. cpp-maker occupe déjà un
// cœur (thread principal), donc au plus ncpus - 1 workers, avec un plancher
// de 1 (mono-cœur : impossible de ne rien lancer). Si `requested > 0`
// dépasse ce budget, un avertissement est émis (une seule fois par
// exécution, pour ne pas spammer en mode `check` multi-modes) et la valeur
// est plafonnée. Un `requested` à 0 signifie « auto ».
int parallel_jobs(int requested) {
	unsigned hc = std::thread::hardware_concurrency();
	int ncpus = hc == 0 ? 1 : (int)hc; // 0 = inconnu
	int budget = std::max(1, ncpus - 1);
	if (requested > budget) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			util::warn("parallélisme demandé ({}) supérieur au budget CPU ({} cœurs au total, "
						"1 réservé à cpp-maker) : plafonné à {}", requested, ncpus, budget);
		}
		return budget;
	}
	return requested > 0 ? requested : budget;
}

ExecStats execute(const std::vector<Task>& tasks, const ExecOptions& o) {
	ExecStats st;
	const size_t n = tasks.size();
	if (n == 0) return st;

	auto dirty = predict_dirty(tasks);
	size_t total = (size_t)std::count(dirty.begin(), dirty.end(), 1);

	std::vector<int> pending(n, 0);
	std::vector<std::vector<size_t>> rdeps(n);
	for (size_t i = 0; i < n; ++i) {
		pending[i] = (int)tasks[i].deps.size();
		for (auto d : tasks[i].deps) rdeps[d].push_back(i);
	}
	std::deque<size_t> ready;
	for (size_t i = 0; i < n; ++i) if (pending[i] == 0) ready.push_back(i);

	std::vector<char> state(n, 0); // 0 = en attente, 1 = ok, 2 = échec/ignorée
	std::mutex mu, print_mu;
	std::condition_variable cv;
	size_t finished = 0, counter = 0;
	bool stop = false;

	int jobs = parallel_jobs(o.jobs);
	jobs = std::max(1, std::min<int>(jobs, (int)n));

	auto run_one = [&](const Task& t) -> bool {
		if (up_to_date(t)) {
			std::lock_guard lk(mu);
			++st.uptodate;
			return true;
		}
		std::error_code ec;
		for (auto& out : t.outputs) {
			if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
			if (t.remove_outputs_first) fs::remove(out, ec);
		}
		if (!t.stamp.empty() && t.stamp.has_parent_path())
			fs::create_directories(t.stamp.parent_path(), ec);
		{
			std::lock_guard lk(print_mu);
			size_t idx;
			{ std::lock_guard lk2(mu); idx = ++counter; }
			util::outln("[{}/{}] {}{:<6}\033[0m {}", idx, std::max(idx, total),
						kind_color(t.kind), t.kind, t.label);
			if (o.verbose) util::outln("      $ {}", util::shell_join(t.cmd));
		}
		auto r = proc::run(t.cmd, {.capture = true, .cwd = std::nullopt});
		bool ok = r.exit_code == 0;
		for (auto& out : t.outputs) if (ok && !fs::exists(out, ec)) ok = false;
		{
			std::lock_guard lk(print_mu);
			if (!ok) {
				util::errln("\033[1;31mÉCHEC :\033[0m {} {} (code {})", t.kind, t.label, r.exit_code);
				if (!o.verbose) util::errln("      $ {}", util::shell_join(t.cmd));
			}
			if (!r.output.empty()) {
				std::cout.flush();
				(ok ? std::cout : std::cerr) << r.output;
				if (r.output.back() != '\n') (ok ? std::cout : std::cerr) << '\n';
			}
		}
		if (!ok) { std::error_code ec2; fs::remove(t.stamp, ec2); return false; }
		if (!t.stamp.empty()) util::write_file_atomic(t.stamp, fingerprint(t));
		std::lock_guard lk(mu);
		++st.ran;
		return true;
	};

	auto worker = [&] {
		std::unique_lock lk(mu);
		for (;;) {
			cv.wait(lk, [&] { return !ready.empty() || finished == n; });
			if (ready.empty()) return;
			size_t i = ready.front();
			ready.pop_front();
			bool dep_failed = std::any_of(tasks[i].deps.begin(), tasks[i].deps.end(),
										  [&](size_t d) { return state[d] == 2; });
			bool abort = stop;
			lk.unlock();
			bool ok = false;
			if (!dep_failed && !abort) ok = run_one(tasks[i]);
			lk.lock();
			if (dep_failed || abort) ++st.skipped;
			else if (!ok) { ++st.failed; if (!o.keep_going) stop = true; }
			state[i] = ok ? 1 : 2;
			++finished;
			for (auto r : rdeps[i]) if (--pending[r] == 0) ready.push_back(r);
			cv.notify_all();
		}
	};

	std::vector<std::thread> pool;
	for (int i = 0; i < jobs; ++i) pool.emplace_back(worker);
	for (auto& th : pool) th.join();
	return st;
}

// Motif simple : '*' (hors '/'), '**' (tout), '?'.
bool glob_match(std::string_view pat, std::string_view s) {
	if (pat.empty()) return s.empty();
	if (pat.starts_with("**")) {
		auto rest = pat.substr(2);
		if (rest.starts_with("/")) rest.remove_prefix(1);
		for (size_t i = 0; i <= s.size(); ++i) if (glob_match(rest, s.substr(i))) return true;
		return false;
	}
	if (pat[0] == '*') {
		for (size_t i = 0; i <= s.size(); ++i) {
			if (glob_match(pat.substr(1), s.substr(i))) return true;
			if (i < s.size() && s[i] == '/') break;
		}
		return false;
	}
	if (s.empty()) return false;
	if (pat[0] == '?' || pat[0] == s[0]) return glob_match(pat.substr(1), s.substr(1));
	return false;
}

bool excluded(const ModuleConfig& m, const fs::path& p) {
	std::string rel = (m.dir == "." ? p : p.lexically_relative(m.dir)).generic_string();
	for (auto& e : m.exclude) {
		std::string ex = fs::path(e).lexically_normal().generic_string();
		if (rel == ex || rel.starts_with(ex + "/") || glob_match(ex, rel)) return true;
	}
	return false;
}

std::vector<Source> discover(const ModuleConfig& m, const std::vector<std::string>& dirs) {
	std::vector<Source> out;
	for (auto& d : dirs) {
		fs::path dir = util::under(m.dir, d);
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) continue;
		fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
		if (ec) continue;
		for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec) || ec) continue;
			auto l = lang_of(it->path());
			if (!l) continue;
			fs::path p = it->path().lexically_normal();
			if (!excluded(m, p)) out.push_back({p, *l});
		}
	}
	std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.path < b.path; });
	out.erase(std::unique(out.begin(), out.end(), [](auto& a, auto& b) { return a.path == b.path; }),
			  out.end());
	return out;
}

bool platform_matches(const std::string& key, const std::string& os) {
	if (key == os) return true;
	if (key == "unix") return os != "windows" && os != "baremetal";
	return false;
}

fs::path object_path(const Plan& pl, const ModuleConfig& m, const fs::path& src) {
	fs::path rel = m.dir == "." ? src : src.lexically_relative(m.dir);
	std::string s = rel.generic_string();
	if (rel.empty() || s.starts_with("..")) s = util::replace_all(src.generic_string(), "../", "__/");
	return pl.build_dir / "obj" / m.name / (s + ".o");
}

std::vector<std::string> pkg_query(const toolchain::Toolchain& t, const std::string& pkg,
										  const std::string& what) {
	std::vector<std::string> cmd = {t.pkg_config, what, pkg};
	bool restricted = t.cross && t.cfg.pkg_config.empty() && t.pkg_config == "pkg-config";
	if (restricted) {
		// Pas de pkg-config dédié à la cible : on limite la recherche aux .pc
		// de la cible (sysroot, multiarch Debian). Sans cela, pkg-config
		// renverrait les en-têtes et bibliothèques de l'hôte.
		std::vector<std::string> dirs;
		auto add = [&](const std::string& d) {
			std::error_code ec;
			if (fs::is_directory(d, ec)) dirs.push_back(d);
		};
		const std::string& sr = t.cfg.sysroot;
		if (!sr.empty())
			for (auto sub : {std::string("/usr/lib/pkgconfig"), "/usr/lib/" + t.triple + "/pkgconfig",
							 std::string("/usr/local/lib/pkgconfig"), std::string("/usr/share/pkgconfig")})
				add(sr + sub);
		if (!t.triple.empty()) {
			add("/usr/lib/" + t.triple + "/pkgconfig");
			add("/usr/" + t.triple + "/lib/pkgconfig");
		}
		add("/usr/share/pkgconfig");
		cmd = {"env", "PKG_CONFIG_LIBDIR=" + util::join(dirs, ":"), "PKG_CONFIG_PATH="};
		if (!sr.empty()) cmd.push_back("PKG_CONFIG_SYSROOT_DIR=" + sr);
		cmd.insert(cmd.end(), {t.pkg_config, what, pkg});
	}
	auto r = proc::capture(cmd);
	if (r.exit_code != 0) {
		std::string hint;
		if (restricted)
			hint = std::format("\n  cible {} : paquet de développement de la cible introuvable "
							   "(.pc cherchés hors de l'hôte). Installez-le pour {} (multiarch ou "
							   "sysroot), ou définissez toolchains.{}.sysroot / pkg_config.",
							   t.triple, t.arch, t.name);
		util::die(std::format("pkg-config : paquet '{}' introuvable ({} {} {}){}\n{}", pkg,
							  t.pkg_config, what, pkg, hint, util::trim(r.output)));
	}
	return util::split_ws(r.output);
}

std::string artifact_name(const toolchain::Toolchain& t, const ModuleConfig& m) {
	if (m.type == "executable") return m.name + t.exe_suffix();
	if (m.type == "static") return "lib" + m.name + ".a";
	if (m.type == "shared") {
		if (t.os == "windows") return m.name + ".dll";
		if (t.os == "macos") return "lib" + m.name + ".dylib";
		return "lib" + m.name + ".so";
	}
	return {};
}


Planner::Planner(Plan& pl) : pl_(pl), p_(*pl.p), t_(*pl.t), mode_(*pl.mode) {}

void Planner::run() {
	pl_.order = cfg::topo_order(p_, pl_.selected);
	for (auto& n : pl_.order) prepare(n);
	for (auto& n : pl_.order) emit_module(n);
	if (pl_.with_tests)
		for (auto& n : pl_.selected) emit_tests(n);
}

std::vector<const PlatformOverride*> Planner::overrides(const ModuleConfig& m) const {
	std::vector<const PlatformOverride*> r;
	for (auto& [k, o] : m.platforms) if (platform_matches(k, t_.os)) r.push_back(&o);
	return r;
}

void Planner::prepare(const std::string& name) {
	auto& mp = pl_.mods[name];
	const ModuleConfig& m = *p_.find_module(name);
	mp.cfg = &m;
	auto deps = cfg::topo_order(p_, {name});
	deps.pop_back();
	mp.closure = deps;

	auto srcs = discover(m, m.source_dirs);
	std::vector<std::string> files = m.source_files;
	for (auto o : overrides(m)) files.insert(files.end(), o->sources.begin(), o->sources.end());
	for (auto& f : files) {
		fs::path p = util::under(m.dir, f);
		auto l = lang_of(p);
		std::error_code ec;
		if (!fs::is_regular_file(p, ec))
			util::die(std::format("{}: source introuvable : {}", m.file.string(), p.string()));
		if (!l) util::die(std::format("{}: extension non reconnue : {}", m.file.string(), p.string()));
		if (std::none_of(srcs.begin(), srcs.end(), [&](auto& s) { return s.path == p; }))
			srcs.push_back({p, *l});
	}
	if (m.type == "header-only" && !srcs.empty())
		util::die(std::format("module '{}' de type header-only mais {} source(s) trouvée(s) "
								"dans {}", name, srcs.size(), util::join(m.source_dirs, ", ")));
	if (m.type != "header-only" && srcs.empty())
		util::die(std::format("module '{}' ({}) : aucune source C/C++ dans {}", name,
								m.file.string(), util::join(m.source_dirs, ", ")));

	fs::path main_path;
	if (!m.main.empty()) {
		main_path = util::under(m.dir, m.main);
		if (std::none_of(srcs.begin(), srcs.end(), [&](auto& s) { return s.path == main_path; }))
			util::die(std::format("{}: main = '{}' ne fait pas partie des sources",
									m.file.string(), m.main));
	} else if (m.type == "executable") {
		for (auto& s : srcs) if (s.path.stem() == "main") { main_path = s.path; break; }
		// v0.6 : sinon, la source qui définit main() (exclue des tests).
		if (main_path.empty()) {
			static const std::regex main_re(R"((^|\n)\s*int\s+main\s*\()");
			for (auto& s : srcs) {
				std::ifstream in(s.path);
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				if (std::regex_search(text, main_re)) { main_path = s.path; break; }
			}
		}
	}
	for (auto& s : srcs) {
		if (s.lang == Lang::CXX) mp.has_cxx = true;
		if (s.lang == Lang::C) mp.has_c = true;
		if (!main_path.empty() && s.path == main_path) mp.main = s;
		else mp.sources.push_back(s);
	}
	if (pl_.with_tests && std::find(pl_.selected.begin(), pl_.selected.end(), name) != pl_.selected.end())
		mp.tests = discover(m, m.test_dirs);

	for (auto& pkg : m.pkg_config) {
		auto cf = pkg_query(t_, pkg, "--cflags");
		auto lf = pkg_query(t_, pkg, "--libs");
		mp.pc_cflags.insert(mp.pc_cflags.end(), cf.begin(), cf.end());
		mp.pc_libs.insert(mp.pc_libs.end(), lf.begin(), lf.end());
	}

	std::string an = artifact_name(t_, m);
	if (m.type == "executable") mp.artifact = pl_.build_dir / "bin" / an;
	else if (m.type == "static") mp.artifact = pl_.build_dir / "lib" / an;
	else if (m.type == "shared") {
		if (t_.os == "windows") {
			mp.artifact = pl_.build_dir / "bin" / an;
			mp.import_lib = pl_.build_dir / "lib" / ("lib" + m.name + ".dll.a");
		} else {
			mp.artifact = pl_.build_dir / "lib" / an;
		}
	}
}

bool Planner::closure_uses_cxx(const ModulePlan& mp, bool tests) const {
	if (mp.has_cxx) return true;
	if (tests && std::any_of(mp.tests.begin(), mp.tests.end(),
								[](auto& s) { return s.lang == Lang::CXX; })) return true;
	for (auto& d : mp.closure) if (pl_.mods.at(d).has_cxx) return true;
	return false;
}

bool Planner::pic(const ModuleConfig& m) const {
	if (t_.os == "windows") return false;
	return m.type == "shared" || (m.type == "static" && p_.static_pic);
}

std::vector<std::string> Planner::compile_flags(const ModulePlan& mp, Lang lang, bool tests) const {
	const ModuleConfig& m = *mp.cfg;
	const auto& tool = (lang == Lang::CXX) ? t_.cxx_tool() : t_.c_tool();
	auto fam = tool.family;

	std::vector<std::string> c = {tool.exe};
	c.insert(c.end(), tool.pre_args.begin(), tool.pre_args.end());

	if (lang == Lang::CXX) {
		auto s = flags::standard_cxx(fam, m.cxx_standard.empty() ? p_.cxx_standard : m.cxx_standard);
		if (!s.empty()) c.push_back(s);
	} else if (lang == Lang::C) {
		auto s = flags::standard_c(fam, m.c_standard.empty() ? p_.c_standard : m.c_standard);
		if (!s.empty()) c.push_back(s);
	}

	auto add = [&](const std::vector<std::string>& v) { c.insert(c.end(), v.begin(), v.end()); };
	add(mode_.flags);
	if (lang != Lang::ASM) add(flags::warnings(fam, p_.warnings));
	// … toolchain/project/module flags inchangés …

	auto def = [&](const std::vector<std::string>& v) {
		for (auto& d : v) c.push_back(flags::define(fam, d));
	};
	def(p_.defines); def(mode_.defines); def(m.defines); def(m.public_defines);
	for (auto& d : mp.closure) def(p_.find_module(d)->public_defines);

	auto inc = [&](const ModuleConfig& mm, const std::vector<std::string>& v) {
		for (auto& i : v) c.push_back(flags::include(fam, util::under(mm.dir, i)));
	};
	inc(m, m.public_includes);
	inc(m, m.private_includes);
	if (tests) inc(m, m.test_dirs);
	for (auto& d : mp.closure) inc(*p_.find_module(d), p_.find_module(d)->public_includes);
	add(mp.pc_cflags);
	for (auto& d : mp.closure) add(pl_.mods.at(d).pc_cflags);
	return c;
}

size_t Planner::add_compile(const ModulePlan& mp, const Source& s, bool tests, fs::path& obj_out) {
	Task t;
	t.source = s.path;
	obj_out = object_path(pl_, *mp.cfg, s.path);
	t.outputs = {obj_out};
	t.depfile = fs::path(obj_out.string() + ".d");
	t.stamp = fs::path(obj_out.string() + ".stamp");
	t.inputs = {s.path};
	t.cmd = compile_flags(mp, s.lang, tests);
	t.tool_version = (s.lang == Lang::CXX ? t_.cxx_tool() : t_.c_tool()).version;
	t.cmd.insert(t.cmd.end(), {"-MMD", "-MP", "-MF", t.depfile.string(),
								"-c", s.path.string(), "-o", obj_out.string()});
	t.kind = s.lang == Lang::CXX ? "CXX" : s.lang == Lang::C ? "CC" : "AS";
	t.label = s.path.string();
	
	pl_.tasks.push_back(std::move(t));
	return pl_.tasks.size() - 1;
}

// Bibliothèques des dépendances, pkg-config et système, dans l'ordre de lien.
void Planner::link_libs(const ModulePlan& mp, std::vector<std::string>& cmd, std::vector<fs::path>& inputs,
				std::vector<size_t>& deps, bool& needs_rpath) const {
	const ModuleConfig& m = *mp.cfg;
	for (auto it = mp.closure.rbegin(); it != mp.closure.rend(); ++it) {
		auto& dp = pl_.mods.at(*it);
		if (dp.cfg->type == "header-only") continue;
		fs::path lib = dp.import_lib.empty() ? dp.artifact : dp.import_lib;
		cmd.push_back(lib.string());
		inputs.push_back(lib);
		if (dp.link_task) deps.push_back(*dp.link_task);
		if (dp.cfg->type == "shared") needs_rpath = true;
	}
	cmd.insert(cmd.end(), mp.pc_libs.begin(), mp.pc_libs.end());
	for (auto& d : mp.closure) {
		auto& dl = pl_.mods.at(d).pc_libs;
		cmd.insert(cmd.end(), dl.begin(), dl.end());
	}
	std::vector<std::string> sys = m.system_libs;
	for (auto o : overrides(m)) sys.insert(sys.end(), o->system_libs.begin(), o->system_libs.end());
	for (auto& d : mp.closure) {
		auto& dm = *p_.find_module(d);
		sys.insert(sys.end(), dm.system_libs.begin(), dm.system_libs.end());
		for (auto o : overrides(dm)) sys.insert(sys.end(), o->system_libs.begin(), o->system_libs.end());
	}
	std::set<std::string> seen;
	for (auto& l : sys) {
		if (!seen.insert(l).second) continue;
		cmd.push_back(l.starts_with("-") ? l : "-l" + l);
	}
	if (t_.platform() == toolchain::Platform::Macos) {
		for (auto& fw : m.frameworks) {
			cmd.push_back("-framework"); cmd.push_back(fw);
		}
	}
}

std::vector<std::string> Planner::link_prefix(const ModulePlan& mp, bool tests) const {
	const auto& tool = closure_uses_cxx(mp, tests) ? t_.cxx_tool() : (t_.cc ? *t_.cc : t_.cxx_tool());
	if (tool.family == toolchain::CompilerFamily::Msvc) return {"link.exe", "/NOLOGO"};
	
	std::vector<std::string> c = {tool.exe};
	c.insert(c.end(), tool.pre_args.begin(), tool.pre_args.end());
	c.insert(c.end(), mode_.link_flags.begin(), mode_.link_flags.end());
	c.insert(c.end(), t_.cfg.ldflags.begin(), t_.cfg.ldflags.end());
	c.insert(c.end(), p_.ldflags.begin(), p_.ldflags.end());
	c.insert(c.end(), mp.cfg->ldflags.begin(), mp.cfg->ldflags.end());
	for (auto o : overrides(*mp.cfg)) c.insert(c.end(), o->ldflags.begin(), o->ldflags.end());
	return c;
}

void Planner::add_rpath(std::vector<std::string>& cmd) const {
	if (t_.os == "windows" || t_.os == "baremetal") return;
	if (t_.os == "macos") cmd.push_back("-Wl,-rpath,@loader_path/../lib");
	else cmd.push_back("-Wl,-rpath,$ORIGIN/../lib");
}

void Planner::emit_module(const std::string& name) {
	auto& mp = pl_.mods[name];
	const ModuleConfig& m = *mp.cfg;
	for (auto& s : mp.sources) {
		fs::path obj;
		mp.object_tasks.push_back(add_compile(mp, s, false, obj));
		mp.objects.push_back(obj);
	}
	if (mp.main) mp.main_task = add_compile(mp, *mp.main, false, mp.main_object);
	if (m.type == "header-only") return;

	Task t;
	t.outputs = {mp.artifact};
	t.stamp = pl_.build_dir / "stamps" / (m.name + ".link.stamp");
	t.label = mp.artifact.string();
	std::vector<fs::path> objs = mp.objects;
	std::vector<size_t> deps = mp.object_tasks;
	if (mp.main) { objs.push_back(mp.main_object); deps.push_back(*mp.main_task); }

	if (m.type == "static") {
		t.kind = "AR";
		t.cmd = {t_.ar, "rcs", mp.artifact.string()};
		for (auto& o : objs) t.cmd.push_back(o.string());
		t.inputs = objs;
		t.remove_outputs_first = true;
		t.tool_version = t_.ar;
	} else {
		t.kind = m.type == "shared" ? "SOLINK" : "LINK";
		t.cmd = link_prefix(mp, false);
		if (m.type == "shared") {
			t.cmd.push_back(t_.os == "macos" ? "-dynamiclib" : "-shared");
			if (t_.os == "windows") {
				t.cmd.push_back("-Wl,--out-implib," + mp.import_lib.string());
				t.outputs.push_back(mp.import_lib);
			} else if (t_.os == "macos") {
				t.cmd.push_back("-Wl,-install_name,@rpath/" + mp.artifact.filename().string());
			} else {
				t.cmd.push_back("-Wl,-soname," + mp.artifact.filename().string());
			}
		}
		for (auto& o : objs) t.cmd.push_back(o.string());
		t.inputs = objs;
		bool rpath = false;
		link_libs(mp, t.cmd, t.inputs, deps, rpath);
		if (rpath) add_rpath(t.cmd);
		t.cmd.push_back("-o");
		t.cmd.push_back(mp.artifact.string());
		t.tool_version = (closure_uses_cxx(mp, false) ? t_.cxx_tool() : t_.cc ? *t_.cc : t_.cxx_tool()).version;
	}
	t.deps = deps;
	pl_.tasks.push_back(std::move(t));
	mp.link_task = pl_.tasks.size() - 1;
}

void Planner::emit_tests(const std::string& name) {
	auto& mp = pl_.mods[name];
	const ModuleConfig& m = *mp.cfg;
	if (mp.tests.empty()) return;
	std::vector<fs::path> objs;
	std::vector<size_t> deps;
	for (auto& s : mp.tests) {
		fs::path obj;
		deps.push_back(add_compile(mp, s, true, obj));
		objs.push_back(obj);
	}
	Task t;
	t.kind = "LINK";
	mp.test_exe = pl_.build_dir / "tests" / (m.name + "-tests" + t_.exe_suffix());
	t.outputs = {mp.test_exe};
	t.stamp = pl_.build_dir / "stamps" / (m.name + ".tests.stamp");
	t.label = mp.test_exe.string();
	t.cmd = link_prefix(mp, true);
	for (auto& o : objs) t.cmd.push_back(o.string());
	t.inputs = objs;
	bool rpath = false;
	if (m.type == "executable") {
		// Les objets du programme, sauf celui qui contient main().
		for (auto& o : mp.objects) { t.cmd.push_back(o.string()); t.inputs.push_back(o); }
		deps.insert(deps.end(), mp.object_tasks.begin(), mp.object_tasks.end());
	} else if (m.type != "header-only") {
		fs::path lib = mp.import_lib.empty() ? mp.artifact : mp.import_lib;
		t.cmd.push_back(lib.string());
		t.inputs.push_back(lib);
		deps.push_back(*mp.link_task);
		if (m.type == "shared") rpath = true;
	}
	link_libs(mp, t.cmd, t.inputs, deps, rpath);
	if (rpath) add_rpath(t.cmd);
	t.cmd.push_back("-o");
	t.cmd.push_back(mp.test_exe.string());
	t.tool_version = (closure_uses_cxx(mp, true) ? t_.cxx_tool() : t_.cc ? *t_.cc : t_.cxx_tool()).version;
	t.deps = deps;
	pl_.tasks.push_back(std::move(t));
	mp.test_task = pl_.tasks.size() - 1;
}



Plan make_plan(const Project& p, const toolchain::Toolchain& t, const ModeConfig& mode,
					  const std::vector<std::string>& selected, bool with_tests) {
	Plan pl;
	pl.p = &p;
	pl.t = &t;
	pl.mode = &mode;
	pl.build_dir = build_dir_for(t.name, mode.name);
	pl.selected = selected;
	pl.with_tests = with_tests;
	Planner(pl).run();
	return pl;
}

// ---------------------------------------------------------------------------
// compile_commands.json
// ---------------------------------------------------------------------------
void write_compdb(const Plan& pl, const fs::path& out) {
	std::string root = fs::current_path().string();
	std::string s = "[\n";
	bool first = true;
	for (auto& t : pl.tasks) {
		if (!t.is_compile()) continue;
		if (!first) s += ",\n";
		first = false;
		s += "  {\n    \"directory\": \"" + util::json_escape(root) + "\",\n    \"arguments\": [";
		for (size_t i = 0; i < t.cmd.size(); ++i) {
			if (i) s += ", ";
			s += "\"" + util::json_escape(t.cmd[i]) + "\"";
		}
		s += "],\n    \"file\": \"" + util::json_escape(t.source.string()) + "\",\n";
		s += "    \"output\": \"" + util::json_escape(t.outputs[0].string()) + "\"\n  }";
	}
	s += "\n]\n";
	std::error_code ec;
	fs::create_directories(out.parent_path(), ec);
	util::write_file_atomic(out, s);
}

// ---------------------------------------------------------------------------
// build.ninja (exécuté depuis la racine : ninja -f build/<tc>/<mode>/build.ninja)
// ---------------------------------------------------------------------------
std::string ninja_path(const std::string& s) {
	std::string r;
	for (char c : s) {
		if (c == '$' || c == ' ' || c == ':') r += '$';
		if (c == '\n') continue;
		r += c;
	}
	return r;
}

std::string ninja_value(const std::string& s) {
    std::string r = util::replace_all(s, "$", "$$");
    // Sous Windows, ninja exécute la commande via `cmd.exe /c` : les lignes
    // multiples doivent être chaînées par `&` (et non par un espace, qui
    // couperait la commande en deux arguments).
    r = util::replace_all(r, "\n", util::host_is_windows() ? " & " : " ");
    return r;
}

fs::path write_ninja(const Plan& pl) {
	std::string s;
	s += "# Généré par cpp-maker " + std::string(CPP_MAKER_VERSION) + " — ne pas modifier.\n";
	s += std::format("# Toolchain : {}   Mode : {}\n", pl.t->name, pl.mode->name);
	s += std::format("# Hôte : {}   Shell : {}\n",
                 util::host_os_string(),
                 util::host_is_windows() ? "cmd.exe" : "/bin/sh");
	s += "# Usage (depuis la racine du projet) : ninja -f " + (pl.build_dir / "build.ninja").string() + "\n\n";
	s += "ninja_required_version = 1.3\n";
	s += "builddir = " + ninja_path(pl.build_dir.string()) + "\n\n";
	s += "rule compile\n  command = $cmd\n  description = $kind $label\n  depfile = $depfile\n  deps = gcc\n\n";
	s += "rule run\n  command = $cmd\n  description = $kind $label\n\n";
	std::vector<std::string> all;
	for (auto& t : pl.tasks) {
		std::string outs, ins, implicit;
		for (auto& o : t.outputs) outs += " " + ninja_path(o.string());
		for (auto& i : t.inputs) ins += " " + ninja_path(i.string());
		std::set<std::string> in_set;
		for (auto& i : t.inputs) in_set.insert(i.string());
		for (auto d : t.deps)
			for (auto& o : pl.tasks[d].outputs)
				if (!in_set.count(o.string())) implicit += " " + ninja_path(o.string());
		std::string cmd = util::shell_join(t.cmd);
		if (t.remove_outputs_first) cmd = "rm -f " + util::shell_quote(t.outputs[0].string()) + " && " + cmd;
		s += std::format("build{}: {}{}{}\n", outs, t.is_compile() ? "compile" : "run", ins,
						 implicit.empty() ? "" : " |" + implicit);
		s += "  cmd = " + ninja_value(cmd) + "\n";
		s += "  kind = " + t.kind + "\n";
		s += "  label = " + ninja_value(t.label) + "\n";
		if (t.is_compile()) s += "  depfile = " + ninja_value(t.depfile.string()) + "\n";
		s += "\n";
		if (!t.is_compile()) for (auto& o : t.outputs) all.push_back(ninja_path(o.string()));
	}
	if (all.empty())
		for (auto& t : pl.tasks) for (auto& o : t.outputs) all.push_back(ninja_path(o.string()));
	s += "build all: phony " + util::join(all, " ") + "\n";
	s += "default all\n";
	fs::path out = pl.build_dir / "build.ninja";
	std::error_code ec;
	fs::create_directories(pl.build_dir, ec);
	util::write_file_atomic(out, s);
	return out;
}

} // namespace build