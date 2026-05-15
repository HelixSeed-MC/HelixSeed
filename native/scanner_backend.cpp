#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)
static std::wstring quote_arg(const std::wstring &arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    bool needs_quotes = false;
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return arg;
    }

    std::wstring out;
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(c);
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

static std::wstring join_commandline(const std::vector<std::wstring> &parts) {
    std::wstring cmd;
    bool first = true;
    for (const std::wstring &p : parts) {
        if (!first) {
            cmd.push_back(L' ');
        }
        first = false;
        cmd += quote_arg(p);
    }
    return cmd;
}

static int run_process(const std::vector<std::wstring> &parts, const std::wstring &workdir) {
    std::wstring cmdline = join_commandline(parts);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        workdir.empty() ? nullptr : workdir.c_str(),
        &si,
        &pi
    );
    if (!ok) {
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}

static fs::path detect_executable_path() {
    wchar_t exe_path_buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return fs::path(exe_path_buf);
}

int wmain(int argc, wchar_t **argv) {
    const fs::path exe_path = detect_executable_path();
    if (exe_path.empty()) {
        return 1;
    }

    fs::path native_dir = exe_path.parent_path();
    fs::path repo_root = native_dir.parent_path();

    const std::vector<fs::path> native_candidates = {
        repo_root / L"native" / L"scanner_native.exe",
        repo_root / L"scanner_native.exe",
    };

    for (const fs::path &candidate : native_candidates) {
        if (!fs::exists(candidate)) {
            continue;
        }
        std::vector<std::wstring> cmd_parts;
        cmd_parts.push_back(candidate.wstring());
        for (int i = 1; i < argc; ++i) {
            cmd_parts.push_back(argv[i]);
        }
        return run_process(cmd_parts, repo_root.wstring());
    }

    std::fwprintf(
        stderr,
        L"[scanner_backend] Native-only mode is enabled and Python fallback is disabled.\n"
        L"[scanner_backend] Missing native scanner CLI. Expected one of:\n"
        L"  - %ls\n"
        L"  - %ls\n",
        native_candidates[0].c_str(),
        native_candidates[1].c_str()
    );
    return 2;
}
#else
static fs::path detect_executable_path(const char *argv0) {
    std::vector<char> path_buf(4096, '\0');
    const ssize_t n = readlink("/proc/self/exe", path_buf.data(), path_buf.size() - 1U);
    if (n > 0) {
        path_buf[static_cast<size_t>(n)] = '\0';
        return fs::path(path_buf.data());
    }
    return fs::absolute(fs::path(argv0));
}

static int run_process(const std::vector<std::string> &parts, const fs::path &workdir) {
    std::vector<char *> argv;
    argv.reserve(parts.size() + 1U);
    for (const std::string &part : parts) {
        argv.push_back(const_cast<char *>(part.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        if (!workdir.empty()) {
            (void)chdir(workdir.string().c_str());
        }
        execvp(argv[0], argv.data());
        std::perror("execvp");
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main(int argc, char **argv) {
    const fs::path exe_path = detect_executable_path(argv[0]);
    fs::path native_dir = exe_path.parent_path();
    fs::path repo_root = native_dir.parent_path();

    const std::vector<fs::path> native_candidates = {
        repo_root / "native" / "scanner_native",
        repo_root / "scanner_native",
    };

    for (const fs::path &candidate : native_candidates) {
        if (!fs::exists(candidate)) {
            continue;
        }
        std::vector<std::string> cmd_parts;
        cmd_parts.push_back(candidate.string());
        for (int i = 1; i < argc; ++i) {
            cmd_parts.push_back(argv[i]);
        }
        return run_process(cmd_parts, repo_root);
    }

    std::fprintf(
        stderr,
        "[scanner_backend] Native-only mode is enabled and Python fallback is disabled.\n"
        "[scanner_backend] Missing native scanner CLI. Expected one of:\n"
        "  - %s\n"
        "  - %s\n",
        native_candidates[0].c_str(),
        native_candidates[1].c_str()
    );
    return 2;
}
#endif
