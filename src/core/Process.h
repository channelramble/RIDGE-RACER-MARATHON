// Child processes (desktop only). Mobile builds compile these as stubs.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rrm {

struct ProcessSpec {
    std::vector<std::string> argv;              // argv[0] = executable path
    std::string workingDir;
    std::map<std::string, std::string> env;     // added/overridden variables
    std::string logFile;                        // stdout+stderr appended here if set
};

// Starts a detached-but-tracked process. Returns pid > 0 on success.
int64_t spawnProcess(const ProcessSpec& spec, std::string& error);
// Non-blocking: true while running; fills exitCode when it has exited.
bool processRunning(int64_t pid, int* exitCode = nullptr);
void terminateProcess(int64_t pid);  // polite, then forceful after a grace period

// Runs to completion, capturing output (for small helper tools). Returns exit code or -1.
int runProcess(const ProcessSpec& spec, std::string* output = nullptr,
               const std::function<bool()>& keepWaiting = {});

// Finds an executable on PATH; returns "" if absent.
std::string findOnPath(const std::string& name);

}  // namespace rrm
