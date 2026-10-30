#include "cpp-maker.hpp"

namespace toolchain {

const Tool& Toolchain::c_tool() const {
	if (!cc) util::die(std::format("toolchain '{}' : aucun compilateur C trouvé{}", name,
								   cfg.cc != "auto" ? " ('" + cfg.cc + "')" : std::string{}));
	return *cc;
}
const Tool& Toolchain::cxx_tool() const {
	if (!cxx) util::die(std::format("toolchain '{}' : aucun compilateur C++ trouvé{}", name,
									cfg.cxx != "auto" ? " ('" + cfg.cxx + "')" : std::string{}));
	return *cxx;
}

CompilerFamily Toolchain::family() const {
	return cxx ? cxx->family : (cc ? cc->family : CompilerFamily::Unknown);
}

Platform Toolchain::platform() const {
	if (os == "windows") return Platform::Windows;
	if (os == "macos") return Platform::Macos;
	if (os == "freebsd") return Platform::Freebsd;
	if (os == "android") return Platform::Android;
	if (os == "baremetal") return Platform::Baremetal;
	if (os == "linux") return Platform::Linux;
	return Platform::Unknown;
}

std::vector<std::string> default_link_dirs(toolchain::Platform p) {
    switch (p) {
        case Platform::Linux:      return {"/usr/local/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib"};
        case Platform::Freebsd:    return {"/usr/local/lib"};
        case Platform::Macos:      return {"/opt/homebrew/lib", "/usr/local/lib"};
        case Platform::Windows:    return {}; // Windows utilise PATH
        case Platform::Android:    return {}; // Android a son propre système
        case Platform::Ios:       return {}; // iOS utilise SDK paths
        case Platform::TvOS:      return {};
        case Platform::WatchOS:   return {};
        case Platform::Solaris:    return {"/usr/lib/64", "/usr/lib"};
        case Platform::AIX:        return {"/usr/lib"};
        case Platform::Haiku:     return {"/boot/common/lib"};
        case Platform::OpenBSD:   return {"/usr/local/lib"};
        case Platform::NetBSD:    return {"/usr/local/lib"};
        case Platform::DragonFly: return {"/usr/local/lib"};
        default:                  return {};
    }
}

std::vector<std::string> default_include_dirs(toolchain::Platform p) {
    switch (p) {
        case Platform::Linux:      return {"/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include"};
        case Platform::Freebsd:    return {"/usr/local/include"};
        case Platform::Macos:      return {"/opt/homebrew/include", "/usr/local/include"};
        case Platform::Windows:    return {};
        case Platform::Android:    return {};
        case Platform::Ios:       return {};
        case Platform::TvOS:      return {};
        case Platform::WatchOS:   return {};
        case Platform::Solaris:    return {"/usr/include"};
        case Platform::AIX:        return {"/usr/include"};
        case Platform::Haiku:     return {"/boot/common/include"};
        case Platform::OpenBSD:   return {"/usr/local/include"};
        case Platform::NetBSD:    return {"/usr/local/include"};
        case Platform::DragonFly: return {"/usr/local/include"};
        default:                  return {};
    }
}

// Flags par défaut selon le compilateur, la plateforme et l'architecture
std::vector<std::string> default_cflags(CompilerFamily family, Platform platform, const std::string& arch) {
	(void)platform; // Parameter may be used in future extensions
	std::vector<std::string> flags;
	
	// Flags communs pour tous les compilateurs
	if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang || 
		family == CompilerFamily::Intel || family == CompilerFamily::Arm) {
		flags.push_back("-Wall");
		flags.push_back("-Wextra");
		flags.push_back("-Wpedantic");
	}
	
	// Flags spécifiques à GCC
	if (family == CompilerFamily::Gcc) {
		flags.push_back("-Wno-unused-command-line-argument");
	}
	
	// Flags spécifiques à Clang
	if (family == CompilerFamily::Clang) {
		flags.push_back("-Wno-unused-command-line-argument");
		flags.push_back("-Wno-unknown-warning-option");
	}
	
	// Flags spécifiques à MSVC
	if (family == CompilerFamily::Msvc) {
		flags.push_back("/W4");
		flags.push_back("/WX");
		flags.push_back("/std:c17");
	}
	
