// output.cpp — options globales --output / --stdout / --stderr / --filter.
//
// Principe : dès qu'au moins une option est active, stdout et/ou stderr sont
// redirigés vers des pipes internes. Un unique thread lecteur (poll sur les
// deux pipes) route chaque ligne vers les fichiers demandés, en appliquant
// éventuellement les filtres --filter/--grep. Un fichier peut servir de
// destination à plusieurs flux (--output partage un même fd entre stdout et
// stderr pour garantir l'entrelacement sans corruption).
#include "cpp-maker.hpp"

namespace output {

namespace {

struct Filter {
    std::string text;
    std::optional<std::regex> re;   // absent → recherche littérale
    bool match(const std::string& line) const {
        return re ? std::regex_search(line, *re) : line.find(text) != std::string::npos;
    }
};

struct State {
    std::vector<Filter> filters;
    std::vector<fs::path> stdout_paths;   // fichiers destination de stdout
    std::vector<fs::path> stderr_paths;   // fichiers destination de stderr
    std::vector<int> stdout_fds;
    std::vector<int> stderr_fds;
    bool stdout_to_terminal = false;      // recopie vers le terminal (si filtre + pas de fichier)
    bool stderr_to_terminal = false;
    bool out_active = false;              // stdout passe par un pipe
    bool err_active = false;              // stderr passe par un pipe
    int saved_out = -1, saved_err = -1;
    std::thread reader;
    size_t lines_total = 0, lines_kept = 0;
    bool active = false;
};

State& state() { static State s; return s; }

void write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0) { if (errno == EINTR) continue; return; }
        s.remove_prefix((size_t)n);
    }
}

void emit(State& s, const std::string& line, bool is_stderr, bool newline) {
    std::string plain = util::strip_ansi(line);
    if (!plain.empty() && plain.back() == '\r') plain.pop_back();
    ++s.lines_total;
    bool keep = s.filters.empty() ||
                std::any_of(s.filters.begin(), s.filters.end(),
                            [&](const Filter& f) { return f.match(plain); });
    if (!keep) return;
    ++s.lines_kept;
    if (newline) plain += '\n';
    if (is_stderr) {
        for (int fd : s.stderr_fds) write_all(fd, plain);
        if (s.stderr_to_terminal) write_all(s.saved_err, plain);
    } else {
        for (int fd : s.stdout_fds) write_all(fd, plain);
        if (s.stdout_to_terminal) write_all(s.saved_out, plain);
    }
}

void drain(State& s, int fd, bool is_stderr, std::string& pending) {
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            pending.append(buf, (size_t)n);
            size_t start = 0;
            for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
                emit(s, pending.substr(start, nl - start), is_stderr, true);
            pending.erase(0, start);
            continue;
        }
        if (n == 0) return;
        if (errno == EINTR) continue;
        return;
    }
}

