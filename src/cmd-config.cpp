#include "cpp-maker.hpp"

namespace commands {

// ---------------------------------------------------------------------------
// Analyse statique des #include pour la détection des dépendances implicites
// ---------------------------------------------------------------------------

// Extrait les chemins des directives #include "…" / <…> d'un fichier source.
std::vector<std::string> scan_includes(const fs::path& file) {
	std::vector<std::string> out;
	std::string content;
	if (!util::read_file(file, content)) return out;
	size_t i = 0;
	while ((i = content.find("#include", i)) != std::string::npos) {
		size_t j = i + 8;
		while (j < content.size() && (content[j] == ' ' || content[j] == '\t')) ++j;
		if (j >= content.size()) break;
		char open = content[j];
		if (open != '"' && open != '<') { i += 8; continue; }
		char close = open == '"' ? '"' : '>';
		auto end = content.find(close, j + 1);
		if (end == std::string::npos || end > content.find('\n', j)) { i = j + 1; continue; }
		out.push_back(content.substr(j + 1, end - j - 1));
		i = end + 1;
	}
	return out;
}

// Indexe chaque en-tête public du workspace : "chemin/relatif.hpp" → module.
std::map<std::string, std::string> public_header_index(const Project& p) {
	std::map<std::string, std::string> idx;
	std::error_code ec;
	for (auto& m : p.modules) {
		for (auto& inc_dir : m.public_includes) {
			fs::path root = util::under(m.dir, inc_dir);
			if (!fs::is_directory(root, ec)) continue;
			for (auto& e : fs::recursive_directory_iterator(root,
					 fs::directory_options::skip_permission_denied, ec)) {
				if (ec) break;
				if (!e.is_regular_file(ec)) continue;
				auto rel = e.path().lexically_relative(root).generic_string();
				idx[rel] = m.name;
			}
		}
	}
	return idx;
}

// En-tête C/C++ (analysé pour les #include comme les sources).
bool is_header_file(const std::string& e) {
	return e == ".h" || e == ".hpp" || e == ".hh" || e == ".hxx" || e == ".h++" ||
		   e == ".ipp" || e == ".tpp" || e == ".inl";
}

// Retourne les couples (module, dépendance_manquante) détectés à partir des
// #include qui pointent vers les en-têtes publics d'un autre module non
// joignable par la fermeture transitive des dépendances déclarées.
std::vector<std::pair<std::string, std::string>> detect_missing_deps(const Project& p) {
	std::set<std::pair<std::string, std::string>> missing;
	auto idx = public_header_index(p);
	for (auto& m : p.modules) {
		// Fermeture transitive des dépendances déjà déclarées.
		std::set<std::string> reachable;
		std::vector<std::string> stack = m.deps;
		while (!stack.empty()) {
			auto n = stack.back(); stack.pop_back();
			if (!reachable.insert(n).second) continue;
			if (auto dm = p.find_module(n))
				for (auto& d : dm->deps) stack.push_back(d);
		}
		// On ignore tests/ (ils ne créent pas de dépendance de compilation).
		// Les en-têtes comptent : un #include dans un en-tête public impose
		// la dépendance à tous les modules qui l'incluent.
		std::vector<fs::path> files;
		for (auto& src : build::discover(m, m.source_dirs)) files.push_back(src.path);
		std::vector<std::string> hdr_dirs = m.public_includes;
		hdr_dirs.insert(hdr_dirs.end(), m.private_includes.begin(), m.private_includes.end());
		for (auto& d : hdr_dirs) {
			fs::path dir = util::under(m.dir, d);
			std::error_code ec;
			if (!fs::is_directory(dir, ec)) continue;
			for (auto& e : fs::recursive_directory_iterator(dir,
					 fs::directory_options::skip_permission_denied, ec)) {
				if (ec) break;
				auto ext = e.path().extension().string();
				if (e.is_regular_file(ec) && is_header_file(ext))
					files.push_back(e.path());
			}
		}
		std::sort(files.begin(), files.end());
		files.erase(std::unique(files.begin(), files.end()), files.end());
		for (auto& file : files) {
			for (auto& inc : scan_includes(file)) {
				auto it = idx.find(inc);
				if (it == idx.end() || it->second == m.name) continue;
				if (reachable.count(it->second)) continue;
				missing.insert({m.name, it->second});
			}
		}
	}
	return {missing.begin(), missing.end()};
}

// Applique les dépendances manquantes : pour chaque module.toml concerné,
// ajoute les nouvelles entrées au tableau [dependencies].modules, valide
// la configuration globale puis remplace le fichier atomiquement (rollback
// en cas d'erreur). Retourne le nombre de modules effectivement modifiés.
int apply_missing_deps(const Project& p,
		const std::vector<std::pair<std::string, std::string>>& missing) {
	std::map<std::string, std::vector<std::string>> to_add;
	for (auto& [mn, dn] : missing) to_add[mn].push_back(dn);
	int fixed = 0;
	for (auto& [mn, adds] : to_add) {
		auto m = p.find_module(mn);
		if (!m) continue;
		fs::path file = editable_module_file(p, *m);
		std::string text;
		if (!util::read_file(file, text)) {
			util::warn("impossible de lire {}", file.string());
			continue;
		}
		std::vector<std::string> deps = m->deps;
		for (auto& d : adds)
			if (std::find(deps.begin(), deps.end(), d) == deps.end())
				deps.push_back(d);
		std::string new_text = replace_toml_array(text, "dependencies", "modules", deps);
		if (new_text == text) continue;
		util::write_file_atomic(file, new_text);
		try {
			auto np = cfg::load(p.root, p.file);
			auto e = cfg::validate_errors(np);
			if (!e.empty()) throw std::runtime_error(e);
		} catch (const std::exception& e) {
			util::write_file_atomic(file, text);
			util::warn("édition annulée pour '{}' : {}", mn, e.what());
			continue;
		}
		util::outln("  \033[1;32m+\033[0m {:<14} ← {}", mn, util::join(adds, ", "));
		++fixed;
	}
	return fixed;
}

// ---------------------------------------------------------------------------
// doctor
// ---------------------------------------------------------------------------
int cmd_doctor(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {}, {"--no-fix", "--check-deps"});
	auto c = load_ctx(false);
	int problems = 0;
	auto mark = [&](bool ok, const std::string& msg, bool fatal = true) {
		if (ok) util::outln("  \033[1;32m[ ok ]\033[0m {}", msg);
		else if (fatal) { util::outln("  \033[1;31m[fail]\033[0m {}", msg); ++problems; }
		else util::outln("  \033[1;33m[ -- ]\033[0m {}", msg);
	};

