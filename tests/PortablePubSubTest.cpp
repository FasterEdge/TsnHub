// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "transport/PortablePubSub.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    tsnhub::Frame source;
    source.sequence = 42;
    source.priority = 6;
    source.stream = "opcua.temperature";
    source.payload = {0x55, 0x41, 0x44, 0x50, 0, 1, 2, 3};
    const auto packet = tsnhub::PortablePubSub::encode(source);
    const auto decoded = tsnhub::PortablePubSub::decode(packet);
    TSNHUB_TEST_REQUIRE(decoded);
    TSNHUB_TEST_REQUIRE(decoded->sequence == source.sequence);
    TSNHUB_TEST_REQUIRE(decoded->priority == source.priority);
    TSNHUB_TEST_REQUIRE(decoded->stream == source.stream);
    TSNHUB_TEST_REQUIRE(decoded->payload == source.payload);
    TSNHUB_TEST_REQUIRE(!tsnhub::PortablePubSub::decode({1, 2, 3}));

    // 头部结构: magic(4) + priority(1) + sequence(8) + streamLen(2) 共 15 字节
    TSNHUB_TEST_REQUIRE_MESSAGE(packet.size() == 15 + source.stream.size() + source.payload.size(),
                                "envelope header size must stay 15 bytes");
    TSNHUB_TEST_REQUIRE(packet[0] == 'T' && packet[1] == 'S' && packet[2] == 'N' && packet[3] == '1');
    TSNHUB_TEST_REQUIRE(packet[4] == source.priority);

    auto malformed = packet;
    malformed[13] = 0xff;
    malformed[14] = 0xff;
    TSNHUB_TEST_REQUIRE(!tsnhub::PortablePubSub::decode(malformed));

    // 声明的 stream 长度超出实际字节时必须拒绝, 不能越界读取。
    auto truncated = packet;
    truncated.resize(15 + source.stream.size() - 1);
    TSNHUB_TEST_REQUIRE_MESSAGE(!tsnhub::PortablePubSub::decode(truncated),
                                "truncated envelope must be rejected");
    // 长度不足 15 字节头的包必须拒绝。
    std::vector<std::uint8_t> headerOnly(packet.begin(), packet.begin() + 14);
    TSNHUB_TEST_REQUIRE_MESSAGE(!tsnhub::PortablePubSub::decode(headerOnly),
                                "packet shorter than the 15-byte header must be rejected");

    // 错误 magic 必须拒绝。
    auto wrongMagic = packet;
    wrongMagic[0] = 'X';
    TSNHUB_TEST_REQUIRE(!tsnhub::PortablePubSub::decode(wrongMagic));

    // 空 stream 是合法的: 15 字节头 + 纯 payload。
    tsnhub::Frame emptyStream;
    emptyStream.sequence = 1;
    emptyStream.priority = 0;
    emptyStream.payload = {0xaa, 0xbb};
    const auto emptyPacket = tsnhub::PortablePubSub::encode(emptyStream);
    TSNHUB_TEST_REQUIRE(emptyPacket.size() == 17);
    auto emptyDecoded = tsnhub::PortablePubSub::decode(emptyPacket);
    TSNHUB_TEST_REQUIRE(emptyDecoded);
    TSNHUB_TEST_REQUIRE(emptyDecoded->stream.empty());
    TSNHUB_TEST_REQUIRE(emptyDecoded->payload == emptyStream.payload);

    // 超过 65535 字节的 stream 无法用 2 字节长度域表达, encode 返回空包。
    tsnhub::Frame oversized;
    oversized.stream.assign(0x10000, 's');
    TSNHUB_TEST_REQUIRE_MESSAGE(tsnhub::PortablePubSub::encode(oversized).empty(),
                                "stream longer than 65535 bytes must not be encoded");

    // 线上优先级字节被截断到 0..7, 与解码端的钳位行为一致。
    auto clamped = packet;
    clamped[4] = 0xff;
    auto clampedDecoded = tsnhub::PortablePubSub::decode(clamped);
    TSNHUB_TEST_REQUIRE(clampedDecoded);
    TSNHUB_TEST_REQUIRE(clampedDecoded->priority == 7);

    TSNHUB_TEST_REPORT("Portable PubSub tests passed");
    return 0;
}
