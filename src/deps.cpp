#include "cpp-maker.hpp"

namespace deps {

fs::path cache_root() {
	if (const char* e = std::getenv("CPP_MAKER_CACHE"))  return fs::path(e) / "git";
	if (const char* e = std::getenv("XDG_CACHE_HOME"))   return fs::path(e) / "cpp-maker" / "git";
#ifdef _WIN32
	if (const char* e = std::getenv("LOCALAPPDATA"))     return fs::path(e) / "cpp-maker" / "git";
#else
	if (const char* e = std::getenv("HOME"))             return fs::path(e) / ".cache" / "cpp-maker" / "git";
#endif
	return fs::temp_directory_path() / "cpp-maker-git";
}

std::string url_key(const std::string& url) {
	util::Hasher h; h.feed(url); return h.hex();
}

fs::path bare_repo(const std::string& url) {
	return cache_root() / (url_key(url) + ".git");
}

fs::path checkout_path(const std::string& url, const std::string& commit) {
	return cache_root() / url_key(url) / commit.substr(0, 16);
}

Resolved resolve(const std::string& url, const std::string& version,
				 const std::string& lock_commit) {
	Resolved r;
	if (!util::have("git")) { r.error = "git introuvable dans le PATH"; return r; }

	fs::path repo = bare_repo(url);
	util::FileLock flock;
	flock.acquire(repo.string() + ".lock");

	std::error_code ec;
	if (!fs::exists(repo / "HEAD", ec)) {
		fs::create_directories(repo.parent_path(), ec);
		util::outln("\033[1;34m[cpp-maker]\033[0m git clone {} …", url);
		auto c = proc::capture({"git", "clone", "--mirror", "--quiet", url, repo.string()});
		if (c.exit_code != 0) {
			fs::remove_all(repo, ec);
			r.error = "git clone a échoué : " + util::trim(c.output);
			return r;
		}
	}

	auto try_rev = [&](const std::string& ref) -> std::string {
		auto rv = proc::capture({"git", "-C", repo.string(), "rev-parse", "--verify", "-q",
								 ref + "^{commit}"});
		return rv.exit_code == 0 ? util::trim(rv.output) : std::string{};
	};

	std::string commit;
	if (!lock_commit.empty()) commit = try_rev(lock_commit);
	if (commit.empty())       commit = try_rev(version);

	if (commit.empty()) {
		auto f = proc::capture({"git", "-C", repo.string(), "remote", "update", "--prune"});
		if (f.exit_code != 0) { r.error = "git fetch a échoué : " + util::trim(f.output); return r; }
		if (!lock_commit.empty()) commit = try_rev(lock_commit);
		if (commit.empty())       commit = try_rev(version);
	}
	if (commit.empty()) {
		r.error = std::format("révision '{}' introuvable dans {}", version, url);
		return r;
	}

	fs::path co = checkout_path(url, commit);
	if (!fs::exists(co / ".cpp-maker-checkout", ec)) {
		fs::remove_all(co, ec);
		fs::create_directories(co.parent_path(), ec);
		(void)proc::capture({"git", "-C", repo.string(), "worktree", "prune"});
		auto w = proc::capture({"git", "-C", repo.string(), "worktree", "add",
								"--detach", "--force", co.string(), commit});
		if (w.exit_code != 0) {
			r.error = "worktree add a échoué : " + util::trim(w.output);
			return r;
		}
		util::write_file(co / ".cpp-maker-checkout", commit + "\n");
	}
	r.ok = true; r.commit = commit; r.path = co;
	return r;
}

} // namespace deps