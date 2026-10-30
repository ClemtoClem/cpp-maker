#include "cpp-maker.hpp"

namespace cfg {

std::vector<ModeConfig> default_modes() {
	return {
		{"release",  {"-O2"}, {}, {}, {}, {}, {"NDEBUG"}},
		{"debug",    {"-O0", "-g3"}, {}, {}, {}, {}, {"DEBUG"}},
		{"relwithdebinfo", {"-O2", "-g"}, {}, {}, {}, {}, {"NDEBUG"}},
		{"minsize",  {"-Os"}, {}, {}, {}, {}, {"NDEBUG"}},
		{"asan",     {"-O1", "-g3", "-fsanitize=address", "-fno-omit-frame-pointer"},
					 {"-fsanitize=address"}, "address", "", {}, {"DEBUG"}},
		{"tsan",     {"-O1", "-g3", "-fsanitize=thread", "-fno-omit-frame-pointer"},
					 {"-fsanitize=thread"}, "thread", "", {}, {"DEBUG"}},
		{"ubsan",    {"-O1", "-g3", "-fsanitize=undefined", "-fno-omit-frame-pointer"},
					 {"-fsanitize=undefined"}, "undefined", "", {}, {"DEBUG"}},
		{"msan",     {"-O1", "-g3", "-fsanitize=memory", "-fsanitize-memory-track-origins=2",
					  "-fno-omit-frame-pointer"},
					 {"-fsanitize=memory"}, "memory", "", {}, {"DEBUG"}},
		{"lsan",     {"-O1", "-g3", "-fsanitize=leak", "-fno-omit-frame-pointer"},
					 {"-fsanitize=leak"}, "leak", "", {}, {"DEBUG"}},
		{"valgrind", {"-O0", "-g3"}, {}, {}, "valgrind",
					 {"--leak-check=full", "--show-leak-kinds=all",
					  "--track-origins=yes", "--error-exitcode=1"}, {"DEBUG"}},
	};
}

const std::set<std::string>& cxx_standards() {
	static const std::set<std::string> s = {
		"c++98","c++03","c++11","c++14","c++17","c++20","c++23","c++26",
		"gnu++98","gnu++03","gnu++11","gnu++14","gnu++17","gnu++20","gnu++23","gnu++26"};
	return s;
}
const std::set<std::string>& c_standards() {
	static const std::set<std::string> s = {
		"c89","c90","c99","c11","c17","c18","c23","c2x",
		"gnu89","gnu90","gnu99","gnu11","gnu17","gnu18","gnu23","gnu2x"};
	return s;
}

// Les modes "-DXXX" dans flags (format v0.3) sont convertis en defines.
void normalize_mode(ModeConfig& m) {
	std::vector<std::string> flags;
	for (auto& f : m.flags) {
		if (f.starts_with("-D") && f.size() > 2) {
			if (std::find(m.defines.begin(), m.defines.end(), f.substr(2)) == m.defines.end())
				m.defines.push_back(f.substr(2));
		} else {
			flags.push_back(f);
		}
	}
	m.flags = std::move(flags);
	if (!m.sanitizer.empty()) {
		auto has = [](const std::vector<std::string>& v) {
			return std::any_of(v.begin(), v.end(),
							   [](auto& f) { return f.starts_with("-fsanitize="); });
		};
		if (!has(m.flags)) {
			m.flags.push_back("-fsanitize=" + m.sanitizer);
			m.flags.push_back("-fno-omit-frame-pointer");
		}
		if (!has(m.link_flags)) m.link_flags.push_back("-fsanitize=" + m.sanitizer);
	}
}

void parse_modes(const toml::Table& build, Project& p) {
	auto mt = toml::get_table(build, "modes");
	if (!mt) return;
	auto defaults = default_modes();
	// Ordre de traitement : un mode passe après le mode utilisateur dont il hérite.
	std::vector<std::string> order, pending;
	for (auto& [mn, mv] : *mt) pending.push_back(mn);
	while (!pending.empty()) {
		bool progress = false;
		for (auto it = pending.begin(); it != pending.end();) {
			auto t = mt->at(*it).table();
			std::optional<std::string> base = t ? toml::get_str(*t, "base") : std::nullopt;
			bool waits = base && *base != *it &&
						 std::find(pending.begin(), pending.end(), *base) != pending.end();
			if (waits) { ++it; continue; }
			order.push_back(*it);
			it = pending.erase(it);
			progress = true;
		}
		if (!progress)
			throw ConfigError("build.modes : héritage circulaire entre " + util::join(pending, ", "));
	}
	for (auto& mn : order) {
		auto t = mt->at(mn).table();
		if (!t) throw ConfigError(std::format("build.modes.{} doit être une table", mn));
		ModeConfig m;
		m.name = mn;
		auto base = toml::get_str(*t, "base");
		auto flags = toml::get_strs(*t, "flags");
		auto lflags = toml::get_strs(*t, "link_flags");
		auto defs = toml::get_strs(*t, "defines");
		toml::set_if(m.sanitizer, toml::get_str(*t, "sanitizer"));
		toml::set_if(m.runner, toml::get_str(*t, "runner"));
		auto rargs = toml::get_strs(*t, "runner_args");
		if (base) {
			const ModeConfig* b = nullptr;
			for (auto& d : p.modes) if (d.name == *base) b = &d;
			if (!b) for (auto& d : defaults) if (d.name == *base) b = &d;
			if (!b) throw ConfigError(std::format("build.modes.{} : base '{}' inconnue", mn, *base));
			ModeConfig bb = *b;
			if (m.sanitizer.empty()) m.sanitizer = bb.sanitizer;
			if (m.runner.empty()) { m.runner = bb.runner; if (!rargs) m.runner_args = bb.runner_args; }
			m.flags = bb.flags; m.link_flags = bb.link_flags; m.defines = bb.defines;
			// les sanitizers de la base ne s'appliquent que si conservés
			if (m.sanitizer != bb.sanitizer) {
				std::erase_if(m.flags, [](auto& f) { return f.starts_with("-fsanitize="); });
				std::erase_if(m.link_flags, [](auto& f) { return f.starts_with("-fsanitize="); });
			}
		}
		if (flags) m.flags = *flags;
		if (lflags) m.link_flags = *lflags;
		if (defs) m.defines = *defs;
		if (rargs) m.runner_args = *rargs;
		normalize_mode(m);
		p.modes.push_back(std::move(m));
	}
}

ToolchainConfig parse_toolchain(const std::string& name, const toml::Table& t) {
	ToolchainConfig c;
	c.name = name;
	c.from_config = true;
	toml::set_if(c.prefix, toml::get_str(t, "prefix"));
	toml::set_if(c.cc, toml::get_str(t, "cc"));
	toml::set_if(c.cxx, toml::get_str(t, "cxx"));
	toml::set_if(c.ar, toml::get_str(t, "ar"));
	toml::set_if(c.strip, toml::get_str(t, "strip"));
	toml::set_if(c.pkg_config, toml::get_str(t, "pkg_config"));
	toml::set_if(c.family, toml::get_str(t, "family"));
	toml::set_if(c.target, toml::get_str(t, "target"));
	toml::set_if(c.sysroot, toml::get_str(t, "sysroot"));
	toml::set_if(c.os, toml::get_str(t, "os"));
	toml::set_if(c.cflags, toml::get_strs(t, "cflags"));
	toml::set_if(c.cxxflags, toml::get_strs(t, "cxxflags"));
	toml::set_if(c.ldflags, toml::get_strs(t, "ldflags"));
	if (auto r = toml::get_strs(t, "runner")) { c.runner = *r; c.runner_set = true; }
	// v0.4 : extra_flags = options de compilation communes C et C++.
	if (auto f = toml::get_strs(t, "extra_flags")) {
		c.cflags.insert(c.cflags.end(), f->begin(), f->end());
		c.cxxflags.insert(c.cxxflags.end(), f->begin(), f->end());
	}
	return c;
}

PlatformOverride parse_platform(const toml::Table& t) {
	PlatformOverride o;
	toml::set_if(o.sources, toml::get_strs(t, "sources"));
	toml::set_if(o.defines, toml::get_strs(t, "defines"));
	toml::set_if(o.system_libs, toml::get_strs(t, "system"));
	toml::set_if(o.cflags, toml::get_strs(t, "cflags"));
	toml::set_if(o.cxxflags, toml::get_strs(t, "cxxflags"));
	toml::set_if(o.ldflags, toml::get_strs(t, "ldflags"));
	return o;
}

// Lit un module (module.toml, ou cpp-maker.toml pour un projet mono-module).
// `implicit` : accepte aussi les clés v0.3 ([project].type, [layout], [compiler]).
ModuleConfig parse_module(const toml::Table& root, const fs::path& dir,
								 const fs::path& file, const std::string& default_name,
								 bool implicit) {
	ModuleConfig m;
	m.dir = dir;
	m.file = file;
	m.name = default_name;
	bool priv_set = false;

	if (implicit) {
		if (auto pt = toml::get_table(root, "project"))
			if (auto t = toml::get_str(*pt, "type")) m.type = *t;
		if (auto lt = toml::get_table(root, "layout")) {
			toml::set_if(m.source_dirs, toml::get_strs(*lt, "source_dirs"));
			toml::set_if(m.test_dirs, toml::get_strs(*lt, "test_dirs"));
		}
		if (auto ct = toml::get_table(root, "compiler"))
			toml::set_if(m.public_includes, toml::get_strs(*ct, "include_dirs"));
	}
	if (auto t = toml::get_table(root, "module")) {
		toml::set_if(m.name, toml::get_str(*t, "name"));
		toml::set_if(m.type, toml::get_str(*t, "type"));
		toml::set_if(m.version, toml::get_str(*t, "version"));
		toml::set_if(m.description,  toml::get_str(*t, "description"));
		toml::set_if(m.main, toml::get_str(*t, "main"));
		toml::set_if(m.layer, toml::get_str(*t, "layer"));
		toml::set_if(m.install, toml::get_bool(*t, "install"));
		toml::set_if(m.c_standard, toml::get_str(*t, "c_standard"));
		toml::set_if(m.cxx_standard, toml::get_str(*t, "standard"));
		toml::set_if(m.cxx_standard, toml::get_str(*t, "cxx_standard"));
	}
	if (auto t = toml::get_table(root, "sources")) {
		toml::set_if(m.source_dirs, toml::get_strs(*t, "dirs"));
		toml::set_if(m.source_files, toml::get_strs(*t, "files"));
		toml::set_if(m.exclude, toml::get_strs(*t, "exclude"));
		toml::set_if(m.main, toml::get_str(*t, "main"));
	}
	if (auto t = toml::get_table(root, "include")) {
		toml::set_if(m.public_includes, toml::get_strs(*t, "public"));
		if (auto pv = toml::get_strs(*t, "private")) { m.private_includes = *pv; priv_set = true; }
	}
	if (auto t = toml::get_table(root, "dependencies")) {
		toml::set_if(m.deps, toml::get_strs(*t, "modules"));
		toml::set_if(m.pkg_config, toml::get_strs(*t, "pkg_config"));
		toml::set_if(m.system_libs, toml::get_strs(*t, "system"));
		if (auto g = toml::get_table(*t, "git")) {
			for (auto& [alias, v] : *g) {
				if (!v.table()) throw ConfigError("dependencies.git." + alias + " doit être une table");
				GitDependency gd;
				gd.alias   = alias;
				gd.url     = toml::get_str(*v.table(), "url").value_or("");
				gd.version = toml::get_str(*v.table(), "version").value_or("HEAD");
				gd.module  = toml::get_str(*v.table(), "module").value_or("");
				gd.subdir  = toml::get_str(*v.table(), "subdir").value_or("");
				if (gd.url.empty())
					throw ConfigError("dependencies.git." + alias + " : champ 'url' requis");
				m.git_deps.push_back(std::move(gd));
			}
		}
	}
	if (auto t = toml::get_table(root, "flags")) {
		toml::set_if(m.defines, toml::get_strs(*t, "defines"));
		toml::set_if(m.public_defines, toml::get_strs(*t, "public_defines"));
		toml::set_if(m.cflags, toml::get_strs(*t, "cflags"));
		toml::set_if(m.cxxflags, toml::get_strs(*t, "cxxflags"));
		toml::set_if(m.ldflags, toml::get_strs(*t, "ldflags"));
	}
	if (auto t = toml::get_table(root, "tests"))
		toml::set_if(m.test_dirs, toml::get_strs(*t, "dirs"));
	if (auto t = toml::get_table(root, "platform")) {
		for (auto& [os, v] : *t) {
			if (!v.table()) throw ConfigError(std::format("platform.{} doit être une table", os));
			m.platforms[os] = parse_platform(*v.table());
		}
	}
	if (m.type == "library") m.type = "static";        // alias v0.3
	if (m.type == "interface") m.type = "header-only";
	if (!priv_set) m.private_includes = m.source_dirs;
	return m;
}

void import_git_dep(Project& p, const ModuleConfig& owner, const GitDependency& gd) {
	std::string alias = gd.alias;
	if (p.find_module(alias))
		throw ConfigError(std::format("{}: dépendance git '{}' : alias '{}' déjà pris", owner.file.string(), gd.url, alias));

	auto it = p.git_lock.find(gd.url);
	std::string locked = it == p.git_lock.end() ? std::string{} : it->second;

	auto r = deps::resolve(gd.url, gd.version, locked);
	if (!r.ok)
		throw ConfigError(std::format("{}: dépendance git '{}' (version {}): {}", owner.file.string(), gd.url, gd.version, r.error));
	p.git_lock[gd.url] = r.commit;

	fs::path dep_root = r.path / gd.subdir;
	fs::path dep_file = dep_root / "cpp-maker.toml";
	if (!fs::exists(dep_file))
		throw ConfigError(std::format("{}: dépendance git '{}' : {} introuvable", owner.file.string(), gd.url, dep_file.generic_string()));

	Project dep;
	try { dep = load(dep_root, dep_file); }
	catch (const std::exception& e) {
		throw ConfigError(std::format("{}: dépendance git '{}' : {}", owner.file.string(), gd.url, e.what()));
	}

	std::string entry_name = gd.module.empty() ? dep.name : gd.module;
	auto entry = dep.find_module(entry_name);
	if (!entry) {
		std::vector<std::string> names;
		for (auto& m : dep.modules) names.push_back(m.name);
		throw ConfigError(std::format("{}: dépendance git '{}' : module '{}' introuvable ({} )",
										owner.file.string(), gd.url, entry_name,
										util::join(names, ", ")));
	}

	// Nom de chaque module du dépôt dans NOTRE espace de noms.
	std::map<std::string, std::string> names;
	for (auto& dm : dep.modules)
		names[dm.name] = dm.name == entry_name ? alias : alias + "__" + dm.name;
	for (auto& [_, local] : names)
		if (p.find_module(local) && local != alias)
			throw ConfigError(std::format("{}: dépendance git '{}' : nom '{}' déjà pris",
											owner.file.string(), gd.url, local));

	util::outln("\033[1;34m[cpp-maker]\033[0m dépendance git {} @ {} ({})",
				gd.url, gd.version, r.commit.substr(0, 10));

	for (auto& dm : dep.modules) {
		ModuleConfig m = dm;
		m.name = names[dm.name];
		m.dir  = dep_root / dm.dir;
		m.file = dep_root / dm.file;
		m.from_git = true;
		m.install  = false;                      // pas installé par le parent
		m.git_deps.clear();                      // déjà résolu récursivement
		for (auto& d : m.deps)
			if (auto j = names.find(d); j != names.end()) d = j->second;
		p.modules.push_back(std::move(m));
	}
}

void resolve_git_modules(Project& p) {
	std::deque<std::pair<std::string, GitDependency>> q;
	for (auto& m : p.modules)
		for (auto& g : m.git_deps) q.push_back({m.name, g});

	std::map<std::string, std::string> url_alias;
	std::set<std::string> done;
	while (!q.empty()) {
		auto [owner_name, gd] = q.front(); q.pop_front();
		if (auto it = url_alias.find(gd.url); it != url_alias.end()) {
			if (it->second != gd.alias)
				throw ConfigError(std::format(
					"dépendance git '{}' déclarée sous deux alias ('{}' et '{}') — "
					"utilisez un alias unique par URL", gd.url, it->second, gd.alias));
			continue;
		}
		url_alias[gd.url] = gd.alias;
		if (!done.insert(gd.url).second) continue;
		auto owner = p.find_module(owner_name);
		if (!owner) continue;
		import_git_dep(p, *owner, gd);   // lève ConfigError en cas d'échec
	}
}

void write_lock_file(Project& p) {
	if (p.git_lock.empty()) return;
	fs::path lock = p.root / "cpp-maker.lock";
	std::string old;
	util::read_file(lock, old);
	std::string s = "# Généré par cpp-maker — versions verrouillées.\n"
					"# Ne pas éditer ; lancez 'cpp-maker deps update' pour rafraîchir.\n\n";
	for (auto& [url, commit] : p.git_lock)
		s += std::format("[git.\"{}\"]\ncommit = \"{}\"\n", url, commit);
	if (s != old) util::write_file_atomic(lock, s);
}

// Charge cpp-maker.toml et, pour un workspace, chaque module.toml.
Project load(const fs::path& root_dir, const fs::path& file) {
	Project p;
	p.root = root_dir;
	p.file = file;
	p.name = root_dir.filename().string();
	std::error_code ec;
	p.has_file = fs::exists(file, ec);

	toml::Table root;
	if (p.has_file) root = toml::parse_file(file);

	try {
		if (auto t = toml::get_table(root, "project")) {
			toml::set_if(p.name, toml::get_str(*t, "name"));
			toml::set_if(p.version, toml::get_str(*t, "version"));
			toml::set_if(p.description, toml::get_str(*t, "description"));
			toml::set_if(p.cxx_standard, toml::get_str(*t, "standard"));
			toml::set_if(p.cxx_standard, toml::get_str(*t, "cxx_standard"));
			toml::set_if(p.c_standard, toml::get_str(*t, "c_standard"));
			toml::set_if(p.min_version, toml::get_str(*t, "minimum_cpp_maker_version"));
		}
		if (auto t = toml::get_table(root, "build")) {
			toml::set_if(p.default_mode, toml::get_str(*t, "default_mode"));
			toml::set_if(p.default_toolchain, toml::get_str(*t, "default_toolchain"));
			if (auto j = toml::get_int(*t, "jobs")) p.jobs = (int)*j;
			toml::set_if(p.warnings, toml::get_strs(*t, "warnings"));
			toml::set_if(p.static_pic, toml::get_bool(*t, "static_pic"));
			toml::set_if(p.cflags, toml::get_strs(*t, "cflags"));
			toml::set_if(p.cxxflags, toml::get_strs(*t, "cxxflags"));
			toml::set_if(p.ldflags, toml::get_strs(*t, "ldflags"));
			toml::set_if(p.defines, toml::get_strs(*t, "defines"));
			parse_modes(*t, p);
		}
		// Modes standard non redéfinis : conservés.
		for (auto& d : default_modes())
			if (!p.find_mode(d.name)) p.modes.push_back(d);

		if (auto t = toml::get_table(root, "toolchains")) {
			for (auto& [n, v] : *t) {
				if (!v.table()) throw ConfigError(std::format("toolchains.{} doit être une table", n));
				p.toolchains[n] = parse_toolchain(n, *v.table());
			}
		}
		// [compiler] v0.3 : compilateur hôte + options globales.
		if (auto t = toml::get_table(root, "compiler")) {
			auto& host = p.toolchains["host"];
			host.name = "host";
			toml::set_if(host.cxx, toml::get_str(*t, "cxx"));
			toml::set_if(host.cc, toml::get_str(*t, "cc"));
			toml::set_if(host.family, toml::get_str(*t, "family"));
			if (auto f = toml::get_strs(*t, "cxxflags"))
				p.cxxflags.insert(p.cxxflags.end(), f->begin(), f->end());
			if (auto f = toml::get_strs(*t, "cflags"))
				p.cflags.insert(p.cflags.end(), f->begin(), f->end());
			if (auto f = toml::get_strs(*t, "ldflags"))
				p.ldflags.insert(p.ldflags.end(), f->begin(), f->end());
		}
		
		if (auto t = toml::get_table(root, "paths")) {
			toml::set_if(p.out_dir,      toml::get_str(*t, "out_dir"));
			toml::set_if(p.backups_dir,  toml::get_str(*t, "backups_dir"));
			if (p.out_dir.empty())     p.out_dir = ".out";
			if (p.backups_dir.empty()) p.backups_dir = ".backups";
		}

		if (auto t = toml::get_table(root, "tests")) {
			if (auto to = toml::get_int(*t, "timeout")) p.test_timeout = (int)*to;
			if (auto env = toml::get_table(*t, "env")) {
				for (auto& [k, v] : *env) {
					if (!v.str()) throw ConfigError(std::format("tests.env.{} doit être une chaîne", k));
					p.test_env.emplace_back(k, *v.str());
				}
			}
		}
		if (auto t = toml::get_table(root, "generate")) {
			for (auto& [k, v] : *t) {
				if (!v.table()) throw ConfigError(std::format("generate.{} doit être une table", k));
				Project::GenerateStep g;
				g.name = k;
				toml::set_if(g.command, toml::get_str(*v.table(), "command"));
				toml::set_if(g.inputs, toml::get_strs(*v.table(), "inputs"));
				toml::set_if(g.outputs, toml::get_strs(*v.table(), "outputs"));
				if (g.command.empty()) throw ConfigError(std::format("generate.{}.command requis", k));
				p.generate_steps.push_back(std::move(g));
			}
		}
		if (auto t = toml::get_table(root, "layers")) {
			toml::set_if(p.layer_order, toml::get_strs(*t, "order"));
			for (auto& [k, v] : *t) {
				if (k == "order") continue;
				if (!v.table()) throw ConfigError(std::format("layers.{} doit être une table", k));
				if (auto f = toml::get_strs(*v.table(), "forbid_pkg_config")) p.layer_forbid_pkg[k] = *f;
			}
		}

		if (auto t = toml::get_table(root, "package")) {
			auto& k = p.package;
			toml::set_if(k.formats, toml::get_strs(*t, "formats"));
			toml::set_if(k.strip, toml::get_bool(*t, "strip"));
			toml::set_if(k.maintainer, toml::get_str(*t, "maintainer"));
			toml::set_if(k.description, toml::get_str(*t, "description"));
			toml::set_if(k.homepage, toml::get_str(*t, "homepage"));
			toml::set_if(k.license, toml::get_str(*t, "license"));
			toml::set_if(k.deb_depends, toml::get_strs(*t, "deb_depends"));
			toml::set_if(k.deb_prefix, toml::get_str(*t, "deb_prefix"));
			toml::set_if(k.extra_files, toml::get_strs(*t, "extra_files"));
			toml::set_if(k.checksums, toml::get_bool(*t, "checksums"));
		}
		if (p.package.description.empty()) p.package.description = p.description;

		if (auto t = toml::get_table(root, "deploy")) {
			if (toml::get_str(*t, "method").has_value() ||
				toml::find(*t, "target_dir")) {
				// [deploy] unique v0.4 : method = rsync | scp | local,
				// host = "user@machine", target_dir, pre/post → cible "default".
				DeployConfig d;
				d.name = "default";
				if (auto m = toml::get_str(*t, "method")) {
					if (*m == "local") d.method = "copy";
					else d.method = *m;
				}
				if (auto h = toml::get_str(*t, "host")) {
					auto at = h->find('@');
					if (at != std::string::npos) {
						d.user = h->substr(0, at);
						d.host = h->substr(at + 1);
					} else {
						d.host = *h;
					}
				}
				toml::set_if(d.path, toml::get_str(*t, "target_dir"));
				toml::set_if(d.before, toml::get_strs(*t, "pre"));
				// v0.4 : 'post' s'exécutait localement, jamais sur la cible.
				toml::set_if(d.after_local, toml::get_strs(*t, "post"));
				toml::set_if(d.script, toml::get_str(*t, "script"));
				p.deploys["default"] = std::move(d);
			} else {
			for (auto& [n, v] : *t) {
				auto dt = v.table();
				if (!dt) throw ConfigError(std::format("deploy.{} doit être une table", n));
				DeployConfig d;
				d.name = n;
				toml::set_if(d.method, toml::get_str(*dt, "method"));
				toml::set_if(d.host, toml::get_str(*dt, "host"));
				toml::set_if(d.user, toml::get_str(*dt, "user"));
				toml::set_if(d.path, toml::get_str(*dt, "path"));
				if (auto port = toml::get_int(*dt, "port")) d.port = (int)*port;
				toml::set_if(d.toolchain, toml::get_str(*dt, "toolchain"));
				toml::set_if(d.mode, toml::get_str(*dt, "mode"));
				toml::set_if(d.artifact, toml::get_str(*dt, "artifact"));
				toml::set_if(d.modules, toml::get_strs(*dt, "modules"));
				toml::set_if(d.ssh_options, toml::get_strs(*dt, "ssh_options"));
				toml::set_if(d.before, toml::get_strs(*dt, "before"));
				toml::set_if(d.after, toml::get_strs(*dt, "after"));
				toml::set_if(d.after_local, toml::get_strs(*dt, "after_local"));
				toml::set_if(d.script, toml::get_str(*dt, "script"));
				toml::set_if(d.delete_extra, toml::get_bool(*dt, "delete"));
				p.deploys[n] = std::move(d);
			}
			} // fin du format v0.5 [deploy.<nom>]
		}

		if (auto t = toml::get_table(root, "workspace")) {
			p.workspace = true;
			toml::set_if(p.module_paths, toml::get_strs(*t, "modules"));
		}
	} catch (const toml::ParseError& e) {
		throw ConfigError(std::format("{}: {}", file.string(), e.what()));
	}

	if (p.workspace) {
		for (auto& mp : p.module_paths) {
			fs::path dir = fs::path(mp).lexically_normal();
			if (dir.is_absolute() || (!dir.empty() && *dir.begin() == ".."))
				throw ConfigError(std::format("{}: module '{}' hors du workspace", file.string(), mp));
			fs::path mf = root_dir / dir / "module.toml";
			if (!fs::exists(mf, ec))
				throw ConfigError(std::format("{}: module '{}' : {} introuvable",
											  file.string(), mp, (dir / "module.toml").string()));
			auto mt = toml::parse_file(mf);
			try {
				p.modules.push_back(parse_module(mt, dir, dir / "module.toml",
												 dir.filename().string(), false));
			} catch (const toml::ParseError& e) {
				throw ConfigError(std::format("{}: {}", (dir / "module.toml").string(), e.what()));
			}
		}
	} else if (toml::get_table(root, "modules")) {
		// Rétro-compatibilité v0.4 : [modules.<nom>] dans cpp-maker.toml.
		// Chaque module reste à la racine du projet (dir = "."), les
		// source_dirs/include_dirs v0.4 étant déjà relatifs à la racine.
		auto mt = toml::get_table(root, "modules");
		try {
			for (auto& [mn, mv] : *mt) {
				auto t = mv.table();
				if (!t) throw toml::ParseError(std::format("modules.{} doit être une table", mn));
				ModuleConfig m;
				m.name = mn;
				m.dir = ".";
				m.file = file.filename();
				// Défauts v0.4 : type static, aucun dossier implicite. Les tests
				// v0.4 (tests/) appartiennent à l'exécutable principal seul.
				m.type = "static";
				m.source_dirs.clear();
				m.public_includes.clear();
				m.test_dirs.clear();
				toml::set_if(m.type, toml::get_str(*t, "type"));
				toml::set_if(m.source_dirs, toml::get_strs(*t, "source_dirs"));
				toml::set_if(m.public_includes, toml::get_strs(*t, "include_dirs"));
				toml::set_if(m.deps, toml::get_strs(*t, "deps"));
				toml::set_if(m.defines, toml::get_strs(*t, "defines"));
				if (auto f = toml::get_strs(*t, "flags")) {
					m.cflags = *f;
					m.cxxflags = std::move(*f);
				}
				toml::set_if(m.ldflags, toml::get_strs(*t, "link_flags"));
				if (m.type == "interface") m.type = "header-only"; // alias v0.4
				if (m.type == "library")   m.type = "static";      // alias v0.3
				m.private_includes = m.source_dirs;
				p.modules.push_back(std::move(m));
			}
			// L'exécutable global v0.4 (sources du projet hors modules, p.ex.
			// src/main.cpp) reste la cible principale : il est chargé comme
			// un module de plus, dépendant de tous les [modules.*] — qui sont
			// désormais liés automatiquement (plus de ldflags manuels).
			{
				std::vector<std::string> dirs = {"src"};
				if (auto lt = toml::get_table(root, "layout"))
					if (auto d = toml::get_strs(*lt, "source_dirs")) dirs = *d;
				auto has_source = [&](const std::string& d) {
					std::error_code dir_ec;
					if (!fs::is_directory(d, dir_ec)) return false;
					fs::recursive_directory_iterator it(d,
						fs::directory_options::skip_permission_denied, dir_ec);
					if (dir_ec) return false;
					for (; it != fs::recursive_directory_iterator(); it.increment(dir_ec)) {
						if (dir_ec) break;
						if (!it->is_regular_file(dir_ec) || dir_ec) continue;
						auto e = it->path().extension().string();
						for (auto* x : {".c", ".cpp", ".cc", ".cxx", ".c++", ".C", ".S", ".s"})
							if (e == x) return true;
					}
					return false;
				};
				bool any = false;
				for (auto& d : dirs) if (has_source(d)) { any = true; break; }
				if (any) {
					try {
						auto m = parse_module(root, ".", file.filename(), p.name, true);
						for (auto& lm : p.modules) m.deps.push_back(lm.name);
						p.modules.push_back(std::move(m));
					} catch (const toml::ParseError& e) {
						throw ConfigError(std::format("{}: {}", file.string(), e.what()));
					}
				}
			}
		} catch (const toml::ParseError& e) {
			throw ConfigError(std::format("{}: {}", file.string(), e.what()));
		}
	} else {
		try {
			p.modules.push_back(parse_module(root, ".", file.filename(), p.name, true));
		} catch (const toml::ParseError& e) {
			throw ConfigError(std::format("{}: {}", file.string(), e.what()));
		}
	}
	resolve_git_modules(p);
	write_lock_file(p);
	return p;
}

// Ordre topologique (dépendances d'abord) de `roots` et de leur fermeture.
// Lève ConfigError en cas de cycle ou de module inconnu.
std::vector<std::string> topo_order(const Project& p, const std::vector<std::string>& roots) {
	std::vector<std::string> order;
	std::map<std::string, int> state; // 0 = blanc, 1 = en cours, 2 = fini
	std::vector<std::string> stack;
	std::function<void(const std::string&)> visit = [&](const std::string& n) {
		auto m = p.find_module(n);
		if (!m) throw ConfigError(std::format("module inconnu : '{}'", n));
		if (state[n] == 2) return;
		if (state[n] == 1) {
			auto it = std::find(stack.begin(), stack.end(), n);
			std::vector<std::string> cyc(it, stack.end());
			cyc.push_back(n);
			throw ConfigError("dépendance circulaire : " + util::join(cyc, " → "));
		}
		state[n] = 1;
		stack.push_back(n);
		for (auto& d : m->deps) visit(d);
		stack.pop_back();
		state[n] = 2;
		order.push_back(n);
	};
	for (auto& r : roots) visit(r);
	return order;
}

// Retourne une chaîne vide si la configuration est valide, sinon un
// diagnostic complet (fichier + champ + nature de l'erreur).
// Couches d'architecture : un module ne dépend que de sa couche ou d'une
// couche inférieure ; les paquets interdits d'une couche ne doivent pas
// apparaître dans la fermeture transitive de ses modules.
std::string validate_layers(const Project& p, const std::string& file) {
	auto rank = [&](const std::string& l) -> int {
		for (size_t i = 0; i < p.layer_order.size(); ++i) if (p.layer_order[i] == l) return (int)i;
		return -1;
	};
	for (auto& [l, f] : p.layer_forbid_pkg) {
		(void)f;
		if (rank(l) < 0)
			return std::format("{}: [layers.{}] : couche absente de layers.order", file, l);
	}
	for (auto& m : p.modules) {
		if (m.layer.empty()) continue;
		if (rank(m.layer) < 0)
			return std::format("{}: couche inconnue '{}' (layers.order : {})", m.file.string(), m.layer,
							   p.layer_order.empty() ? "vide" : util::join(p.layer_order, ", "));
	}
	for (auto& m : p.modules) {
		if (m.layer.empty()) continue;
		for (auto& d : m.deps) {
			auto dm = p.find_module(d);
			if (!dm || dm->layer.empty()) continue;
			if (rank(dm->layer) > rank(m.layer))
				return std::format("{}: '{}' (couche {}) ne peut pas dépendre de '{}' (couche {}, "
								   "supérieure) — inversez la dépendance ou déplacez le code",
								   m.file.string(), m.name, m.layer, d, dm->layer);
		}
		auto it = p.layer_forbid_pkg.find(m.layer);
		if (it == p.layer_forbid_pkg.end()) continue;
		// Fermeture transitive des dépendances de modules.
		std::vector<const ModuleConfig*> stack{&m};
		std::set<std::string> seen{m.name};
		while (!stack.empty()) {
			const ModuleConfig* cur = stack.back();
			stack.pop_back();
			for (auto& pkg : cur->pkg_config)
				if (std::find(it->second.begin(), it->second.end(), pkg) != it->second.end())
					return std::format("{}: le paquet '{}' est interdit dans la couche {} ; "
									   "'{}' en dépend{}", m.file.string(), pkg, m.layer, m.name,
									   cur == &m ? "" : std::format(" via '{}'", cur->name));
			for (auto& d : cur->deps)
				if (auto dm = p.find_module(d); dm && seen.insert(d).second) stack.push_back(dm);
		}
	}
	return {};
}

std::string validate_errors(const Project& p) {
	std::string file = p.file.string();
	if (!cfg::cxx_standards().count(p.cxx_standard))
		return std::format("{}: champ 'project.standard' invalide : '{}'", file, p.cxx_standard);
	if (!cfg::c_standards().count(p.c_standard))
		return std::format("{}: champ 'project.c_standard' invalide : '{}'", file, p.c_standard);
	if (!util::is_safe_name(p.name))
		return std::format("{}: champ 'project.name' invalide : '{}'", file, p.name);
	if (!util::is_safe_name(p.out_dir))
		return std::format("{}: [paths] out_dir invalide : '{}'", file, p.out_dir);
	if (!util::is_safe_name(p.backups_dir))
		return std::format("{}: [paths] backups_dir invalide : '{}'", file, p.backups_dir);
	if (p.jobs < 0) return std::format("{}: 'build.jobs' doit être ≥ 0", file);
	if (!p.find_mode(p.default_mode))
		return std::format("{}: 'build.default_mode' : mode inconnu '{}'", file, p.default_mode);
	for (auto& m : p.modes) {
		if (!util::is_safe_name(m.name))
			return std::format("{}: nom de mode invalide : '{}'", file, m.name);
		for (auto& san : util::split_list(m.sanitizer))
			if (!sanitize::find(sanitize::normalize(san)))
				return std::format("{}: mode '{}' : sanitizer '{}' inconnu (cpp-maker help sanitize)",
								   file, m.name, san);
		if (auto err = sanitize::check_compatible(sanitize::from_mode(m)); !err.empty())
			return std::format("{}: mode '{}' : {}", file, m.name, err);
		if (!m.runner.empty() && m.runner != "valgrind")
			return std::format("{}: runner '{}' non supporté (valgrind)", file, m.runner);
	}
	for (auto& [n, t] : p.toolchains) {
		if (!util::is_safe_name(n))
			return std::format("{}: nom de toolchain invalide : '{}'", file, n);
		if (t.family != "auto" && t.family != "gcc" && t.family != "clang")
			return std::format("{}: toolchains.{}.family invalide : '{}'", file, n, t.family);
		if (!t.os.empty() && t.os != "linux" && t.os != "windows" && t.os != "macos" &&
			t.os != "android" && t.os != "freebsd" && t.os != "baremetal")
			return std::format("{}: toolchains.{}.os invalide : '{}'", file, n, t.os);
	}
	std::set<std::string> names;
	for (auto& m : p.modules) {
		std::string mf = m.file.string();
		if (!util::is_safe_name(m.name))
			return std::format("{}: nom de module invalide : '{}'", mf, m.name);
		if (!names.insert(m.name).second)
			return std::format("{}: module '{}' défini deux fois", mf, m.name);
		static const std::set<std::string> types = {"executable", "static", "shared", "header-only"};
		if (!types.count(m.type))
			return std::format("{}: type de module invalide : '{}' "
							   "(executable|static|shared|header-only)", mf, m.type);
		if (!m.cxx_standard.empty() && !cfg::cxx_standards().count(m.cxx_standard))
			return std::format("{}: standard C++ invalide : '{}'", mf, m.cxx_standard);
		if (!m.c_standard.empty() && !cfg::c_standards().count(m.c_standard))
			return std::format("{}: standard C invalide : '{}'", mf, m.c_standard);
		for (auto& d : m.deps) {
			if (d == m.name) return std::format("{}: le module '{}' dépend de lui-même", mf, d);
		}
		for (auto& [os, o] : m.platforms) {
			(void)o;
			static const std::set<std::string> oses = {"linux", "windows", "macos", "android",
													   "freebsd", "baremetal", "unix"};
			if (!oses.count(os))
				return std::format("{}: plate-forme inconnue [platform.{}]", mf, os);
		}
	}
	for (auto& m : p.modules) {
		for (auto& d : m.deps) {
			auto dm = p.find_module(d);
			if (!dm)
				return std::format("{}: dépendance inconnue '{}' (modules : {})", m.file.string(), d,
								   [&] { std::vector<std::string> v; for (auto& x : p.modules) v.push_back(x.name); return util::join(v, ", "); }());
			if (dm->type == "executable")
				return std::format("{}: '{}' dépend de l'exécutable '{}' (seules les bibliothèques "
								   "peuvent être des dépendances)", m.file.string(), m.name, d);
		}
	}
	try {
		std::vector<std::string> all;
		for (auto& m : p.modules) all.push_back(m.name);
		topo_order(p, all);
	} catch (const ConfigError& e) {
		return std::format("{}: {}", file, e.what());
	}
	if (auto err = validate_layers(p, file); !err.empty()) return err;
	for (auto& [n, d] : p.deploys) {
		if (d.method != "rsync" && d.method != "scp" && d.method != "copy" && d.method != "script")
			return std::format("{}: deploy.{}.method invalide : '{}' (rsync|scp|copy|script)", file, n, d.method);
		if (d.method == "script") {
			if (d.script.empty())
				return std::format("{}: deploy.{}.script requis pour la méthode 'script'", file, n);
			continue;
		}
		if (d.method != "copy" && d.host.empty())
			return std::format("{}: deploy.{}.host requis pour la méthode '{}'", file, n, d.method);
		if (d.path.empty())
			return std::format("{}: deploy.{}.path requis", file, n);
		static const std::set<std::string> arts = {"stage", "tar.gz", "tar.xz", "zip", "deb"};
		if (!arts.count(d.artifact))
			return std::format("{}: deploy.{}.artifact invalide : '{}'", file, n, d.artifact);
	}
	for (auto& f : p.package.formats) {
		static const std::set<std::string> fmts = {"tar.gz", "tar.xz", "zip", "deb"};
		if (!fmts.count(f))
			return std::format("{}: package.formats : format inconnu '{}' (tar.gz|tar.xz|zip|deb)", file, f);
	}
	return {};
}

} // namespace cfg