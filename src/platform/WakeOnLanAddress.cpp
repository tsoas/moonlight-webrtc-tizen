#include "platform/WakeOnLanAddress.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#endif

namespace gateway::platform {

std::optional<std::string> peerHost(std::string_view peerAddress)
{
    std::string_view host;
    if (!peerAddress.empty() && peerAddress.front() == '[') {
        const auto closing = peerAddress.find(']');
        if (closing == std::string_view::npos) {
            return std::nullopt;
        }
        host = peerAddress.substr(1, closing - 1);
    } else {
        const auto separator = peerAddress.rfind(':');
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        host = peerAddress.substr(0, separator);
    }

    // getnameinfo() spells IPv4-mapped addresses in lower case.
    constexpr std::string_view MappedIpv4Prefix = "::ffff:";
    if (host.substr(0, MappedIpv4Prefix.size()) == MappedIpv4Prefix
        && host.find('.') != std::string_view::npos) {
        host.remove_prefix(MappedIpv4Prefix.size());
    }
    if (host.empty()) {
        return std::nullopt;
    }
    return std::string(host);
}

std::optional<std::string> formatMacAddress(const unsigned char* bytes, std::size_t length)
{
    constexpr std::size_t MacAddressLength = 6;
    if (bytes == nullptr || length != MacAddressLength) {
        return std::nullopt;
    }
    const bool allZero = std::all_of(bytes, bytes + length, [](unsigned char byte) { return byte == 0; });
    if (allZero || (bytes[0] & 0x01) != 0) {
        return std::nullopt;
    }
    char text[18] = {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return std::string(text);
}

std::optional<std::string> wakeOnLanMacAddressForPeer(std::string_view peerAddress)
{
#ifndef _WIN32
    (void)peerAddress;
    return std::nullopt;
#else
    auto host = peerHost(peerAddress);
    if (!host) {
        return std::nullopt;
    }

    SOCKADDR_INET destination{};
    ULONG scopeId = 0;
    if (const auto zone = host->find('%'); zone != std::string::npos) {
        scopeId = std::strtoul(host->c_str() + zone + 1, nullptr, 10);
        host->erase(zone);
    }
    if (InetPtonA(AF_INET, host->c_str(), &destination.Ipv4.sin_addr) == 1) {
        destination.si_family = AF_INET;
    } else if (InetPtonA(AF_INET6, host->c_str(), &destination.Ipv6.sin6_addr) == 1) {
        destination.si_family = AF_INET6;
        destination.Ipv6.sin6_scope_id = scopeId;
    } else {
        return std::nullopt;
    }

    DWORD interfaceIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex) != NO_ERROR) {
        return std::nullopt;
    }

    constexpr ULONG Flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST
        | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    // The adapter list can grow between the sizing call and the real one.
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, Flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) {
        return std::nullopt;
    }

    for (auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr;
         adapter = adapter->Next) {
        const DWORD adapterIndex = destination.si_family == AF_INET ? adapter->IfIndex : adapter->Ipv6IfIndex;
        if (adapterIndex == interfaceIndex) {
            return formatMacAddress(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        }
    }
    return std::nullopt;
#endif
}

} // namespace gateway::platform