	// Flags pour architectures spécifiques
	if (arch == "x86_64" || arch == "amd64") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m64");
		}
	} else if (arch == "x86" || arch == "i386") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m32");
		}
	} else if (arch == "arm" || arch == "aarch64") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-march=" + arch);
		}
	}
	
	return flags;
}

std::vector<std::string> default_cxxflags(CompilerFamily family, Platform platform, const std::string& arch) {
	(void)platform; // Parameter may be used in future extensions
	std::vector<std::string> flags;
	
	// Flags communs
	if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang || 
		family == CompilerFamily::Intel || family == CompilerFamily::Arm) {
		flags.push_back("-Wall");
		flags.push_back("-Wextra");
		flags.push_back("-Wpedantic");
	}
	
	// Standard C++ par défaut
	if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang || 
		family == CompilerFamily::Intel || family == CompilerFamily::Arm) {
		flags.push_back("-std=c++23");
	} else if (family == CompilerFamily::Msvc) {
		flags.push_back("/std:c++20"); // MSVC ne supporte pas encore C++23 complet
	}
	
	// Flags spécifiques à Clang
	if (family == CompilerFamily::Clang) {
		flags.push_back("-Wno-unused-command-line-argument");
		flags.push_back("-Wno-unknown-warning-option");
	}
	
	// Flags pour architectures spécifiques
	if (arch == "x86_64" || arch == "amd64") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m64");
		}
	} else if (arch == "x86" || arch == "i386") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m32");
		}
	}
	
	return flags;
}

std::vector<std::string> default_ldflags(CompilerFamily family, Platform platform, const std::string& arch) {
	std::vector<std::string> flags;
	
	// Flags de linkage pour Linux
	if (platform == Platform::Linux) {
		flags.push_back("-Wl,--as-needed");
		flags.push_back("-Wl,--gc-sections");
	}
	
	// Flags pour macOS
	if (platform == Platform::Macos) {
		flags.push_back("-Wl,-dead_strip");
		flags.push_back("-Wl,-rpath,@loader_path");
	}
	
	// Flags pour Windows (MinGW)
	if (platform == Platform::Windows && family != CompilerFamily::Msvc) {
		flags.push_back("-static-libstdc++");
		flags.push_back("-static-libgcc");
	}
	
	// Architecture spécifique
	if (arch == "x86_64" || arch == "amd64") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m64");
		}
	} else if (arch == "x86" || arch == "i386") {
		if (family == CompilerFamily::Gcc || family == CompilerFamily::Clang) {
			flags.push_back("-m32");
		}
	}
	
	return flags;
}

