// Incast Governor - real OS process harness for multiprocess tests.
//
// Spawns genuine child processes with a redirected stdout pipe, can terminate
// them with an uncatchable kill, and never uses a timeout: a hang is a defect
// and must surface as a hang.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_TESTS_PROCESS_HARNESS_HPP
#define INCAST_TESTS_PROCESS_HARNESS_HPP

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace igprocess {

enum class ReadResult { Line, EndOfStream, Error };

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { terminate(); close(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& executable, const std::vector<std::string>& arguments);
    ReadResult read_line(std::string& line);
    void terminate();
    void close();
    // Blocks until the process exits without any timeout. Returns the exit code.
    int wait();
    [[nodiscard]] bool running() const;

private:
#if defined(_WIN32)
    void* process_ = nullptr;
    void* thread_ = nullptr;
    void* read_handle_ = nullptr;
    void* write_handle_ = nullptr;
    int exit_code_ = 0;
    bool reaped_ = false;
#else
    int pid_ = -1;
    int read_fd_ = -1;
    int write_fd_ = -1;
    int exit_code_ = 0;
    bool reaped_ = false;
#endif
    std::string pending_{};
};

}  // namespace igprocess

#if defined(_WIN32)

namespace igprocess {
namespace detail {

inline std::string quote_argument(const std::string& argument) {
    if (argument.find_first_of(" \t\"") == std::string::npos) return argument;
    std::string result("\"");
    for (const char character : argument) {
        if (character == '"') result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

}  // namespace detail
}  // namespace igprocess

inline bool igprocess::ChildProcess::start(const std::string& executable,
                                           const std::vector<std::string>& arguments) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (CreatePipe(&read_handle, &write_handle, &attributes, 0) == FALSE) return false;
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

    std::string command = detail::quote_argument(executable);
    for (const auto& argument : arguments) {
        command.push_back(' ');
        command.append(detail::quote_argument(argument));
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_handle;
    startup.hStdError = write_handle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION information{};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &information);
    CloseHandle(write_handle);
    if (created == FALSE) {
        CloseHandle(read_handle);
        return false;
    }
    process_ = information.hProcess;
    thread_ = information.hThread;
    read_handle_ = read_handle;
    return true;
}

inline igprocess::ReadResult igprocess::ChildProcess::read_line(std::string& line) {
    for (;;) {
        const std::size_t newline = pending_.find('\n');
        if (newline != std::string::npos) {
            line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return ReadResult::Line;
        }
        char buffer[512];
        DWORD read = 0;
        const BOOL ok = ReadFile(static_cast<HANDLE>(read_handle_), buffer, sizeof(buffer), &read, nullptr);
        if (ok == FALSE || read == 0) {
            if (!pending_.empty()) {
                line = pending_;
                pending_.clear();
                return ReadResult::Line;
            }
            return ReadResult::EndOfStream;
        }
        pending_.append(buffer, read);
    }
}

inline void igprocess::ChildProcess::terminate() {
    if (process_ == nullptr) return;
    if (running()) {
        TerminateProcess(static_cast<HANDLE>(process_), 0xC000013A);
    }
}

inline int igprocess::ChildProcess::wait() {
    if (process_ == nullptr) return -1;
    if (!reaped_) {
        WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
        exit_code_ = static_cast<int>(code);
        reaped_ = true;
    }
    return exit_code_;
}

inline bool igprocess::ChildProcess::running() const {
    if (process_ == nullptr) return false;
    return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

inline void igprocess::ChildProcess::close() {
    if (read_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(read_handle_));
    if (thread_ != nullptr) CloseHandle(static_cast<HANDLE>(thread_));
    if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
    read_handle_ = nullptr;
    thread_ = nullptr;
    process_ = nullptr;
}

#else

inline bool igprocess::ChildProcess::start(const std::string& executable,
                                           const std::vector<std::string>& arguments) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    std::vector<std::string> storage;
    storage.push_back(executable);
    for (const auto& argument : arguments) storage.push_back(argument);
    std::vector<char*> argv;
    for (auto& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, write_fd_, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, write_fd_, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, read_fd_);
    const int spawned = posix_spawn(&pid_, executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(write_fd_);
    write_fd_ = -1;
    if (spawned != 0) {
        ::close(read_fd_);
        read_fd_ = -1;
        pid_ = -1;
        return false;
    }
    return true;
}

inline igprocess::ReadResult igprocess::ChildProcess::read_line(std::string& line) {
    for (;;) {
        const std::size_t newline = pending_.find('\n');
        if (newline != std::string::npos) {
            line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return ReadResult::Line;
        }
        char buffer[512];
        const ssize_t read = ::read(read_fd_, buffer, sizeof(buffer));
        if (read <= 0) {
            if (!pending_.empty()) {
                line = pending_;
                pending_.clear();
                return ReadResult::Line;
            }
            return read == 0 ? ReadResult::EndOfStream : ReadResult::Error;
        }
        pending_.append(buffer, static_cast<std::size_t>(read));
    }
}

inline void igprocess::ChildProcess::terminate() {
    if (pid_ > 0 && running()) ::kill(pid_, SIGKILL);
}

inline int igprocess::ChildProcess::wait() {
    if (pid_ <= 0) return -1;
    if (!reaped_) {
        int status = 0;
        waitpid(pid_, &status, 0);
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        reaped_ = true;
    }
    return exit_code_;
}

inline bool igprocess::ChildProcess::running() const {
    if (pid_ <= 0) return false;
    int status = 0;
    return waitpid(pid_, &status, WNOHANG) == 0;
}

inline void igprocess::ChildProcess::close() {
    if (read_fd_ >= 0) ::close(read_fd_);
    read_fd_ = -1;
}

#endif

#endif  // INCAST_TESTS_PROCESS_HARNESS_HPP
