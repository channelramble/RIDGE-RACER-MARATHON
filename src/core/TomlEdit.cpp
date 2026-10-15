// Comment-preserving TOML edits. toml++ validates documents and tells us where
// existing values live; the edit itself is a textual splice so comments,
// alignment and key order survive.
#include "core/ConfigFiles.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>

#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 1
#include "toml.hpp"

namespace rrm::cfg {

// Defined in ConfigFiles.cpp.
bool readTextFile(const std::string& path, std::string& out, bool* exists);
bool writeTextFileAtomic(const std::string& path, const std::string& text);

namespace {

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool parseToml(std::string_view text, toml::table& out, std::string& error) {
    toml::parse_result r = toml::parse(text, std::string_view{"config.toml"});
    if (!r) {
        const auto& e = r.error();
        error = std::string(e.description()) + " (line " + std::to_string(e.source().begin.line) + ")";
        return false;
    }
    out = std::move(r).table();
    return true;
}

// Splits a dotted key path ("video.internal_resolution", a."b.c".d). Returns false if malformed.
bool splitKeyPath(std::string_view s, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool any = false;
    size_t i = 0;
    auto flush = [&]() -> bool {
        std::string_view t = trimView(cur);
        if (t.empty() && !any) return false;
        if (!any) {
            // Bare keys: A-Z a-z 0-9 _ - only.
            for (unsigned char c : t)
                if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
        }
        out.emplace_back(any ? cur : std::string(t));
        cur.clear();
        any = false;
        return true;
    };
    while (i < s.size()) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            // Quoted segment: everything up to the closing quote is literal (basic strings unescape).
            std::string q;
            size_t j = i + 1;
            bool closed = false;
            while (j < s.size()) {
                if (c == '"' && s[j] == '\\' && j + 1 < s.size()) {
                    char e = s[j + 1];
                    q += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                    j += 2;
                    continue;
                }
                if (s[j] == c) {
                    closed = true;
                    break;
                }
                q += s[j++];
            }
            if (!closed) return false;
            if (!trimView(cur).empty()) return false;
            cur = q;
            any = true;
            i = j + 1;
            continue;
        }
        if (c == '.') {
            if (!flush()) return false;
            ++i;
            continue;
        }
        if (any && c != ' ' && c != '\t') return false;  // junk after a quoted segment
        if (!any) cur += c;
        ++i;
    }
    return flush();
}

bool isBareKey(const std::string& k) {
    if (k.empty()) return false;
    for (unsigned char c : k)
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

std::string keyText(const std::string& k) {
    if (isBareKey(k)) return k;
    std::string out = "\"";
    for (char c : k) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string pathText(const std::vector<std::string>& parts, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i) out += '.';
        out += keyText(parts[i]);
    }
    return out;
}

struct Lines {
    std::vector<size_t> starts;  // byte offset of each line start
    explicit Lines(const std::string& text) {
        starts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n') starts.push_back(i + 1);
    }
    size_t count() const { return starts.size(); }
};