void read_loop(int fd_out, int fd_err) {
    auto& s = state();
    std::string pending[2];
    bool eof[2] = {fd_out < 0, fd_err < 0};
    while (!eof[0] || !eof[1]) {
        struct pollfd pfds[2];
        int nfds = 0, idx[2] = {-1, -1};
        if (!eof[0]) { pfds[nfds] = {fd_out, POLLIN, 0}; idx[0] = nfds++; }
        if (!eof[1]) { pfds[nfds] = {fd_err, POLLIN, 0}; idx[1] = nfds++; }
        int pr = poll(pfds, nfds, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        for (int st = 0; st < 2; ++st) {
            if (idx[st] < 0) continue;
            short rev = pfds[idx[st]].revents;
            if (!(rev & (POLLIN | POLLHUP | POLLERR))) continue;
            int fd = st == 0 ? fd_out : fd_err;
            drain(s, fd, st == 1, pending[st]);
            // Un drain qui ne trouve plus rien alors que POLLHUP est posé → EOF.
            if (rev & (POLLHUP | POLLERR)) eof[st] = true;
        }
    }
    if (!pending[0].empty()) emit(s, pending[0], false, false);
    if (!pending[1].empty()) emit(s, pending[1], true, false);
    if (fd_out >= 0) ::close(fd_out);
    if (fd_err >= 0) ::close(fd_err);
}

void finish() {
    auto& s = state();
    if (!s.active) return;
    s.active = false;
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    // Ferme les extrémités d'écriture côté cpp-maker : le lecteur voit EOF
    // dès que les enfants ont rendu la main.
    if (s.out_active) ::dup2(s.saved_out, STDOUT_FILENO);
    if (s.err_active) ::dup2(s.saved_err, STDERR_FILENO);
    if (s.reader.joinable()) s.reader.join();
    std::set<int> all_fds;
    for (int fd : s.stdout_fds) all_fds.insert(fd);
    for (int fd : s.stderr_fds) all_fds.insert(fd);
    for (int fd : all_fds) ::close(fd);

    // Résumé sur le stderr d'origine (le pipe est déjà fermé).
    std::set<std::string> stdout_set, stderr_set;
    for (auto& p : s.stdout_paths) stdout_set.insert(p.string());
    for (auto& p : s.stderr_paths) stderr_set.insert(p.string());
    std::set<std::string> uniq;
    for (auto& x : stdout_set) uniq.insert(x);
    for (auto& x : stderr_set) uniq.insert(x);
    if (!uniq.empty()) {
        std::string msg = s.filters.size()
            ? std::format("sortie filtrée ({} ligne(s) sur {}) :", s.lines_kept, s.lines_total)
            : std::format("sortie écrite ({} ligne(s)) :", s.lines_kept);
        for (auto& p : uniq) {
            bool in_out = stdout_set.count(p), in_err = stderr_set.count(p);
            const char* label = (in_out && in_err) ? "stdout+stderr"
                              : in_out ? "stdout" : "stderr";
            msg += std::format("\n  {} ({})", p, label);
        }
        write_all(s.saved_err, msg + "\n");
    }
    if (s.saved_out >= 0) ::close(s.saved_out);
    if (s.saved_err >= 0) ::close(s.saved_err);
}

} // namespace

bool any_filter_active() {
    return !state().filters.empty();
}

// Après chargement du projet : réécrit les chemins sans répertoire
// en préfixant par la valeur `[paths] out_dir` du projet.
void rebind_paths(const Project& p) {
    auto& s = state();
    if (p.out_dir.empty() || p.out_dir == ".out") return;   // défaut déjà appliqué
    auto rebase = [&](std::vector<fs::path>& v) {
        for (auto& x : v) {
            if (x.is_absolute()) continue;
            // Ne rebase que si le chemin a été auto-préfixé par .out/
            if (x.has_parent_path() && x.parent_path() == ".out") {
                fs::path name = x.filename();
                x = fs::path(p.out_dir) / name;
            }
        }
    };
    rebase(s.stdout_paths);
    rebase(s.stderr_paths);
}

std::vector<std::string> extract_options(std::vector<std::string>& args, bool filter_is_global,
                                          const std::string& out_dir) {
    auto& s = state();
    std::vector<std::string> patterns;
    std::vector<std::string> rest;
    auto resolve = [&](const std::string& val) -> fs::path {
        fs::path p = val;
        if (!p.is_absolute() && !p.has_parent_path())
            p = fs::path(out_dir) / p;
        return p;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        if (a == "--" || a == "--args") {
            rest.insert(rest.end(), args.begin() + (long)i, args.end());
            break;
        }
        std::string name = a, val;
        bool has_eq = a.starts_with("--") && a.find('=') != std::string::npos;
        if (has_eq) { name = a.substr(0, a.find('=')); val = a.substr(a.find('=') + 1); }
        bool is_output = name == "--output";
        bool is_stdout = name == "--stdout";
        bool is_stderr = name == "--stderr";
                bool is_filter = name == "--grep" || (filter_is_global && name == "--filter");
        if (!is_output && !is_stdout && !is_stderr && !is_filter) {
            rest.push_back(args[i]);
            continue;
        }
        if (!has_eq) {
            if (i + 1 >= args.size()) util::die("valeur manquante pour " + name);
            val = args[++i];
        }
        if (val.empty()) util::die(name + " : nom de fichier vide");
        if (is_filter) {
            patterns.push_back(val);
        } else if (is_output) {
            // --output : un seul fd, partagé stdout+stderr → entrelacement
            // préservé et pas de duplication dans le fichier.
            fs::path p = resolve(val);
            s.stdout_paths.push_back(p);
            s.stderr_paths.push_back(p);
        } else if (is_stdout) {
            s.stdout_paths.push_back(resolve(val));
        } else { // --stderr
            s.stderr_paths.push_back(resolve(val));
        }
    }
    args = std::move(rest);
    return patterns;
}

