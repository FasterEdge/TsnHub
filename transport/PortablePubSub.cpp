// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "transport/PortablePubSub.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace tsnhub {
namespace {
constexpr std::array<std::uint8_t, 4> magic{{'T', 'S', 'N', '1'}};
void append16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}
void append64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for(int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}
std::uint16_t read16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}
std::uint64_t read64(const std::uint8_t *data) {
    std::uint64_t value = 0;
    for(int i = 0; i < 8; ++i) value = (value << 8) | data[i];
    return value;
}
} // namespace

std::vector<std::uint8_t> PortablePubSub::encode(const Frame &frame) {
    if(frame.stream.size() > 0xffff) return {};
    std::vector<std::uint8_t> out;
    out.reserve(15 + frame.stream.size() + frame.payload.size());
    out.insert(out.end(), magic.begin(), magic.end());
    out.push_back(frame.priority);
    append64(out, frame.sequence);
    append16(out, static_cast<std::uint16_t>(frame.stream.size()));
    out.insert(out.end(), frame.stream.begin(), frame.stream.end());
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

std::optional<Frame> PortablePubSub::decode(const std::vector<std::uint8_t> &packet) {
    if(packet.size() < 15 || !std::equal(magic.begin(), magic.end(), packet.begin())) return std::nullopt;
    const auto streamLength = read16(packet.data() + 13);
    if(packet.size() < 15u + streamLength) return std::nullopt;
    Frame frame;
    frame.priority = std::min<std::uint8_t>(packet[4], 7);
    frame.sequence = read64(packet.data() + 5);
    frame.stream.assign(reinterpret_cast<const char *>(packet.data() + 15), streamLength);
    frame.payload.assign(packet.begin() + 15 + streamLength, packet.end());
    frame.receivedAt = std::chrono::steady_clock::now();
    return frame;
}

UdpPubSubEndpoint::UdpPubSubEndpoint(UdpEndpoint input, UdpEndpoint output)
    : outputAddress_(std::move(output)) { input_.bind(input); }
std::optional<Frame> UdpPubSubEndpoint::receive(std::chrono::milliseconds timeout) {
    auto packet = input_.receive(65507, timeout);
    if(!packet) return std::nullopt;
    return PortablePubSub::decode(*packet);
}
void UdpPubSubEndpoint::publish(const Frame &frame) {
    auto packet = PortablePubSub::encode(frame);
    if(packet.empty()) throw std::runtime_error("cannot encode PubSub frame");
    output_.sendTo(outputAddress_, packet);
}
} // namespace tsnhub
