#include "platform/WakeOnLanAddress.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    using gateway::platform::formatMacAddress;
    using gateway::platform::peerHost;
    using gateway::platform::wakeOnLanMacAddressForPeer;

    try {
        require(peerHost("192.168.1.50:54321") == "192.168.1.50",
                "An IPv4 peer must drop its port");
        require(peerHost("::ffff:192.168.1.50:54321") == "192.168.1.50",
                "An IPv4-mapped peer must be reported as IPv4");
        require(peerHost("[fe80::1%12]:8000") == "fe80::1%12",
                "A bracketed IPv6 peer must keep its zone");
        require(peerHost("fe80::1:8000") == "fe80::1",
                "An unbracketed IPv6 peer must drop only its port");
        require(!peerHost("no-port") && !peerHost(":8000") && !peerHost("[::1"),
                "Malformed peer addresses must be rejected");

        const unsigned char adapter[] = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
        require(formatMacAddress(adapter, sizeof(adapter)) == "00:1A:2B:3C:4D:5E",
                "An adapter address must use the canonical upper-case form");
        const unsigned char zero[6] = {};
        const unsigned char multicast[] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
        require(!formatMacAddress(zero, sizeof(zero)) && !formatMacAddress(multicast, sizeof(multicast)),
                "Addresses that a magic packet cannot wake must be rejected");
        require(!formatMacAddress(adapter, 8) && !formatMacAddress(nullptr, 6),
                "Only 6-byte adapter addresses are Wake-on-LAN targets");

        require(!wakeOnLanMacAddressForPeer("not an address:1"),
                "An unparseable peer must not resolve to an adapter");
        // Loopback has no physical address, so it must never be offered to a TV.
        require(!wakeOnLanMacAddressForPeer("127.0.0.1:8000"),
                "The loopback adapter must not be offered as a Wake-on-LAN target");

        std::cout << "Wake-on-LAN address tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Wake-on-LAN address test failed: " << error.what() << '\n';
        return 1;
    }
}