// Byte offset of a toml++ source position (1-based line; 1-based column in code points).
size_t byteOffset(const std::string& text, const Lines& lines, uint32_t line, uint32_t column) {
    if (line == 0 || line > lines.count()) return std::string::npos;
    size_t pos = lines.starts[line - 1];
    for (uint32_t c = 1; c < column && pos < text.size() && text[pos] != '\n'; ++c) {
        unsigned char b = static_cast<unsigned char>(text[pos]);
        pos += (b < 0x80) ? 1 : (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
    }
    return std::min(pos, text.size());
}

std::string lineAt(const std::string& text, const Lines& lines, size_t idx) {
    size_t b = lines.starts[idx];
    size_t e = idx + 1 < lines.count() ? lines.starts[idx + 1] : text.size();
    return text.substr(b, e - b);
}

// A "[a.b]" / "[[a.b]]" header on line idx (empty path if the line is not a header).
bool headerPath(const std::string& rawLine, std::vector<std::string>& path, bool& arrayTable) {
    std::string_view l = trimView(rawLine);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.remove_suffix(1);
    if (l.empty() || l.front() != '[') return false;
    arrayTable = l.size() > 1 && l[1] == '[';
    size_t start = arrayTable ? 2 : 1;
    // Find the closing bracket outside quotes.
    char quote = 0;
    size_t close = std::string_view::npos;
    for (size_t i = start; i < l.size(); ++i) {
        char c = l[i];
        if (quote) {
            if (c == '\\' && quote == '"') {
                ++i;
                continue;
            }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == ']') {
            close = i;
            break;
        }
    }
    if (close == std::string_view::npos) return false;
    return splitKeyPath(l.substr(start, close - start), path);
}

bool samePath(const std::vector<std::string>& a, const std::vector<std::string>& b, size_t bCount) {
    if (a.size() != bCount) return false;
    for (size_t i = 0; i < bCount; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

bool isBlankOrComment(const std::string& line) {
    std::string_view t = trimView(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.remove_suffix(1);
    return t.empty() || t.front() == '#';
}

bool isBlank(const std::string& line) {
    std::string_view t = trimView(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.remove_suffix(1);
    return t.empty();
}

// Inserts `newLine` (without EOL) after the last key/value (or header) line in [from, to);
// in a section without any, after its last non-blank line (e.g. a leading comment block).
std::string insertAfterLastContent(const std::string& text, const Lines& lines, size_t from, size_t to,
                                   const std::string& newLine, const std::string& eol) {
    long after = -1;  // insert after this line index
    for (size_t i = from; i < to && i < lines.count(); ++i)
        if (!isBlankOrComment(lineAt(text, lines, i))) after = static_cast<long>(i);
    if (after < 0) {
        for (size_t i = from; i < to && i < lines.count(); ++i)
            if (!isBlank(lineAt(text, lines, i))) after = static_cast<long>(i);
    }
    if (after < 0) after = static_cast<long>(from) - 1;
    size_t next = static_cast<size_t>(after + 1);
    size_t pos = next < lines.count() ? lines.starts[next] : text.size();
    std::string out = text;
    std::string ins = newLine + eol + "\n";
    if (pos == text.size() && !text.empty() && text.back() != '\n') ins = eol + "\n" + newLine;
    out.insert(pos, ins);
    return out;
}

// One key. On success `text` is updated and still parses.
bool applyOne(std::string& text, const std::vector<std::string>& parts, const std::string& literal, std::string& error) {
    toml::table root;
    if (!parseToml(text, root, error)) return false;
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r" : "";

    // 1) Existing value: splice the new literal over the old one.
    toml::node* node = &root;
    size_t depth = 0;
    for (; depth < parts.size(); ++depth) {
        toml::table* t = node->as_table();
        if (!t) break;
        toml::node* next = t->get(parts[depth]);
        if (!next) break;
        node = next;
    }
    if (depth == parts.size()) {
        if (node->is_table()) {
            error = "\"" + pathText(parts, parts.size()) + "\" is a table, not a value";
            return false;
        }
        Lines lines(text);
        const auto& src = node->source();
        size_t b = byteOffset(text, lines, src.begin.line, src.begin.column);
        size_t e = byteOffset(text, lines, src.end.line, src.end.column);
        if (b == std::string::npos || e == std::string::npos || e < b) {
            error = "could not locate the value of " + pathText(parts, parts.size());
            return false;
        }
        std::string candidate = text.substr(0, b) + literal + text.substr(e);
        toml::table check;
        std::string err2;
        if (!parseToml(candidate, check, err2)) {
            error = "edit produced invalid TOML: " + err2;
            return false;
        }
        text = std::move(candidate);
        return true;
    }

    // 2) Missing key: add "key = value" to the right [table] section, creating it if needed.
    const size_t parentCount = parts.size() - 1;
    const std::string& key = parts.back();
    Lines lines(text);
    std::vector<std::pair<size_t, std::vector<std::string>>> headers;  // line index, path (plain tables only)
    std::vector<size_t> allHeaders;
    for (size_t i = 0; i < lines.count(); ++i) {
        std::vector<std::string> hp;
        bool arr = false;
        if (headerPath(lineAt(text, lines, i), hp, arr)) {
            allHeaders.push_back(i);
            if (!arr) headers.emplace_back(i, std::move(hp));
        }
    }
    auto nextHeaderAfter = [&](size_t idx) {
        for (size_t h : allHeaders)
            if (h > idx) return h;
        return lines.count();
    };
    std::vector<std::string> candidates;
    if (parentCount == 0) {
        size_t end = allHeaders.empty() ? lines.count() : allHeaders.front();
        if (end == 0) candidates.push_back(keyText(key) + " = " + literal + eol + "\n" + text);
        else candidates.push_back(insertAfterLastContent(text, lines, 0, end, keyText(key) + " = " + literal, eol));
    } else {
        // a) the parent table has its own header
        for (auto& [idx, hp] : headers) {
            if (samePath(hp, parts, parentCount)) {
                candidates.push_back(
                    insertAfterLastContent(text, lines, idx, nextHeaderAfter(idx), keyText(key) + " = " + literal, eol));
                break;
            }
        }
        // b) a new [parent] section at the end of the file
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n') out += eol + "\n";
            if (!out.empty()) out += eol + "\n";
            out += "[" + pathText(parts, parentCount) + "]" + eol + "\n" + keyText(key) + " = " + literal + eol + "\n";
            candidates.push_back(std::move(out));
        }
        // c) a dotted key inside the deepest ancestor that has a header (or the root)
        size_t bestLen = 0, bestIdx = SIZE_MAX;
        for (auto& [idx, hp] : headers) {
            if (hp.size() < parentCount && hp.size() > bestLen) {
                bool prefix = true;
                for (size_t i = 0; i < hp.size(); ++i) prefix = prefix && hp[i] == parts[i];
                if (prefix) {
                    bestLen = hp.size();
                    bestIdx = idx;
                }
            }
        }
        std::vector<std::string> rest(parts.begin() + static_cast<long>(bestLen), parts.end());
        std::string dotted = pathText(rest, rest.size()) + " = " + literal;
        if (bestIdx != SIZE_MAX) {
            candidates.push_back(insertAfterLastContent(text, lines, bestIdx, nextHeaderAfter(bestIdx), dotted, eol));
        } else {
            size_t end = allHeaders.empty() ? lines.count() : allHeaders.front();
            if (end == 0) candidates.push_back(dotted + eol + "\n" + text);
            else candidates.push_back(insertAfterLastContent(text, lines, 0, end, dotted, eol));
        }
    }
    std::string lastError;
    for (auto& c : candidates) {
        toml::table check;
        if (!parseToml(c, check, lastError)) continue;
        // Make sure the value really landed where we wanted it.
        toml::node* n = &check;
        for (auto& p : parts) {
            toml::table* t = n ? n->as_table() : nullptr;
            n = t ? t->get(p) : nullptr;
        }
        if (!n || n->is_table()) continue;
        text = std::move(c);
        return true;
    }

    // 3) Last resort: let toml++ re-serialise the document (loses comments).
    toml::table* t = &root;
    for (size_t i = 0; i < parentCount; ++i) {
        toml::node* next = t->get(parts[i]);
        if (!next) {
            t->insert(parts[i], toml::table{});
            next = t->get(parts[i]);
        }
        t = next ? next->as_table() : nullptr;
        if (!t) {
            error = "cannot set " + pathText(parts, parts.size()) + ": " + pathText(parts, i + 1) + " is not a table";
            return false;
        }
    }
    toml::table litDoc;
    if (!parseToml("v = " + literal, litDoc, error)) return false;
    toml::node* litNode = litDoc.get("v");
    if (!litNode) {
        error = "invalid value " + literal;
        return false;
    }
    litNode->visit([&](auto&& v) { t->insert_or_assign(key, v); });
    std::ostringstream ss;
    ss << root;
    text = ss.str() + "\n";
    return true;
}

}  // namespace

bool tomlSet(const std::string& path, const std::vector<std::pair<std::string, std::string>>& dottedKeyToTomlValue,
             std::string* error) {
    std::string err;
    std::string text;
    bool exists = false;
    if (!readTextFile(path, text, &exists) && exists) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    {
        toml::table check;
        if (!parseToml(text, check, err)) {
            if (error) *error = path + " is not valid TOML: " + err;
            return false;
        }
    }
    for (auto& [dotted, literal] : dottedKeyToTomlValue) {
        std::vector<std::string> parts;
        if (!splitKeyPath(dotted, parts) || parts.empty()) {
            if (error) *error = "invalid TOML key \"" + dotted + "\"";
            return false;
        }
        toml::table lit;
        if (!parseToml("v = " + literal, lit, err)) {
            if (error) *error = "invalid TOML value for " + dotted + ": " + literal;
            return false;
        }
        if (!applyOne(text, parts, literal, err)) {
            if (error) *error = dotted + ": " + err;
            return false;
        }
    }
    if (!writeTextFileAtomic(path, text)) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace rrm::cfg
