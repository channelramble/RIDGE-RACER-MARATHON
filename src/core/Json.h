#pragma once

#include <filesystem>
#include <string>

#include "nlohmann/json.hpp"

namespace rrm {

using json = nlohmann::json;

bool readJsonFile(const std::filesystem::path& path, json& out, std::string* error = nullptr);
bool writeJsonFile(const std::filesystem::path& path, const json& value);  // atomic replace

template <typename T>
T jget(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->template get<T>();
    } catch (...) {
        return fallback;
    }
}

}  // namespace rrm