std::optional<Tool> probe(const std::string& exe, const std::vector<std::string>& pre) {
	if (!util::have(exe)) return std::nullopt;
	
	Tool t; 
	t.exe = exe; 
	t.pre_args = pre;
	
	std::string name = fs::path(exe).filename().string();
	util::lower(name);
	
	// Détection par nom de fichier
	if (name == "cl" || name == "cl.exe") {
		t.family = CompilerFamily::Msvc;
	} else if (name.starts_with("tcc")) {
		t.family = CompilerFamily::Tcc;
	} else if (name == "icx" || name == "icpc" || name == "icc" || name == "icpx") {
		t.family = CompilerFamily::Intel;
	} else if (name == "nvcc") {
		t.family = CompilerFamily::Nvidia;
	} else if (name == "armclang" || name == "armcc" || name == "armcxx") {
		t.family = CompilerFamily::Arm;
	} else if (name == "emcc" || name == "em++") {
		t.family = CompilerFamily::Emcc;
	} else if (name == "pgcc" || name == "pgc++" || name == "pgi") {
		t.family = CompilerFamily::Pgi;
	} else if (name == "cc" || name == "CC" || name == "suncc") {
		t.family = CompilerFamily::SunPro;
	} else if (name == "xlc" || name == "xlc++" || name == "ibmcpp") {
		t.family = CompilerFamily::XlC;
	} else if (name == "zig" || name == "zigcc" || name == "zigc++") {
		t.family = CompilerFamily::Zig;
	}
	
	// Détection par version
	auto r = proc::capture({exe, "--version"});
	
	// MSVC ne répond pas à --version ; on tente /?
	if (r.exit_code != 0 && (name == "cl" || name == "cl.exe")) {
		r = proc::capture({exe, "/?"});
	}
	
	// Essayer d'autres options pour certains compilateurs
	if (r.exit_code != 0 && (name == "nvcc")) {
		r = proc::capture({exe, "--version"});
		if (r.exit_code != 0) r = proc::capture({exe, "-V"});
	}
	
	if (r.exit_code != 0 && (name == "icx" || name == "icpc" || name == "icc")) {
		r = proc::capture({exe, "-V"});
	}
	
	if (r.exit_code != 0 && (name == "pgcc" || name == "pgc++")) {
		r = proc::capture({exe, "-V"});
	}
	
	// Si le compilateur est encore inconnu, essayons de détecter par la sortie
	if (t.family == CompilerFamily::Unknown) {
		std::string lower_output = util::lower(r.output);
		
		if (lower_output.find("clang") != std::string::npos) {
			t.family = CompilerFamily::Clang;
		} else if (lower_output.find("gcc") != std::string::npos ||
				   lower_output.find("free software foundation") != std::string::npos) {
			t.family = CompilerFamily::Gcc;
		} else if (lower_output.find("intel") != std::string::npos ||
				   lower_output.find("oneapi") != std::string::npos) {
			t.family = CompilerFamily::Intel;
		} else if (lower_output.find("nvidia") != std::string::npos ||
				   lower_output.find("cuda") != std::string::npos) {
			t.family = CompilerFamily::Nvidia;
		} else if (lower_output.find("arm") != std::string::npos && 
				   lower_output.find("compiler") != std::string::npos) {
			t.family = CompilerFamily::Arm;
		} else if (lower_output.find("emscripten") != std::string::npos) {
			t.family = CompilerFamily::Emcc;
		} else if (lower_output.find("portland group") != std::string::npos ||
				   lower_output.find("pgi") != std::string::npos) {
			t.family = CompilerFamily::Pgi;
		} else if (lower_output.find("oracle") != std::string::npos ||
				   lower_output.find("sun") != std::string::npos) {
			t.family = CompilerFamily::SunPro;
		} else if (lower_output.find("ibm") != std::string::npos ||
				   lower_output.find("xl") != std::string::npos) {
			t.family = CompilerFamily::XlC;
		} else if (lower_output.find("zig") != std::string::npos) {
			t.family = CompilerFamily::Zig;
		}
	}
	
	auto nl = r.output.find('\n');
	t.version = r.output.substr(0, nl == std::string::npos ? r.output.size() : nl);
	util::trim(t.version);
	
	return t;
}

std::string dumpmachine(const Tool& t) {
	std::vector<std::string> cmd = {t.exe};
	cmd.insert(cmd.end(), t.pre_args.begin(), t.pre_args.end());
	cmd.push_back("-dumpmachine");
	auto r = proc::capture(cmd);
	if (r.exit_code != 0) return {};
	return util::trim(r.output);
}

std::string normalize_arch(std::string a) {
	util::lower(a);
	
	// x86 architectures
	if (a == "amd64" || a == "x64" || a == "x86_64" || a == "ia64" || a == "em64t") 
		return "x86_64";
	if (a == "i386" || a == "i486" || a == "i586" || a == "i686" || a == "x86" || a == "ia32") 
		return "x86";
	
	// ARM architectures
	if (a == "arm64" || a == "aarch64" || a == "arm64ec") 
		return "aarch64";
	if (a.starts_with("armv") || a == "armhf" || a == "armel" || a == "arm") 
		return "arm";
	
	// RISC-V
	if (a == "riscv" || a == "riscv32" || a == "riscv64")
		return a;
	
	// MIPS
	if (a == "mips" || a == "mipsel" || a == "mips64" || a == "mips64el")
		return a;
	
	// PowerPC
	if (a == "ppc" || a == "powerpc" || a == "ppc64" || a == "powerpc64" || 
		a == "ppc64le" || a == "powerpc64le" || a == "ppc64el")
		return a;
	
	// SPARC
	if (a == "sparc" || a == "sparc64" || a == "sun4u")
		return a;
	
	// S390
	if (a == "s390" || a == "s390x")
		return a;
	
	// LoongArch
	if (a == "loongarch" || a == "loongarch64")
		return "loongarch64";
	
	// Other
	if (a == "wasm" || a == "wasm32" || a == "wasm64")
		return "wasm";
	
	return a;
}

