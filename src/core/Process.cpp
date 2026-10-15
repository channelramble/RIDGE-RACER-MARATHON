#include "core/Process.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/Platform.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace rrm {

namespace {

#if defined(__APPLE__) && TARGET_OS_IPHONE
constexpr bool kCanSpawn = false;
#elif defined(__ANDROID__)
constexpr bool kCanSpawn = false;
#else
constexpr bool kCanSpawn = true;
#endif

}  // namespace

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Quotes one argument following the MSVCRT CommandLineToArgvW rules.
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::mutex g_mutex;
std::map<int64_t, HANDLE> g_handles;

std::vector<wchar_t> buildEnvBlock(const std::map<std::string, std::string>& extra) {
    std::map<std::wstring, std::wstring, std::less<>> vars;
    // Windows variable names are case-insensitive; normalise keys for merging.
    auto upper = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towupper(c);
        return s;
    };
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> merged;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring entry = p;
            size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            std::wstring k = entry.substr(0, eq);
            merged[upper(k)] = {k, entry.substr(eq + 1)};
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& [k, v] : extra) {
        std::wstring wk = widen(k);
        merged[upper(wk)] = {wk, widen(v)};
    }
    std::vector<wchar_t> block;
    for (auto& [_, kv] : merged) {
        std::wstring line = kv.first + L"=" + kv.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

HANDLE openLog(const std::string& path) {
    if (path.empty()) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(widen(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return h;
}

}  // namespace

int64_t spawnProcess(const ProcessSpec& spec, std::string& error) {
    if (spec.argv.empty()) {
        error = "empty command";
        return 0;
    }
    std::wstring cmd;
    for (size_t i = 0; i < spec.argv.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quoteArg(widen(spec.argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE log = openLog(spec.logFile);
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    auto env = buildEnvBlock(spec.env);
    std::wstring wd = widen(spec.workingDir);
    BOOL ok = CreateProcessW(widen(spec.argv[0]).c_str(), cmd.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
                             CREATE_UNICODE_ENVIRONMENT, env.data(), wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ") for " + spec.argv[0];
        return 0;
    }
    CloseHandle(pi.hThread);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles[(int64_t)pi.dwProcessId] = pi.hProcess;
    return (int64_t)pi.dwProcessId;
}

bool processRunning(int64_t pid, int* exitCode) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(pid);
        if (it == g_handles.end()) return false;
        h = it->second;
    }
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    if (exitCode) *exitCode = (int)code;
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseHandle(h);
    g_handles.erase(pid);
    return false;
}

void terminateProcess(int64_t pid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(pid);
    if (it != g_handles.end()) TerminateProcess(it->second, 1);
}

int runProcess(const ProcessSpec& spec, std::string* output, const std::function<bool()>& keepWaiting) {
    if (spec.argv.empty()) return -1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (size_t i = 0; i < spec.argv.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quoteArg(widen(spec.argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    auto env = buildEnvBlock(spec.env);
    std::wstring wd = widen(spec.workingDir);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                             env.data(), wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return -1;
    }
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) && n > 0)
            if (output) output->append(buf, n);
    });
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if (keepWaiting && !keepWaiting()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    reader.join();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);
    return (int)code;
}

std::string findOnPath(const std::string& name) {
    wchar_t buf[MAX_PATH];
    std::wstring wname = widen(name);
    if (SearchPathW(nullptr, wname.c_str(), L".exe", MAX_PATH, buf, nullptr)) return narrow(buf);
    return {};
}

#else  // POSIX

namespace {

std::mutex g_mutex;
std::map<int64_t, int> g_exitCodes;  // reaped children

std::vector<std::string> buildEnv(const std::map<std::string, std::string>& extra) {
    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (!eq) continue;
        vars[std::string(*e, eq - *e)] = eq + 1;
    }
    for (auto& [k, v] : extra) vars[k] = v;
    std::vector<std::string> out;
    for (auto& [k, v] : vars) out.push_back(k + "=" + v);
    return out;
}

pid_t forkExec(const ProcessSpec& spec, int outFd, std::string& error) {
    std::vector<std::string> envStrings = buildEnv(spec.env);
    std::vector<char*> envp, argv;
    for (auto& s : envStrings) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    for (auto& s : spec.argv) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return -1;
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls from here on.
        setsid();  // own process group so terminate() can signal the whole tree
        if (outFd >= 0) {
            dup2(outFd, STDOUT_FILENO);
            dup2(outFd, STDERR_FILENO);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        if (!spec.workingDir.empty() && chdir(spec.workingDir.c_str()) != 0) _exit(126);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    return pid;
}

}  // namespace

int64_t spawnProcess(const ProcessSpec& spec, std::string& error) {
    if (!kCanSpawn) {
        error = "Launching other programs is not possible on this platform.";
        return 0;
    }
    if (spec.argv.empty()) {
        error = "empty command";
        return 0;
    }
    if (access(spec.argv[0].c_str(), X_OK) != 0) {
        error = "Not executable: " + spec.argv[0];
        return 0;
    }
    int fd = -1;
    if (!spec.logFile.empty()) fd = open(spec.logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    pid_t pid = forkExec(spec, fd, error);
    if (fd >= 0) close(fd);
    return pid > 0 ? (int64_t)pid : 0;
}

bool processRunning(int64_t pid, int* exitCode) {
    if (pid <= 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_exitCodes.find(pid);
        if (it != g_exitCodes.end()) {
            if (exitCode) *exitCode = it->second;
            return false;
        }
    }
    int status = 0;
    pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
    if (r == 0) return true;
    int code = -1;
    if (r == (pid_t)pid) code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_exitCodes[pid] = code;
    if (exitCode) *exitCode = code;
    return false;
}

void terminateProcess(int64_t pid) {
    if (pid <= 0 || !processRunning(pid)) return;
    kill(-(pid_t)pid, SIGTERM);
    kill((pid_t)pid, SIGTERM);
    std::thread([pid] {
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!processRunning(pid)) return;
        }
        kill(-(pid_t)pid, SIGKILL);
        kill((pid_t)pid, SIGKILL);
    }).detach();
}

int runProcess(const ProcessSpec& spec, std::string* output, const std::function<bool()>& keepWaiting) {
    if (!kCanSpawn || spec.argv.empty()) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    std::string error;
    ProcessSpec s = spec;
    if (s.argv[0].find('/') == std::string::npos) {
        std::string found = findOnPath(s.argv[0]);
        if (found.empty()) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
        s.argv[0] = found;
    }
    pid_t pid = forkExec(s, fds[1], error);
    close(fds[1]);
    if (pid <= 0) {
        close(fds[0]);
        return -1;
    }
    std::thread reader([&] {
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0)
            if (output) output->append(buf, (size_t)n);
    });
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) break;
        if (keepWaiting && !keepWaiting()) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    reader.join();
    close(fds[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string findOnPath(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin:/usr/sbin:/sbin";
#if defined(__APPLE__)
    p += ":/opt/homebrew/bin:/usr/local/bin";
#endif
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        start = end + 1;
    }
    return {};
}

#endif

}  // namespace rrm
