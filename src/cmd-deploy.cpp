#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

namespace commands {

// ---------------------------------------------------------------------------
// package / install / deploy
// ---------------------------------------------------------------------------

// Le mode produit-il des informations de débogage ? (strip inutile sinon)
bool mode_has_debug(const ModeConfig& m) {
	return std::any_of(m.flags.begin(), m.flags.end(), [](auto& f) { return f.starts_with("-g"); });
}

void copy_into(const fs::path& src, const fs::path& dst_dir, std::vector<fs::path>& installed) {
	std::error_code ec;
	fs::create_directories(dst_dir, ec);
	if (ec) util::die(std::format("impossible de créer {} : {}", dst_dir.string(), ec.message()));
	fs::path dst = dst_dir / src.filename();
	if (fs::is_directory(src, ec)) {
		fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		if (ec) util::die(std::format("copie de {} → {} : {}", src.string(), dst.string(), ec.message()));
		for (auto& e : fs::recursive_directory_iterator(dst, ec))
			if (e.is_regular_file()) installed.push_back(e.path());
	} else {
		fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
		if (ec) util::die(std::format("copie de {} → {} : {}", src.string(), dst.string(), ec.message()));
		installed.push_back(dst);
	}
}

// Contenu d'un répertoire d'en-têtes copié dans include/.
void copy_headers(const fs::path& inc_dir, const fs::path& dst, std::vector<fs::path>& installed) {
	std::error_code ec;
	if (!fs::is_directory(inc_dir, ec)) return;
	for (auto& e : fs::directory_iterator(inc_dir, ec)) copy_into(e.path(), dst, installed);
}

std::string pc_file(const Ctx& c, const ModuleConfig& m, const build::ModulePlan& mp) {
	std::string s = "prefix=${pcfiledir}/../..\nexec_prefix=${prefix}\n"
					"libdir=${prefix}/lib\nincludedir=${prefix}/include\n\n";
	s += "Name: " + m.name + "\n";
	s += "Description: " + (c.p.package.description.empty() ? m.name + " (" + c.p.name + ")"
															 : c.p.package.description) + "\n";
	s += "Version: " + (m.version.empty() ? c.p.version : m.version) + "\n";
	std::vector<std::string> req = m.pkg_config;
	for (auto& d : m.deps) req.push_back(d);
	if (!req.empty()) s += "Requires: " + util::join(req, ", ") + "\n";
	if (m.type != "header-only") s += "Libs: -L${libdir} -l" + m.name + "\n";
	std::vector<std::string> priv;
	for (auto& l : m.system_libs) priv.push_back(l.starts_with("-") ? l : "-l" + l);
	if (!priv.empty()) s += "Libs.private: " + util::join(priv, " ") + "\n";
	std::string cflags = "-I${includedir}";
	for (auto& d : m.public_defines) cflags += " -D" + d;
	s += "Cflags: " + cflags + "\n";
	(void)mp;
	return s;
}

// Copie les artefacts installables dans dest (bin/ lib/ include/ share/).
std::vector<fs::path> stage(const Ctx& c, const Session& s, const fs::path& dest, bool strip) {
	std::vector<fs::path> installed;
	const auto& t = *s.tc;
	std::vector<fs::path> to_strip;
	for (auto& n : s.plan->order) {
		auto& mp = s.plan->mods.at(n);
		const ModuleConfig& m = *mp.cfg;
		if (!m.install) continue;
		bool selected = std::find(s.plan->selected.begin(), s.plan->selected.end(), n) != s.plan->selected.end();
		if (!selected) {
			// Dépendance tirée par un module demandé : seul le runtime est utile.
			if (m.type == "shared") {
				copy_into(mp.artifact, dest / (t.os == "windows" ? "bin" : "lib"), installed);
				to_strip.push_back(installed.back());
			}
			continue;
		}
		if (m.type == "executable") {
			copy_into(mp.artifact, dest / "bin", installed);
			to_strip.push_back(installed.back());
			continue;
		}
		if (m.type == "shared") {
			copy_into(mp.artifact, dest / (t.os == "windows" ? "bin" : "lib"), installed);
			to_strip.push_back(installed.back());
			if (!mp.import_lib.empty()) copy_into(mp.import_lib, dest / "lib", installed);
		} else if (m.type == "static") {
			copy_into(mp.artifact, dest / "lib", installed);
		}
		for (auto& inc : m.public_includes) copy_headers(util::under(m.dir, inc), dest / "include", installed);
		fs::path pc = dest / "lib" / "pkgconfig" / (m.name + ".pc");
		util::write_file(pc, pc_file(c, m, mp));
		installed.push_back(pc);
	}
	std::error_code ec;
	for (auto& e : fs::directory_iterator(".", ec)) {
		auto fn = e.path().filename().string();
		auto up = fn;
		for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
		if (e.is_regular_file() && (up.starts_with("README") || up.starts_with("LICENSE") ||
									up.starts_with("COPYING") || up.starts_with("CHANGELOG")))
			copy_into(e.path(), dest / "share" / "doc" / c.p.name, installed);
	}
	for (auto& f : c.p.package.extra_files) {
		if (!fs::exists(f, ec)) util::die("package.extra_files : introuvable : " + f);
		copy_into(f, dest / "share" / c.p.name, installed);
	}
	if (strip && !to_strip.empty()) {
		if (!util::have(t.strip)) {
			util::warn("{} introuvable : binaires non strippés", t.strip);
		} else {
			for (auto& f : to_strip) {
				bool lib = f.extension() == ".so" || f.extension() == ".dll" || f.extension() == ".dylib";
				std::vector<std::string> cmd = {t.strip};
				if (lib) cmd.push_back("--strip-unneeded");
				cmd.push_back(f.string());
				auto r = proc::capture(cmd);
				if (r.exit_code != 0) util::warn("strip {} : {}", f.string(), util::trim(r.output));
			}
		}
	}
	return installed;
}

std::string package_basename(const Ctx& c, const toolchain::Toolchain& t) {
	return std::format("{}-{}-{}-{}", c.p.name, c.p.version, t.os, t.arch);
}

void run_checked(const std::vector<std::string>& cmd, bool verbose,
						const std::optional<fs::path>& cwd = std::nullopt) {
	if (verbose) util::outln("      $ {}{}", cwd ? "(cd " + cwd->string() + ") " : "", util::shell_join(cmd));
	auto r = proc::run(cmd, {.capture = true, .cwd = cwd});
	if (r.exit_code != 0)
		util::die(std::format("commande en échec (code {}) : {}\n{}", r.exit_code,
							  util::shell_join(cmd), util::trim(r.output)));
}

std::string human_size(uintmax_t n) {
	if (n >= 1024 * 1024) return std::format("{:.1f} Mio", (double)n / (1024.0 * 1024.0));
	if (n >= 1024) return std::format("{:.1f} Kio", (double)n / 1024.0);
	return std::format("{} o", n);
}

std::string deb_name(const Project& p) {
	std::string pkg;
	for (char ch : util::lower(p.name)) pkg += (std::isalnum((unsigned char)ch) || ch == '.' || ch == '+') ? ch : '-';
	return pkg;
}
std::string deb_filename(const Project& p, const toolchain::Toolchain& t) {
	return std::format("{}_{}_{}.deb", deb_name(p), p.version, toolchain::deb_arch(t));
}

// Construit un paquet .deb ; retourne son chemin.
fs::path make_deb(const Ctx& c, const Session& s, const fs::path& dist, bool strip, bool verbose) {
	if (!util::have("dpkg-deb")) util::die("dpkg-deb introuvable : format deb indisponible");
	auto& p = c.p;
	if (p.version.empty() || !std::isdigit((unsigned char)p.version[0]))
		util::die("format deb : project.version doit commencer par un chiffre");
	std::string pkg = deb_name(p);
	std::string arch = toolchain::deb_arch(*s.tc);
	fs::path root = s.plan->build_dir / "deb" / std::format("{}_{}_{}", pkg, p.version, arch);
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::path prefix = fs::path(p.package.deb_prefix).relative_path();
	auto files = stage(c, s, root / prefix, strip);
	uintmax_t kb = 0;
	for (auto& f : files) kb += fs::file_size(f, ec) / 1024 + 1;
	std::string control = std::format(
		"Package: {}\nVersion: {}\nArchitecture: {}\nMaintainer: {}\nInstalled-Size: {}\n"
		"Section: misc\nPriority: optional\n", pkg, p.version, arch, p.package.maintainer, kb);
	if (!p.package.deb_depends.empty()) control += "Depends: " + util::join(p.package.deb_depends, ", ") + "\n";
	if (!p.package.homepage.empty()) control += "Homepage: " + p.package.homepage + "\n";
	std::string desc = p.package.description.empty() ? p.name : p.package.description;
	control += "Description: " + desc + "\n";
	util::write_file(root / "DEBIAN" / "control", control);
	fs::permissions(root / "DEBIAN", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
					fs::perms::others_read | fs::perms::others_exec, ec);
	fs::path out = fs::absolute(dist / deb_filename(p, *s.tc));
	run_checked({"dpkg-deb", "--build", "--root-owner-group", root.string(), out.string()}, verbose);
	return out;
}

// Produit les archives demandées ; retourne les fichiers créés.
std::vector<fs::path> make_packages(const Ctx& c, const Session& s,
										   const std::vector<std::string>& formats,
										   bool strip, bool verbose, fs::path* stage_out = nullptr) {
	std::string base = package_basename(c, *s.tc);
	fs::path stage_parent = s.plan->build_dir / "stage";
	fs::path stage_dir = stage_parent / base;
	std::error_code ec;
	fs::remove_all(stage_dir, ec);
	fs::create_directories(stage_dir, ec);
	auto files = stage(c, s, stage_dir, strip);
	util::outln("\033[1;34m[cpp-maker]\033[0m staging : {} ({} fichier(s))", stage_dir.string(), files.size());
	if (stage_out) *stage_out = stage_dir;

	fs::path dist = fs::absolute("dist");
	fs::create_directories(dist, ec);
	std::vector<fs::path> out;
	for (auto& f : formats) {
		if (f == "tar.gz" || f == "tar.xz") {
			if (!util::have("tar")) util::die("tar introuvable");
			fs::path a = dist / (base + "." + f);
			run_checked({"tar", "-C", stage_parent.string(), f == "tar.gz" ? "-czf" : "-cJf",
						 a.string(), base}, verbose);
			out.push_back(a);
		} else if (f == "zip") {
			if (!util::have("zip")) util::die("zip introuvable : format zip indisponible");
			fs::path a = dist / (base + ".zip");
			fs::remove(a, ec);
			run_checked({"zip", "-qry", a.string(), base}, verbose, fs::absolute(stage_parent));
			out.push_back(a);
		} else if (f == "deb") {
			out.push_back(make_deb(c, s, dist, strip, verbose));
		} else if (f != "stage") {
			util::die("format de paquet inconnu : " + f);
		}
	}
	if (c.p.package.checksums && !out.empty() && util::have("sha256sum")) {
		std::vector<std::string> cmd = {"sha256sum"};
		for (auto& f : out) cmd.push_back(f.filename().string());
		auto r = proc::run(cmd, {.capture = true, .cwd = dist});
		if (r.exit_code == 0) {
			fs::path sums = dist / (base + ".sha256");
			util::write_file(sums, r.output);
			out.push_back(sums);
		}
	}
	return out;
}

int cmd_package(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--format"}, {"--no-strip"});
	mode_from_positional(o);
	auto c = load_ctx();
	std::string mode = pick_mode(c, o, "release");
	auto formats = o.values.count("--format") ? util::split_list(o.value("--format")) : c.p.package.formats;
	for (auto& f : formats) {
		static const std::set<std::string> ok = {"tar.gz", "tar.xz", "zip", "deb", "stage"};
		if (!ok.count(f)) util::die("format inconnu : " + f + " (tar.gz|tar.xz|zip|deb|stage)");
	}
	auto mods = select_modules(c, o);
	for (auto& tcn : pick_toolchains(c, o)) {
		auto s = prepare(c, o, tcn, mode, mods, false);
		if (run_plan(c, s, o) != 0) return 1;
		if (o.dry_run) continue;
		bool strip = c.p.package.strip && !o.flag("--no-strip") && !mode_has_debug(*s.mode);
		auto files = make_packages(c, s, formats, strip, o.verbose);
		for (auto& f : files) {
			std::error_code ec;
			util::outln("  \033[1;32m📦\033[0m {:<56} {}", fs::relative(f, c.p.root, ec).string(),
						human_size(fs::file_size(f, ec)));
		}
	}
	return 0;
}

int cmd_install(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {"--prefix", "--destdir"}, {"--no-strip"});
	mode_from_positional(o);
	help::require_no_positional(o, "install");
	help::require_no_passthrough(o, "install");

