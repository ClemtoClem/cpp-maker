// cpp-maker.hpp — API publique du moteur de production cpp-maker.
// Ce header contient : types, déclarations, et les fonctions inline
// (templates + helpers très courts) qui doivent rester visibles.
#pragma once

#include <algorithm>
#include <regex>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifdef __unix__
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/utsname.h>
#  include <sys/wait.h>
#  include <sys/file.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

inline constexpr std::string_view CPP_MAKER_VERSION = "0.8.0";

// ============================================================================
// util
// ============================================================================
namespace util {

// ---------------------------------------------------------------------------
// Plate-forme de l'HÔTE — là où tourne cpp-maker, PAS la cible de compilation.
// Pour la cible, voir Toolchain::os (déduit du triplet).
// Utilisée partout où le comportement dépend du système qui exécute l'outil :
//   - format du build.ninja (cmd.exe vs /bin/sh),
//   - lancement de processus (proc::run),
//   - séparateur de liste PATH (':' vs ';'),
//   - encodage, symlinks, permissions…
// ---------------------------------------------------------------------------
inline std::string host_os_string() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// Raccourcis booléens pour les branchements les plus fréquents.
inline bool host_is_windows()  { return host_os_string() == "windows"; }
inline bool host_is_posix()    { return !host_is_windows(); }

// Séparateur de liste dans $PATH (";" sous Windows, ":" ailleurs).
inline char path_list_sep()    { return host_is_windows() ? ';' : ':'; }

inline bool color() {
#ifdef __unix__
	static bool c = isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
	return c;
#else
	return false;
#endif
}

inline std::string strip_ansi(std::string_view s) {
	std::string r;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
			size_t j = i + 2;
			while (j < s.size() && s[j] != 'm') ++j;
			i = j;
		} else {
			r += s[i];
		}
	}
	return r;
}

template <class... Args>
void out(std::format_string<Args...> f, Args&&... a) {
	std::string s = std::format(f, std::forward<Args>(a)...);
	std::cout << (color() ? s : strip_ansi(s));
}
template <class... Args>
void outln(std::format_string<Args...> f, Args&&... a) {
	std::string s = std::format(f, std::forward<Args>(a)...);
	std::cout << (color() ? s : strip_ansi(s)) << '\n';
}
template <class... Args>
void errln(std::format_string<Args...> f, Args&&... a) {
	std::string s = std::format(f, std::forward<Args>(a)...);
	std::cout.flush();
	std::cerr << (color() ? s : strip_ansi(s)) << '\n';
}
template <class... Args>
void warn(std::format_string<Args...> f, Args&&... a) {
	errln("\033[1;33mwarning:\033[0m {}", std::format(f, std::forward<Args>(a)...));
}

[[noreturn]] inline void die(std::string_view m) {
	errln("\033[1;31merror:\033[0m {}", m);
	std::exit(1);
}

struct Hasher {
	uint64_t h = 1469598103934665603ULL;
	void feed(std::string_view s) {
		for (char ch : s) { h ^= static_cast<unsigned char>(ch); h *= 1099511628211ULL; }
		h ^= 0xff; h *= 1099511628211ULL;
	}
	void feed_u64(uint64_t v) {
		for (int i = 0; i < 8; ++i) { h ^= (v & 0xff); h *= 1099511628211ULL; v >>= 8; }
	}
	std::string hex() const { return std::format("{:016x}", h); }
};

inline std::string join(const std::vector<std::string>& v, std::string_view sep) {
	std::string r;
	for (size_t i = 0; i < v.size(); ++i) { if (i) r += sep; r += v[i]; }
	return r;
}

inline std::string trim(std::string_view s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string_view::npos) return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> split_ws(std::string_view s);

inline std::vector<std::string> split_list(std::string_view s, char sep = ',') {
	std::vector<std::string> r;
	size_t start = 0;
	while (start <= s.size()) {
		size_t end = s.find(sep, start);
		if (end == std::string_view::npos) end = s.size();
		auto t = trim(s.substr(start, end - start));
		if (!t.empty()) r.push_back(t);
		start = end + 1;
	}
	return r;
}