void setup(const std::vector<std::string>& patterns) {
    auto& s = state();
    for (auto& p : patterns) {
        Filter f{p, std::nullopt};
        try { f.re.emplace(p); } catch (const std::regex_error&) {}  // motif littéral
        s.filters.push_back(std::move(f));
    }
    // Un filtre sans aucun fichier destination : on garde le terminal
    // comme destination, en appliquant quand même le filtre.
    bool want_out = !s.stdout_paths.empty() || (!s.filters.empty() && s.stderr_paths.empty()
                                                  && s.stdout_paths.empty());
    bool want_err = !s.stderr_paths.empty() || (!s.filters.empty() && s.stdout_paths.empty()
                                                  && s.stderr_paths.empty());
    if (!want_out && !want_err && s.filters.empty()) return;
#ifdef __unix__
    // Calcule la couleur AVANT redirection : fichier ⇒ pas de couleurs.
    if (s.stdout_paths.empty() && s.stderr_paths.empty()) (void)util::color();

    // Ouvre (ou réutilise) un fd par fichier destination. Le même fichier
    // demandé pour stdout et stderr (cas --output) n'est ouvert qu'une fois.
    std::map<std::string, int> fds_by_path;
    auto open_for = [&](const std::vector<fs::path>& paths, std::vector<int>& out) {
        for (auto& p : paths) {
            auto key = p.string();
            auto it = fds_by_path.find(key);
            if (it != fds_by_path.end()) { out.push_back(it->second); continue; }
            if (p.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
            }
            int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0)
                util::die(std::format("{} : impossible d'ouvrir {} : {}",
                                      paths.size() && &paths == &s.stdout_paths ? "--stdout" : "--stderr",
                                      p.string(), std::strerror(errno)));
            fds_by_path[key] = fd;
            out.push_back(fd);
        }
    };
    open_for(s.stdout_paths, s.stdout_fds);
    open_for(s.stderr_paths, s.stderr_fds);

    // Filtre sans fichier destination : on réinjecte les lignes filtrées
    // sur le terminal (sinon --filter seul ne servirait à rien).
    s.stdout_to_terminal = s.stdout_paths.empty() && !s.filters.empty();
    s.stderr_to_terminal = s.stderr_paths.empty() && !s.filters.empty();
    // Mais si --stdout fourni sans --stderr, on ne double pas stderr
    // sur le terminal : stderr continue à sortir normalement (non redirigé).
    s.stderr_to_terminal = s.stderr_to_terminal && s.stderr_paths.empty() && s.stdout_paths.empty();

    s.saved_out = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    s.saved_err = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);

    int pfd_out[2] = {-1, -1}, pfd_err[2] = {-1, -1};
    if (want_out) {
        if (::pipe(pfd_out) != 0) util::die(std::string("pipe (stdout) : ") + std::strerror(errno));
        ::fcntl(pfd_out[0], F_SETFD, FD_CLOEXEC);
    }
    if (want_err) {
        if (::pipe(pfd_err) != 0) util::die(std::string("pipe (stderr) : ") + std::strerror(errno));
        ::fcntl(pfd_err[0], F_SETFD, FD_CLOEXEC);
    }
    std::cout.flush();
    std::cerr.flush();
    if (want_out) {
        ::dup2(pfd_out[1], STDOUT_FILENO);
        ::close(pfd_out[1]);
        s.out_active = true;
    }
    if (want_err) {
        ::dup2(pfd_err[1], STDERR_FILENO);
        ::close(pfd_err[1]);
        s.err_active = true;
    }
    s.reader = std::thread(read_loop,
                           want_out ? pfd_out[0] : -1,
                           want_err ? pfd_err[0] : -1);
    s.active = true;
    std::atexit(finish);
#else
    util::die("--output / --stdout / --stderr / --filter ne sont pas encore supportés sur cette plate-forme");
#endif
}

} // namespace output