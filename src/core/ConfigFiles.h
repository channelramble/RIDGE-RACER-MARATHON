// Minimal, comment-preserving editors for the config formats backends use.
// They change only the keys Marathon owns and keep everything else intact.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rrm::cfg {

// INI ("[Section]\nKey = Value"). Missing sections/keys are appended.
class Ini {
public:
    bool load(const std::string& path);  // a missing file is an empty document
    bool save(const std::string& path) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    // Keys that repeat, one value per line (PCSX2's bindings: "Cross = A" then "Cross = B").
    // setList() removes every line of the key in the section and writes the values as
    // consecutive lines where the first one was (else at the end of the section); an empty
    // list just removes the key.
    void setList(const std::string& section, const std::string& key, const std::vector<std::string>& values);
    std::vector<std::string> getList(const std::string& section, const std::string& key) const;
    // Makes the section hold exactly these "key = value" lines (comments in it are dropped;
    // a repeated section header is merged into the first one).
    void replaceSection(const std::string& section, const std::vector<std::pair<std::string, std::string>>& lines);
    bool hasSection(const std::string& section) const;
    std::string text() const;
    void parse(const std::string& text);
private:
    std::vector<std::string> lines_;
    bool spacedEquals_ = true;  // "Key = Value" vs "Key=Value", learnt from the file
};

// Indentation-based YAML maps of scalars, the subset RPCS3's config.yml uses:
// set({"Video", "Resolution Scale"}, "300").
class YamlMap {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void set(const std::vector<std::string>& keyPath, const std::string& value);
    std::string get(const std::vector<std::string>& keyPath, const std::string& def = "") const;
    std::string text() const;
    void parse(const std::string& text);
private:
    std::vector<std::string> lines_;
};

// TOML edits go through toml++ (third_party/tomlplusplus): see TomlEdit.cpp.
bool tomlSet(const std::string& path, const std::vector<std::pair<std::string, std::string>>& dottedKeyToTomlValue,
             std::string* error = nullptr);

}  // namespace rrm::cfg
