// Wake-on-LAN sender for the Tizen client.
//
// A Tizen web application cannot send a UDP datagram from JavaScript. Samsung's Tizen Sockets
// extension exposes POSIX sockets to WebAssembly instead, but refuses them on the browser main
// thread, so every request is sent from its own detached pthread and its outcome is posted back
// to the main thread as Module.onWakeResult(requestId, sentCount, errorCode).
//
// The destinations follow BrightCraft Moonlight Tizen's Wake-on-LAN implementation.

#include <arpa/inet.h>
#include <emscripten.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    MacAddressSize = 6,
    MagicPacketSize = 6 + 16 * MacAddressSize,
    WakeOnLanPort = 9,
};

typedef struct {
    int requestId;
    unsigned char macAddress[MacAddressSize];
    char unicastAddress[INET_ADDRSTRLEN];
} WakeRequest;

typedef struct {
    int sent;
    int error;
} SendResult;

static void recordSend(SendResult* result, ssize_t bytes)
{
    if (bytes == MagicPacketSize) {
        result->sent += 1;
    } else if (result->error == 0) {
        result->error = bytes < 0 ? errno : EIO;
    }
}

static void sendIpv4(const unsigned char* packet, const char* unicastAddress, SendResult* result)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        result->error = errno;
        return;
    }

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(WakeOnLanPort);

    // The limited broadcast reaches a sleeping PC whose address has aged out of every ARP cache.
    const int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == 0) {
        destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        recordSend(result, sendto(sock, packet, MagicPacketSize, 0,
                                  (const struct sockaddr*)&destination, sizeof(destination)));
    } else if (result->error == 0) {
        result->error = errno;
    }

    // Unicast to the Gateway's last address still arrives on networks that drop broadcasts,
    // for as long as the TV (or the router) remembers which MAC address owns that IP.
    if (unicastAddress[0] != '\0'
        && inet_pton(AF_INET, unicastAddress, &destination.sin_addr) == 1) {
        recordSend(result, sendto(sock, packet, MagicPacketSize, 0,
                                  (const struct sockaddr*)&destination, sizeof(destination)));
    }
    close(sock);
}

static void sendIpv6(const unsigned char* packet, SendResult* result)
{
    const int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return;
    }
    // Some access points filter IPv4 broadcast but still forward the all-nodes multicast group.
    struct sockaddr_in6 destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin6_family = AF_INET6;
    destination.sin6_port = htons(WakeOnLanPort);
    if (inet_pton(AF_INET6, "ff02::1", &destination.sin6_addr) == 1) {
        recordSend(result, sendto(sock, packet, MagicPacketSize, 0,
                                  (const struct sockaddr*)&destination, sizeof(destination)));
    }
    close(sock);
}

static void* sendWakeRequest(void* argument)
{
    WakeRequest* request = (WakeRequest*)argument;

    unsigned char packet[MagicPacketSize];
    memset(packet, 0xFF, MacAddressSize);
    for (int repetition = 1; repetition <= 16; ++repetition) {
        memcpy(packet + repetition * MacAddressSize, request->macAddress, MacAddressSize);
    }

    SendResult result = {0, 0};
    sendIpv4(packet, request->unicastAddress, &result);
    sendIpv6(packet, &result);

    MAIN_THREAD_ASYNC_EM_ASM({ Module["onWakeResult"]($0, $1, $2); },
                             request->requestId, result.sent, result.error);
    free(request);
    return NULL;
}

// Returns 0 when the request was queued; its outcome arrives through Module.onWakeResult.
EMSCRIPTEN_KEEPALIVE int wol_send(int requestId,
                                  const unsigned char* macAddress,
                                  const char* unicastAddress)
{
    WakeRequest* request = (WakeRequest*)calloc(1, sizeof(WakeRequest));
    if (request == NULL) {
        return ENOMEM;
    }
    request->requestId = requestId;
    memcpy(request->macAddress, macAddress, MacAddressSize);
    if (unicastAddress != NULL) {
        strncpy(request->unicastAddress, unicastAddress, sizeof(request->unicastAddress) - 1);
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int status = pthread_create(&thread, &attributes, sendWakeRequest, request);
    pthread_attr_destroy(&attributes);
    if (status != 0) {
        free(request);
        return status;
    }
    return 0;
}
