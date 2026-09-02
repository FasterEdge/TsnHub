// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "transport/UdpSocket.hpp"
#include "tsn/Frame.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsnhub {

// A compact cross-platform envelope for carrying OPC UA PubSub network-message
// bytes through the simulator. The payload is opaque: native UADP bytes can be
// forwarded unchanged when produced by open62541 or another OPC UA stack.
class PortablePubSub {
public:
    static std::vector<std::uint8_t> encode(const Frame &frame);
    static std::optional<Frame> decode(const std::vector<std::uint8_t> &packet);
};

class UdpPubSubEndpoint {
public:
    UdpPubSubEndpoint(UdpEndpoint input, UdpEndpoint output);
    std::optional<Frame> receive(std::chrono::milliseconds timeout);
    void publish(const Frame &frame);
private:
    UdpSocket input_;
    UdpSocket output_;
    UdpEndpoint outputAddress_;
};

} // namespace tsnhub
