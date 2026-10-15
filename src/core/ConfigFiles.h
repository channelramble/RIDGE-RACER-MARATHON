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
