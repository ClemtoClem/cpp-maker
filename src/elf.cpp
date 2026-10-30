#include "cpp-maker.hpp"

namespace elf {

uint16_t rd16(const std::string& b, size_t o, bool be) {
	if (o + 2 > b.size()) return 0;
	auto u = (unsigned char)b[o];
	auto v = (unsigned char)b[o + 1];
	return be ? (uint16_t)((u << 8) | v) : (uint16_t)((v << 8) | u);
}
uint32_t rd32(const std::string& b, size_t o, bool be) {
	if (o + 4 > b.size()) return 0;
	uint32_t r = 0;
	for (size_t i = 0; i < 4; ++i)
		r = (r << 8) | (unsigned char)b[o + (be ? i : size_t(3) - i)];
	return r;
}
uint64_t rd64(const std::string& b, size_t o, bool be) {
	if (o + 8 > b.size()) return 0;
	uint64_t r = 0;
	for (size_t i = 0; i < 8; ++i)
		r = (r << 8) | (unsigned char)b[o + (be ? i : size_t(7) - i)];
	return r;
}

const char* machine_name(uint16_t m) {
	switch (m) {
		case 3:   return "x86";
		case 8:   return "MIPS";
		case 20:  return "PowerPC";
		case 21:  return "PowerPC64";
		case 40:  return "ARM";
		case 50:  return "IA-64";
		case 62:  return "x86-64";
		case 183: return "AArch64";
		case 243: return "RISC-V";
		case 247: return "LoongArch";
		default:  return nullptr;
	}
}

const char* abi_name(unsigned char a) {
	switch (a) {
		case 0:  return "UNIX System V";
		case 2:  return "HP-UX";
		case 3:  return "Linux";
		case 6:  return "Solaris";
		case 9:  return "FreeBSD";
		case 12: return "OpenBSD";
		default: return "inconnu";
	}
}

// Analyse un fichier ELF dans buf à l'offset `off` (début d'en-tête).
// Remplit in ; retourne false si ce n'est pas un ELF.
bool parse_at(const std::string& buf, size_t off, Info& in) {
	if (off + 16 > buf.size()) return false;
	if ((unsigned char)buf[off] != 0x7f || buf[off + 1] != 'E' ||
		buf[off + 2] != 'L' || buf[off + 3] != 'F') return false;
	bool is64 = (unsigned char)buf[off + 4] == 2;
	bool be = (unsigned char)buf[off + 5] == 2;
	in.elf = true;
	in.cls = is64 ? "ELF64" : "ELF32";
	in.endian = be ? "big endian" : "little endian";
	in.abi = abi_name((unsigned char)buf[off + 7]);

	uint16_t e_type = rd16(buf, off + 16, be);
	uint16_t e_machine = rd16(buf, off + 18, be);
	switch (e_type) {
		case 1: in.type = "REL (relocalisable)"; break;
		case 2: in.type = "EXEC (exécutable)"; break;
		case 3: in.type = "DYN (position indépendante — PIE ou .so)"; break;
		case 4: in.type = "CORE (core dump)"; break;
		default: in.type = std::format("type {}", e_type); break;
	}
	if (auto* n = machine_name(e_machine)) in.machine = n;
	else in.machine = std::format("machine {}", e_machine);

	uint64_t shoff;
	uint16_t shentsize, shnum, shstrndx;
	if (is64) {
		in.entry = rd64(buf, off + 24, be);
		shoff = rd64(buf, off + 40, be);
		shentsize = rd16(buf, off + 58, be);
		shnum = rd16(buf, off + 60, be);
		shstrndx = rd16(buf, off + 62, be);
	} else {
		in.entry = rd32(buf, off + 24, be);
		shoff = rd32(buf, off + 32, be);
		shentsize = rd16(buf, off + 46, be);
		shnum = rd16(buf, off + 48, be);
		shstrndx = rd16(buf, off + 50, be);
	}
	in.ok = true;
	// Les offsets de l'en-tête sont relatifs au début de CE fichier ELF : pour
	// un membre d'archive ar, il faut leur ajouter `off`.
	if (shoff == 0 || shnum == 0 || shentsize == 0 || off + shoff > buf.size()) return true;

	// En-tête de section i → type, drapeaux, offset, taille. Retourne false
	// hors bornes ; sinon l'offset du nom (0 = sans nom).
	auto section_at = [&](uint16_t i, uint32_t& name_off, uint32_t& sh_type,
						  uint64_t& sh_flags, uint64_t& sh_offset, uint64_t& sh_size) {
		size_t h = off + (size_t)shoff + (size_t)i * shentsize;
		if (h + shentsize > buf.size()) return false;
		name_off = rd32(buf, h, be);
		sh_type = rd32(buf, h + 4, be);
		sh_flags = is64 ? rd64(buf, h + 8, be) : rd32(buf, h + 8, be);
		sh_offset = is64 ? rd64(buf, h + 24, be) : rd32(buf, h + 16, be);
		sh_size = is64 ? rd64(buf, h + 32, be) : rd32(buf, h + 20, be);
		return true;
	};
	auto section_name = [&](uint64_t strtab_off, uint32_t name_off) {
		size_t p = (size_t)strtab_off + name_off;
		if (p >= buf.size()) return std::string{};
		size_t e = buf.find('\0', p);
		if (e == std::string::npos) e = buf.size();
		return buf.substr(p, e - p);
	};

	// Table des noms de sections (section shstrndx).
	uint64_t strtab = 0;
	{
		uint32_t n, t; uint64_t f, o, s;
		if (shstrndx < shnum && section_at(shstrndx, n, t, f, o, s)) strtab = off + o;
	}

	constexpr uint32_t SHT_NOBITS = 8;
	constexpr uint64_t SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4;
	for (uint16_t i = 0; i < shnum; ++i) {
		uint32_t name_off = 0, sh_type = 0;
		uint64_t sh_flags = 0, sh_offset = 0, sh_size = 0;
		if (!section_at(i, name_off, sh_type, sh_flags, sh_offset, sh_size)) continue;
		(void)sh_offset;
		if (!(sh_flags & SHF_ALLOC)) continue;
		// Tailles « Berkeley » (comme `size`) : .rodata compte dans text.
		if (sh_type == SHT_NOBITS) in.bss += sh_size;
		else if (sh_flags & SHF_EXECINSTR || !(sh_flags & SHF_WRITE)) in.text += sh_size;
		else in.data += sh_size;
		in.sections.push_back({section_name(strtab, name_off), sh_size, sh_flags});
	}
	return true;
}

// Nom de la section de sortie qu'utilisera l'éditeur de liens pour une
// section d'objet : .text._Z… (-ffunction-sections, COMDAT) → .text, etc.
std::string output_section(const std::string& n) {
	for (auto base : {".text", ".rodata", ".data.rel.ro", ".data", ".bss", ".tbss", ".tdata",
					  ".init_array", ".fini_array", ".gcc_except_table", ".ARM.extab", ".ARM.exidx"})
		if (n == base || n.starts_with(std::string(base) + "."))
			return base;
	return n;
}

// Analyse un fichier : ELF direct, ou archive ar (.a) dont les objets sont
// sommés (text/data/bss de l'ensemble des membres ELF).
Info read(const fs::path& p) {
	Info in;
	std::string buf;
	if (!util::read_file(p, buf)) { in.note = "illisible"; return in; }
	in.file_size = buf.size();
	if (parse_at(buf, 0, in)) return in;   // ELF simple
	if (buf.starts_with("!<arch>\n")) {
		Info acc;
		acc.file_size = in.file_size;
		size_t pos = 8;
		while (pos + 60 <= buf.size()) {
			if (buf[pos + 58] != '`' || buf[pos + 59] != '\n') break;   // fin d'en-tête
			long sz = 0;
			bool digits = false;
			for (size_t k = 48; k < 58; ++k) {
				char ch = buf[pos + k];
				if (ch == ' ') continue;
				if (ch < '0' || ch > '9') break;
				sz = sz * 10 + (ch - '0');
				digits = true;
			}
			if (!digits) break;
			size_t data = pos + 60;
			if (sz < 0 || data + (size_t)sz > buf.size()) break;
			Info m;
			if (parse_at(buf, data, m)) {
				++acc.members;
				acc.text += m.text;
				acc.data += m.data;
				acc.bss += m.bss;
				// Sections cumulées par nom sur l'ensemble des membres (-v).
				for (auto sec : m.sections) {
					sec.name = output_section(sec.name);
					auto it = std::find_if(acc.sections.begin(), acc.sections.end(),
										   [&](auto& x) { return x.name == sec.name; });
					if (it == acc.sections.end()) acc.sections.push_back(sec);
					else { it->size += sec.size; it->flags |= sec.flags; }
				}
			}
			pos = data + (size_t)sz + ((size_t)sz & 1);   // alignement pair
		}
		if (acc.members > 0) { acc.ok = true; return acc; }
		in.note = "archive ar sans membre ELF";
		return in;
	}
	in.note = "pas un fichier ELF (PE/Windows, binaire brut…)";
	return in;
}

} // namespace elf