inline std::string replace_all(std::string s, std::string_view from, std::string_view to) {
	if (from.empty()) return s;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

inline std::string lower(std::string s) {
	for (auto& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

inline bool is_safe_name(std::string_view s) {
	if (s.empty() || s == "." || s == "..") return false;
	return std::all_of(s.begin(), s.end(), [](unsigned char c) {
		return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+';
	});
}

std::optional<fs::path> find_in_path(const std::string& name);
inline bool have(const std::string& name) { return find_in_path(name).has_value(); }

std::string shell_quote(const std::string& a);
std::string shell_join(const std::vector<std::string>& v);
std::string json_escape(std::string_view s);
std::string xml_escape(std::string_view s);

bool read_file(const fs::path& p, std::string& out);
bool write_file_atomic(const fs::path& p, const std::string& content);
void write_file(const fs::path& p, const std::string& content);

inline fs::path under(const fs::path& dir, const fs::path& p) {
	if (p.is_absolute()) return p.lexically_normal();
	if (dir.empty() || dir == ".") return p.lexically_normal();
	return (dir / p).lexically_normal();
}

inline std::string elapsed(std::chrono::steady_clock::time_point t0) {
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();
	return std::format("{}.{:02}s", ms / 1000, (ms % 1000) / 10);
}

#ifdef __unix__
class FileLock {
	int fd_ = -1;
public:
	FileLock() = default;
	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;
	FileLock(FileLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
	FileLock& operator=(FileLock&& o) noexcept {
		if (this != &o) { release(); fd_ = o.fd_; o.fd_ = -1; }
		return *this;
	}
	~FileLock() { release(); }

	void release() {
		if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); fd_ = -1; }
	}
	bool held() const { return fd_ >= 0; }

	bool acquire(const fs::path& p, std::string_view waiting_message = {}) {
		release();
		std::error_code ec;
		if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
		int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0) return false;
		if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
			if (errno != EWOULDBLOCK) { ::close(fd); return false; }
			std::string holder;
			char buf[32] = {};
			ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0);
			if (n > 0) {
				int pid = std::atoi(buf);
				if (pid > 0) holder = std::format(" (PID {})", pid);
			}
			if (!waiting_message.empty())
				outln("\033[1;33m{}\033[0m{}", waiting_message, holder);
			if (::flock(fd, LOCK_EX) != 0) { ::close(fd); return false; }
			outln("\033[1;32mVerrou obtenu, poursuite.\033[0m");
		}
		(void)!::ftruncate(fd, 0);
		auto s = std::format("{}\n", (int)::getpid());
		(void)!::write(fd, s.data(), s.size());
		fd_ = fd;
		return true;
	}
};
#else
class FileLock {
public:
	FileLock() = default;
	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;
	bool acquire(const fs::path&, std::string_view = {}) { return true; }
	void release() {}
	bool held() const { return false; }
};
#endif

} // namespace util

// ============================================================================
// proc
// ============================================================================
namespace proc {

struct Result {
	int exit_code = 0;
	bool signaled = false;
	int signal = 0;
	bool timed_out = false;
	std::string output;
};

struct Options {
	bool capture = false;
	std::optional<fs::path> cwd;
	int timeout_sec = 0;
	std::vector<std::pair<std::string, std::string>> env{};
};

Result run(const std::vector<std::string>& args, const Options& o = {});

inline Result capture(const std::vector<std::string>& args) {
	Options o; o.capture = true;
	return run(args, o);
}

} // namespace proc


struct Project;

// ============================================================================
// output — options globales --output <fichier> / --filter <motif>
// ============================================================================
namespace output {

void rebind_paths(const Project& p);

// Retire --output / --grep (et --filter si `filter_is_global`) de `args`
// (avant un éventuel `--`) ; renvoie les motifs de filtre trouvés.
// `out_dir` : préfixe appliqué aux noms de fichier sans répertoire.
std::vector<std::string> extract_options(std::vector<std::string>& args, bool filter_is_global,
                                         const std::string& out_dir = ".out");
// Vrai si au moins un filtre --filter/--grep est actif (backup refuse --file - + filtre).
bool any_filter_active();

// Redirige stdout/stderr (cpp-maker et processus enfants) à travers le filtre,
// vers le terminal ou le fichier --output. Sans option : ne fait rien.
void setup(const std::vector<std::string>& patterns);

} // namespace output