	util::outln("\033[1;36mcpp-maker doctor\033[0m (v{})\n", CPP_MAKER_VERSION);
	util::outln("\033[1mConfiguration\033[0m");
	mark(c.p.has_file, "cpp-maker.toml présent");
	auto err = cfg::validate_errors(c.p);
	mark(err.empty(), err.empty() ? std::format("configuration valide ({} module(s))", c.p.modules.size())
								  : "configuration invalide : " + err);

	util::outln("\n\033[1mOutils\033[0m");
	for (auto [tool, use] : std::initializer_list<std::pair<const char*, const char*>>{
			 {"ar", "bibliothèques statiques"}, {"pkg-config", "dépendances pkg-config"},
			 {"ninja", "--backend ninja"}, {"tar", "paquets tar.gz/tar.xz"}, {"zip", "paquets zip"},
			 {"dpkg-deb", "paquets deb"}, {"sha256sum", "sommes de contrôle"},
			 {"rsync", "deploy rsync"}, {"ssh", "deploy rsync/scp"}, {"valgrind", "mode valgrind"},
			 {"qemu-arm", "tests ARM 32 bits"}, {"qemu-aarch64", "tests ARM 64 bits"},
			 {"wine", "tests Windows"}}) {
		bool ok = util::have(tool);
		mark(ok, std::format("{:<12} {}", tool, ok ? use : std::string(use) + " — indisponible"),
			 std::string(tool) == "ar");
	}

	std::vector<std::string> names = {c.p.default_toolchain};
	for (auto& [n, t] : c.p.toolchains) if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
	for (auto& n : names) {
		util::outln("\n\033[1mToolchain {}\033[0m", n);
		try {
			auto t = toolchain::resolve(c.p, n);
			print_toolchain(t, "         ");
			if (t.cc) {
				std::string e;
				bool ok = toolchain::smoke_compile(*t.cc, "c", c.p.c_standard, &e);
				mark(ok, std::format("C   -std={} : compilation + édition de liens", c.p.c_standard));
				if (!ok) util::outln("           {}", toolchain::truncate(e, 3));
			}
			if (t.cxx) {
				std::string e;
				bool ok = toolchain::smoke_compile(*t.cxx, "c++", c.p.cxx_standard, &e);
				mark(ok, std::format("C++ -std={} : compilation + édition de liens", c.p.cxx_standard));
				if (!ok) util::outln("           {}", toolchain::truncate(e, 3));
			}
			if (!t.cross && t.cxx) {
				for (auto san : {"address", "thread", "undefined", "leak", "memory"}) {
					toolchain::Tool tool = *t.cxx;
					tool.pre_args.push_back(std::string("-fsanitize=") + san);
					bool ok = toolchain::smoke_compile(tool, "c++", c.p.cxx_standard);
					mark(ok, std::format("sanitizer {}", san), false);
				}
			}
			if (t.cross) mark(t.can_execute(), t.can_execute() ? "exécution via " + util::join(t.runner, " ")
																: "exécution impossible : " + t.runner_note, false);
		} catch (const std::exception& e) {
			mark(false, e.what());
		}
	}
	auto prefixes = toolchain::detect_prefixes();
	if (!prefixes.empty()) {
		util::outln("\n\033[1mCompilateurs croisés détectés dans le PATH\033[0m (utilisables avec -t <triplet>)");
		for (auto& p : prefixes) util::outln("  {}", p.substr(0, p.size() - 1));
	}
	
