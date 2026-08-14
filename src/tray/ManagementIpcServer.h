#pragma once

#include "gateway/ManagementIpcProtocol.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace gateway::tray {
class ManagementIpcServer {
public:
    ManagementIpcServer();
    ~ManagementIpcServer();
    void start();
    void stop();
    managementipc::Result execute(const managementipc::Command& command);

private:
    void run();
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<managementipc::Command> pending_;
    std::optional<managementipc::Result> result_;
    std::string lastFailure_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<void*> activePipe_ = nullptr;
    std::thread thread_;
};
} // namespace gateway::tray
