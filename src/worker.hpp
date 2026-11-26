#pragma once

#include "image.hpp"

#include <filesystem>
#include <memory>

class Worker {
public:
    explicit Worker(const std::filesystem::path& executable);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    unsigned long pid() const;
    Bytes process(Operation operation, const Bytes& rgb, bool inject_failure = false);
    void terminate();
    void finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

int worker_main();
