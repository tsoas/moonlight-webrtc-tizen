#include "gateway/GatewayLifecycle.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    gateway::GatewayShutdownSignal shutdown;
    require(!shutdown.requested(), "New lifecycle signal must not already be stopped");
    require(!shutdown.waitFor(std::chrono::milliseconds(1)), "Shutdown signal returned too early");

    std::thread waiter([&shutdown] { shutdown.wait(); });
    shutdown.request();
    waiter.join();

    require(shutdown.requested(), "Shutdown request was not preserved");
    require(shutdown.waitFor(std::chrono::milliseconds(1)), "Repeated shutdown wait must be immediate");
    return 0;
}
