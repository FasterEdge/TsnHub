#include "transport/UdpSocket.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tsnhub {
namespace {
#ifdef _WIN32
using SystemSocket = SOCKET;
#else
using SystemSocket = int;
#endif

SystemSocket systemSocket(UdpSocket::NativeSocket value) {
    return static_cast<SystemSocket>(value);
}

#ifdef _WIN32
struct WinsockRuntime {
    WinsockRuntime() { WSADATA data{}; if(WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~WinsockRuntime() { WSACleanup(); }
};
WinsockRuntime &winsockRuntime() { static WinsockRuntime runtime; return runtime; }
#endif

sockaddr_in resolve(const UdpEndpoint &endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if(inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) == 1)
        return address;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if(getaddrinfo(endpoint.host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
        throw std::runtime_error("cannot resolve UDP host: " + endpoint.host);
    address.sin_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return address;
}
} // namespace

UdpSocket::UdpSocket() {
#ifdef _WIN32
    (void)winsockRuntime();
#endif
    socket_ = static_cast<NativeSocket>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if(socket_ == invalidSocket)
        throw std::runtime_error("cannot create UDP socket");
}

UdpSocket::~UdpSocket() { close(); }
UdpSocket::UdpSocket(UdpSocket &&other) noexcept : socket_(other.socket_) { other.socket_ = invalidSocket; }
UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if(this != &other) { close(); socket_ = other.socket_; other.socket_ = invalidSocket; }
    return *this;
}
void UdpSocket::close() noexcept {
    if(socket_ == invalidSocket) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket_));
#else
    ::close(socket_);
#endif
    socket_ = invalidSocket;
}
void UdpSocket::bind(const UdpEndpoint &endpoint) {
    const auto address = resolve(endpoint);
    int reuse = 1;
    setsockopt(systemSocket(socket_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    if(::bind(systemSocket(socket_), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
        throw std::runtime_error("cannot bind UDP endpoint " + endpoint.host + ":" + std::to_string(endpoint.port));
}
void UdpSocket::sendTo(const UdpEndpoint &endpoint, const std::vector<std::uint8_t> &payload) const {
    const auto address = resolve(endpoint);
    const auto sent = ::sendto(systemSocket(socket_), reinterpret_cast<const char *>(payload.data()),
                               static_cast<int>(payload.size()), 0,
                               reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    if(sent < 0 || static_cast<std::size_t>(sent) != payload.size())
        throw std::runtime_error("UDP send failed");
}
std::optional<std::vector<std::uint8_t>> UdpSocket::receive(std::size_t maxBytes,
                                                            std::chrono::milliseconds timeout) const {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(systemSocket(socket_), &readSet);
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
    const auto ready = select(0, &readSet, nullptr, nullptr, &value);
#else
    const auto ready = select(systemSocket(socket_) + 1, &readSet, nullptr, nullptr, &value);
#endif
    if(ready == 0) return std::nullopt;
    if(ready < 0) throw std::runtime_error("UDP receive select failed");
    std::vector<std::uint8_t> payload(maxBytes);
    const auto received = ::recvfrom(systemSocket(socket_), reinterpret_cast<char *>(payload.data()),
                                     static_cast<int>(payload.size()), 0, nullptr, nullptr);
    if(received < 0) throw std::runtime_error("UDP receive failed");
    payload.resize(static_cast<std::size_t>(received));
    return payload;
}
} // namespace tsnhub
