// TEMPORARY, TEST-ONLY stand-in for src/core/RomSet.cpp, used by
// tests/recomp/CMakeLists.txt only when the real file (or miniz) is missing.
// Every operation fails with an explanation. Never add this to the main build.
#include "core/RomSet.h"

namespace rrm {

bool findFileByCrc(const std::string&, const std::string&, std::string&) { return false; }

bool writeStoredZip(const std::vector<std::pair<std::string, std::string>>&, const std::string&, std::string& error) {
    error = "RomSet is not available in this test build";
    return false;
}

bool writeSingleFileZip(const std::string&, const std::string&, const std::string&, std::string& error) {
    error = "RomSet is not available in this test build";
    return false;
}

bool rebuildMameSet(const GameDef&, const std::string&, const std::string&, std::string& error) {
    error = "RomSet is not available in this test build";
    return false;
}

}  // namespace rrm