std::string os_from_triple(const std::string& t) {
	std::string lower_t = util::lower(t);
	
	// Windows variants
	if (lower_t.find("mingw") != std::string::npos || 
		lower_t.find("windows") != std::string::npos ||
		lower_t.find("cygwin") != std::string::npos || 
		lower_t.find("msvc") != std::string::npos ||
		lower_t.find("win32") != std::string::npos ||
		lower_t.find("win64") != std::string::npos) 
		return "windows";
	
	// Apple platforms
	if (lower_t.find("apple") != std::string::npos || 
		lower_t.find("darwin") != std::string::npos) {
		if (lower_t.find("ios") != std::string::npos) return "ios";
		if (lower_t.find("tvos") != std::string::npos) return "tvos";
		if (lower_t.find("watchos") != std::string::npos) return "watchos";
		return "macos";
	}
	
	// Android
	if (lower_t.find("android") != std::string::npos) 
		return "android";
	
	// BSD variants
	if (lower_t.find("freebsd") != std::string::npos) 
		return "freebsd";
	if (lower_t.find("openbsd") != std::string::npos) 
		return "openbsd";
	if (lower_t.find("netbsd") != std::string::npos) 
		return "netbsd";
	if (lower_t.find("dragonfly") != std::string::npos) 
		return "dragonfly";
	
	// Linux
	if (lower_t.find("linux") != std::string::npos) 
		return "linux";
	
	// Unix variants
	if (lower_t.find("solaris") != std::string::npos || 
		lower_t.find("sun") != std::string::npos) 
		return "solaris";
	if (lower_t.find("aix") != std::string::npos) 
		return "aix";
	
	// Other Unix-like
	if (lower_t.find("haiku") != std::string::npos) 
		return "haiku";
	if (lower_t.find("hurd") != std::string::npos) 
		return "hurd";
	
	// Real-time OS
	if (lower_t.find("qnx") != std::string::npos) 
		return "qnx";
	if (lower_t.find("vxworks") != std::string::npos) 
		return "vxworks";
	
	// Bare metal / embedded
	if (lower_t.find("none") != std::string::npos || 
		lower_t.find("elf") != std::string::npos ||
		lower_t.ends_with("eabi") || 
		lower_t.ends_with("eabihf") ||
		lower_t.find("embedded") != std::string::npos ||
		lower_t.find("baremetal") != std::string::npos ||
		lower_t.find("freestanding") != std::string::npos) 
		return "baremetal";
	
	// Default
	return "linux";
}

std::string host_arch() {
#ifdef __unix__
	utsname u{};
	if (uname(&u) == 0) return normalize_arch(u.machine);
#endif
	return "unknown";
}
// toolchain.cpp — remplace la version existante.
std::string host_os() {
    return util::host_os_string();
}

std::string deb_arch(const Toolchain& t) {
	auto a = t.arch;
	if (a == "x86_64") return "amd64";
	if (a == "x86") return "i386";
	if (a == "aarch64") return "arm64";
	if (a == "arm") return t.triple.find("hf") != std::string::npos ? "armhf" : "armel";
	if (a == "riscv64") return "riscv64";
	if (a.starts_with("powerpc64le") || a == "ppc64le") return "ppc64el";
	return a;
}

std::vector<std::string> auto_runner(const Toolchain& t, std::string& note) {
	if (t.os == "windows") {
		for (auto w : {"wine64", "wine"})
			if (util::have(w)) return {w};
		note = "cible Windows : installez wine pour exécuter les binaires";
		return {};
	}
	if (t.os == "baremetal") { note = "cible bare-metal : exécution impossible sur l'hôte"; return {}; }
	if (t.os == "macos") { note = "cible macOS : exécution impossible sur l'hôte"; return {}; }
	static const std::map<std::string, std::string> qemu = {
		{"arm", "qemu-arm"}, {"aarch64", "qemu-aarch64"}, {"x86", "qemu-i386"},
		{"x86_64", "qemu-x86_64"}, {"riscv64", "qemu-riscv64"}, {"riscv32", "qemu-riscv32"},
		{"mips", "qemu-mips"}, {"mipsel", "qemu-mipsel"}, {"mips64", "qemu-mips64"},
		{"mips64el", "qemu-mips64el"}, {"powerpc", "qemu-ppc"}, {"powerpc64", "qemu-ppc64"},
		{"powerpc64le", "qemu-ppc64le"}, {"s390x", "qemu-s390x"},
	};
	auto it = qemu.find(t.arch);
	if (it == qemu.end()) { note = "aucun émulateur connu pour l'architecture " + t.arch; return {}; }
	std::string exe;
	for (auto cand : {it->second, it->second + "-static"})
		if (util::have(cand)) { exe = cand; break; }
	if (exe.empty()) {
		note = std::format("installez {} (paquet qemu-user) pour exécuter les binaires {}",
						   it->second, t.arch);
		return {};
	}
	std::vector<std::string> r = {exe};
	std::string sysroot = t.cfg.sysroot;
	std::error_code ec;
	if (sysroot.empty() && !t.triple.empty() && fs::is_directory("/usr/" + t.triple, ec))
		sysroot = "/usr/" + t.triple;
	if (!sysroot.empty()) { r.push_back("-L"); r.push_back(sysroot); }
	return r;
}