	auto c = load_ctx();
	std::string mode = pick_mode(c, o, "release");
	std::string tc   = help::require_single_toolchain(c, o, {}, "install");

	fs::path prefix = o.value("--prefix", "/usr/local");
	fs::path target = o.values.count("--destdir")
					? fs::path(o.value("--destdir")) / prefix.relative_path()
					: prefix;

	return help::with_session(c, o, tc, mode, select_modules(c, o), false,
		[&](Session& s) {
			bool strip = c.p.package.strip && !o.flag("--no-strip")
						&& !mode_has_debug(*s.mode);
			auto files = stage(c, s, target, strip);
			std::string manifest;
			for (auto& f : files) { util::outln("  installé : {}", f.string()); manifest += f.string() + "\n"; }
			util::write_file(s.plan->build_dir / "install_manifest.txt", manifest);
			util::outln("\033[1;32m{} fichier(s) installé(s) dans {}\033[0m", files.size(), target.string());
			return 0;
		});
}

std::string expand(std::string s, const std::map<std::string, std::string>& vars) {
	for (auto& [k, v] : vars) s = util::replace_all(s, "{" + k + "}", v);
	return s;
}

int cmd_deploy(const std::vector<std::string>& args) {
	auto o = parse_opts(args, {}, {"--list"});
	auto c = load_ctx();
	if (o.flag("--list") || o.positional.empty()) {
		if (c.p.deploys.empty()) {
			util::outln("Aucune cible de déploiement : ajoutez une table [deploy.<nom>] dans cpp-maker.toml");
			return o.positional.empty() && !o.flag("--list") ? 1 : 0;
		}
		util::outln("\033[1;36mCibles de déploiement\033[0m");
		for (auto& [n, d] : c.p.deploys)
			util::outln("  {:<12} {} → {}{}{} ({} · {} · {})", n, d.method,
						d.user.empty() ? "" : d.user + "@", d.host.empty() ? "" : d.host + ":", d.path,
						d.toolchain.empty() ? c.p.default_toolchain : d.toolchain, d.mode, d.artifact);
		return 0;
	}
	std::string name = o.positional[0];
	auto it = c.p.deploys.find(name);
	if (it == c.p.deploys.end()) util::die("cible de déploiement inconnue : " + name);
	DeployConfig d = it->second;
	std::string mode = pick_mode(c, o, d.mode);
	auto tcs = pick_toolchains(c, o, d.toolchain);
	if (tcs.size() != 1) util::die("deploy : une seule toolchain à la fois");
	Opts so = o;
	if (so.modules.empty()) { so.modules = d.modules; if (so.modules.empty()) so.all = true; }
	auto s = prepare(c, so, tcs[0], mode, select_modules(c, so), false);

	if (run_plan(c, s, o) != 0) return 1;

	std::string target = (d.user.empty() ? "" : d.user + "@") + d.host;
	std::vector<std::string> ssh = {"ssh"};
	if (d.port) { ssh.push_back("-p"); ssh.push_back(std::to_string(d.port)); }
	ssh.insert(ssh.end(), d.ssh_options.begin(), d.ssh_options.end());

	fs::path stage_dir;
	std::vector<fs::path> packages;
	std::map<std::string, std::string> vars = {
		{"name", c.p.name}, {"version", c.p.version},
		{"toolchain", s.tc->name}, {"mode", mode}, {"host", d.host}, {"artifact", ""}};
	d.path = expand(d.path, vars);
	vars["path"] = d.path;

	auto exec = [&](const std::vector<std::string>& cmd) {
		util::outln("  \033[1;35m$\033[0m {}", util::shell_join(cmd));
		if (o.dry_run) return;
		auto r = proc::run(cmd);
		if (r.exit_code != 0) util::die(std::format("déploiement interrompu (code {})", r.exit_code));
	};

	util::outln("\033[1;34m[cpp-maker]\033[0m déploiement '{}' : {} → {}{}", name, d.method,
				target.empty() ? "" : target + ":", d.path);
	bool strip = c.p.package.strip && !mode_has_debug(*s.mode);
	if (!o.dry_run) {
		std::vector<std::string> fmts;
		if (d.artifact != "stage") fmts.push_back(d.artifact);
		packages = make_packages(c, s, fmts, strip, o.verbose, &stage_dir);
		std::erase_if(packages, [](auto& f) { return f.extension() == ".sha256"; });
	} else {
		stage_dir = s.plan->build_dir / "stage" / package_basename(c, *s.tc);
		if (d.artifact == "deb") packages.push_back("dist/" + deb_filename(c.p, *s.tc));
		else if (d.artifact != "stage") packages.push_back("dist/" + package_basename(c, *s.tc) + "." + d.artifact);
	}
	if (!packages.empty()) vars["artifact"] = d.path + "/" + packages[0].filename().string();

	for (auto& b : d.before) exec({"sh", "-c", expand(b, vars)});

	// Éléments à transférer : contenu du staging, ou le paquet.
	std::vector<fs::path> items;
	if (d.artifact == "stage") {
		std::error_code ec;
		if (!o.dry_run)
			for (auto& e : fs::directory_iterator(stage_dir, ec)) items.push_back(e.path());
		else items.push_back(stage_dir / "*");
	} else {
		items = packages;
	}

	if (d.method == "script") {
		// v0.4 : script local recevant le staging dans $CPP_MAKER_DIST.
		exec({"sh", "-c", "CPP_MAKER_DIST=" + util::shell_quote(fs::absolute(stage_dir).string()) +
						  "; export CPP_MAKER_DIST; " + expand(d.script, vars)});
	} else if (d.method == "copy") {
		if (!o.dry_run) {
			std::vector<fs::path> done;
			for (auto& i : items) copy_into(i, d.path, done);
			util::outln("  {} fichier(s) copié(s) dans {}", done.size(), d.path);
		} else {
			for (auto& i : items) util::outln("  [dry-run] copie {} → {}", i.string(), d.path);
		}
		for (auto& a : d.after) exec({"sh", "-c", expand(a, vars)});
	} else {
		auto mk = ssh;
		mk.push_back(target);
		mk.push_back("mkdir -p " + util::shell_quote(d.path));
		exec(mk);
		if (d.method == "rsync") {
			std::vector<std::string> cmd = {"rsync", "-az"};
			if (d.delete_extra) cmd.push_back("--delete");
			if (o.verbose) cmd.push_back("-v");
			cmd.push_back("-e");
			cmd.push_back(util::shell_join(ssh));
			if (d.artifact == "stage") cmd.push_back(stage_dir.string() + "/");
			else for (auto& i : items) cmd.push_back(i.string());
			cmd.push_back(target + ":" + d.path + "/");
			exec(cmd);
		} else { // scp
			std::vector<std::string> cmd = {"scp", "-r"};
			if (d.port) { cmd.push_back("-P"); cmd.push_back(std::to_string(d.port)); }
			cmd.insert(cmd.end(), d.ssh_options.begin(), d.ssh_options.end());
			for (auto& i : items) cmd.push_back(i.string());
			cmd.push_back(target + ":" + d.path + "/");
			exec(cmd);
		}
		for (auto& a : d.after) {
			auto cmd = ssh;
			cmd.push_back(target);
			cmd.push_back(expand(a, vars));
			exec(cmd);
		}
	}
	for (auto& a : d.after_local) exec({"sh", "-c", expand(a, vars)});
	util::outln("\033[1;32m✓ déploiement '{}' {}\033[0m", name, o.dry_run ? "simulé (dry-run)" : "terminé");
	return 0;
}

} // namespace commands