// ============================================================================
// toml
// ============================================================================
namespace toml {

struct Value;
using Array = std::vector<Value>;
using Table = std::map<std::string, Value>;

struct Value {
	std::variant<std::monostate, std::string, int64_t, bool, Array, Table> v;

	const Table* table() const { return std::get_if<Table>(&v); }
	Table* table() { return std::get_if<Table>(&v); }
	const Array* array() const { return std::get_if<Array>(&v); }
	const std::string* str() const { return std::get_if<std::string>(&v); }
	std::optional<int64_t> integer() const {
		if (auto p = std::get_if<int64_t>(&v)) return *p;
		return std::nullopt;
	}
	std::optional<bool> boolean() const {
		if (auto p = std::get_if<bool>(&v)) return *p;
		return std::nullopt;
	}
};

struct ParseError : std::runtime_error {
	ParseError(std::string m) : std::runtime_error(std::move(m)) {}
};

class Parser {
public:
	Parser(std::string text, std::string file);
	Table parse();
private:
	std::string text_;
	std::string file_;
	size_t i_ = 0;
	int line_ = 1;

	[[noreturn]] void fail(const std::string& m);
	bool eof() const;
	char peek() const;
	char next();
	void skip_ws();
	void skip_ws_and_comments();
	void expect_eol();
	bool is_bare_key_char(char c) const;
	std::string parse_key();
	std::vector<std::string> parse_key_path();
	Table& ensure_table(Table& root, const std::vector<std::string>& path);
	std::string parse_string();
	Value parse_value();
	Array parse_array();
	Table parse_inline_table();
	bool parse_bool();
	int64_t parse_int();
};

Table parse_file(const fs::path& p);

inline const Value* find(const Table& t, const std::string& k) {
	auto it = t.find(k);
	return it == t.end() ? nullptr : &it->second;
}
inline const Table* get_table(const Table& t, const std::string& k) {
	auto v = find(t, k);
	if (!v) return nullptr;
	if (!v->table()) throw ParseError(std::format("'{}' doit être une table", k));
	return v->table();
}
inline std::optional<std::string> get_str(const Table& t, const std::string& k) {
	auto v = find(t, k);
	if (!v) return std::nullopt;
	if (!v->str()) throw ParseError(std::format("'{}' doit être une chaîne", k));
	return *v->str();
}
inline std::optional<std::vector<std::string>> get_strs(const Table& t, const std::string& k) {
	auto v = find(t, k);
	if (!v) return std::nullopt;
	if (auto s = v->str()) return std::vector<std::string>{*s};
	auto a = v->array();
	if (!a) throw ParseError(std::format("'{}' doit être un tableau de chaînes", k));
	std::vector<std::string> r;
	for (auto& x : *a) {
		if (!x.str()) throw ParseError(std::format("'{}' doit être un tableau de chaînes", k));
		r.push_back(*x.str());
	}
	return r;
}
inline std::optional<int64_t> get_int(const Table& t, const std::string& k) {
	auto v = find(t, k);
	if (!v) return std::nullopt;
	if (!v->integer()) throw ParseError(std::format("'{}' doit être un entier", k));
	return *v->integer();
}
inline std::optional<bool> get_bool(const Table& t, const std::string& k) {
	auto v = find(t, k);
	if (!v) return std::nullopt;
	if (!v->boolean()) throw ParseError(std::format("'{}' doit être un booléen", k));
	return *v->boolean();
}

template <class T>
inline void set_if(T& dst, const std::optional<T>& v) { if (v) dst = *v; }

} // namespace toml

// ============================================================================
// Structures de configuration (namespace global)
// ============================================================================
struct ModeConfig {
	std::string name;
	std::vector<std::string> flags;
	std::vector<std::string> link_flags;
	std::string sanitizer;
	std::string runner;
	std::vector<std::string> runner_args;
	std::vector<std::string> defines;
};

