// Comment- and order-preserving editors for INI and the YAML subset used by
// RPCS3. TOML lives in TomlEdit.cpp (toml++ based).
#include "core/ConfigFiles.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include "core/Platform.h"

namespace rrm::cfg {

// ---- shared helpers (also used by TomlEdit.cpp through these declarations) ------------

bool readTextFile(const std::string& path, std::string& out, bool* exists);
bool writeTextFileAtomic(const std::string& path, const std::string& text);

bool readTextFile(const std::string& path, std::string& out, bool* exists) {
    out.clear();
    std::error_code ec;
    fs::path p = utf8ToPath(path);
    bool there = fs::exists(p, ec);
    if (exists) *exists = there;
    if (!there) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !in.bad();
}

bool writeTextFileAtomic(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::path p = utf8ToPath(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".rrm-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        // Windows cannot always rename over an existing file.
        std::error_code ec2;
        fs::remove(p, ec2);
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

std::vector<std::string> splitLines(const std::string& text) {
    // Keeps a trailing '\r' on each line so CRLF files round-trip unchanged.
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

bool usesCrlf(const std::vector<std::string>& lines) {
    for (auto& l : lines)
        if (!l.empty() && l.back() == '\r') return true;
    return false;
}

std::string_view stripCr(std::string_view s) {
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// ---- INI line model ---------------------------------------------------------------------

enum class IniKind { Blank, Comment, Section, KeyValue, Other };

struct IniLine {
    IniKind kind = IniKind::Other;
    std::string_view name;   // section name or key
    std::string_view value;  // value (KeyValue)
    size_t valueStart = 0;   // byte offset of the value inside the raw line
};

IniLine classifyIni(std::string_view raw, bool first) {
    IniLine out;
    std::string_view line = stripCr(raw);
    size_t bomSkip = (first && line.substr(0, kBom.size()) == kBom) ? kBom.size() : 0;
    std::string_view body = trimView(line.substr(bomSkip));
    if (body.empty()) {
        out.kind = IniKind::Blank;
        return out;
    }
    if (body.front() == ';' || body.front() == '#') {
        out.kind = IniKind::Comment;
        return out;
    }
    if (body.front() == '[') {
        size_t close = body.rfind(']');
        if (close != std::string_view::npos && close > 0) {
            out.kind = IniKind::Section;
            out.name = trimView(body.substr(1, close - 1));
            return out;
        }
    }
    size_t eq = line.find('=', bomSkip);
    if (eq != std::string_view::npos) {
        out.kind = IniKind::KeyValue;
        out.name = trimView(line.substr(bomSkip, eq - bomSkip));
        size_t v = eq + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        out.valueStart = v;
        out.value = trimView(line.substr(v));
        return out;
    }
    out.kind = IniKind::Other;
    return out;
}

}  // namespace

// ---- Ini -------------------------------------------------------------------------------------

bool Ini::load(const std::string& path) {
    std::string text;
    bool exists = false;
    if (!readTextFile(path, text, &exists)) {
        parse(std::string());
        return !exists;  // a missing file is an empty document
    }
    parse(text);
    return true;
}

bool Ini::save(const std::string& path) const { return writeTextFileAtomic(path, text()); }

void Ini::parse(const std::string& text) {
    lines_ = splitLines(text);
    spacedEquals_ = true;
    for (size_t i = 0; i < lines_.size(); ++i) {
        IniLine l = classifyIni(lines_[i], i == 0);
        if (l.kind != IniKind::KeyValue) continue;
        std::string_view raw = stripCr(lines_[i]);
        size_t eq = raw.find('=');
        bool spaceBefore = eq > 0 && (raw[eq - 1] == ' ' || raw[eq - 1] == '\t');
        bool spaceAfter = eq + 1 < raw.size() && (raw[eq + 1] == ' ' || raw[eq + 1] == '\t');
        spacedEquals_ = spaceBefore || spaceAfter;
        break;
    }
}

std::string Ini::text() const { return joinLines(lines_); }

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    std::string_view current;
    bool inGlobal = true;
    for (size_t i = 0; i < lines_.size(); ++i) {
        IniLine l = classifyIni(lines_[i], i == 0);
        if (l.kind == IniKind::Section) {
            current = l.name;
            inGlobal = false;
            continue;
        }
        if (l.kind != IniKind::KeyValue) continue;
        bool sectionMatches = section.empty() ? inGlobal : (!inGlobal && ieq(current, section));
        if (sectionMatches && ieq(l.name, key)) return std::string(l.value);
    }
    return def;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    const bool crlf = usesCrlf(lines_);
    const std::string eol = crlf ? "\r" : "";
    bool found = false;
    bool inTarget = section.empty();  // keys before the first header form the global section
    bool firstOccurrence = section.empty();
    bool seenTarget = section.empty();
    long insertAfter = -1;       // last non-blank line of the first matching section
    long headerIndex = -1;
    for (size_t i = 0; i < lines_.size(); ++i) {
        IniLine l = classifyIni(lines_[i], i == 0);
        if (l.kind == IniKind::Section) {
            bool match = !section.empty() && ieq(l.name, section);
            if (inTarget && firstOccurrence) firstOccurrence = false;  // leaving the first occurrence
            inTarget = match;
            if (match && !seenTarget) {
                seenTarget = true;
                firstOccurrence = true;
                headerIndex = static_cast<long>(i);
                insertAfter = static_cast<long>(i);
            }
            continue;
        }
        if (!inTarget) continue;
        if (firstOccurrence && l.kind != IniKind::Blank) insertAfter = static_cast<long>(i);
        if (l.kind == IniKind::KeyValue && ieq(l.name, key)) {
            // Replace the value, keeping the key spelling, spacing and line ending.
            std::string& raw = lines_[i];
            bool hadCr = !raw.empty() && raw.back() == '\r';
            std::string prefix = raw.substr(0, l.valueStart);
            if (prefix.empty() || prefix.back() == '=') {
                if (spacedEquals_ && !prefix.empty()) prefix += ' ';
            }
            raw = prefix + value + (hadCr ? "\r" : "");
            found = true;
        }
    }
    if (found) return;

    std::string newLine = key + (spacedEquals_ ? " = " : "=") + value + eol;
    if (section.empty()) {
        // Global keys go before the first section header (after any leading comments/keys).
        long pos = -1;
        for (size_t i = 0; i < lines_.size(); ++i) {
            IniLine l = classifyIni(lines_[i], i == 0);
            if (l.kind == IniKind::Section) break;
            if (l.kind != IniKind::Blank) pos = static_cast<long>(i);
        }
        lines_.insert(lines_.begin() + (pos + 1), newLine);
        return;
    }
    if (headerIndex >= 0) {
        lines_.insert(lines_.begin() + (insertAfter + 1), newLine);
        return;
    }
    // Append a new section.
    if (!lines_.empty() && classifyIni(lines_.back(), lines_.size() == 1).kind != IniKind::Blank) lines_.push_back(eol);
    lines_.push_back("[" + section + "]" + eol);
    lines_.push_back(newLine);
}

// ---- YAML (indentation-based maps of scalars) ----------------------------------------------------

namespace {

struct YamlLine {
    bool structural = false;  // a "key:" or "key: value" line
    bool sequenceItem = false;
    bool blankOrComment = false;
    int indent = 0;
    std::string key;          // unquoted
    std::string_view value;   // raw text after ':' (trimmed), may be empty
    size_t valueStart = 0;    // byte offset of the value in the raw line (or where it would start)
    size_t colon = 0;         // byte offset of ':'
};

YamlLine classifyYaml(const std::string& rawLine) {
    YamlLine out;
    std::string_view line = stripCr(rawLine);
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    out.indent = static_cast<int>(i);
    std::string_view body = trimView(line.substr(i));
    if (body.empty() || body.front() == '#' || body == "---" || body == "...") {
        out.blankOrComment = true;
        return out;
    }
    if (body.front() == '-' && (body.size() == 1 || body[1] == ' ')) {
        out.sequenceItem = true;
        return out;
    }
    // Key: quoted or plain.
    size_t keyEnd = std::string_view::npos, colon = std::string_view::npos;
    if (line[i] == '"' || line[i] == '\'') {
        char q = line[i];
        size_t j = i + 1;
        std::string key;
        while (j < line.size()) {
            if (q == '"' && line[j] == '\\' && j + 1 < line.size()) {
                key += line[j + 1];
                j += 2;
                continue;
            }
            if (line[j] == q) {
                if (q == '\'' && j + 1 < line.size() && line[j + 1] == '\'') {
                    key += '\'';
                    j += 2;
                    continue;
                }
                break;
            }
            key += line[j++];
        }
        if (j >= line.size()) return out;
        keyEnd = j + 1;
        size_t k = keyEnd;
        while (k < line.size() && line[k] == ' ') ++k;
        if (k >= line.size() || line[k] != ':') return out;
        colon = k;
        out.key = key;
    } else {
        // First ':' followed by space or end of line.
        for (size_t j = i; j < line.size(); ++j) {
            if (line[j] == ':' && (j + 1 == line.size() || line[j + 1] == ' ' || line[j + 1] == '\t')) {
                colon = j;
                break;
            }
            if (line[j] == '#' && j > i && line[j - 1] == ' ') break;  // comment before any colon
        }
        if (colon == std::string_view::npos) return out;
        out.key = std::string(trimView(line.substr(i, colon - i)));
    }
    out.structural = true;
    out.colon = colon;
    size_t v = colon + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    out.valueStart = v;
    out.value = trimView(line.substr(v));
    return out;
}

bool needsYamlQuotes(const std::string& v) {
    if (v.empty()) return true;
    if (v.front() == ' ' || v.back() == ' ') return true;
    static const std::string indicators = "-?:,[]{}#&*!|>'\"%@`";
    if (indicators.find(v.front()) != std::string::npos) {
        // "-1" / "-0.5" are fine as plain numbers.
        bool numeric = v.front() == '-' && v.size() > 1 && (std::isdigit(static_cast<unsigned char>(v[1])) || v[1] == '.');
        if (!numeric) return true;
    }
    if (v.find(": ") != std::string::npos || v.find(" #") != std::string::npos) return true;
    if (v.back() == ':') return true;
    return false;
}

std::string yamlQuoted(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string yamlScalar(const std::string& v) { return needsYamlQuotes(v) ? yamlQuoted(v) : v; }

std::string yamlKey(const std::string& k) {
    if (!k.empty() && !needsYamlQuotes(k) && k.find(':') == std::string::npos) return k;
    return yamlQuoted(k);
}

// Byte offset where a trailing " # comment" starts on a structural line (npos if none).
size_t yamlCommentStart(std::string_view line, size_t valueStart) {
    size_t i = valueStart;
    if (i < line.size() && (line[i] == '"' || line[i] == '\'')) {
        char q = line[i++];
        while (i < line.size()) {
            if (q == '"' && line[i] == '\\') {
                i += 2;
                continue;
            }
            if (line[i] == q) {
                ++i;
                break;
            }
            ++i;
        }
    }
    size_t hash = line.find(" #", i > 0 ? i - 1 : 0);
    return hash;
}

std::string unquoteYaml(std::string_view v) {
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        char q = v.front();
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            if (q == '"' && v[i] == '\\' && i + 2 < v.size()) {
                out += v[++i];
                continue;
            }
            if (q == '\'' && v[i] == '\'' && i + 2 < v.size() && v[i + 1] == '\'') {
                out += '\'';
                ++i;
                continue;
            }
            out += v[i];
        }
        return out;
    }
    // Strip a trailing comment from plain scalars.
    size_t hash = v.find(" #");
    if (hash != std::string_view::npos) v = trimView(v.substr(0, hash));
    return std::string(v);
}

// End (exclusive) of the block owned by the structural line at `index`.
size_t yamlBlockEnd(const std::vector<std::string>& lines, size_t index, size_t limit) {
    YamlLine head = classifyYaml(lines[index]);
    size_t last = index;  // last line that belongs to the block
    for (size_t j = index + 1; j < limit; ++j) {
        YamlLine l = classifyYaml(lines[j]);
        if (l.blankOrComment) continue;
        if (l.indent > head.indent || (l.sequenceItem && l.indent == head.indent)) {
            last = j;
            continue;
        }
        break;
    }
    return last + 1;
}

}  // namespace

bool YamlMap::load(const std::string& path) {
    std::string text;
    bool exists = false;
    if (!readTextFile(path, text, &exists)) {
        parse(std::string());
        return !exists;
    }
    parse(text);
    return true;
}

bool YamlMap::save(const std::string& path) const { return writeTextFileAtomic(path, text()); }

void YamlMap::parse(const std::string& text) {
    std::string t = text;
    if (t.compare(0, kBom.size(), kBom) == 0) t.erase(0, kBom.size());
    lines_ = splitLines(t);
}

std::string YamlMap::text() const { return joinLines(lines_); }

std::string YamlMap::get(const std::vector<std::string>& keyPath, const std::string& def) const {
    if (keyPath.empty()) return def;
    size_t begin = 0, end = lines_.size();
    int parentIndent = -1;
    for (size_t d = 0; d < keyPath.size(); ++d) {
        bool found = false;
        int childIndent = -1;
        for (size_t i = begin; i < end; ++i) {
            YamlLine l = classifyYaml(lines_[i]);
            if (!l.structural || l.indent <= parentIndent) continue;
            if (childIndent < 0) childIndent = l.indent;
            if (l.indent != childIndent) continue;
            if (l.key == keyPath[d]) {
                if (d + 1 == keyPath.size()) return unquoteYaml(l.value);
                begin = i + 1;
                end = yamlBlockEnd(lines_, i, end);
                parentIndent = l.indent;
                found = true;
                break;
            }
        }
        if (!found) return def;
    }
    return def;
}

void YamlMap::set(const std::vector<std::string>& keyPath, const std::string& value) {
    if (keyPath.empty()) return;
    const std::string eol = usesCrlf(lines_) ? "\r" : "";
    // Indentation unit learnt from the document (RPCS3 uses 2).
    int unit = 2;
    for (size_t i = 0; i < lines_.size(); ++i) {
        YamlLine l = classifyYaml(lines_[i]);
        if (l.structural && l.indent > 0) {
            unit = l.indent;
            break;
        }
    }
    size_t begin = 0, end = lines_.size();
    int parentIndent = -1;
    for (size_t d = 0; d < keyPath.size(); ++d) {
        int childIndent = -1;
        long hit = -1;
        for (size_t i = begin; i < end; ++i) {
            YamlLine l = classifyYaml(lines_[i]);
            if (!l.structural || l.indent <= parentIndent) continue;
            if (childIndent < 0) childIndent = l.indent;
            if (l.indent != childIndent) continue;
            if (l.key == keyPath[d]) {
                hit = static_cast<long>(i);
                break;
            }
        }
        if (hit >= 0) {
            std::string& raw = lines_[static_cast<size_t>(hit)];
            YamlLine l = classifyYaml(raw);
            bool hadCr = !raw.empty() && raw.back() == '\r';
            if (d + 1 == keyPath.size()) {
                // Keep everything up to and including ':' plus one space and any trailing comment.
                std::string_view body = stripCr(raw);
                size_t comment = l.value.empty() ? std::string_view::npos : yamlCommentStart(body, l.valueStart);
                std::string tail = comment == std::string_view::npos ? std::string() : std::string(body.substr(comment));
                raw = raw.substr(0, l.colon + 1) + " " + yamlScalar(value) + tail + (hadCr ? "\r" : "");
                return;
            }
            if (!l.value.empty()) {
                // "Key: {}" or a scalar where a map is needed: turn it into a map header.
                raw = raw.substr(0, l.colon + 1) + (hadCr ? "\r" : "");
            }
            end = yamlBlockEnd(lines_, static_cast<size_t>(hit), end);
            begin = static_cast<size_t>(hit) + 1;
            parentIndent = l.indent;
            continue;
        }
        // Missing from here on: create the remaining path at the end of the parent block.
        int indent = childIndent >= 0 ? childIndent : (parentIndent < 0 ? 0 : parentIndent + unit);
        size_t insertAt = end;
        // Do not swallow trailing blank/comment lines that separate this block from the next one.
        while (insertAt > begin && classifyYaml(lines_[insertAt - 1]).blankOrComment) --insertAt;
        if (parentIndent < 0 && insertAt == 0 && !lines_.empty()) insertAt = lines_.size();
        std::vector<std::string> add;
        for (size_t k = d; k < keyPath.size(); ++k) {
            std::string pad(static_cast<size_t>(indent), ' ');
            if (k + 1 == keyPath.size()) add.push_back(pad + yamlKey(keyPath[k]) + ": " + yamlScalar(value) + eol);
            else add.push_back(pad + yamlKey(keyPath[k]) + ":" + eol);
            indent += unit;
        }
        lines_.insert(lines_.begin() + static_cast<long>(insertAt), add.begin(), add.end());
        return;
    }
}

}  // namespace rrm::cfg
