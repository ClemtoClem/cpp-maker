// cmd-backup.cpp — export du code d'un module dans un fichier XML.
#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

namespace commands {

namespace {

// Encodage CDATA sûr. Un `]]>` est splitté en `]]` + `]]><![CDATA[>` pour
// ne pas terminer le bloc prématurément (norme XML 1.0, §2.7). Les octets
// de contrôle autres que \t \n \r sont retirés : la sortie reste valide
// même si un fichier contient des octets binaires accidentels.
std::string xml_cdata(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 32);
    out += "<![CDATA[";
    auto emit_bytes = [&](size_t from, size_t to) {
        for (size_t k = from; k < to && k < s.size(); ++k) {
            unsigned char c = static_cast<unsigned char>(s[k]);
            if (c == 0x09 || c == 0x0A || c == 0x0D || c >= 0x20)
                out += static_cast<char>(c);
        }
    };
    size_t i = 0;
    while (i < s.size()) {
        auto pos = s.find("]]>", i);
        if (pos == std::string_view::npos) { emit_bytes(i, s.size()); break; }
        emit_bytes(i, pos + 2);
        out += "]]><![CDATA[>";
        i = pos + 3;
    }
    out += "]]>";
    return out;
}

bool is_header_ext(std::string_view ext) {
    return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" ||
           ext == ".h++" || ext == ".ipp" || ext == ".tpp" || ext == ".inl";
}

// En-têtes d'un module : parcourt public_includes + private_includes.
std::vector<fs::path> discover_headers(const ModuleConfig& m) {
    std::vector<fs::path> out;
    std::set<fs::path> seen;
    std::vector<std::string> dirs = m.public_includes;
    dirs.insert(dirs.end(), m.private_includes.begin(), m.private_includes.end());
    for (auto& d : dirs) {
        fs::path root = util::under(m.dir, d);
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (auto& e : fs::recursive_directory_iterator(root,
                    fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (!is_header_ext(e.path().extension().string())) continue;
            auto p = e.path().lexically_normal();
            if (seen.insert(p).second) out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Chemin affiché : relatif au dossier du module, préfixé par `./`
// sauf s'il est absolu.
std::string display_path(const ModuleConfig& m, const fs::path& p) {
    fs::path rel = p;
    if (!m.dir.empty() && m.dir != ".") {
        auto r = p.lexically_relative(m.dir);
        if (!r.empty() && !r.generic_string().starts_with(".."))
            rel = r;
    }
    std::string s = rel.generic_string();
    if (!s.starts_with("/") && !s.starts_with("./")) s = "./" + s;
    return s;
}

std::string module_xml(const Project&, const ModuleConfig& m, const Opts& o,
                       std::string_view indent, size_t& file_count) {
    file_count = 0;
    std::string in(indent);
    std::string in1 = in + "  ";
    std::string in2 = in + "    ";

    std::string x = in + "<module";
    x += " name=\"" + util::xml_escape(m.name) + "\"";
    if (!m.type.empty())        x += " type=\"" + util::xml_escape(m.type) + "\"";
    if (!m.version.empty())     x += " version=\"" + util::xml_escape(m.version) + "\"";
    if (!m.layer.empty())       x += " layer=\"" + util::xml_escape(m.layer) + "\"";
    if (!m.dir.empty())         x += " dir=\"" + util::xml_escape(m.dir.generic_string()) + "\"";
    if (!m.description.empty()) x += " description=\"" + util::xml_escape(m.description) + "\"";
    x += ">\n";

    auto emit = [&](const fs::path& path, std::string_view role) {
        std::string ext = path.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        if (ext.empty()) ext = "file";
        std::string content;
        if (!util::read_file(path, content)) {
            util::warn("backup : impossible de lire {}", path.string());
            return;
        }
        x += in1 + "<file type=\"" + util::xml_escape(ext) + "\"";
        x += " role=\"" + std::string(role) + "\"";
        x += " path=\"" + util::xml_escape(display_path(m, path)) + "\"";
        x += " size=\"" + std::to_string(content.size()) + "\">\n";
        x += in2 + "<content>" + xml_cdata(content) + "</content>\n";
        x += in1 + "</file>\n";
        ++file_count;
    };

    std::set<fs::path> emitted;
    if (!o.flag("--no-sources")) {
        std::vector<fs::path> srcs;
        for (auto& s : build::discover(m, m.source_dirs)) srcs.push_back(s.path);
        for (auto& f : m.source_files) {
            auto p2 = util::under(m.dir, f);
            if (std::find(srcs.begin(), srcs.end(), p2) == srcs.end())
                srcs.push_back(p2);
        }
        std::sort(srcs.begin(), srcs.end());
        srcs.erase(std::unique(srcs.begin(), srcs.end()), srcs.end());
        for (auto& s : srcs) if (emitted.insert(s).second) emit(s, "source");
    }
    if (!o.flag("--no-headers"))
        for (auto& h : discover_headers(m)) if (emitted.insert(h).second) emit(h, "header");
    if (!o.flag("--no-tests")) {
        std::vector<fs::path> ts;
        for (auto& s : build::discover(m, m.test_dirs)) ts.push_back(s.path);
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
        for (auto& t : ts) if (emitted.insert(t).second) emit(t, "test");
    }
    x += in + "</module>\n";
    return x;
}

} // namespace

int cmd_backup(const std::vector<std::string>& args) {
    // --file : nouveau nom canonique. --out est accepté comme alias
    // (rétro-compat) mais --stdout n'a PLUS le sens « écrire sur stdout » :
    // c'est désormais l'option globale. Pour l'ancien comportement,
    // utiliser --file -.
    auto o = parse_opts(args, {"--file", "--out"}, {"--no-sources", "--no-headers", "--no-tests", "--to-stdout"});
    help::require_no_passthrough(o, "backup");

    if (o.modules.empty() && o.positional.size() == 1) {
        o.modules.push_back(o.positional[0]);
        o.positional.clear();
    }
    help::require_no_positional(o, "backup");

    auto c = load_ctx();
    auto mods = select_modules(c, o);

    auto get_opt = [&](std::initializer_list<const char*> keys) {
        for (auto* k : keys) {
            auto it = o.values.find(k);
            if (it != o.values.end()) return it->second;
        }
        return std::string{};
    };
    std::string file_opt = get_opt({"--file", "--out"});
    bool to_stdout = o.flag("--to-stdout") || file_opt == "-";
    // --stdout/-stderr/-output restent globaux : si l'utilisateur a fait
    // `cpp-maker backup --stdout foo.xml`, output.cpp a déjà redirigé
    // stdout vers .out/foo.xml ; notre écriture sur std::cout y atterrira.
    // Ce chemin est légitime : on l'accepte, sans rebaptiser l'option.

    // Résolution du chemin de sortie par défaut :
    //   - si --file donné (≠ "-") : tel quel (respecte chemin absolu ou relatif)
    //   - si stdout demandé          : stdout
    //   - sinon                      : <backups_dir>/backup-<module>.xml
    fs::path out_path;
    if (!file_opt.empty() && file_opt != "-") {
        out_path = fs::path(file_opt);
    } else if (to_stdout) {
        // rien, on écrit sur std::cout (déjà redirigé si --stdout global)
    } else {
        std::string name = mods.size() == 1 ? "backup-" + mods[0] + ".xml" : "backup.xml";
        out_path = fs::path(c.p.backups_dir) / name;
    }

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    size_t total_files = 0;
    if (mods.size() == 1) {
        auto m = c.p.find_module(mods[0]);
        if (!m) util::die("module inconnu : " + mods[0]);
        xml += module_xml(c.p, *m, o, "", total_files);
    } else {
        xml += "<backup version=\"1\" project=\"" + util::xml_escape(c.p.name) + "\">\n";
        for (auto& name : mods) {
            auto m = c.p.find_module(name);
            if (!m) { util::warn("backup : module inconnu {}", name); continue; }
            size_t fc = 0;
            xml += module_xml(c.p, *m, o, "  ", fc);
            total_files += fc;
        }
        xml += "</backup>\n";
    }

    if (to_stdout) {
        // std::cout est lui-même éventuellement redirigé par --stdout global.
        std::cout << xml;
        std::cout.flush();
        // Diagnostic : sur stderr d'origine (pas de fichier), comme le
        // résumé de --output.
        util::errln("\033[2mbackup : {} module(s), {} fichier(s) sur stdout\033[0m",
                    mods.size(), total_files);
        return 0;
    }
    // Crée le répertoire cible (backups_dir ou chemin explicite).
    if (out_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
    }
    util::write_file(out_path, xml);
    std::error_code ec;
    auto sz = fs::file_size(out_path, ec);
    util::outln("\033[1;32m✓ backup écrit :\033[0m {} ({} module(s), {} fichier(s), {})",
                out_path.generic_string(), mods.size(), total_files,
                ec ? "taille inconnue" : std::format("{} Kio", sz / 1024));
    return 0;
}

} // namespace commands