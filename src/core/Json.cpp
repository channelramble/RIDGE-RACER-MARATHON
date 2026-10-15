#include "core/Json.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace rrm {

bool readJsonFile(const std::filesystem::path& path, json& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        out = json::parse(ss.str(), nullptr, true, true /* ignore comments */);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = path.string() + ": " + e.what();
        return false;
    }
}

bool writeJsonFile(const std::filesystem::path& path, const json& value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump(2) << '\n';
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows cannot rename over an existing file in every case.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

}  // namespace rrm
