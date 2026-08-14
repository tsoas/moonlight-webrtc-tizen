#pragma once

#include "gateway/GatewayLifecycle.h"

#include <functional>
#include <string>

namespace gateway {

class WindowsServiceHost {
public:
    using ReadyCallback = std::function<void()>;
    using RuntimeCallback = std::function<int(const ReadyCallback&)>;

    static int run(const std::wstring& serviceName,
                   GatewayShutdownSignal& shutdown,
                   const RuntimeCallback& runtime);
};

} // namespace gateway