	util::outln("\n\033[1mDépendances entre modules\033[0m");
	auto missing = detect_missing_deps(c.p);
	if (missing.empty()) {
		util::outln("  \033[1;32m[ ok ]\033[0m toutes les dépendances déclarées couvrent les #include");
	} else {
		util::outln("  {} dépendance(s) implicite(s) détectée(s) :", missing.size());
		for (auto& [mn, dn] : missing)
			util::outln("    \033[1;33m·\033[0m {:<14} → {}", mn, dn);
		if (o.flag("--check-deps")) {
			util::outln("  \033[2m(--check-deps : rien n'a été modifié)\033[0m");
			problems += (int)missing.size();
		} else if (o.flag("--no-fix")) {
			util::outln("  \033[2m(--no-fix : utilisez 'cpp-maker doctor' pour enregistrer)\033[0m");
			problems += (int)missing.size();
		} else {
			util::outln("  Enregistrement dans les module.toml…");
			int fixed = apply_missing_deps(c.p, missing);
			if (fixed == 0) {
				util::outln("  aucune modification (fichiers non éditables ou déjà à jour)");
				problems += (int)missing.size();
			} else {
				util::outln("  \033[1;32m{} module(s) mis à jour\033[0m "
							"— relancez 'cpp-maker doctor' pour confirmer", fixed);
			}
		}
	}

	if (problems == 0) util::outln("\033[1;32mAucun problème détecté.\033[0m");
	else util::outln("\033[1;31m{} problème(s) détecté(s).\033[0m", problems);
	return problems == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// config show | validate | set
// ---------------------------------------------------------------------------
// cpp-maker config set <clé> <valeur>
// Modifie cpp-maker.toml de manière contrôlée : la clé doit exister, la
// nouvelle configuration est validée AVANT un remplacement atomique du
// fichier (écriture temporaire + rename), donc une valeur invalide ne
// peut jamais corrompre le fichier existant.
int cmd_config_set(const std::vector<std::string>& args) {
	if (args.size() != 3) util::die("usage : cpp-maker config set <clé> <valeur>");
	const std::string& key = args[1];
	const std::string& val = args[2];

	static const std::set<std::string> allowed = {
		"project.name", "project.version", "project.type", "project.standard", "project.c_standard",
		"project.description", "module.type", "compiler.cxx", "compiler.cc", "compiler.family",
		"build.default_mode", "build.default_toolchain", "build.jobs",
	};
	static const std::set<std::string> integers = {"build.jobs"};
	if (!allowed.count(key)) {
		std::vector<std::string> v(allowed.begin(), allowed.end());
		util::die(std::format("clé '{}' non modifiable via 'config set' (autorisées : {})",
							  key, util::join(v, ", ")));
	}

	auto root = find_root(fs::current_path());
	if (!root) util::die("cpp-maker.toml introuvable : lancez 'cpp-maker new' d'abord");
	fs::path file = *root / "cpp-maker.toml";

	std::ifstream in(file);
	if (!in) util::die("impossible de lire " + file.string());
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line)) lines.push_back(line);

	bool replaced = false;
	std::string cur_table;
	for (auto& ln : lines) {
		std::string t = util::trim(ln);
		if (t.empty() || t[0] == '#') continue;
		if (t.front() == '[' && t.find(']') != std::string::npos) {
			cur_table = util::trim(t.substr(1, t.find(']') - 1));
			continue;
		}
		auto eq = t.find('=');
		if (eq == std::string::npos) continue;
		std::string k = util::trim(t.substr(0, eq));
		if (cur_table + "." + k != key) continue;

		std::string newval;
		if (integers.count(key)) {
			if (val.empty() || !std::all_of(val.begin(), val.end(),
											[](unsigned char ch) { return std::isdigit(ch); }))
				util::die(std::format("valeur invalide pour {} : '{}'", key, val));
			newval = val;
		} else {
			std::string esc;
			for (char ch : val) {
				if (ch == '"' || ch == '\\') esc += '\\';
				esc += ch;
			}
			newval = std::format("\"{}\"", esc);
		}
		size_t indent = ln.find_first_not_of(" \t");
		ln = std::format("{}{} = {}", std::string(indent, ' '), k, newval);
		replaced = true;
	}
	if (!replaced) util::die(std::format("clé '{}' introuvable dans {}", key, file.string()));

	std::string content;
	for (auto& l : lines) content += l + "\n";
	fs::path tmp = file;
	tmp += ".tmp";
	util::write_file(tmp, content);
	std::error_code ec;
	try {
		auto p = cfg::load(*root, tmp);
		p.file = file;
		std::string e = cfg::validate_errors(p);
		if (!e.empty()) throw std::runtime_error(e);
	} catch (const std::exception& e) {
		fs::remove(tmp, ec);
		util::die(std::format("valeur refusée, fichier inchangé : {}", e.what()));
	}
	fs::rename(tmp, file, ec);
	if (ec) { fs::remove(tmp, ec); util::die("échec du remplacement de " + file.string()); }
	util::outln("\033[1;32m{} = {}\033[0m (configuration validée)", key, val);
	return 0;
}

