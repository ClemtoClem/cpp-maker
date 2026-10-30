#include "cpp-maker.hpp"
#include "cmd-helpers.hpp"

void print_help() {
    util::outln(R"(cpp-maker {} — moteur de production unifié pour C et C++

USAGE
    cpp-maker <commande> [options]
    cpp-maker help                liste les commandes
    cpp-maker help <commande>     aide détaillée (usage, alias, exemples)
    cpp-maker <commande> --help   idem, directement depuis la commande

PROJET
    new / add / module / deps / info / modules / graph / size
    config show | validate | set
    doctor

CYCLE DE VIE
    build | run | test | check | package | install | deploy
    generate ninja|compdb | clean

TOOLCHAINS
    toolchain list | show | which | use | install | remove

OPTIONS DE SORTIE (toutes commandes, cumulables)
    --output <f>   stdout + stderr combinés dans <f>
    --stdout <f>   stdout seul dans <f>
    --stderr <f>   stderr seul dans <f>
    --file <f>     fichier d'export spécifique à la commande (backup, …)
    --filter <m>   ne garde que les lignes correspondant à <m> (regex ou littéral)
    --grep <m>     synonyme de --filter (pratique pour test/check)

    Les chemins sans répertoire sont préfixés par [paths] out_dir (.out par défaut).
    Les trois options --output/--stdout/--stderr sont indépendantes :
    cpp-maker build -m ui --output all.log --stderr err.log --stdout out.log
    → all.log (les deux flux), out.log (stdout), err.log (stderr).

Dépendances git (voir cpp-maker help complet) :
    [dependencies.git.<alias>]
    url = "https://…"
    version = "v2.1.0"
)", CPP_MAKER_VERSION);
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    // Options globales placées AVANT la commande : `cpp-maker --output log build`.
    size_t lead = 0;
    while (lead < args.size()) {
        const std::string& a = args[lead];
        auto is = [&](std::string_view n) { return a == n || a.starts_with(std::string(n) + "="); };
        if (!is("--output") && !is("--filter") && !is("--grep")) break;
        lead += a.find('=') != std::string::npos ? size_t{1} : size_t{2};
    }
    lead = std::min(lead, args.size());
    std::vector<std::string> leading(args.begin(), args.begin() + (long)lead);
    auto patterns = output::extract_options(leading, true);
    args.erase(args.begin(), args.begin() + (long)lead);
    if (!leading.empty()) util::die("argument inattendu : " + leading[0]);

    if (args.empty()) {
        if (!patterns.empty()) util::die("--output / --filter : commande manquante");
        commands::help::print_list();
        return 0;
    }
    if (args[0] == "-h" || args[0] == "--help") { print_help(); return 0; }
    if (args[0] == "--version" || args[0] == "version") {
        util::outln("cpp-maker {}", CPP_MAKER_VERSION);
        return 0;
    }
    if (args[0] == "help") {
        std::vector<std::string> sub(args.begin() + 1, args.end());
        return commands::help::cmd_help(sub);
    }

    // Alias : `cpp-maker valgrind …` → `cpp-maker run --mode valgrind -- …`
    if (args[0] == "valgrind") {
        std::vector<std::string> a = {"--mode", "valgrind", "--"};
        std::vector<std::string> rest(args.begin() + 1, args.end());
        auto more = output::extract_options(rest, true);
        patterns.insert(patterns.end(), more.begin(), more.end());
        a.insert(a.end(), rest.begin(), rest.end());
        output::setup(patterns);
        return commands::cmd_run(a);
    }

    auto* spec = commands::help::find(args[0]);
    if (!spec) util::die("commande inconnue : " + args[0] + " (cpp-maker help list)");

    std::vector<std::string> rest(args.begin() + 1, args.end());

    // `--help` / `-h` sur n'importe quelle commande, AVANT parse_opts
    // (sinon "--help" serait rejeté comme option inconnue).
    if (std::find(rest.begin(), rest.end(), "--help") != rest.end() ||
        std::find(rest.begin(), rest.end(), "-h")     != rest.end()) {
        commands::help::print_command(*spec);
        return 0;
    }

    // Pour test/check, `--filter` après la commande garde son sens historique
    // (filtre sur les noms de tests) ; `--grep` filtre alors la sortie.
    bool filter_is_global = spec->name != "test" && spec->name != "check";
    auto more = output::extract_options(rest, filter_is_global);
    patterns.insert(patterns.end(), more.begin(), more.end());
    output::setup(patterns);

    try {
        return spec->run(rest);
    } catch (const std::exception& e) {
        util::die(e.what());
    }
}