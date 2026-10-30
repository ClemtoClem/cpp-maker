#include "cpp-maker.hpp"

namespace toml {

Parser::Parser(std::string text, std::string file)
	: text_(std::move(text)), file_(std::move(file)) {}

void Parser::fail(const std::string& m) {
	throw ParseError(std::format("{}:{}: {}", file_, line_, m));
}
bool Parser::eof() const { return i_ >= text_.size(); }
char Parser::peek() const { return eof() ? '\0' : text_[i_]; }
char Parser::next() {
	char c = peek();
	if (c == '\n') ++line_;
	if (!eof()) ++i_;
	return c;
}
void Parser::skip_ws() {
	while (!eof() && (peek() == ' ' || peek() == '\t')) next();
}
void Parser::skip_ws_and_comments() {
	for (;;) {
		char c = peek();
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { next(); continue; }
		if (c == '#') { while (!eof() && peek() != '\n') next(); continue; }
		break;
	}
}
void Parser::expect_eol() {
	skip_ws();
	if (eof()) return;
	if (peek() == '#') { while (!eof() && peek() != '\n') next(); }
	if (eof()) return;
	if (peek() != '\n' && peek() != '\r') fail("fin de ligne attendue");
	if (peek() == '\r') next();
	if (peek() == '\n') next();
}
bool Parser::is_bare_key_char(char c) const {
	return std::isalnum((unsigned char)c) || c == '_' || c == '-';
}
std::string Parser::parse_key() {
	skip_ws();
	if (peek() == '"' || peek() == '\'') return parse_string();
	std::string k;
	while (!eof() && is_bare_key_char(peek())) k += next();
	if (k.empty()) fail("clé attendue");
	return k;
}
std::vector<std::string> Parser::parse_key_path() {
	std::vector<std::string> path;
	path.push_back(parse_key());
	skip_ws();
	while (peek() == '.') {
		next(); skip_ws();
		path.push_back(parse_key());
		skip_ws();
	}
	return path;
}
Table& Parser::ensure_table(Table& root, const std::vector<std::string>& path) {
	Table* cur = &root;
	for (auto& p : path) {
		auto& slot = (*cur)[p];
		if (std::holds_alternative<std::monostate>(slot.v)) slot.v = Table{};
		if (!slot.table()) fail(std::format("'{}' n'est pas une table", p));
		cur = slot.table();
	}
	return *cur;
}
std::string Parser::parse_string() {
	char q = next();
	std::string s;
	while (!eof()) {
		char c = next();
		if (c == q) return s;
		if (c == '\n') fail("chaîne non terminée en fin de ligne");
		if (q == '"' && c == '\\') {
			char e = next();
			switch (e) {
				case 'n': s += '\n'; break;
				case 't': s += '\t'; break;
				case 'r': s += '\r'; break;
				case '"': s += '"'; break;
				case '\\': s += '\\'; break;
				default: s += e;
			}
		} else {
			s += c;
		}
	}
	fail("chaîne non terminée");
}
Value Parser::parse_value() {
	char c = peek();
	if (c == '"' || c == '\'') return Value{parse_string()};
	if (c == '[') return Value{parse_array()};
	if (c == '{') return Value{parse_inline_table()};
	if (c == 't' || c == 'f') return Value{parse_bool()};
	if (c == '-' || c == '+' || std::isdigit((unsigned char)c)) return Value{parse_int()};
	fail(std::format("valeur invalide (caractère '{}')", c));
}
Array Parser::parse_array() {
	next();
	Array a;
	for (;;) {
		skip_ws_and_comments();
		if (peek() == ']') { next(); return a; }
		a.push_back(parse_value());
		skip_ws_and_comments();
		if (peek() == ',') { next(); continue; }
		if (peek() == ']') { next(); return a; }
		fail("',' ou ']' attendu dans un tableau");
	}
}
Table Parser::parse_inline_table() {
	next();
	Table t;
	skip_ws();
	if (peek() == '}') { next(); return t; }
	for (;;) {
		skip_ws();
		auto path = parse_key_path();
		std::string key = path.back();
		path.pop_back();
		Table& dst = path.empty() ? t : ensure_table(t, path);
		skip_ws();
		if (next() != '=') fail("'=' attendu dans une table inline");
		skip_ws();
		if (dst.count(key)) fail(std::format("clé '{}' déjà définie", key));
		dst[key] = parse_value();
		skip_ws();
		char c = next();
		if (c == ',') continue;
		if (c == '}') return t;
		fail("',' ou '}' attendu dans une table inline");
	}
}
bool Parser::parse_bool() {
	if (text_.compare(i_, 4, "true") == 0) { i_ += 4; return true; }
	if (text_.compare(i_, 5, "false") == 0) { i_ += 5; return false; }
	fail("booléen attendu");
}
int64_t Parser::parse_int() {
	bool neg = false;
	if (peek() == '-' || peek() == '+') { neg = peek() == '-'; next(); }
	int64_t v = 0;
	bool any = false;
	while (!eof() && (std::isdigit((unsigned char)peek()) || peek() == '_')) {
		char c = next();
		if (c == '_') continue;
		v = v * 10 + (c - '0');
		any = true;
	}
	if (!any) fail("entier attendu");
	return neg ? -v : v;
}

Table Parser::parse() {
	Table root;
	Table* cur = &root;
	std::set<std::string> defined_tables;
	while (!eof()) {
		skip_ws_and_comments();
		if (eof()) break;
		if (peek() == '[') {
			next();
			if (peek() == '[') fail("les tableaux de tables [[...]] ne sont pas supportés");
			skip_ws();
			auto path = parse_key_path();
			if (next() != ']') fail("']' attendu après le nom de table");
			std::string full = util::join(path, ".");
			if (!defined_tables.insert(full).second)
				fail(std::format("table [{}] définie deux fois", full));
			cur = &ensure_table(root, path);
			expect_eol();
		} else {
			auto path = parse_key_path();
			std::string key = path.back();
			path.pop_back();
			Table& dst = path.empty() ? *cur : ensure_table(*cur, path);
			skip_ws();
			if (next() != '=') fail("'=' attendu");
			skip_ws();
			Value v = parse_value();
			if (dst.count(key)) fail(std::format("clé '{}' déjà définie", key));
			dst[key] = std::move(v);
			expect_eol();
		}
	}
	return root;
}

Table parse_file(const fs::path& p) {
	std::string content;
	if (!util::read_file(p, content))
		throw ParseError(std::format("{}: impossible d'ouvrir le fichier", p.string()));
	Parser parser(std::move(content), p.string());
	return parser.parse();
}

} // namespace toml