int cmd_config_show(const std::vector<std::string>& filters) {
	auto c = load_ctx(false);
	auto& p = c.p;

	// Chaque entrée est (clé pointée, valeur formatée). Scalaires entre
	// guillemets (comme dans cpp-maker.toml), tableaux entre crochets.
	// Les champs optionnels vides sont omis ; les champs structurants
	// (project.*, build.warnings…) sont toujours présents.
	std::vector<std::pair<std::string, std::string>> entries;
	auto add = [&](std::string k, std::string v) {
		entries.emplace_back(std::move(k), std::move(v));
	};
	auto s_ = [](std::string_view x) { return std::format("\"{}\"", x); };
	auto l_ = [](const std::vector<std::string>& v) {
		std::string r = "[";
		for (size_t i = 0; i < v.size(); ++i) r += (i ? ", \"" : "\"") + v[i] + "\"";
		return r + "]";
	};
	auto b_ = [](bool x) { return std::string(x ? "true" : "false"); };

	add("project.name", s_(p.name));
	add("project.version", s_(p.version));
	add("project.description", s_(p.description));
	add("project.standard", s_(p.cxx_standard));
	add("project.c_standard", s_(p.c_standard));
	if (!p.min_version.empty()) add("project.minimum_cpp_maker_version", s_(p.min_version));

	add("build.default_mode", s_(p.default_mode));
	add("build.default_toolchain", s_(p.default_toolchain));
	add("build.jobs", std::to_string(p.jobs));
	add("build.static_pic", b_(p.static_pic));
	add("build.warnings", l_(p.warnings));
	if (!p.cflags.empty())   add("build.cflags", l_(p.cflags));
	if (!p.cxxflags.empty()) add("build.cxxflags", l_(p.cxxflags));
	if (!p.ldflags.empty())  add("build.ldflags", l_(p.ldflags));
	if (!p.defines.empty())  add("build.defines", l_(p.defines));

	for (auto& [n, t] : p.toolchains) {
		const std::string pre = "toolchains." + n + ".";
		if (!t.prefix.empty())     add(pre + "prefix", s_(t.prefix));
		add(pre + "cc", s_(t.cc));
		add(pre + "cxx", s_(t.cxx));
		if (!t.ar.empty())         add(pre + "ar", s_(t.ar));
		if (!t.strip.empty())      add(pre + "strip", s_(t.strip));
		if (!t.pkg_config.empty()) add(pre + "pkg_config", s_(t.pkg_config));
		add(pre + "family", s_(t.family));
		if (!t.target.empty())     add(pre + "target", s_(t.target));
		if (!t.sysroot.empty())    add(pre + "sysroot", s_(t.sysroot));
		if (!t.os.empty())         add(pre + "os", s_(t.os));
		if (!t.cflags.empty())     add(pre + "cflags", l_(t.cflags));
		if (!t.cxxflags.empty())   add(pre + "cxxflags", l_(t.cxxflags));
		if (!t.ldflags.empty())    add(pre + "ldflags", l_(t.ldflags));
		if (t.runner_set)          add(pre + "runner", l_(t.runner));
	}

	for (auto& m : p.modes) {
		const std::string pre = "modes." + m.name + ".";
		add(pre + "flags", l_(m.flags));
		if (!m.link_flags.empty())  add(pre + "link_flags", l_(m.link_flags));
		if (!m.defines.empty())     add(pre + "defines", l_(m.defines));
		if (!m.sanitizer.empty())   add(pre + "sanitizer", s_(m.sanitizer));
		if (!m.runner.empty())      add(pre + "runner", s_(m.runner));
		if (!m.runner_args.empty()) add(pre + "runner_args", l_(m.runner_args));
	}

	add("tests.timeout", std::to_string(p.test_timeout));
	for (auto& [k, v] : p.test_env) add("tests.env." + k, s_(v));

	for (auto& g : p.generate_steps) {
		const std::string pre = "generate." + g.name + ".";
		add(pre + "command", s_(g.command));
		if (!g.inputs.empty())  add(pre + "inputs", l_(g.inputs));
		if (!g.outputs.empty()) add(pre + "outputs", l_(g.outputs));
	}

	if (!p.layer_order.empty()) add("layers.order", l_(p.layer_order));
	for (auto& [layer, forbid] : p.layer_forbid_pkg)
		add("layers." + layer + ".forbid_pkg_config", l_(forbid));

	auto& pk = p.package;
	add("package.formats", l_(pk.formats));
	add("package.strip", b_(pk.strip));
	add("package.maintainer", s_(pk.maintainer));
	if (!pk.description.empty())  add("package.description", s_(pk.description));
	if (!pk.homepage.empty())     add("package.homepage", s_(pk.homepage));
	if (!pk.license.empty())      add("package.license", s_(pk.license));
	if (!pk.deb_depends.empty())  add("package.deb_depends", l_(pk.deb_depends));
	add("package.deb_prefix", s_(pk.deb_prefix));
	if (!pk.extra_files.empty())  add("package.extra_files", l_(pk.extra_files));
	add("package.checksums", b_(pk.checksums));

	for (auto& [n, d] : p.deploys) {
		const std::string pre = "deploy." + n + ".";
		add(pre + "method", s_(d.method));
		if (!d.host.empty())        add(pre + "host", s_(d.host));
		if (!d.user.empty())        add(pre + "user", s_(d.user));
		if (!d.path.empty())        add(pre + "path", s_(d.path));
		if (d.port)                 add(pre + "port", std::to_string(d.port));
		if (!d.toolchain.empty())   add(pre + "toolchain", s_(d.toolchain));
		add(pre + "mode", s_(d.mode));
		add(pre + "artifact", s_(d.artifact));
		if (!d.modules.empty())     add(pre + "modules", l_(d.modules));
		if (!d.ssh_options.empty()) add(pre + "ssh_options", l_(d.ssh_options));
		if (!d.before.empty())      add(pre + "before", l_(d.before));
		if (!d.after.empty())       add(pre + "after", l_(d.after));
		if (!d.after_local.empty()) add(pre + "after_local", l_(d.after_local));
		if (!d.script.empty())      add(pre + "script", s_(d.script));
		if (d.delete_extra)         add(pre + "delete", "true");
	}

	if (p.workspace) add("workspace.modules", l_(p.module_paths));

	for (auto& m : p.modules) {
		const std::string pre = "module." + m.name + ".";
		add(pre + "type", s_(m.type));
		if (!m.version.empty()) add(pre + "version", s_(m.version));
		add(pre + "dir", s_(m.dir.generic_string()));
		add(pre + "source_dirs", l_(m.source_dirs));
		if (!m.source_files.empty()) add(pre + "source_files", l_(m.source_files));
		if (!m.exclude.empty())      add(pre + "exclude", l_(m.exclude));
		if (!m.main.empty())         add(pre + "main", s_(m.main));
		if (!m.layer.empty())        add(pre + "layer", s_(m.layer));
		add(pre + "public_includes", l_(m.public_includes));
		if (m.private_includes != m.source_dirs)
			add(pre + "private_includes", l_(m.private_includes));
		if (!m.deps.empty())         add(pre + "dependencies.modules", l_(m.deps));
		if (!m.pkg_config.empty())   add(pre + "dependencies.pkg_config", l_(m.pkg_config));
		if (!m.system_libs.empty())  add(pre + "dependencies.system", l_(m.system_libs));
		if (!m.defines.empty())         add(pre + "flags.defines", l_(m.defines));
		if (!m.public_defines.empty())  add(pre + "flags.public_defines", l_(m.public_defines));
		if (!m.cflags.empty())          add(pre + "flags.cflags", l_(m.cflags));
		if (!m.cxxflags.empty())        add(pre + "flags.cxxflags", l_(m.cxxflags));
		if (!m.ldflags.empty())         add(pre + "flags.ldflags", l_(m.ldflags));
		add(pre + "install", b_(m.install));
	}

	// Sélection : chaque filtre matche la clé exacte et/ou son préfixe
	// suivi de '.' (build.warnings, toolchains.host, modes.release…).
	std::vector<std::pair<std::string, std::string>> selected;
	std::vector<std::string> unknown;
	if (filters.empty()) {
		selected = entries;
	} else {
		std::set<std::pair<std::string, std::string>> seen;
		for (auto& f : filters) {
			std::string prefix = f.empty() ? f : f + ".";
			bool any = false;
			for (auto& e : entries) {
				if (e.first == f || e.first.starts_with(prefix)) {
					if (seen.insert(e).second) selected.push_back(e);
					any = true;
				}
			}
			if (!any) unknown.push_back(f);
		}
		if (filters.size() > 1)
			std::sort(selected.begin(), selected.end(),
					  [](auto& a, auto& b) { return a.first < b.first; });
	}

	// Suggestion pour les clés inconnues (« nam » → « project.name »).
	for (auto& f : unknown) {
		std::vector<std::string> sugg;
		for (auto& e : entries)
			if (e.first.size() > f.size() && e.first.ends_with("." + f))
				sugg.push_back(e.first);
		util::errln("\033[1;33mwarning:\033[0m clé inconnue : '{}'{}", f,
					sugg.empty() ? "" : " (essayez : " + util::join(sugg, ", ") + ")");
	}

	if (selected.empty()) {
		if (filters.empty()) util::outln("(configuration vide)");
		return filters.empty() ? 0 : 1;
	}

	size_t w = 0;
	for (auto& [k, v] : selected) w = std::max(w, k.size());
	for (auto& [k, v] : selected) util::outln("{:<{}} = {}", k, w, v);

	return unknown.empty() ? 0 : 1;
}

