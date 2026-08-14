#pragma once

#include "gateway/ServiceIpcProtocol.h"

#include <atomic>
#include <functional>
#include <thread>

namespace gateway::serviceipc {

class ServiceIpcServer {
public:
    using StatusProvider = std::function<StatusSnapshot()>;

    explicit ServiceIpcServer(StatusProvider statusProvider);
    ~ServiceIpcServer();

    ServiceIpcServer(const ServiceIpcServer&) = delete;
    ServiceIpcServer& operator=(const ServiceIpcServer&) = delete;

    void start();
    void stop();

private:
    void run();

    StatusProvider statusProvider_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<void*> activePipe_ = nullptr;
    std::thread thread_;
};

} // namespace gateway::serviceipc