std::vector<std::string> host_candidates(bool cxx, const std::string& family_str) {

	CompilerFamily fam = CompilerFamily::Unknown;
	if (family_str == "msvc") fam = CompilerFamily::Msvc;
	else if (family_str == "tcc") fam = CompilerFamily::Tcc;
	else if (family_str == "gcc") fam = CompilerFamily::Gcc;
	else if (family_str == "clang") fam = CompilerFamily::Clang;
	std::vector<std::string> r;
	if (const char* e = std::getenv(cxx ? "CXX" : "CC"); e && *e) r.push_back(e);

	if (fam == CompilerFamily::Msvc || fam == CompilerFamily::Unknown) {
		// cl.exe est rarement dans le PATH hors "Developer Command Prompt".
		// On tente les emplacements standard VS 2019/2022.
		for (auto v : {"2022", "2019"}) {
			for (auto ed : {"Community", "Professional", "Enterprise", "BuildTools"}) {
				fs::path cl = std::format("C:/Program Files/Microsoft Visual Studio/{}/{}/VC/Tools/MSVC",
											v, ed);
				std::error_code ec;
				if (!fs::is_directory(cl, ec)) continue;
				for (auto& e : fs::directory_iterator(cl, ec))
					r.push_back((e.path() / "bin/Hostx64/x64/cl.exe").string());
			}
		}
	}
	if (fam == CompilerFamily::Tcc || fam == CompilerFamily::Unknown) {
		r.push_back("tcc");
	}
	if (fam == CompilerFamily::Gcc || fam == CompilerFamily::Unknown) {
		for (auto c : cxx ? std::vector<std::string>{"g++", "g++-14", "g++-13"}
							: std::vector<std::string>{"gcc", "gcc-14", "gcc-13"})
			r.push_back(c);
	}
	if (fam == CompilerFamily::Clang || fam == CompilerFamily::Unknown) {
		for (auto c : cxx ? std::vector<std::string>{"clang++", "clang++-20", "clang++-19"}
							: std::vector<std::string>{"clang", "clang-20", "clang-19"})
			r.push_back(c);
	}
	return r;
}

std::vector<std::string> detect_prefixes() {
	std::set<std::string> out;
	const char* env = std::getenv("PATH");
	for (auto& dir : util::split_list(env ? env : "/usr/bin", util::path_list_sep())) {
		std::error_code ec;
		fs::directory_iterator it(dir, ec);
		if (ec) continue;
		for (; it != fs::directory_iterator(); it.increment(ec)) {
			if (ec) break;
			auto n = it->path().filename().string();
			for (std::string suf : {"-gcc", "-g++"}) {
				if (n.size() > suf.size() && n.ends_with(suf)) {
					auto pre = n.substr(0, n.size() - suf.size() + 1);
					if (std::count(pre.begin(), pre.end(), '-') >= 3) out.insert(pre);
				}
			}
		}
	}
	return {out.begin(), out.end()};
}