struct ToolchainConfig {
	std::string name;
	std::string prefix;
	std::string cc = "auto";
	std::string cxx = "auto";
	std::string ar, strip, pkg_config;
	std::string family = "auto";  // "auto" | "gcc" | "clang" | "msvc" | "tcc" | "intel" | "nvidia" | "arm" | "emcc" | "pgi" | "sunpro" | "xlc" | "zig"
    std::string os;               // "" = déduit
	std::string target;
	std::string sysroot;
	std::vector<std::string> cflags, cxxflags, ldflags;
	std::vector<std::string> runner;
	bool runner_set = false;
	bool from_config = false;
	// Options pour la cross-compilation
	std::string cross_prefix;      // Préfixe pour les outils de cross-compilation (ex: "arm-linux-gnueabihf-")
	bool use_cross_tools = false;  // Utiliser les outils de cross-compilation
};

struct PlatformOverride {
	std::vector<std::string> sources, defines, system_libs, cflags, cxxflags, ldflags;
};

struct GitDependency {
	std::string url, version = "HEAD", module, alias, subdir;
};

struct ModuleConfig {
	std::string name;
	std::string type = "executable";
	std::string version;
	std::string description;
	fs::path dir;
	fs::path file;
	std::vector<std::string> source_dirs = {"src"};
	std::vector<std::string> source_files;
	std::vector<std::string> exclude;
	std::string main;
	std::string layer;
	std::vector<std::string> public_includes = {"include"};
	std::vector<std::string> private_includes;
	std::vector<std::string> deps;
	std::vector<std::string> pkg_config;
	std::vector<std::string> system_libs;
	std::vector<std::string> defines, public_defines;
	std::vector<std::string> cflags, cxxflags, ldflags;
	std::vector<std::string> test_dirs = {"tests"};
	std::string c_standard, cxx_standard;
	std::map<std::string, PlatformOverride> platforms;
	std::vector<std::string> frameworks;  // macOS : ["Cocoa", "Metal"]
	bool install = true;
	std::vector<GitDependency> git_deps;
	bool from_git = false;

	bool is_library() const { return type == "static" || type == "shared" || type == "header-only"; }
};

struct PackageConfig {
	std::vector<std::string> formats = {"tar.gz"};
	bool strip = true;
	std::string maintainer = "unknown <unknown@example.com>";
	std::string description;
	std::string homepage;
	std::string license;
	std::vector<std::string> deb_depends;
	std::string deb_prefix = "/usr";
	std::vector<std::string> extra_files;
	bool checksums = true;
};

struct DeployConfig {
	std::string name;
	std::string method = "rsync";
	std::string host, user, path;
	int port = 0;
	std::string toolchain;
	std::string mode = "release";
	std::string artifact = "stage";
	std::vector<std::string> modules;
	std::vector<std::string> ssh_options;
	std::vector<std::string> before;
	std::vector<std::string> after;
	std::vector<std::string> after_local;
	std::string script;
	bool delete_extra = false;
};

struct Project {
	fs::path root;
	fs::path file;
	bool has_file = false;
	std::string name = "project";
	std::string version = "0.1.0";
	std::string description;
	std::string cxx_standard = "c++23";
	std::string c_standard = "c17";
	std::string min_version;
	std::string default_mode = "debug";
	std::string default_toolchain = "host";
	// Répertoires de sortie (configurables via [paths] dans cpp-maker.toml).
	std::string out_dir = ".out";          // --output / --stdout / --stderr sans chemin
	std::string backups_dir = ".backups";  // cpp-maker backup sans --file
	int jobs = 0;
	int test_timeout = 300;
	std::vector<std::pair<std::string, std::string>> test_env;
	std::vector<std::string> layer_order;
	struct GenerateStep { std::string name, command; std::vector<std::string> inputs, outputs; };
	std::vector<GenerateStep> generate_steps;
	std::map<std::string, std::vector<std::string>> layer_forbid_pkg;
	bool static_pic = true;
	std::vector<std::string> warnings = {
		"-Wall", "-Wextra", "-Wpedantic", "-Wformat=2", "-Wshadow", "-Wconversion",
		"-Wsign-conversion", "-Wdouble-promotion", "-Wundef", "-Wcast-align",
		"-Wcast-qual", "-Woverloaded-virtual", "-Wnon-virtual-dtor"
	};
	std::vector<std::string> cflags, cxxflags, ldflags, defines;
	bool workspace = false;
	std::vector<std::string> module_paths;
	std::vector<ModeConfig> modes;
	std::map<std::string, ToolchainConfig> toolchains;
	std::vector<ModuleConfig> modules;
	PackageConfig package;
	std::map<std::string, DeployConfig> deploys;
	std::map<std::string, std::string> git_lock;