int cmd_config(const std::vector<std::string>& args) {
	if (args.empty()) util::die("usage : cpp-maker config <show [clé…]|validate|set>");
	if (args[0] == "show")
		return cmd_config_show({args.begin() + 1, args.end()});
	if (args[0] == "validate") {
		auto c = load_ctx(false);
		auto e = cfg::validate_errors(c.p);
		if (!e.empty()) { util::errln("\033[1;31m{}\033[0m", e); return 1; }
		util::outln("\033[1;32mConfiguration valide.\033[0m ({} module(s))", c.p.modules.size());
		return 0;
	}
	if (args[0] == "set") return cmd_config_set(args);
	util::die("sous-commande config inconnue : " + args[0]);
}

// ---------------------------------------------------------------------------
// toolchain : profils, détection, gestion des versions gcc/g++
// ---------------------------------------------------------------------------
bool is_debian_like() {
	return fs::exists("/etc/debian_version") && fs::exists("/usr/bin/apt-get");
}

std::vector<int> list_installed_gcc_versions() {
	std::set<int> versions;
	std::vector<fs::path> dirs;
	if (const char* path_env = std::getenv("PATH"))
		for (auto& d : util::split_list(path_env, ':')) dirs.emplace_back(d);
	dirs.emplace_back("/usr/bin");
	for (auto& dir : dirs) {
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) continue;
		for (auto& e : fs::directory_iterator(dir, ec)) {
			const auto name = e.path().filename().string();
			auto parse = [&](std::string_view prefix) -> int {
				if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return -1;
				int v = 0;
				for (char ch : name.substr(prefix.size())) {
					if (!std::isdigit((unsigned char)ch)) return -1;
					v = v * 10 + (ch - '0');
				}
				return v;
			};
			int v = parse("gcc-");
			if (v <= 0) v = parse("g++-");
			if (v > 0) versions.insert(v);
		}
	}
	return {versions.begin(), versions.end()};
}

