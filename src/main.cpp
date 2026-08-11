#include <iostream>

#include <rtc/rtc.hpp>

int main()
{
    rtc::Configuration configuration;
    rtc::PeerConnection peerConnection(configuration);

    peerConnection.onStateChange([](rtc::PeerConnection::State state) {
        std::cout << "PeerConnection state: " << state << '\n';
    });

    std::cout << "libdatachannel PeerConnection created OK\n";
    return 0;
}