	const ModeConfig* find_mode(const std::string& n) const {
		for (auto& m : modes) if (m.name == n) return &m;
		return nullptr;
	}
	const ModuleConfig* find_module(const std::string& n) const {
		for (auto& m : modules) if (m.name == n) return &m;
		return nullptr;
	}
};

// ============================================================================
// deps
// ============================================================================
namespace deps {

fs::path cache_root();
std::string url_key(const std::string& url);
fs::path bare_repo(const std::string& url);
fs::path checkout_path(const std::string& url, const std::string& commit);

struct Resolved { bool ok = false; std::string commit; fs::path path; std::string error; };
Resolved resolve(const std::string& url, const std::string& version,
				 const std::string& lock_commit = {});

} // namespace deps

// ============================================================================
// cfg
// ============================================================================
namespace cfg {

struct ConfigError : std::runtime_error {
	ConfigError(std::string m) : std::runtime_error(std::move(m)) {}
};

// Anticipée : utilisée par import_git_dep (récursion via les dépôts git).
Project load(const fs::path& root_dir, const fs::path& file);

std::vector<ModeConfig> default_modes();
const std::set<std::string>& cxx_standards();
const std::set<std::string>& c_standards();
void normalize_mode(ModeConfig& m);

void parse_modes(const toml::Table& build, Project& p);
ToolchainConfig parse_toolchain(const std::string& name, const toml::Table& t);
PlatformOverride parse_platform(const toml::Table& t);
ModuleConfig parse_module(const toml::Table& root, const fs::path& dir,
						  const fs::path& file, const std::string& default_name,
						  bool implicit);

void import_git_dep(Project& p, const ModuleConfig& owner, const GitDependency& gd);
void resolve_git_modules(Project& p);
void write_lock_file(Project& p);

std::vector<std::string> topo_order(const Project& p, const std::vector<std::string>& roots);
std::string validate_layers(const Project& p, const std::string& file);
std::string validate_errors(const Project& p);

} // namespace cfg

// ============================================================================
// toolchain
// ============================================================================
namespace toolchain {

enum class CompilerFamily { 
	Gcc, Clang, Msvc, Tcc, 
	// Compilateurs additionnels
	Intel,      // Intel ICC/icpc/icx
	Nvidia,     // NVIDIA CUDA (nvcc)
	Arm,        // ARM Compiler (armclang, armcc)
	Emcc,       // Emscripten
	Pgi,        // Portland Group
	SunPro,     // Oracle Sun Studio
	XlC,        // IBM XL C/C++
	Zig,        // Zig as C/C++ compiler
	Unknown 
};

enum class Platform { 
	Linux, Windows, Macos, Freebsd, Android, Baremetal, 
	// Plateformes additionnelles
	Ios,        // iOS
	TvOS,       // Apple TV OS
	WatchOS,    // Apple Watch OS
	Haiku,      // Haiku OS
	Solaris,    // Oracle Solaris
	AIX,        // IBM AIX
	Hurd,       // GNU Hurd
	DragonFly,  // DragonFly BSD
	OpenBSD,    // OpenBSD
	NetBSD,     // NetBSD
	Qnx,        // QNX
	VxWorks,    // VxWorks
	Unknown 
};

struct Tool {
	std::string exe;
	std::vector<std::string> pre_args;
	CompilerFamily family = CompilerFamily::Unknown;
	std::string version;
};

struct Toolchain {
	std::string name;
	ToolchainConfig cfg;
	std::optional<Tool> cc, cxx;
	std::string ar, strip, pkg_config;
	std::string triple, arch, os;
	bool cross = false;
	std::vector<std::string> runner;
	std::string runner_note;

	bool can_execute() const { return !cross || !runner.empty(); }
	std::string exe_suffix() const { return os == "windows" ? ".exe" : ""; }

	const Tool& c_tool() const;
	const Tool& cxx_tool() const;

