// TEMPORARY, TEST-ONLY stand-in for src/core/ConfigFiles.cpp (cfg::Ini, cfg::YamlMap,
// cfg::tomlSet), used by tests/recomp/CMakeLists.txt only while the real
// implementation does not exist yet. Never add this file to the main build and
// delete it once src/core/ConfigFiles.cpp lands (the CMake file prefers the real one).
#include "core/ConfigFiles.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "toml.hpp"

namespace rrm::cfg {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeText(const std::string& path, const std::string& text) {
    std::filesystem::path p(std::u8string(path.begin(), path.end()));
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

// "[name]" -> name, else empty with ok=false
bool sectionName(const std::string& line, std::string& name) {
    std::string t = trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
        name = trim(t.substr(1, t.size() - 2));
        return true;
    }
    return false;
}

bool keyOf(const std::string& line, std::string& key) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = trim(t.substr(0, eq));
    return !key.empty();
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

}  // namespace

bool Ini::load(const std::string& path) {
    std::string text;
    if (!readText(path, text)) {
        lines_.clear();
        return true;  // a missing file is an empty document
    }
    parse(text);
    return true;
}

bool Ini::save(const std::string& path) const { return writeText(path, text()); }

void Ini::parse(const std::string& text) {
    lines_.clear();
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(line);
    }
    for (auto& l : lines_) {
        std::string k;
        if (keyOf(l, k)) {
            size_t eq = l.find('=');
            spacedEquals_ = eq > 0 && l[eq - 1] == ' ';
            break;
        }
    }
}

std::string Ini::text() const {
    std::string out;
    for (auto& l : lines_) out += l + "\n";
    return out;
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    std::string cur;
    for (auto& l : lines_) {
        std::string name, k;
        if (sectionName(l, name)) {
            cur = name;
            continue;
        }
        if (iequals(cur, section) && keyOf(l, k) && iequals(k, key)) {
            std::string v = trim(l.substr(l.find('=') + 1));
            return v;
        }
    }
    return def;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    const std::string newLine = key + (spacedEquals_ ? " = " : "=") + value;
    std::string cur;
    int lastInSection = section.empty() ? -1 : -2;  // -2: section not seen yet
    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string name, k;
        if (sectionName(lines_[i], name)) {
            cur = name;
            if (iequals(cur, section)) lastInSection = (int)i;
            continue;
        }
        if (iequals(cur, section)) {
            if (keyOf(lines_[i], k) && iequals(k, key)) {
                lines_[i] = newLine;
                return;
            }
            if (!trim(lines_[i]).empty()) lastInSection = (int)i;
        }
    }
    if (lastInSection == -2) {
        if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.push_back("");
        lines_.push_back("[" + section + "]");
        lines_.push_back(newLine);
        return;
    }
    lines_.insert(lines_.begin() + (lastInSection + 1), newLine);
}

bool YamlMap::load(const std::string& path) {
    std::string text;
    if (readText(path, text)) parse(text);
    return true;
}
bool YamlMap::save(const std::string& path) const { return writeText(path, text()); }
void YamlMap::set(const std::vector<std::string>&, const std::string&) {}
std::string YamlMap::get(const std::vector<std::string>&, const std::string& def) const { return def; }
std::string YamlMap::text() const {
    std::string out;
    for (auto& l : lines_) out += l + "\n";
    return out;
}
void YamlMap::parse(const std::string& text) {
    lines_.clear();
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) lines_.push_back(line);
}

bool tomlSet(const std::string& path, const std::vector<std::pair<std::string, std::string>>& kv, std::string* error) {
    toml::table doc;
    std::string text;
    if (readText(path, text) && !text.empty()) {
        try {
            doc = toml::parse(text);
        } catch (const toml::parse_error& e) {
            if (error) *error = std::string("TOML parse error in ") + path + ": " + std::string(e.description());
            return false;
        }
    }
    for (auto& [dotted, literal] : kv) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t dot = dotted.find('.', start);
            parts.push_back(dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        toml::table* t = &doc;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            toml::node* n = t->get(parts[i]);
            if (!n || !n->is_table()) {
                t->insert_or_assign(parts[i], toml::table{});
                n = t->get(parts[i]);
            }
            t = n->as_table();
        }
        toml::table tmp;
        try {
            tmp = toml::parse("v = " + literal);
        } catch (const toml::parse_error& e) {
            if (error) *error = "bad TOML value for " + dotted + ": " + literal;
            return false;
        }
        if (toml::node* v = tmp.get("v")) {
            v->visit([&](auto& node) { t->insert_or_assign(parts.back(), node); });
        }
    }
    std::ostringstream out;
    out << doc << "\n";
    if (!writeText(path, out.str())) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace rrm::cfg
