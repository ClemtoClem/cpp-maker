#include "cpp-maker.hpp"

namespace util {

std::vector<std::string> split_ws(std::string_view s) {
	std::vector<std::string> r;
	std::string cur;
	for (char c : s) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (!cur.empty()) { r.push_back(cur); cur.clear(); }
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) r.push_back(cur);
	return r;
}

std::optional<fs::path> find_in_path(const std::string& name) {
	if (name.empty()) return std::nullopt;
	auto executable = [](const fs::path& p) {
		std::error_code ec;
		if (!fs::is_regular_file(p, ec)) return false;
#ifdef __unix__
		return ::access(p.c_str(), X_OK) == 0;
#else
		return true;
#endif
	};
	if (name.find('/') != std::string::npos) {
		if (executable(name)) return fs::path(name);
		return std::nullopt;
	}
	const char* env = std::getenv("PATH");
	std::string path = env ? env : "/usr/local/bin:/usr/bin:/bin";
	for (auto& dir : split_list(path, path_list_sep())) {
		fs::path cand = fs::path(dir) / name;
		if (executable(cand)) return cand;
	}
	return std::nullopt;
}

std::string shell_quote(const std::string& a) {
	if (a.empty()) return "''";
	bool plain = std::all_of(a.begin(), a.end(), [](unsigned char c) {
		return std::isalnum(c) || std::strchr("_-+=/.,:@%^", c) != nullptr;
	});
	if (plain) return a;
	return "'" + replace_all(a, "'", "'\\''") + "'";
}
std::string shell_join(const std::vector<std::string>& v) {
	std::string r;
	for (size_t i = 0; i < v.size(); ++i) { if (i) r += ' '; r += shell_quote(v[i]); }
	return r;
}

std::string json_escape(std::string_view s) {
	std::string r;
	for (char ch : s) {
		auto c = static_cast<unsigned char>(ch);
		switch (c) {
			case '"':  r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\n': r += "\\n"; break;
			case '\r': r += "\\r"; break;
			case '\t': r += "\\t"; break;
			default:
				if (c < 0x20) r += std::format("\\u{:04x}", c);
				else r += (char)c;
		}
	}
	return r;
}

std::string xml_escape(std::string_view s) {
	std::string r;
	for (char ch : s) {
		auto c = static_cast<unsigned char>(ch);
		switch (c) {
			case '<': r += "&lt;"; break;
			case '>': r += "&gt;"; break;
			case '&': r += "&amp;"; break;
			case '"': r += "&quot;"; break;
			default:
				if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') r += '?';
				else r += (char)c;
		}
	}
	return r;
}

bool read_file(const fs::path& p, std::string& out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign((std::istreambuf_iterator<char>(f)), {});
	return true;
}

bool write_file_atomic(const fs::path& p, const std::string& content) {
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(content.data(), (std::streamsize)content.size());
		if (!f) return false;
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);
	if (ec) { fs::remove(tmp, ec); return false; }
	return true;
}

void write_file(const fs::path& p, const std::string& content) {
	std::error_code ec;
	if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) die("impossible d'écrire " + p.string());
	f << content;
	if (!f) die("échec d'écriture de " + p.string());
}

} // namespace util