	CompilerFamily family() const;  // famille du compilateur C++
    Platform platform() const;      // à partir de `os`
};

struct TrialResult { std::string name; std::string status; };

std::vector<std::string> default_link_dirs(Platform p);
std::vector<std::string> default_include_dirs(Platform p);
std::vector<std::string> default_cflags(CompilerFamily family, Platform platform, const std::string& arch);
std::vector<std::string> default_cxxflags(CompilerFamily family, Platform platform, const std::string& arch);
std::vector<std::string> default_ldflags(CompilerFamily family, Platform platform, const std::string& arch);
std::optional<Tool> probe(const std::string& exe, const std::vector<std::string>& pre = {});
std::string dumpmachine(const Tool& t);
std::string normalize_arch(std::string a);
std::string os_from_triple(const std::string& t);
std::string host_arch();
std::string host_os();
std::string deb_arch(const Toolchain& t);
std::vector<std::string> auto_runner(const Toolchain& t, std::string& note);
std::vector<std::string> host_candidates(bool cxx, const std::string& family);
std::vector<std::string> detect_prefixes();
std::optional<ToolchainConfig> config_for(const Project& p, const std::string& name);
Toolchain resolve(const Project& p, const std::string& name,
				  const std::string& cc_override = {}, const std::string& cxx_override = {});
bool smoke_compile(const Tool& tool, const std::string& lang, const std::string& standard,
				   std::string* err_out = nullptr, bool link = true);
std::string truncate(const std::string& s, size_t max_lines = 5);
std::optional<Tool> detect_working(const std::string& lang, const std::string& family,
								   const std::string& forced, const std::string& standard,
								   std::vector<TrialResult>* log = nullptr);

} // namespace toolchain

// ============================================================================
// build
// ============================================================================
namespace build {

enum class Lang { C, CXX, ASM };
std::optional<Lang> lang_of(const fs::path& p);

struct Task {
	std::string kind;
	std::string label;
	std::vector<std::string> cmd;
	std::vector<fs::path> outputs;
	std::vector<fs::path> inputs;
	fs::path source;
	fs::path depfile;
	fs::path stamp;
	std::vector<size_t> deps;
	std::string tool_version;
	bool remove_outputs_first = false;
	bool is_compile() const { return !source.empty(); }
};

std::vector<fs::path> parse_depfile(const fs::path& p);
std::string fingerprint(const Task& t);
bool up_to_date(const Task& t);
std::vector<char> predict_dirty(const std::vector<Task>& tasks);

struct ExecOptions { int jobs = 0; bool verbose = false; bool keep_going = false; };
struct ExecStats { int ran = 0, uptodate = 0, failed = 0, skipped = 0; };
std::string kind_color(const std::string& k);
int parallel_jobs(int requested = 0);
ExecStats execute(const std::vector<Task>& tasks, const ExecOptions& o);

struct Source { fs::path path; Lang lang; };

struct ModulePlan {
	const ModuleConfig* cfg = nullptr;
	std::vector<Source> sources;
	std::optional<Source> main;
	std::vector<Source> tests;
	std::vector<std::string> closure;
	std::vector<std::string> pc_cflags, pc_libs;
	bool has_cxx = false, has_c = false;
	fs::path artifact, import_lib;
	std::optional<size_t> link_task;
	std::vector<fs::path> objects;
	std::vector<size_t> object_tasks;
	std::optional<size_t> main_task;
	fs::path main_object;
	fs::path test_exe;
	std::optional<size_t> test_task;
};

struct Plan {
	const Project* p = nullptr;
	const toolchain::Toolchain* t = nullptr;
	const ModeConfig* mode = nullptr;
	fs::path build_dir;
	std::vector<Task> tasks;
	std::vector<std::string> order;
	std::vector<std::string> selected;
	std::map<std::string, ModulePlan> mods;
	bool with_tests = false;
};

inline fs::path build_dir_for(const std::string& tc, const std::string& mode) {
	return fs::path("build") / tc / mode;
}
bool glob_match(std::string_view pat, std::string_view s);
bool excluded(const ModuleConfig& m, const fs::path& p);
std::vector<Source> discover(const ModuleConfig& m, const std::vector<std::string>& dirs);
bool platform_matches(const std::string& key, const std::string& os);
fs::path object_path(const Plan& pl, const ModuleConfig& m, const fs::path& src);
std::vector<std::string> pkg_query(const toolchain::Toolchain& t, const std::string& pkg,
								   const std::string& what);
std::string artifact_name(const toolchain::Toolchain& t, const ModuleConfig& m);

class Planner {
public:
	Planner(Plan& pl);
	void run();
private:
	Plan& pl_;
	const Project& p_;
	const toolchain::Toolchain& t_;
	const ModeConfig& mode_;
	std::vector<const PlatformOverride*> overrides(const ModuleConfig& m) const;
	void prepare(const std::string& name);
	bool closure_uses_cxx(const ModulePlan& mp, bool tests) const;
	bool pic(const ModuleConfig& m) const;
	std::vector<std::string> compile_flags(const ModulePlan& mp, Lang lang, bool tests) const;
	size_t add_compile(const ModulePlan& mp, const Source& s, bool tests, fs::path& obj_out);
	void link_libs(const ModulePlan& mp, std::vector<std::string>& cmd,
				   std::vector<fs::path>& inputs, std::vector<size_t>& deps,
				   bool& needs_rpath) const;
	std::vector<std::string> link_prefix(const ModulePlan& mp, bool tests) const;
	void add_rpath(std::vector<std::string>& cmd) const;
	void emit_module(const std::string& name);
	void emit_tests(const std::string& name);
};

Plan make_plan(const Project& p, const toolchain::Toolchain& t, const ModeConfig& mode,
			   const std::vector<std::string>& selected, bool with_tests);
void write_compdb(const Plan& pl, const fs::path& out);
std::string ninja_path(const std::string& s);
std::string ninja_value(const std::string& s);
fs::path write_ninja(const Plan& pl);

} // namespace build

