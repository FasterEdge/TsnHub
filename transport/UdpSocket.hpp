// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsnhub {

struct UdpEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;
    UdpSocket(UdpSocket &&other) noexcept;
    UdpSocket &operator=(UdpSocket &&other) noexcept;

    void bind(const UdpEndpoint &endpoint);
    void sendTo(const UdpEndpoint &endpoint, const std::vector<std::uint8_t> &payload) const;
    std::optional<std::vector<std::uint8_t>> receive(std::size_t maxBytes,
                                                     std::chrono::milliseconds timeout) const;

public:
#ifdef _WIN32
    using NativeSocket = std::uintptr_t;
    static constexpr NativeSocket invalidSocket = static_cast<NativeSocket>(~std::uintptr_t{0});
#else
    using NativeSocket = int;
    static constexpr NativeSocket invalidSocket = -1;
#endif

private:
    NativeSocket socket_{invalidSocket};
    void close() noexcept;
};

} // namespace tsnhub