std::optional<ToolchainConfig> config_for(const Project& p, const std::string& name) {
	if (auto it = p.toolchains.find(name); it != p.toolchains.end()) {
		auto c = it->second;
		c.name = name;
		return c;
	}
	ToolchainConfig c;
	c.name = name;
	if (name == "host") return c;
	if (name == "gcc") { c.cc = "gcc"; c.cxx = "g++"; return c; }
	if (name == "clang") { c.cc = "clang"; c.cxx = "clang++"; return c; }
	for (auto [suf, csuf, is_cxx] : {std::tuple{"clang++", "clang", true}, {"clang", "clang", false},
									 {"g++", "gcc", true}, {"gcc", "gcc", false}}) {
		std::string s = suf;
		auto pos = name.rfind(s);
		if (pos == std::string::npos) continue;
		if (pos != 0 && name[pos - 1] != '-' && name[pos - 1] != '/') continue;
		std::string tail = name.substr(pos + s.size());
		if (!tail.empty() && !(tail[0] == '-' && std::all_of(tail.begin() + 1, tail.end(), ::isdigit)))
			continue;
		if (!util::have(name)) continue;
		std::string prefix = name.substr(0, pos);
		std::string other = prefix + (is_cxx ? std::string(csuf) : (s == "gcc" ? "g++" : "clang++")) + tail;
		if (is_cxx) { c.cxx = name; c.cc = util::have(other) ? other : "none"; }
		else        { c.cc = name; c.cxx = util::have(other) ? other : "none"; }
		if (!prefix.empty()) c.prefix = prefix;
		return c;
	}
	std::string prefix = name.ends_with("-") ? name : name + "-";
	if (util::have(prefix + "gcc") || util::have(prefix + "g++")) {
		c.prefix = prefix;
		return c;
	}
	return std::nullopt;
}