// ============================================================================
// elf
// ============================================================================
namespace elf {

struct Section { std::string name; uint64_t size = 0; uint64_t flags = 0; };

struct Info {
	bool ok = false;
	bool elf = false;
	std::string note;
	std::string cls;
	std::string endian;
	std::string type;
	std::string machine;
	std::string abi;
	uint64_t entry = 0;
	uint64_t text = 0, data = 0, bss = 0;
	uint64_t file_size = 0;
	int members = 0;
	std::vector<Section> sections;
	uint64_t dec() const { return text + data + bss; }
};

uint16_t rd16(const std::string& b, size_t o, bool be);
uint32_t rd32(const std::string& b, size_t o, bool be);
uint64_t rd64(const std::string& b, size_t o, bool be);
const char* machine_name(uint16_t m);
const char* abi_name(unsigned char a);
bool parse_at(const std::string& buf, size_t off, Info& in);
std::string output_section(const std::string& n);
Info read(const fs::path& p);

} // namespace elf

// ============================================================================
// commands
// ============================================================================
namespace commands {

struct Ctx { Project p; std::string cwd_module; };

struct Opts {
	std::string mode;
	std::vector<std::string> modules, toolchains;
	std::string cc, cxx;
	int jobs = 0;
	bool verbose = false, dry_run = false, keep_going = false, all = false, clean = false;
	std::vector<std::string> positional, passthrough;
	std::map<std::string, std::string> values;
	std::set<std::string> flags;
	// --sanitize, --sanitize-trap, --no-sanitize-recover (listes) ; --sanitize-ignorelist
	std::vector<std::string> sanitize, sanitize_trap, no_sanitize_recover;
	std::string sanitize_ignorelist;

	std::string value(const std::string& k, const std::string& def = {}) const {
		auto it = values.find(k);
		return it == values.end() ? def : it->second;
	}
	bool flag(const std::string& k) const { return flags.count(k) > 0; }
};

struct Session {
	std::unique_ptr<toolchain::Toolchain> tc;
	std::unique_ptr<build::Plan> plan;
	util::FileLock lock;
	const ModeConfig* mode = nullptr;
	std::unique_ptr<ModeConfig> derived_mode;   // mode + options --sanitize*
};

} // namespace commands