// Résout /usr/bin/gcc → … → /usr/bin/gcc-N, en suivant les symlinks.
std::optional<int> resolve_gcc_version(const fs::path& link) {
	std::error_code ec;
	if (!fs::exists(link, ec)) return std::nullopt;
	auto canon = fs::canonical(link, ec);
	if (ec) return std::nullopt;
	auto name = canon.filename().string();
	for (auto prefix : {std::string_view("gcc-"), std::string_view("g++-")}) {
		if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
			int v = 0;
			bool ok = true;
			for (char ch : name.substr(prefix.size())) {
				if (!std::isdigit((unsigned char)ch)) { ok = false; break; }
				v = v * 10 + (ch - '0');
			}
			if (ok && v > 0) return v;
		}
	}
	return std::nullopt;
}

int run_as_root_rc(const std::vector<std::string>& argv) {
#ifdef __unix__
	bool is_root = (::geteuid() == 0);
#else
	bool is_root = false;
#endif
	std::vector<std::string> full;
	if (!is_root) {
		if (!util::have("sudo")) {
			util::errln("privilèges root ou 'sudo' requis pour : {}", util::join(argv, " "));
			return 127;
		}
		full.push_back("sudo");
	}
	full.insert(full.end(), argv.begin(), argv.end());
	auto r = proc::capture(full);
	if (r.exit_code != 0 && !r.output.empty()) std::cerr << r.output << '\n';
	return r.exit_code;
}

void run_as_root(const std::vector<std::string>& argv) {
	int rc = run_as_root_rc(argv);
	if (rc != 0) util::die(std::format("commande échouée (code {}) : {}", rc, util::join(argv, " ")));
}

