#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace gateway::platform {

// Numeric host of a "host:port" peer address, the form libdatachannel reports for an accepted
// WebSocket. Bracketed IPv6 is accepted, and an IPv4-mapped IPv6 host is returned as IPv4.
std::optional<std::string> peerHost(std::string_view peerAddress);

// "AA:BB:CC:DD:EE:FF" for a 6-byte adapter address. Other lengths, the all-zero address and
// group (multicast) addresses cannot be woken by a magic packet and yield nullopt.
std::optional<std::string> formatMacAddress(const unsigned char* bytes, std::size_t length);

// MAC address of the local adapter Windows routes to the peer through, which is the adapter a
// Wake-on-LAN packet sent by that peer has to reach.
std::optional<std::string> wakeOnLanMacAddressForPeer(std::string_view peerAddress);

} // namespace gateway::platform