Toolchain resolve(const Project& p, const std::string& name,
				  const std::string& cc_override, const std::string& cxx_override) {
	auto oc = config_for(p, name);
	if (!oc) {
		std::vector<std::string> known = {"host", "gcc", "clang"};
		for (auto& [n, t] : p.toolchains)
			if (std::find(known.begin(), known.end(), n) == known.end()) known.push_back(n);
		for (auto& pre : detect_prefixes()) known.push_back(pre.substr(0, pre.size() - 1));
		throw std::runtime_error(std::format("toolchain inconnue : '{}'\n  disponibles : {}", name,
											 util::join(known, ", ")));
	}
	Toolchain t;
	t.cfg = *oc;
	auto& c = t.cfg;
	if (!cc_override.empty()) c.cc = cc_override;
	if (!cxx_override.empty()) c.cxx = cxx_override;
	t.name = name;
	if (!cc_override.empty() || !cxx_override.empty()) {
		std::string base = fs::path(!cxx_override.empty() ? cxx_override : cc_override).filename().string();
		t.name = util::is_safe_name(base) ? base : "custom";
	}
	if (!util::is_safe_name(t.name)) {
		std::string s;
		for (char ch : t.name) s += (std::isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '_';
		t.name = s;
	}

	std::vector<std::string> pre;
	if (!c.target.empty()) pre.push_back("--target=" + c.target);
	if (!c.sysroot.empty()) pre.push_back("--sysroot=" + c.sysroot);

	auto pick = [&](bool cxx) -> std::optional<Tool> {
		std::string spec = cxx ? c.cxx : c.cc;
		if (spec == "none") return std::nullopt;
		if (spec != "auto") return probe(spec, pre);
		if (!c.prefix.empty()) {
			if (c.family == "clang") {
				auto v = pre;
				if (c.target.empty())
					v.insert(v.begin(), "--target=" + c.prefix.substr(0, c.prefix.size() - 1));
				return probe(cxx ? "clang++" : "clang", v);
			}
			return probe(c.prefix + (cxx ? "g++" : "gcc"), pre);
		}
		for (auto& cand : host_candidates(cxx, c.family))
			if (auto r = probe(cand, pre)) return r;
		return std::nullopt;
	};
	t.cc = pick(false);
	t.cxx = pick(true);
	if (!t.cc && !t.cxx)
		throw std::runtime_error(std::format("toolchain '{}' : aucun compilateur C ou C++ utilisable "
							  "(cc = \"{}\", cxx = \"{}\"{})", name, c.cc, c.cxx,
							  c.prefix.empty() ? "" : ", prefix = \"" + c.prefix + "\""));

	auto bin = [&](const std::string& explicit_, const std::string& tool) {
		if (!explicit_.empty()) return explicit_;
		if (!c.prefix.empty() && util::have(c.prefix + tool)) return c.prefix + tool;
		const Tool& any = t.cxx ? *t.cxx : *t.cc;
		if (any.family == toolchain::CompilerFamily::Clang && !c.target.empty() && util::have("llvm-" + tool)) return "llvm-" + tool;
		if (any.family == toolchain::CompilerFamily::Gcc && tool == "ar") {
			auto exe = fs::path(any.exe).filename().string();
			auto pos = exe.find("g++");
			if (pos == std::string::npos) pos = exe.find("gcc");
			if (pos != std::string::npos) {
				std::string gar = exe.substr(0, pos) + "gcc-ar" + exe.substr(pos + 3);
				if (util::have(gar)) return gar;
			}
		}
		return tool;
	};
	t.ar = bin(c.ar, "ar");
	t.strip = bin(c.strip, "strip");
	if (!c.pkg_config.empty()) t.pkg_config = c.pkg_config;
	else if (!c.prefix.empty() && util::have(c.prefix + "pkg-config")) t.pkg_config = c.prefix + "pkg-config";
	else t.pkg_config = "pkg-config";

	t.triple = dumpmachine(t.cxx ? *t.cxx : *t.cc);
	if (t.triple.empty() && !c.target.empty()) t.triple = c.target;
	if (t.triple.empty() && !c.prefix.empty()) t.triple = c.prefix.substr(0, c.prefix.size() - 1);
	t.arch = normalize_arch(t.triple.empty() ? host_arch() : t.triple.substr(0, t.triple.find('-')));
	t.os = !c.os.empty() ? c.os : (t.triple.empty() ? host_os() : os_from_triple(t.triple));
	t.cross = t.arch != host_arch() || t.os != host_os();

	if (c.runner_set) {
		t.runner = c.runner;
		if (t.runner.empty() && t.cross) t.runner_note = "runner = [] : exécution désactivée";
	} else if (t.cross) {
		t.runner = auto_runner(t, t.runner_note);
	}
	return t;
}

bool smoke_compile(const Tool& tool, const std::string& lang, const std::string& standard,
				   std::string* err_out, bool link) {
	std::error_code ec;
	auto tmp = fs::temp_directory_path(ec);
	if (ec) return true;
#ifdef __unix__
	auto dir = tmp / std::format("cpp_maker_smoke_{}_{}", ::getpid(),
								 std::hash<std::thread::id>{}(std::this_thread::get_id()));
#else
	auto dir = tmp / "cpp_maker_smoke";
#endif
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	if (ec) return true;
	bool c = lang == "c";
	auto src = dir / (c ? "smoke.c" : "smoke.cpp");
	auto bin = dir / "smoke.bin";
	{
		std::ofstream f(src);
		if (c) f << "#include <stdio.h>\nint main(void){ printf(\"0\\n\"); return 0; }\n";
		else   f << "#include <iostream>\nint main(){ std::cout << 0 << '\\n'; return 0; }\n";
	}
	std::vector<std::string> cmd = {tool.exe};
	cmd.insert(cmd.end(), tool.pre_args.begin(), tool.pre_args.end());
	cmd.push_back("-std=" + standard);
	if (!link) cmd.push_back("-c");
	cmd.insert(cmd.end(), {src.string(), "-o", bin.string()});
	auto r = proc::capture(cmd);
	fs::remove_all(dir, ec);
	if (r.exit_code == 0) return true;
	if (err_out) *err_out = r.output;
	return false;
}

std::string truncate(const std::string& s, size_t max_lines) {
	size_t lines = 0;
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] == '\n' && ++lines >= max_lines) return s.substr(0, i);
	return s;
}

std::optional<Tool> detect_working(const std::string& lang, const std::string& family,
								   const std::string& forced, const std::string& standard,
								   std::vector<TrialResult>* log) {
	std::vector<std::string> cands = forced.empty() || forced == "auto"
		? host_candidates(lang != "c", family) : std::vector<std::string>{forced};
	for (auto& cand : cands) {
		auto i = probe(cand);
		if (!i) { if (log) log->push_back({cand, "non trouvé"}); continue; }
		std::string err;
		if (smoke_compile(*i, lang, standard, &err)) {
			if (log) log->push_back({cand, "OK — " + i->version});
			return i;
		}
		if (log) log->push_back({cand, "compilation triviale en échec"});
	}
	return std::nullopt;
}

} // namespace toolchain