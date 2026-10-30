// flags.hpp — traduction des options cpp-maker vers chaque compilateur.
#pragma once
#include "cpp-maker.hpp"

namespace flags {

// --- Include ---------------------------------------------------------------
inline std::string include(toolchain::CompilerFamily f, const fs::path& dir) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "/I" + dir.string();
        default:                    return "-I" + dir.string();  // GCC, Clang, TCC
    }
}

// --- Define ----------------------------------------------------------------
inline std::string define(toolchain::CompilerFamily f, std::string_view d) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "/D" + std::string(d);
        default:                    return "-D" + std::string(d);
    }
}

// --- Standard --------------------------------------------------------------
inline std::string standard_cxx(toolchain::CompilerFamily f, std::string_view std) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc: {
            // MSVC utilise /std:c++20 (pas de c++23 avant VS 17.10+)
            std::string v(std);
            if (v.starts_with("c++")) v = "c++" + v.substr(3);
            if (v.starts_with("gnu++")) v = "c++" + v.substr(5);
            return "/std:" + v;
        }
        case toolchain::CompilerFamily::Tcc:
            return "";  // TCC ne supporte pas -std= de manière fiable
        default:
            return "-std=" + std::string(std);
    }
}
inline std::string standard_c(toolchain::CompilerFamily f, std::string_view std) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "";  // /std:c11 /std:c17 récents seulement
        case toolchain::CompilerFamily::Tcc:   return "";
        default:                    return "-std=" + std::string(std);
    }
}

// --- Sortie objet / exécutable --------------------------------------------
inline std::string object_output(toolchain::CompilerFamily f, const fs::path& obj) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "/Fo" + obj.string();
        default:                    return "-o " + obj.string();  // sera split
    }
}
inline std::string exe_output(toolchain::CompilerFamily f, const fs::path& exe) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "/Fe" + exe.string();
        default:                    return "-o " + exe.string();
    }
}

// --- Avertissements --------------------------------------------------------
inline std::vector<std::string> warnings(toolchain::CompilerFamily f,
                                         const std::vector<std::string>& gcc_style) {
    if (f != toolchain::CompilerFamily::Msvc) return gcc_style;
    // Traduction minimale : -Wall -Wextra → /W4 ; le reste est ignoré.
    std::vector<std::string> out;
    for (auto& w : gcc_style) {
        if (w == "-Wall" || w == "-Wextra") out.push_back("/W4");
        else if (w == "-Wpedantic")          out.push_back("/permissive-");
        else if (w.starts_with("-Wno-"))     out.push_back("/wd" + w.substr(5)); // approximation
    }
    return out;
}

// --- Link ------------------------------------------------------------------
inline std::string lib_dir(toolchain::CompilerFamily f, const fs::path& dir) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return "/LIBPATH:" + dir.string();
        default:                    return "-L" + dir.string();
    }
}
inline std::string lib(toolchain::CompilerFamily f, std::string_view name) {
    switch (f) {
        case toolchain::CompilerFamily::Msvc:  return name.ends_with(".lib") ? std::string(name)
                                                                  : std::string(name) + ".lib";
        default:                    return "-l" + std::string(name);
    }
}

// --- Flags par défaut selon plateforme -------------------------------------
inline std::vector<std::string> platform_link_defaults(toolchain::Platform p,
                                                       toolchain::CompilerFamily f) {
    std::vector<std::string> r;
    switch (p) {
        case toolchain::Platform::Linux:
            if (f == toolchain::CompilerFamily::Gcc || f == toolchain::CompilerFamily::Clang)
                r.push_back("-pthread");
            break;
        case toolchain::Platform::Freebsd:
            r.push_back("-L/usr/local/lib");
            r.push_back("-I/usr/local/include");
            break;
        case toolchain::Platform::Macos:
            // Les frameworks se lient via -framework ; à déclarer dans le module.
            break;
        case toolchain::Platform::Windows:
            if (f == toolchain::CompilerFamily::Msvc) {
                r.push_back("kernel32.lib");
                r.push_back("user32.lib");
            }
            break;
        default: break;
    }
    return r;
}

} // namespace flags