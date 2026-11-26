#pragma once

#include "scheduler.hpp"

#include <future>
#include <memory>

using EventCallback = std::function<void(const std::string&)>;

// Callbacks run serially in the scheduler. They must return without waiting for
// another scheduler operation or calling submit recursively.
class SharedScheduler {
public:
    explicit SharedScheduler(const std::filesystem::path& executable, unsigned workers = 4);
    ~SharedScheduler();
    SharedScheduler(const SharedScheduler&) = delete;
    SharedScheduler& operator=(const SharedScheduler&) = delete;

    std::future<SchedulerResult> submit(std::string job_id, Image source, Operation operation,
                                        bool high, TileCommit commit, EventCallback on_event,
                                        bool inject_failure = false);
    bool terminate_worker(unsigned long pid);
    std::vector<unsigned long> busy_pids() const;
    std::string snapshot() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
