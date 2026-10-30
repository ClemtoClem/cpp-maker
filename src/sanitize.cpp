// sanitize.cpp — catalogue des sanitizers et options --sanitize*.
//
// Les sanitizers demandés en ligne de commande s'ajoutent à ceux du mode
// choisi. Le résultat est un mode dérivé (« debug+address+undefined ») qui a
// son propre dossier build/<tc>/<mode>/, pour ne pas écraser les objets du
// mode d'origine.
#include "cpp-maker.hpp"

namespace sanitize {

using commands::Opts;

const std::vector<Info>& catalog() {
	static const std::vector<Info> c = {
		// --- Runtimes complets (exclusifs entre eux) ------------------------
		{"address", "AddressSanitizer (ASan)",
		 "Détecte les accès mémoire invalides : débordements de tas, de pile et de "
		 "globales, use-after-free, use-after-return, double free. Inclut la "
		 "détection de fuites (LSan) sous Linux. Coût : ~2x en temps, ~3x en mémoire.",
		 false, true},
		{"thread", "ThreadSanitizer (TSan)",
		 "Détecte les accès concurrents non synchronisés (data races) et certains "
		 "interblocages. Coût : 5 à 15x en temps, 5 à 10x en mémoire.",
		 false, true},
		{"memory", "MemorySanitizer (MSan) — clang uniquement",
		 "Détecte les lectures de mémoire non initialisée. Tout le code doit être "
		 "instrumenté (y compris la bibliothèque standard C++ : libc++ compilée avec "
		 "MSan) sinon des faux positifs apparaissent. L'origine des valeurs est "
		 "suivie (-fsanitize-memory-track-origins=2).",
		 true, true},
		{"leak", "LeakSanitizer (LSan)",
		 "Détecte les fuites mémoire à la fin du programme, sans le surcoût d'ASan. "
		 "Déjà inclus dans 'address' ; combinable seulement avec 'address'.",
		 false, true},
		{"hwaddress", "HWASan — AddressSanitizer assisté par le matériel",
		 "Version alternative d'ASan utilisant le marquage des pointeurs (Top Byte "
		 "Ignore). Surtout AArch64/Android ; consomme beaucoup moins de mémoire qu'ASan.",
		 false, true},
		{"safe-stack", "SafeStack — clang uniquement",
		 "Sépare la pile en deux : une pile sûre (adresse de retour, variables "
		 "locales critiques) et une pile non sûre (tableaux, tampons sujets aux "
		 "débordements). Protection de production, faible coût.",
		 true, true},
		// --- UndefinedBehaviorSanitizer (combinables entre eux et avec un runtime)
		{"undefined", "UBSan — groupe général des comportements indéfinis",
		 "Active shift, integer-divide-by-zero, null, signed-integer-overflow, "
		 "bounds, alignment, vptr, et d'autres (unreachable, return, vla-bound…). "
		 "N'inclut ni float-divide-by-zero ni unsigned-integer-overflow.",
		 false, false},
		{"shift", "UBSan",
		 "Détecte les décalages de bits invalides : décalage négatif ou de plus de "
		 "bits que la taille du type, décalage d'une valeur signée négative.",
		 false, false},
		{"integer-divide-by-zero", "UBSan",
		 "Détecte les divisions (et modulos) d'entiers par zéro.", false, false},
		{"float-divide-by-zero", "UBSan",
		 "Détecte les divisions de flottants par zéro. Non inclus dans 'undefined' : "
		 "le résultat (inf/NaN) est défini par IEEE 754.",
		 false, false},
		{"null", "UBSan",
		 "Détecte l'utilisation ou le déréférencement d'un pointeur nul, et la "
		 "création d'une référence nulle.", false, false},
		{"signed-integer-overflow", "UBSan",
		 "Détecte les dépassements de capacité des entiers signés (+, -, *, ++…).",
		 false, false},
		{"unsigned-integer-overflow", "UBSan — clang uniquement",
		 "Détecte les dépassements des entiers non signés. Ce n'est PAS un "
		 "comportement indéfini (arithmétique modulo 2^n) : optionnel, utile pour "
		 "traquer les bugs logiques ; attention aux hachages qui en dépendent.",
		 true, false},
		{"bounds", "UBSan",
		 "Détecte les accès hors limites d'un tableau quand la taille est connue "
		 "à la compilation (tableaux statiques, VLA).", false, false},
		{"alignment", "UBSan",
		 "Détecte l'utilisation de pointeurs mal alignés pour leur type.",
		 false, false},
		{"vptr", "UBSan — C++ uniquement",
		 "Vérifie que le type dynamique d'un objet correspond au type attendu lors "
		 "d'appels de méthodes virtuelles, de dynamic_cast et d'accès aux membres. "
		 "Nécessite le RTTI.", false, false},
		// --- Protections de production --------------------------------------
		{"cfi", "Control Flow Integrity — clang uniquement",
		 "Protège contre la corruption du flot de contrôle en vérifiant la validité "
		 "des cibles d'appels indirects (fonctions virtuelles, pointeurs de "
		 "fonctions, casts). Ajoute automatiquement -flto et -fvisibility=hidden "
		 "(édition de liens avec lld ou gold requise).",
		 true, false},
		{"shadow-call-stack", "ShadowCallStack",
		 "Sauvegarde l'adresse de retour dans une pile dédiée (registre x18) pour "
		 "la protéger contre l'écrasement. AArch64 et RISC-V uniquement.",
		 false, false},
	};
	return c;
}

const Info* find(std::string_view name) {
	for (auto& i : catalog()) if (i.name == name) return &i;
	return nullptr;
}

std::string normalize(std::string_view raw) {
	std::string s(raw);
	for (std::string_view pre : {"-fsanitize=", "fsanitize="})
		if (s.starts_with(pre)) { s = s.substr(pre.size()); break; }
	static const std::map<std::string, std::string> aliases = {
		{"asan", "address"}, {"tsan", "thread"}, {"msan", "memory"}, {"lsan", "leak"},
		{"ubsan", "undefined"}, {"hwasan", "hwaddress"}, {"safestack", "safe-stack"},
	};
	if (auto it = aliases.find(s); it != aliases.end()) s = it->second;
	return s;
}

// Découpe une liste "a,b" (éventuellement répétée) en noms normalisés et
// connus. `allow_all` accepte aussi « all » (pour -trap / -recover).
static std::vector<std::string> parse_list(const std::vector<std::string>& raw,
										   std::string_view opt, bool allow_all) {
	std::vector<std::string> r;
	for (auto& item : raw) {
		for (auto& part : util::split_list(item)) {
			std::string n = normalize(part);
			if (!allow_all && n == "all")
				util::die(std::format("{} all : réservé à build, run, test et check", opt));
			if (!(allow_all && n == "all") && !find(n))
				util::die(std::format("{} : sanitizer inconnu '{}' (voir : cpp-maker help sanitize)",
									  opt, part));
			if (std::find(r.begin(), r.end(), n) == r.end()) r.push_back(n);
		}
	}
	return r;
}

bool wants_all(const Opts& o) {
	for (auto& item : o.sanitize)
		for (auto& part : util::split_list(item))
			if (normalize(part) == "all") return true;
	return false;
}

std::vector<Opts> expand_all(const Project& p, const Opts& o, const std::string& tc_name,
							 const std::string& mode) {
	if (!wants_all(o)) return {o};
	// Une passe par runtime : ils sont exclusifs, donc un binaire chacun.
	// UBSan accompagne ASan (combinables, ça évite une passe de plus).
	static const std::vector<std::vector<std::string>> runtimes = {
		{"address", "undefined"}, {"thread"}, {"leak"}, {"memory"}, {"hwaddress"},
	};
	// Les autres noms donnés avec « all » s'ajoutent à chaque passe.
	std::vector<std::string> extra;
	for (auto& item : o.sanitize)
		for (auto& part : util::split_list(item))
			if (auto n = normalize(part); n != "all" && std::find(extra.begin(), extra.end(), n) == extra.end())
				extra.push_back(n);

	auto tc = toolchain::resolve(p, tc_name, o.cc, o.cxx);
	const ModeConfig* base = p.find_mode(mode);
	auto fam = tc.family();
	bool clang = fam == toolchain::CompilerFamily::Clang || fam == toolchain::CompilerFamily::Unknown;
	bool hw_arch = tc.arch.starts_with("aarch64") || tc.arch.starts_with("arm64") ||
				   tc.arch.starts_with("riscv");

	std::vector<Opts> r;
	std::vector<std::string> skipped;
	for (auto& rt : runtimes) {
		std::vector<std::string> pass = rt;
		for (auto& e : extra) if (std::find(pass.begin(), pass.end(), e) == pass.end()) pass.push_back(e);
		std::string why;
		if (!clang)
			for (auto& s : pass)
				if (auto* i = find(s); i && i->clang_only) { why = s + " nécessite clang"; break; }
		if (why.empty() && rt[0] == "hwaddress" && !hw_arch)
			why = std::format("architecture '{}' (AArch64 / RISC-V requis)", tc.arch.empty() ? "?" : tc.arch);
		if (why.empty()) {
			auto all = base ? from_mode(*base) : std::vector<std::string>{};
			for (auto& s : pass) if (std::find(all.begin(), all.end(), s) == all.end()) all.push_back(s);
			why = check_compatible(all);
		}
		if (!why.empty()) { skipped.push_back(std::format("{} ({})", rt[0], why)); continue; }
		Opts po = o;
		po.sanitize = {util::join(pass, ",")};
		r.push_back(std::move(po));
	}
	for (auto& s : skipped) util::warn("--sanitize all : passe {} ignorée", s);
	if (r.empty()) util::die("--sanitize all : aucune passe possible avec la toolchain '" + tc.name + "'");
	util::outln("\033[2m--sanitize all : {} passe(s) sur {}\033[0m", r.size(), tc.name);
	return r;
}

std::vector<std::string> from_mode(const ModeConfig& m) {
	std::vector<std::string> r;
	auto add = [&](std::string_view list) {
		for (auto& p : util::split_list(std::string(list))) {
			std::string n = normalize(p);
			if (!n.empty() && std::find(r.begin(), r.end(), n) == r.end()) r.push_back(n);
		}
	};
	add(m.sanitizer);
	for (auto& f : m.flags)
		if (f.starts_with("-fsanitize=")) add(std::string_view(f).substr(11));
	return r;
}

std::string check_compatible(const std::vector<std::string>& sans) {
	// Ces runtimes remplacent l'allocateur / instrumentent toute la mémoire :
	// un seul à la fois.
	static const std::set<std::string> exclusive = {"address", "hwaddress", "thread", "memory", "safe-stack"};
	std::vector<std::string> ex;
	for (auto& s : sans) if (exclusive.count(s)) ex.push_back(s);
	if (ex.size() > 1)
		return std::format("sanitizers incompatibles : {} (un seul runtime parmi "
						   "address, hwaddress, thread, memory, safe-stack)", util::join(ex, " + "));
	bool leak = std::find(sans.begin(), sans.end(), "leak") != sans.end();
	if (leak && !ex.empty() && ex[0] != "address")
		return std::format("sanitizers incompatibles : leak + {} (leak ne se combine qu'avec address)", ex[0]);
	return {};
}

bool requested(const Opts& o) {
	return !o.sanitize.empty() || !o.sanitize_trap.empty() ||
		   !o.no_sanitize_recover.empty() || !o.sanitize_ignorelist.empty();
}

ModeConfig apply(const ModeConfig& base, const Opts& o, const toolchain::Toolchain& tc) {
	ModeConfig m = base;
	auto extra = parse_list(o.sanitize, "--sanitize", false);
	auto trap = parse_list(o.sanitize_trap, "--sanitize-trap", true);
	auto norec = parse_list(o.no_sanitize_recover, "--no-sanitize-recover", true);

	std::vector<std::string> all = from_mode(base);
	std::vector<std::string> added;
	for (auto& s : extra)
		if (std::find(all.begin(), all.end(), s) == all.end()) { all.push_back(s); added.push_back(s); }

	if (auto err = check_compatible(all); !err.empty())
		util::die(std::format("mode '{}' : {}", base.name, err));

	// Sanitizers réservés à clang : erreur claire plutôt que celle du compilateur.
	auto fam = tc.family();
	if (fam != toolchain::CompilerFamily::Clang && fam != toolchain::CompilerFamily::Unknown) {
		std::vector<std::string> clang_only;
		for (auto& s : all) if (auto* i = find(s); i && i->clang_only) clang_only.push_back(s);
		if (!o.sanitize_ignorelist.empty()) clang_only.push_back("--sanitize-ignorelist");
		if (!clang_only.empty())
			util::die(std::format("{} : nécessite clang (toolchain '{}'). Essayez --cxx clang++ --cc clang.",
								  util::join(clang_only, ", "), tc.name));
	}
	auto has = [&](std::string_view s) { return std::find(all.begin(), all.end(), s) != all.end(); };
	if ((has("hwaddress") || has("shadow-call-stack")) &&
		!tc.arch.starts_with("aarch64") && !tc.arch.starts_with("arm64") && !tc.arch.starts_with("riscv"))
		util::warn("hwaddress / shadow-call-stack visent AArch64 (ou RISC-V) ; architecture '{}'",
				   tc.arch.empty() ? "?" : tc.arch);
	// Mode utilisé tel quel s'il n'y a aucune option --sanitize* :
	// seules les vérifications ci-dessus s'appliquent.
	if (!requested(o)) return base;

	auto push = [](std::vector<std::string>& v, const std::string& f) {
		if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
	};
	// Remplace les -fsanitize= existants par la liste complète.
	std::erase_if(m.flags, [](auto& f) { return f.starts_with("-fsanitize="); });
	std::erase_if(m.link_flags, [](auto& f) { return f.starts_with("-fsanitize="); });
	if (!all.empty()) {
		std::string list = "-fsanitize=" + util::join(all, ",");
		m.flags.push_back(list);
		m.link_flags.push_back(list);
		m.sanitizer = util::join(all, ",");
		push(m.flags, "-fno-omit-frame-pointer");
		bool debug_info = std::any_of(m.flags.begin(), m.flags.end(),
									  [](auto& f) { return f.starts_with("-g"); });
		if (!debug_info) m.flags.push_back("-g");   // piles d'appel lisibles
	}
	if (has("memory")) push(m.flags, "-fsanitize-memory-track-origins=2");
	if (has("cfi")) {
		push(m.flags, "-flto");
		push(m.flags, "-fvisibility=hidden");
		push(m.link_flags, "-flto");
	}
	if (!trap.empty()) {
		std::string f = "-fsanitize-trap=" + util::join(trap, ",");
		m.flags.push_back(f);
		m.link_flags.push_back(f);
	}
	if (!norec.empty()) m.flags.push_back("-fno-sanitize-recover=" + util::join(norec, ","));
	if (!o.sanitize_ignorelist.empty()) {
		fs::path p = o.sanitize_ignorelist;
		if (p.is_relative()) p = fs::absolute(p);
		std::error_code ec;
		if (!fs::is_regular_file(p, ec))
			util::die("--sanitize-ignorelist : fichier introuvable : " + p.string());
		m.flags.push_back("-fsanitize-ignorelist=" + p.string());
	}

	// Dossier de build distinct : build/<tc>/<mode>+<sanitizers>/
	for (auto& s : added) m.name += "+" + s;
	if (!trap.empty()) m.name += "+trap";
	if (!norec.empty()) m.name += "+norecover";
	return m;
}

} // namespace sanitize
