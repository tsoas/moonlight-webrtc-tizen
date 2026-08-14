#pragma once

#include "gateway/ManagementIpcProtocol.h"

#include <atomic>
#include <functional>
#include <thread>

namespace gateway::managementipc {

class ManagementIpcClient {
public:
    using Handler = std::function<Result(const Command&)>;
    using Logger = std::function<void(const std::string&)>;
    ManagementIpcClient(Handler handler, Logger logger);
    ~ManagementIpcClient();
    void start();
    void stop();

private:
    void run();
    Handler handler_;
    Logger logger_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<void*> activePipe_ = nullptr;
    std::thread thread_;
};
} // namespace gateway::managementipc
