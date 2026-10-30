#include "cpp-maker.hpp"

namespace proc {
#if defined(_WIN32)
#include <windows.h>

Result run(const std::vector<std::string>& args, const Options& o) {
    Result r;
    if (args.empty()) { r.exit_code = 127; return r; }

    // Ligne de commande : chaque argument entre guillemets, échappement \"
    std::wstring cmdline;
    for (auto& a : args) {
        std::wstring w(a.begin(), a.end());
        cmdline += L'"';
        for (wchar_t c : w) { if (c == L'"') cmdline += L'\\'; cmdline += c; }
        cmdline += L"\" ";
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    si.dwFlags = STARTF_USESTDHANDLES;

    // Redirection optionnelle vers un pipe (capture)
    HANDLE rd = nullptr, wr = nullptr;
    if (o.capture) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = wr; si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::wstring cwd = o.cwd ? std::wstring(o.cwd->wstring()) : std::wstring();
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             0, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                             &si, &pi);
    if (!ok) { r.exit_code = 127; return r; }
    if (o.capture) CloseHandle(wr);

    if (o.capture) {
        char buf[8192]; DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0)
            r.output.append(buf, n);
        CloseHandle(rd);
    }

    if (o.timeout_sec > 0)
        WaitForSingleObject(pi.hProcess, (DWORD)o.timeout_sec * 1000);
    else
        WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = (int)code;
    if (o.timeout_sec > 0 && code == STILL_ACTIVE) {
        TerminateProcess(pi.hProcess, 1);
        r.timed_out = true;
        r.exit_code = 124;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return r;
}

#elif defined(__unix__)

Result run(const std::vector<std::string>& args, const Options& o) {
	Result r;
	if (args.empty()) { r.exit_code = 127; r.output = "empty command"; return r; }
	if (!o.capture) { std::cout.flush(); std::cerr.flush(); }

	std::vector<char*> argv;
	argv.reserve(args.size() + 1);
	for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
	argv.push_back(nullptr);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	auto cleanup = [&] {
		posix_spawn_file_actions_destroy(&fa);
		posix_spawnattr_destroy(&attr);
	};

	if (o.timeout_sec > 0) {
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
		posix_spawnattr_setpgroup(&attr, 0);
	}
	if (o.cwd) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
		posix_spawn_file_actions_addchdir_np(&fa, o.cwd->c_str());
#else
		cleanup();
		r.exit_code = 127; r.output = "changement de répertoire non supporté"; return r;
#endif
	}

	int pfd[2] = {-1, -1};
	if (o.capture) {
		if (pipe(pfd) != 0) {
			cleanup();
			r.exit_code = 127; r.output = "pipe failed"; return r;
		}
		posix_spawn_file_actions_addclose(&fa, pfd[0]);
		posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&fa, pfd[1], STDERR_FILENO);
		posix_spawn_file_actions_addclose(&fa, pfd[1]);
	}

	std::vector<std::string> env_storage;
	std::vector<char*> envp;
	if (!o.env.empty()) {
		for (char** e = environ; *e; ++e) {
			std::string_view kv(*e);
			auto eq = kv.find('=');
			std::string_view key = kv.substr(0, eq);
			bool overridden = std::any_of(o.env.begin(), o.env.end(),
										  [&](auto& p) { return p.first == key; });
			if (!overridden) env_storage.emplace_back(kv);
		}
		for (auto& [k, v] : o.env) env_storage.push_back(k + "=" + v);
		for (auto& s : env_storage) envp.push_back(s.data());
		envp.push_back(nullptr);
	}

	pid_t pid = -1;
	int rc = posix_spawnp(&pid, args[0].c_str(), &fa, &attr, argv.data(),
						  o.env.empty() ? environ : envp.data());
	cleanup();
	if (o.capture) close(pfd[1]);

	if (rc != 0) {
		if (pfd[0] >= 0) close(pfd[0]);
		r.exit_code = 127;
		r.output = std::format("spawn '{}' failed: {}", args[0], std::strerror(rc));
		return r;
	}

	using clock = std::chrono::steady_clock;
	auto deadline = clock::now() + std::chrono::seconds(o.timeout_sec);
	bool eof = !o.capture;
	bool exited = false;
	int status = 0;
	char buf[8192];

	auto read_some = [&](int wait_ms) {
		pollfd p{pfd[0], POLLIN, 0};
		int pr = poll(&p, 1, wait_ms);
		if (pr <= 0) return false;
		ssize_t n = ::read(pfd[0], buf, sizeof buf);
		if (n > 0) { r.output.append(buf, (size_t)n); return true; }
		if (n == 0 || (errno != EINTR && errno != EAGAIN)) eof = true;
		return false;
	};

	while (!(exited && eof)) {
		if (!eof && !exited) read_some(50);
		if (!exited) {
			bool block = eof && o.timeout_sec <= 0;
			pid_t w = waitpid(pid, &status, block ? 0 : WNOHANG);
			if (w == pid) exited = true;
			else if (w < 0 && errno != EINTR) { exited = true; status = 0; }
		}
		if (exited && !eof) {
			while (!eof && read_some(0)) {}
			eof = true;
		}
		if (!exited && o.timeout_sec > 0 && clock::now() > deadline) {
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
			r.timed_out = true;
			exited = true;
		}
		if (!exited && eof && o.timeout_sec > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (pfd[0] >= 0) close(pfd[0]);

	if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status)) {
		r.signaled = true; r.signal = WTERMSIG(status);
		r.exit_code = 128 + r.signal;
	}
	if (r.timed_out) r.exit_code = 124;
	return r;
}

#else

Result run(const std::vector<std::string>& args, const Options& o) {
	Result r;
	if (args.empty()) { r.exit_code = 127; return r; }
	std::string cmd;
	for (size_t i = 0; i < args.size(); ++i) {
		if (i) cmd += ' ';
		cmd += '"' + args[i] + '"';
	}
	if (o.capture) cmd += " 2>&1";
	FILE* p = _popen(cmd.c_str(), "r");
	if (!p) { r.exit_code = 127; return r; }
	char buf[4096];
	while (fgets(buf, sizeof buf, p)) { if (o.capture) r.output += buf; else std::fputs(buf, stdout); }
	r.exit_code = _pclose(p);
	return r;
}
#endif

} // namespace proc