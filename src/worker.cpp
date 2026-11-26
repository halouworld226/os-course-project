#include "worker.hpp"
#include "framing.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <utility>
#include <chrono>
#include <thread>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

std::mutex spawn_mutex;

#ifdef _WIN32
using PipeHandle = HANDLE;
const PipeHandle invalid_pipe = nullptr;
void close_pipe(PipeHandle handle) { CloseHandle(handle); }
#else
using PipeHandle = int;
constexpr PipeHandle invalid_pipe = -1;
void close_pipe(PipeHandle handle) { close(handle); }
#endif

struct PipeEnd {
    PipeHandle handle = invalid_pipe;
    PipeEnd() = default;
    explicit PipeEnd(PipeHandle value) : handle(value) {}
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;
    PipeEnd(PipeEnd&& other) noexcept : handle(std::exchange(other.handle, invalid_pipe)) {}
    PipeEnd& operator=(PipeEnd&& other) noexcept {
        reset();
        handle = std::exchange(other.handle, invalid_pipe);
        return *this;
    }
    ~PipeEnd() { reset(); }
    void reset() {
        if (handle != invalid_pipe) close_pipe(std::exchange(handle, invalid_pipe));
    }
};

#ifdef _WIN32
struct ProcessAttributes {
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    ProcessAttributes() {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) throw std::runtime_error("cannot size worker process attributes");
        list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
        if (!list) throw std::runtime_error("cannot allocate worker process attributes");
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
            HeapFree(GetProcessHeap(), 0, list);
            list = nullptr;
            throw std::runtime_error("cannot initialize worker process attributes");
        }
    }

    ~ProcessAttributes() {
        if (list) {
            DeleteProcThreadAttributeList(list);
            HeapFree(GetProcessHeap(), 0, list);
        }
    }
};