// ============================================================================
// sanitize — catalogue des sanitizers et options --sanitize*
// ============================================================================
namespace sanitize {

struct Info {
	std::string_view name;      // valeur de -fsanitize=
	std::string_view family;    // famille / restriction (une ligne)
	std::string_view detail;    // explication
	bool clang_only;
	bool runtime;               // runtime complet (exclusif, sauf leak)
};

const std::vector<Info>& catalog();
const Info* find(std::string_view name);
// "-fsanitize=cfi" → "cfi", "asan" → "address"…
std::string normalize(std::string_view raw);
// Sanitizers actifs d'un mode (champ `sanitizer` + flags -fsanitize=).
std::vector<std::string> from_mode(const ModeConfig& m);
// Message d'erreur si la combinaison est impossible, vide sinon.
std::string check_compatible(const std::vector<std::string>& sans);
bool requested(const commands::Opts& o);
// « --sanitize all » : une copie de `o` par runtime (address+undefined, thread,
// leak, memory, hwaddress), sans les passes impossibles pour la toolchain ou
// le mode. Sans « all », renvoie {o}.
bool wants_all(const commands::Opts& o);
std::vector<commands::Opts> expand_all(const Project& p, const commands::Opts& o,
									   const std::string& tc_name, const std::string& mode);
// Mode dérivé : `base` + options --sanitize* ; vérifie la compatibilité.
ModeConfig apply(const ModeConfig& base, const commands::Opts& o, const toolchain::Toolchain& tc);

} // namespace sanitize

namespace commands {

struct TestCaseResult {
	std::string suite, name, status;
	double seconds = 0;
	std::string output;
	int code = 0;
};

// Utilitaires partagés entre sous-commandes
std::vector<int> parse_semver(const std::string& s);
std::optional<fs::path> find_root(const fs::path& from);
Ctx load_ctx(bool validate = true);
int parse_int_arg(const std::string& opt, const std::string& s);
Opts parse_opts(const std::vector<std::string>& args,
				const std::set<std::string>& value_opts = {},
				const std::set<std::string>& bool_opts = {});
std::string pick_mode(const Ctx& c, const Opts& o, const std::string& def = {});
void mode_from_positional(Opts& o);
std::vector<std::string> pick_toolchains(const Ctx& c, const Opts& o, const std::string& def = {});
std::vector<std::string> select_modules(const Ctx& c, const Opts& o);
Session prepare(const Ctx& c, const Opts& o, const std::string& tc_name,
				const std::string& mode, const std::vector<std::string>& modules,
				bool tests, bool lock = true);
void print_header(const Ctx& c, const Session& s);
bool run_generate_steps(const Project& p, bool verbose);
int run_plan(const Ctx& c, Session& s, const Opts& o);
int run_ninja(const Ctx& c, Session& s, const Opts& o);
std::optional<std::vector<std::string>> launcher(const Session& s, std::string& why);

// Utilitaires TOML édition (utilisés par cmd-module et doctor)
std::string replace_toml_array(const std::string& text, const std::string& section,
							   const std::string& key, const std::vector<std::string>& values);
fs::path editable_module_file(const Project& p, const ModuleConfig& m);

// Sous-commandes
int cmd_build(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_test(const std::vector<std::string>& args);
int cmd_check(const std::vector<std::string>& args);
int cmd_generate(const std::vector<std::string>& args);
int cmd_clean(const std::vector<std::string>& args);

int cmd_graph(const std::vector<std::string>& args = {});
int cmd_modules(const std::vector<std::string>& args);
int cmd_info(const std::vector<std::string>& args);
int cmd_size(const std::vector<std::string>& args);
void cmd_new(const std::vector<std::string>& rest);
int cmd_add(const std::vector<std::string>& args);
int cmd_module(const std::vector<std::string>& args);
int cmd_deps(const std::vector<std::string>& args);

int cmd_package(const std::vector<std::string>& args);
int cmd_install(const std::vector<std::string>& args = {});
int cmd_deploy(const std::vector<std::string>& args = {});

int cmd_doctor(const std::vector<std::string>& args = {});
int cmd_config(const std::vector<std::string>& args);
int cmd_toolchain(const std::vector<std::string>& args);

int cmd_backup(const std::vector<std::string>& args);

void write_junit(const fs::path& file, const std::vector<TestCaseResult>& results);
int do_test(const Ctx& c, const Opts& o, const std::string& mode,
			std::vector<TestCaseResult>& results);
int print_test_summary(const std::vector<TestCaseResult>& results);

void print_toolchain(const toolchain::Toolchain& t, const std::string& indent = "  ");

} // namespace commands

// main.cpp
void print_help();