bool confirm(const std::string& question, bool auto_yes) {
	if (auto_yes) return true;
	util::out("{} [y/N] ", question);
	std::cout.flush();
	std::string line;
	if (!std::getline(std::cin, line)) return false;
	return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

int parse_version(const std::string& s) {
	int v = parse_int_arg("version", s);
	if (v <= 0 || v > 99) util::die(std::format("version invalide : '{}'", s));
	return v;
}

// Lit la version majeure réellement utilisée par un compilateur.
std::optional<int> gcc_major_version(const std::string& cxx) {
	auto r = proc::capture({cxx, "-dumpversion"});
	if (r.exit_code != 0) return std::nullopt;
	auto out = util::trim(r.output);
	if (out.empty()) return std::nullopt;
	try { return std::stoi(out); } catch (...) { return std::nullopt; }
}

int cmd_toolchain(const std::vector<std::string>& args) {
	if (args.empty() || args[0] == "list" || args[0] == "ls") {
		std::optional<Project> p;
		if (auto root = find_root(fs::current_path())) {
			try { p = cfg::load(*root, *root / "cpp-maker.toml"); } catch (...) {}
		}
		util::outln("\033[1;36mToolchains du projet\033[0m");
		if (!p || p->toolchains.empty()) util::outln("  (aucun profil — 'host' implicite)");
		else for (auto& [n, t] : p->toolchains) {
			std::string what = !t.prefix.empty() ? "prefix " + t.prefix
							 : std::format("cc {} · cxx {}", t.cc, t.cxx);
			if (!t.target.empty()) what += " · target " + t.target;
			util::outln("  {:<12} {}{}", n, what, p->default_toolchain == n ? "  \033[1;32m(défaut)\033[0m" : "");
		}
		util::outln("\033[1;36mToolchains intégrées\033[0m");
		util::outln("  host         compilateur natif (CC/CXX, gcc, clang…)");
		util::outln("  gcc          gcc / g++");
		util::outln("  clang        clang / clang++");
		auto pre = toolchain::detect_prefixes();
		util::outln("\033[1;36mCompilateurs croisés détectés\033[0m (-t <triplet>)");
		if (pre.empty()) util::outln("  (aucun)");
		for (auto& x : pre) {
			std::string tools;
			for (auto t : {"gcc", "g++"}) if (util::have(x + t)) tools += std::string(tools.empty() ? "" : ", ") + x + t;
			util::outln("  {:<28} {}", x.substr(0, x.size() - 1), tools);
		}

		auto versions = list_installed_gcc_versions();
		auto cur_gcc = resolve_gcc_version("/usr/bin/gcc");
		auto cur_gxx = resolve_gcc_version("/usr/bin/g++");
		util::outln("\033[1;36mVersions gcc/g++ de l'hôte\033[0m");
		if (versions.empty()) util::outln("  (aucune)");
		for (int v : versions)
			util::outln("  gcc/g++-{}{}", v, (cur_gcc == v || cur_gxx == v) ? "  \033[1;32m(défaut)\033[0m" : "");
		if (cur_gcc) util::outln("  /usr/bin/gcc → gcc-{}", *cur_gcc);
		if (cur_gxx) util::outln("  /usr/bin/g++ → g++-{}", *cur_gxx);
		return 0;
	}

	const std::string& sub = args[0];

	if (sub == "show") {
		auto c = load_ctx(false);
		std::string n = args.size() > 1 ? args[1] : c.p.default_toolchain;
		auto t = toolchain::resolve(c.p, n);
		util::outln("\033[1;36mToolchain {}\033[0m", t.name);
		print_toolchain(t);
		util::outln("  pkg-config: {}", t.pkg_config);
		util::outln("  build dir : build/{}/<mode>", t.name);
		return 0;
	}

	if (sub == "which") {
		auto c = load_ctx(false);
		auto t = toolchain::resolve(c.p, args.size() > 1 ? args[1] : c.p.default_toolchain);
		if (t.cxx) util::outln("{}", t.cxx->exe);
		if (t.cc) util::outln("{}", t.cc->exe);
		return 0;
	}

	if (sub == "use") {
		if (args.size() < 2) util::die("usage : cpp-maker toolchain use <version>");
		int v = parse_version(args[1]);
		fs::path gcc = std::format("/usr/bin/gcc-{}", v);
		fs::path gxx = std::format("/usr/bin/g++-{}", v);
		if (!fs::exists(gcc) || !fs::exists(gxx))
			util::die(std::format("gcc-{0}/g++-{0} introuvables — "
								  "installez-les avec `cpp-maker toolchain install {0}`", v));
		util::outln("Configuration de gcc-{0}/g++-{0} comme version par défaut…", v);
		int rc_install = run_as_root_rc({"update-alternatives", "--install", "/usr/bin/gcc", "gcc",
										 gcc.string(), "200", "--slave", "/usr/bin/g++", "g++", gxx.string()});
		if (rc_install != 0) {
			run_as_root({"update-alternatives", "--install", "/usr/bin/gcc", "gcc", gcc.string(), "200"});
			(void)run_as_root_rc({"update-alternatives", "--install", "/usr/bin/g++", "g++", gxx.string(), "200"});
		}
		int rc_set_gcc = run_as_root_rc({"update-alternatives", "--set", "gcc", gcc.string()});
		int rc_set_gxx = run_as_root_rc({"update-alternatives", "--set", "g++", gxx.string()});
		auto used = gcc_major_version("g++");
		if (!used || *used != v) {
			util::errln("\033[1;31mÉchec : la version active reste g++-{}.\033[0m",
						used ? std::to_string(*used) : std::string("?"));
			util::errln("  --set gcc rc = {}, --set g++ rc = {}", rc_set_gcc, rc_set_gxx);
			auto q = proc::capture({"update-alternatives", "--display", "gcc"});
			if (!q.output.empty()) std::cerr << q.output;
			return 1;
		}
		util::outln("\033[1;32mDéfaut mis à jour.\033[0m g++ → g++-{}", v);
		return 0;
	}

	if (sub == "install") {
		if (args.size() < 2) util::die("usage : cpp-maker toolchain install <version|triplet>");
		bool yes = std::any_of(args.begin(), args.end(), [](auto& a) { return a == "-y" || a == "--yes"; });
		if (!is_debian_like())
			util::die("l'installation automatique n'est supportée que sur Debian/Ubuntu (apt)");
		// Compilateur croisé : cpp-maker toolchain install arm-linux-gnueabihf
		if (!std::isdigit((unsigned char)args[1][0])) {
			std::string triple = args[1];
			if (!util::is_safe_name(triple)) util::die("triplet invalide : " + triple);
			std::vector<std::string> pkgs = {"gcc-" + triple, "g++-" + triple};
			if (triple.starts_with("x86_64-w64-mingw32") || triple.starts_with("i686-w64-mingw32"))
				pkgs = {"mingw-w64"};
			if (!confirm(std::format("Installer {} (+ qemu-user) ?", util::join(pkgs, " ")), yes)) return 1;
			run_as_root({"apt-get", "update"});
			std::vector<std::string> cmd = {"apt-get", "install", "-y"};
			cmd.insert(cmd.end(), pkgs.begin(), pkgs.end());
			if (!triple.starts_with("x86_64-w64")) cmd.push_back("qemu-user");
			run_as_root(cmd);
			util::outln("\033[1;32m{} installé.\033[0m Utilisation : cpp-maker build -t {}", triple, triple);
			return 0;
		}
		int v = parse_version(args[1]);
		if (!confirm(std::format("Ajouter le PPA ubuntu-toolchain-r/test (si nécessaire) et installer "
								 "gcc-{0} g++-{0} ?", v), yes))
			return 1;
		if (!util::have("add-apt-repository")) {
			util::outln("Installation de software-properties-common…");
			run_as_root({"apt-get", "install", "-y", "software-properties-common"});
		}
		util::outln("Ajout du PPA ubuntu-toolchain-r/test…");
		run_as_root({"add-apt-repository", "ppa:ubuntu-toolchain-r/test", "-y"});
		util::outln("Mise à jour de l'index apt…");
		run_as_root({"apt-get", "update"});
		util::outln("Installation de gcc-{0} et g++-{0}…", v);
		run_as_root({"apt-get", "install", "-y", std::format("gcc-{}", v), std::format("g++-{}", v)});
		util::outln("\033[1;32mgcc-{0}/g++-{0} installés.\033[0m", v);
		util::outln("Pour les activer par défaut : cpp-maker toolchain use {}", v);
		return 0;
	}

	if (sub == "remove" || sub == "uninstall" || sub == "purge") {
		if (args.size() < 2) util::die("usage : cpp-maker toolchain remove <version>");
		int v = parse_version(args[1]);
		if (!is_debian_like())
			util::die("la suppression automatique n'est supportée que sur Debian/Ubuntu (apt)");
		auto cur_gcc = resolve_gcc_version("/usr/bin/gcc");
		auto cur_gxx = resolve_gcc_version("/usr/bin/g++");
		if (cur_gcc == v || cur_gxx == v)
			util::die(std::format("gcc-{0}/g++-{0} est actuellement le défaut. "
								  "Changez d'abord avec `cpp-maker toolchain use <autre>`", v));
		bool yes = std::any_of(args.begin(), args.end(), [](auto& a) { return a == "-y" || a == "--yes"; });
		if (!confirm(std::format("Purger gcc-{0} et g++-{0} (avec --autoremove) ?", v), yes)) return 1;
		run_as_root({"apt-get", "purge", "--autoremove", "-y", std::format("gcc-{}", v), std::format("g++-{}", v)});
		util::outln("\033[1;32mgcc-{0}/g++-{0} supprimés.\033[0m", v);
		return 0;
	}

	util::die("sous-commande toolchain inconnue : " + sub +
			  " (attendu : list | show | which | use | install | remove)");
}

} // namespace commands