PipeEnd child_error_handle() {
    const auto source = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE duplicate = nullptr;
    if (source && source != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return PipeEnd(duplicate);
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    duplicate = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (duplicate == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create worker error stream");
    return PipeEnd(duplicate);
}
#endif

std::pair<PipeEnd, PipeEnd> make_pipe() {
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE input, output;
    if (!CreatePipe(&input, &output, &attributes, 0)) throw std::runtime_error("CreatePipe failed");
#else
    int ends[2];
    if (pipe(ends) == -1) throw std::runtime_error("pipe failed");
    if (fcntl(ends[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(ends[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(ends[0]);
        close(ends[1]);
        throw std::runtime_error("cannot secure worker pipe descriptors");
    }
    const auto input = ends[0];
    const auto output = ends[1];
#endif
    return {PipeEnd(input), PipeEnd(output)};
}

std::size_t read_pipe(PipeHandle handle, std::uint8_t* data, std::size_t size) {
#ifdef _WIN32
    DWORD count = 0;
    if (ReadFile(handle, data, static_cast<DWORD>(size), &count, nullptr)) return count;
    if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
#else
    ssize_t count;
    do { count = read(handle, data, size); } while (count < 0 && errno == EINTR);
    if (count >= 0) return static_cast<std::size_t>(count);
#endif
    throw std::runtime_error("worker pipe read failed");
}

std::size_t write_pipe(PipeHandle handle, const std::uint8_t* data, std::size_t size) {
#ifdef _WIN32
    DWORD count = 0;
    if (WriteFile(handle, data, static_cast<DWORD>(size), &count, nullptr)) return count;
#else
    ssize_t count;
    do { count = write(handle, data, size); } while (count < 0 && errno == EINTR);
    if (count >= 0) return static_cast<std::size_t>(count);
#endif
    throw std::runtime_error("worker pipe write failed");
}

} // namespace

struct Worker::State {
    PipeEnd request;
    PipeEnd response;
    unsigned long pid = 0;
#ifdef _WIN32
    HANDLE process = nullptr;
#endif
    bool active = false;

    bool stop() noexcept {
        request.reset();
        response.reset();
        if (!active) return true;
        active = false;
#ifdef _WIN32
        DWORD code = 1;
        const auto success = WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0 &&
                             GetExitCodeProcess(process, &code) && code == 0;
        CloseHandle(process);
        return success;
#else
        int status = 0;
        pid_t reaped;
        do { reaped = waitpid(static_cast<pid_t>(pid), &status, 0); } while (reaped < 0 && errno == EINTR);
        return reaped > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    }

    ~State() { stop(); }
};

Worker::Worker(const std::filesystem::path& executable) : state_(std::make_unique<State>()) {
    std::lock_guard<std::mutex> spawn_lock(spawn_mutex);
    auto [child_input, parent_output] = make_pipe();
    auto [parent_input, child_output] = make_pipe();
#ifdef _WIN32
    if (!SetHandleInformation(parent_output.handle, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_input.handle, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("cannot secure parent pipe handles");
    }
    auto child_error = child_error_handle();
    ProcessAttributes attributes;
    HANDLE inherited[]{child_input.handle, child_output.handle, child_error.handle};
    if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        throw std::runtime_error("cannot restrict worker handle inheritance");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input.handle;
    startup.StartupInfo.hStdOutput = child_output.handle;
    startup.StartupInfo.hStdError = child_error.handle;
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + executable.wstring() + L"\" --worker";
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &startup.StartupInfo, &process)) throw std::runtime_error("cannot start worker process");
    CloseHandle(process.hThread);
    state_->process = process.hProcess;
    state_->pid = process.dwProcessId;
#else
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("cannot initialize worker spawn actions");
    int error = posix_spawn_file_actions_adddup2(&actions, child_input.handle, STDIN_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, child_output.handle, STDOUT_FILENO);
    for (auto fd : {child_input.handle, child_output.handle, parent_input.handle, parent_output.handle}) {
        if (!error && fd != STDIN_FILENO && fd != STDOUT_FILENO) error = posix_spawn_file_actions_addclose(&actions, fd);
    }
    auto path = executable.string();
    char worker_arg[] = "--worker";
    char* args[]{path.data(), worker_arg, nullptr};
    pid_t pid = 0;
    if (!error) error = posix_spawn(&pid, path.c_str(), &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) throw std::runtime_error("cannot start worker process");
    state_->pid = static_cast<unsigned long>(pid);
#endif
    state_->active = true;
    state_->request = std::move(parent_output);
    state_->response = std::move(parent_input);
}

Worker::~Worker() = default;

unsigned long Worker::pid() const { return state_->pid; }

void Worker::terminate() {
    if (!state_->active) throw std::runtime_error("worker has already stopped");
#ifdef _WIN32
    if (!TerminateProcess(state_->process, 99)) throw std::runtime_error("cannot terminate owned worker");
#else
    if (kill(static_cast<pid_t>(state_->pid), SIGKILL) != 0 && errno != ESRCH) {
        throw std::runtime_error("cannot terminate owned worker");
    }
#endif
}

Bytes Worker::process(Operation operation, const Bytes& rgb, bool inject_failure) {
    Bytes request;
    request.reserve(1 + rgb.size());
    request.push_back(static_cast<std::uint8_t>(operation) | (inject_failure ? 128 : 0));
    request.insert(request.end(), rgb.begin(), rgb.end());
    write_frame([&](auto* data, auto size) { return write_pipe(state_->request.handle, data, size); }, request);
    if (inject_failure) terminate();
    Bytes result;
    if (!read_frame([&](auto* data, auto size) { return read_pipe(state_->response.handle, data, size); },
                    result, rgb.size())) throw std::runtime_error("worker exited before returning a tile");
    return result;
}

void Worker::finish() {
    if (!state_->stop()) throw std::runtime_error("worker did not exit successfully");
}

int worker_main() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    auto input = [](auto* data, auto size) { return std::fread(data, 1, size, stdin); };
    auto output = [](auto* data, auto size) { return std::fwrite(data, 1, size, stdout); };
    try {
        unsigned delay_ms = 0;
        if (const auto* delay = std::getenv("OS_DEMO_DELAY_MS")) {
            const auto end = delay + std::strlen(delay);
            const auto parsed = std::from_chars(delay, end, delay_ms);
            if (parsed.ec != std::errc{} || parsed.ptr != end || delay_ms > 2000) {
                throw std::runtime_error("OS_DEMO_DELAY_MS must be 0..2000");
            }
        }
        Bytes tile;
        while (read_frame(input, tile, 1 + tile_edge * tile_edge * 3)) {
            if (tile.size() < 4) throw std::runtime_error("missing operation or RGB pixel");
            const auto operation = static_cast<Operation>(tile.front() & 127);
            if (tile.front() & 128) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            tile.erase(tile.begin());
            if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            process_pixels(operation, tile);
            write_frame(output, tile);
            if (std::fflush(stdout) != 0) return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "worker: " << error.what() << '\n';
        return 1;
    }
    return std::ferror(stdin) ? 1 : 0;
}
