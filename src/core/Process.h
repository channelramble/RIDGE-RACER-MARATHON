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
void terminateProcess(int64_t pid);  // polite, then forceful after a grace period (3 s)
// Asks a process and the programs it started to quit, the way closing their windows would:
// WM_CLOSE to their visible top-level windows on Windows, SIGTERM to the process group elsewhere.
// Returns false when nothing could be asked (e.g. no window yet on Windows).
bool requestProcessExit(int64_t pid);
// Ends a process and the programs it started at once (TerminateJobObject / SIGKILL to the group).
void killProcess(int64_t pid);

// Runs to completion, capturing output (for small helper tools). Returns exit code or -1.
int runProcess(const ProcessSpec& spec, std::string* output = nullptr,
               const std::function<bool()>& keepWaiting = {});

// Finds an executable on PATH; returns "" if absent.
std::string findOnPath(const std::string& name);

}  // namespace rrm
