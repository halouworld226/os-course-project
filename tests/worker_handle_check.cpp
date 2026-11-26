#include "worker.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--worker") return worker_main();
#ifdef _WIN32
    wchar_t directory[MAX_PATH + 1]{};
    wchar_t path[MAX_PATH + 1]{};
    HANDLE unrelated = INVALID_HANDLE_VALUE;
    try {
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"osh", 0, path)) {
            throw std::runtime_error("cannot create handle inheritance test file");
        }
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        unrelated = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, &attributes, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (unrelated == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock test file");
        Worker worker(std::filesystem::absolute(argv[0]));
        CloseHandle(unrelated);
        unrelated = INVALID_HANDLE_VALUE;
        // If the child inherited the unrelated exclusive file handle, reopening
        // this file fails until the worker exits. Keep the worker alive here.
        unrelated = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (unrelated == INVALID_HANDLE_VALUE) throw std::runtime_error("worker inherited unrelated file lock");
        CloseHandle(unrelated);
        unrelated = INVALID_HANDLE_VALUE;
        const auto output = worker.process(Operation::invert, Bytes{0, 10, 255});
        if (output != Bytes{255, 245, 0}) throw std::runtime_error("worker pipes failed after handle restriction");
        worker.finish();
        DeleteFileW(path);
        std::cout << "PASS: active worker does not inherit unrelated exclusive file handle; pipe processing succeeds\n";
    } catch (const std::exception& error) {
        if (unrelated != INVALID_HANDLE_VALUE) CloseHandle(unrelated);
        if (path[0]) DeleteFileW(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    std::cout << "SKIP: Windows handle inheritance test\n";
#endif
}
